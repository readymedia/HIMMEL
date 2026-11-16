// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/transport.h"

#include <algorithm>
#include <cmath>

namespace himmel::engine {

using namespace himmel::protocol;

namespace {

constexpr double kEps = 1e-6;  // "just before the out point": the last frame of a range

double wrap_into(double pos, double a, double b) {
    const double len = b - a;
    if (len <= 0) return a;
    double p = std::fmod(pos - a, len);
    if (p < 0) p += len;
    return a + p;
}

bool once_mode(uint8_t m) {
    return m == static_cast<uint8_t>(PlayMode::OnceHold) || m == static_cast<uint8_t>(PlayMode::OnceEject) ||
           m == static_cast<uint8_t>(PlayMode::OnceNext);
}

}  // namespace

// ---------------------------------------------------------------- beat clock
void BeatClock::reset(uint64_t now_us) {
    bpm_ = 120.0;
    anchor_beat_ = 0.0;
    anchor_us_ = now_us;
    tap_n_ = 0;
    tapped_ = false;
}

double BeatClock::beat(uint64_t t_us) const {
    const double dt = t_us >= anchor_us_ ? static_cast<double>(t_us - anchor_us_) : -static_cast<double>(anchor_us_ - t_us);
    return anchor_beat_ + dt / 1e6 * bpm_ / 60.0;
}

void BeatClock::anchor(uint64_t now_us) {
    anchor_beat_ = beat(now_us);
    anchor_us_ = now_us;
}

void BeatClock::set_bpm(double bpm, uint64_t now_us) {
    anchor(now_us);
    bpm_ = std::clamp(bpm, kMinBpm, kMaxBpm);
}

void BeatClock::resync(uint64_t now_us) {
    anchor_beat_ = 0.0;
    anchor_us_ = now_us;
}

void BeatClock::tap(uint64_t now_us) {
    if (tap_n_ > 0 && (now_us < tap_us_[tap_n_ - 1] || now_us - tap_us_[tap_n_ - 1] > kTapTimeoutUs)) tap_n_ = 0;
    if (tap_n_ == 8) {
        for (int i = 1; i < 8; ++i) tap_us_[i - 1] = tap_us_[i];
        tap_n_ = 7;
    }
    tap_us_[tap_n_++] = now_us;
    if (tap_n_ >= 2) {
        const double interval_us = static_cast<double>(tap_us_[tap_n_ - 1] - tap_us_[0]) / (tap_n_ - 1);
        if (interval_us > 0) {
            anchor(now_us);
            bpm_ = std::clamp(60e6 / interval_us, kMinBpm, kMaxBpm);
            tapped_ = true;
        }
    }
    // Every tap lands on a beat: the nearest whole beat is "now".
    anchor(now_us);
    anchor_beat_ = std::round(anchor_beat_);
}

double grid_beats(GridDiv g) {
    switch (g) {
        case GridDiv::Quarter: return 0.25;
        case GridDiv::Beat: return 1.0;
        case GridDiv::Bar: return 4.0;
        case GridDiv::TwoBars: return 8.0;
        case GridDiv::FourBars: return 16.0;
        default: return 0.0;
    }
}

// ---------------------------------------------------------------- playhead
void Playhead::range(const ClipGeom& g, const LayerTransport& t, double& a, double& b) {
    const double n = static_cast<double>(g.frames);
    a = std::clamp(static_cast<double>(t.in), 0.0, 1.0) * n;
    b = std::clamp(static_cast<double>(t.out), 0.0, 1.0) * n;
    if (b < a) std::swap(a, b);
    if (b - a < 1.0) {
        b = std::min(n, a + 1.0);
        a = std::max(0.0, b - 1.0);
    }
}

double Playhead::base_rate(const ClipGeom& g, const LayerTransport& t, double bpm) {
    if (t.time_base == static_cast<uint8_t>(TimeBase::Bpm) && bpm > 0) {
        double a, b;
        range(g, t, a, b);
        return (b - a) / (std::max(0.25, static_cast<double>(t.beats)) * 60.0 / bpm);
    }
    return g.fps;
}

double Playhead::velocity(const ClipGeom& g, const LayerTransport& t, const Head& h, double bpm) const {
    return base_rate(g, t, bpm) * speed_now_ * (t.backward ? -1.0 : 1.0) * h.dir;
}

void Playhead::set_inner_phase(const ClipGeom& g, const LayerTransport& t, Head& h) {
    if (!t.inner_loop) {
        h.inner = 0;
        return;
    }
    const double n = static_cast<double>(g.frames);
    const double la = std::clamp(static_cast<double>(t.loop_in), 0.0, 1.0) * n;
    const double lb = std::clamp(static_cast<double>(t.loop_out), 0.0, 1.0) * n;
    h.inner = static_cast<uint8_t>(h.pos < la   ? PlayheadPhase::Intro
                                   : h.pos < lb ? PlayheadPhase::InnerLoop
                                                : PlayheadPhase::Outro);
}

void Playhead::start(const ClipGeom& g, const LayerTransport& t, double pos, double speed_now) {
    frames_ = g.frames;
    main_ = Head{};
    main_.pos = std::clamp(pos, 0.0, std::max(0.0, static_cast<double>(g.frames) - kEps));
    ov_ = Override::None;
    ov_beats_ = 0.0;
    speed_now_ = glide_from_ = glide_to_ = speed_now;
    glide_t_ = 0.0;
    glide_init_ = true;
    set_inner_phase(g, t, main_);
}

void Playhead::clear() {
    frames_ = 0;
    main_ = Head{};
    ov_ = Override::None;
    ov_beats_ = 0.0;
    glide_init_ = false;
}

uint32_t Playhead::rng_next() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_ = x;
}

