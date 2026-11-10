#include "himmel/mk2/input.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace himmel::mk2 {

namespace {

constexpr int kEncoderSpan = 16;
constexpr int kKnobMinSpan = 1000;  // ni-mk2 says 0..1000; we measured ~1008 per turn

int wrap_delta(int d, int span) {
    if (d > span / 2) d -= span;
    if (d < -span / 2) d += span;
    return d;
}

int16_t clamp16(int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }

}  // namespace

int ControlsParser::knob_value(std::span<const uint8_t> p, const KnobPos& k) const {
    if (k.byte < 0 || k.byte >= static_cast<int>(p.size())) return 0;
    if (k.word) {
        if (k.byte + 1 >= static_cast<int>(p.size())) return 0;
        return p[k.byte] | (p[k.byte + 1] << 8);
    }
    return k.low_nibble ? (p[k.byte] & 0x0F) : (p[k.byte] >> 4);
}

void ControlsParser::parse(std::span<const uint8_t> payload, uint64_t t_us, EventBatch& out) {
    if (payload.size() < static_cast<size_t>(kControlsPayload)) return;
    auto p = payload.first(kControlsPayload);
    if (!valid_) {  // first report after (re)connect: baseline only, no events
        std::copy(p.begin(), p.end(), last_.begin());
        for (int k = 0; k < kKnobs; ++k) knob_max_seen_[k] = knob_value(p, map_.knobs[k]);
        valid_ = true;
        return;
    }
    const std::span<const uint8_t> prev(last_);
    for (int i = 0; i < kButtons; ++i) {
        const auto& b = map_.buttons[i];
        if (b.byte < 0) continue;
        const bool now = (p[b.byte] >> b.bit) & 1;
        const bool was = (prev[b.byte] >> b.bit) & 1;
        if (now != was)
            out.push({t_us, now ? EventType::ButtonDown : EventType::ButtonUp, static_cast<uint8_t>(i), 0, 0, 0});
    }
    for (int k = 0; k < kKnobs; ++k) {
        const int v = knob_value(p, map_.knobs[k]);
        const int old = knob_value(prev, map_.knobs[k]);
        if (v == old) continue;
        knob_max_seen_[k] = std::max(knob_max_seen_[k], v);
        const int span = std::max(kKnobMinSpan, knob_max_seen_[k] + 1);
        out.push({t_us, EventType::KnobDelta, static_cast<uint8_t>(k), clamp16(wrap_delta(v - old, span)),
                  static_cast<uint16_t>(v), 0});
    }
    const int e = knob_value(p, map_.encoder), eo = knob_value(prev, map_.encoder);
    if (e != eo)
        out.push({t_us, EventType::EncoderDelta, 0, clamp16(wrap_delta(e - eo, kEncoderSpan)), static_cast<uint16_t>(e), 0});
    std::copy(p.begin(), p.end(), last_.begin());
}

void parse_pad_report(std::span<const uint8_t> payload, std::array<uint16_t, kPads>& by_hw_id) {
    // 32 little-endian words: (id << 12) | pressure. Words 16..31 repeat 0..15.
    const size_t words = std::min<size_t>(payload.size() / 2, 16);
    for (size_t w = 0; w < words; ++w) {
        const uint16_t v = static_cast<uint16_t>(payload[2 * w] | (payload[2 * w + 1] << 8));
        by_hw_id[v >> 12] = v & 0x0FFF;
    }
}

uint8_t pad_velocity(int p, const PadConfig& cfg) {
    const float x = std::clamp((p - 60) / static_cast<float>(std::max(1, cfg.full - 60)), 0.0f, 1.0f);
    const int v = static_cast<int>(std::lround(127.0f * std::pow(x, cfg.gamma)));
    return static_cast<uint8_t>(std::clamp(v, 1, 127));
}

void PadTracker::process(const std::array<uint16_t, kPads>& by_hw_id, uint64_t t, EventBatch& out) {
    for (int pad = 0; pad < kPads; ++pad) {
        const int hw = map_.pad_hw_id[pad];
        if (hw < 0) continue;
        const int floor = map_.noise_floor[pad];
        const int pr = by_hw_id[hw];
        const uint8_t id = static_cast<uint8_t>(pad + 1);
        Pad& s = state_[pad];
        const bool released = pr < floor + cfg_.release;
        auto hit = [&]() {
            out.push({t, EventType::PadHit, id, pad_velocity(s.window_max - floor, cfg_),
                      static_cast<uint16_t>(s.window_max), 0});
            s.phase = Phase::Held;
            s.last_sent = pr;
            s.t_last_sent = t;
        };
        switch (s.phase) {
            case Phase::Idle:
                if (pr > floor + cfg_.onset) {
                    s = {Phase::Window, t, pr, 0, 0};
                }
                break;
            case Phase::Window:
                if (t - s.t_onset <= cfg_.window_us) s.window_max = std::max(s.window_max, pr);
                if (released) {  // let go inside the window: a short tap, or nothing
                    if (s.window_max >= floor + cfg_.confirm) {
                        hit();
                        out.push({t, EventType::PadRelease, id, 0, 0, 0});
                    }
                    s.phase = Phase::Idle;
                } else if (t - s.t_onset >= cfg_.window_us) {
                    if (s.window_max >= floor + cfg_.confirm || pr >= floor + cfg_.confirm) hit();
                    else s.phase = Phase::WaitConfirm;  // very slow press: wait until it is real
                }
                break;
            case Phase::WaitConfirm:
                if (released) s.phase = Phase::Idle;  // ghost touch, no events
                else if (pr >= floor + cfg_.confirm) hit();  // velocity stays low (slow press)
                break;
            case Phase::Held:
                if (released) {
                    out.push({t, EventType::PadRelease, id, 0, 0, 0});
                    s.phase = Phase::Idle;
                } else if (std::abs(pr - s.last_sent) >= cfg_.aftertouch_step &&
                           t - s.t_last_sent >= cfg_.aftertouch_interval_us) {
                    out.push({t, EventType::PadPressure, id, static_cast<int16_t>(pr), static_cast<uint16_t>(pr), 0});
                    s.last_sent = pr;
                    s.t_last_sent = t;
                }
                break;
        }
    }
}

}  // namespace himmel::mk2