uint32_t Playhead::move(const ClipGeom& g, const LayerTransport& t, Head& h, double df) {
    if (df == 0.0 || g.frames <= 0) return kEvNone;
    double a, b;
    range(g, t, a, b);
    const double n = static_cast<double>(g.frames);
    h.pos += df;

    // The 4-point loop: intro -> [loop-in, loop-out) until released.
    if (t.inner_loop && h.inner != static_cast<uint8_t>(PlayheadPhase::Outro) && h.inner != 0) {
        const double la = std::clamp(static_cast<double>(t.loop_in), 0.0, 1.0) * n;
        const double lb = std::clamp(static_cast<double>(t.loop_out), 0.0, 1.0) * n;
        if (lb - la >= 1.0) {
            if (df > 0 && h.pos >= lb) {
                h.pos = wrap_into(h.pos, la, lb);
                h.inner = static_cast<uint8_t>(PlayheadPhase::InnerLoop);
                return kEvLooped;
            }
            if (df < 0 && h.inner == static_cast<uint8_t>(PlayheadPhase::InnerLoop) && h.pos < la) {
                h.pos = wrap_into(h.pos, la, lb);
                return kEvLooped;
            }
        }
    }

    const auto mode = static_cast<PlayMode>(t.mode);
    uint32_t ev = kEvNone;
    switch (mode) {
        case PlayMode::PingPong: {
            const double top = b - kEps;
            for (int k = 0; k < 4 && (h.pos > top || h.pos < a); ++k) {
                if (h.pos > top) {
                    h.pos = 2.0 * top - h.pos;
                    h.dir = -h.dir;
                } else {
                    h.pos = 2.0 * a - h.pos;
                    h.dir = -h.dir;
                    ev |= kEvLooped;  // one there-and-back is one loop
                }
            }
            h.pos = std::clamp(h.pos, a, top);
            break;
        }
        case PlayMode::OnceHold:
        case PlayMode::OnceEject:
        case PlayMode::OnceNext:
            if (h.pos >= b || h.pos < a) {
                h.pos = h.pos >= b ? b - kEps : a;
                if (!h.ended) ev |= kEvEnded;
                h.ended = true;
            }
            break;
        case PlayMode::Scratch: h.pos = std::clamp(h.pos, 0.0, n - kEps); break;
        default:  // Loop, Random, Chop, Step
            if (h.pos >= b || h.pos < a) {
                const bool fwd = h.pos >= b;
                h.pos = wrap_into(h.pos, a, b);
                ev |= kEvLooped;
                // GUESS: after the outro, a looping clip starts over with its intro (the inner loop re-arms).
                if (t.inner_loop && fwd) h.inner = static_cast<uint8_t>(PlayheadPhase::Intro);
            }
            break;
    }
    return ev;
}

uint32_t Playhead::step_events(const LayerTransport& t, Head& h, const TickIn& in) {
    const auto mode = static_cast<PlayMode>(t.mode);
    const double interval = std::max(1.0 / 16.0, static_cast<double>(mode == PlayMode::Step ? t.step_div : t.interval));
    int n = 0;
    if (t.time_base == static_cast<uint8_t>(TimeBase::Bpm)) {
        if (in.beat1 > in.beat0)
            n = static_cast<int>(std::floor(in.beat1 / interval) - std::floor(in.beat0 / interval));
    } else {
        h.timer += in.dt;
        n = static_cast<int>(std::floor(h.timer / interval));
        h.timer -= n * interval;
    }
    return static_cast<uint32_t>(std::clamp(n, 0, 4));
}

uint32_t Playhead::advance(const ClipGeom& g, const LayerTransport& t, bool playing, float speed_target,
                           const TickIn& in) {
    if (g.frames <= 0) return kEvNone;
    if (frames_ != g.frames) {  // first tick, or the clip's length became known
        frames_ = g.frames;
        main_.pos = std::clamp(main_.pos, 0.0, static_cast<double>(g.frames) - kEps);
    }
    // Speed glide (clips-and-playback.md §2.7): a new target ramps from where the speed is now.
    const double target = speed_target;
    if (!glide_init_) speed_now_ = glide_from_ = glide_to_ = target, glide_init_ = true;
    if (target != glide_to_) {
        glide_from_ = speed_now_;
        glide_to_ = target;
        glide_t_ = 0.0;
    }
    const double glide_s = std::max(0.0, static_cast<double>(t.glide)) * 60.0 / std::max(1.0, in.bpm);
    if (glide_s <= 0.0) {
        speed_now_ = glide_to_;
    } else {
        glide_t_ += in.dt;
        const double p = std::min(1.0, glide_t_ / glide_s);
        speed_now_ = glide_from_ + (glide_to_ - glide_from_) * p;
    }
    backward_ = t.backward != 0;
    if (!playing) return kEvNone;
    if (landed_) {  // a jump shows its exact frame (the warm one) for this frame, then plays on
        landed_ = false;
        return kEvNone;
    }

    const auto mode = static_cast<PlayMode>(t.mode);
    double a, b;
    range(g, t, a, b);
    uint32_t ev = kEvNone;
    if (!(main_.ended && once_mode(t.mode))) {
        if (mode == PlayMode::Random || mode == PlayMode::Chop || mode == PlayMode::Step) {
            const uint32_t n = step_events(t, main_, in);
            for (uint32_t k = 0; k < n; ++k) {
                if (mode == PlayMode::Random) {
                    const double u = (rng_next() & 0xFFFFFF) / 16777216.0;
                    main_.pos = wrap_into(main_.pos + (u * 2.0 - 1.0) * t.distance * (b - a), a, b);
                    set_inner_phase(g, t, main_);
                } else if (mode == PlayMode::Chop) {
                    const int slices = std::clamp<int>(t.slices, 2, 16);
                    const uint32_t s = rng_next() % static_cast<uint32_t>(slices);
                    main_.pos = a + s * (b - a) / slices;
                    set_inner_phase(g, t, main_);
                } else {  // Step: k frames per step, in the play direction; speed 0 holds
                    const double sign = (speed_now_ < 0 ? -1.0 : speed_now_ > 0 ? 1.0 : 0.0) * (t.backward ? -1.0 : 1.0);
                    ev |= move(g, t, main_, sign * std::max<int>(1, t.step_frames));
                }
            }
        }
        if (mode != PlayMode::Step && mode != PlayMode::Scratch)
            ev |= move(g, t, main_, velocity(g, t, main_, in.bpm) * in.dt);
    }

    const double n = static_cast<double>(g.frames);
    switch (ov_) {
        case Override::BeatLoop: {
            ov_elapsed_ += velocity(g, t, main_, in.bpm) * in.dt;
            double e = std::fmod(ov_elapsed_, ov_len_);
            if (e < 0) e += ov_len_;
            ov_pos_ = wrap_into(ov_start_ + e, 0.0, n);
            break;
        }
        case Override::Reverse:
            ov_pos_ = wrap_into(ov_pos_ - velocity(g, t, main_, in.bpm) * in.dt, a, b);
            break;
        default: break;
    }
    return ev;
}

void Playhead::jump(const ClipGeom& g, const LayerTransport& t, double pos) {
    ov_ = Override::None;
    ov_beats_ = 0.0;
    main_.pos = std::clamp(pos, 0.0, std::max(0.0, static_cast<double>(g.frames) - kEps));
    main_.ended = false;
    set_inner_phase(g, t, main_);
    landed_ = true;
}

void Playhead::beat_loop(const ClipGeom& g, const LayerTransport& t, double beats, double bpm) {
    const double len = std::max(1.0, beats * 60.0 / std::max(1.0, bpm) * std::fabs(base_rate(g, t, bpm) * speed_now_));
    ov_beats_ = beats;
    if (ov_ == Override::BeatLoop) {  // a new length keeps the loop's start (pressure: shorter / longer)
        ov_len_ = len;
        return;
    }
    // GUESS: the loop starts at the playhead now (not quantised back to the last beat); the
    // launch itself can wait for the grid. Confidence medium.
    ov_start_ = pos();
    ov_pos_ = ov_start_;
    ov_elapsed_ = 0.0;
    ov_len_ = len;
    ov_ = Override::BeatLoop;
}

void Playhead::beat_loop_off(bool catch_up) {
    if (ov_ != Override::BeatLoop) return;
    if (!catch_up) main_.pos = ov_pos_;
    ov_ = Override::None;
    ov_beats_ = 0.0;
}

void Playhead::scratch_hold(const ClipGeom& g, const LayerTransport& t) {
    if (t.mode == static_cast<uint8_t>(PlayMode::Scratch) || ov_ == Override::Scratch || g.frames <= 0) return;
    ov_pos_ = pos();
    ov_beats_ = 0.0;
    ov_ = Override::Scratch;
}

void Playhead::scratch_move(const ClipGeom& g, const LayerTransport& t, double p) {
    if (g.frames <= 0) return;
    double a, b;
    range(g, t, a, b);
    const double q = std::clamp(p, a, b - kEps);
    if (t.mode == static_cast<uint8_t>(PlayMode::Scratch)) {  // the controller is the only thing that moves it
        main_.pos = q;
        return;
    }
    scratch_hold(g, t);
    ov_pos_ = q;
}

void Playhead::scratch_release(bool snap_back) {
    if (ov_ != Override::Scratch) return;
    if (!snap_back) main_.pos = ov_pos_;
    ov_ = Override::None;
}

void Playhead::reverse(bool held) {
    if (held && ov_ == Override::None) {
        ov_pos_ = main_.pos;
        ov_ = Override::Reverse;
    } else if (!held && ov_ == Override::Reverse) {
        ov_ = Override::None;  // catch-up: back where the clip would have been (§2.1)
    }
}

void Playhead::release_inner() {
    if (main_.inner == static_cast<uint8_t>(PlayheadPhase::Intro) ||
        main_.inner == static_cast<uint8_t>(PlayheadPhase::InnerLoop))
        main_.inner = static_cast<uint8_t>(PlayheadPhase::Outro);
}

void Playhead::mode_changed() {
    main_.ended = false;
    main_.timer = 0.0;
    main_.dir = 1;
}

double Playhead::pos() const { return ov_ != Override::None ? ov_pos_ : main_.pos; }

int64_t Playhead::frame() const {
    if (frames_ <= 0) return -1;
    return std::clamp<int64_t>(static_cast<int64_t>(std::floor(pos())), 0, frames_ - 1);
}

int Playhead::direction() const {
    const int base = (speed_now_ < 0 ? -1 : 1) * main_.dir * (backward_ ? -1 : 1);
    return ov_ == Override::Reverse ? -base : base;
}

PlayheadPhase Playhead::phase(bool playing) const {
    if (frames_ <= 0) return PlayheadPhase::Empty;
    if (!playing) return PlayheadPhase::Paused;
    if (main_.inner) return static_cast<PlayheadPhase>(main_.inner);
    return PlayheadPhase::Playing;
}

}  // namespace himmel::engine
