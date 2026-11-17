// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Pure part of the music feed (render loop: no allocation, no locks, no I/O).
#include "himmel/engine/reactions.h"

#include <algorithm>
#include <cmath>

namespace himmel::engine {

namespace {

using audio::Sig;
using ::himmel::fx::Signal;

// audio block index -> FX signal (Beat / Bar come from the clock).
struct Map {
    Sig from;
    Signal to;
};
constexpr Map kMap[] = {{Sig::Level, Signal::Level}, {Sig::Bass, Signal::Bass},     {Sig::Mid, Signal::Mid},
                        {Sig::High, Signal::High},   {Sig::Kick, Signal::Kick},     {Sig::Snare, Signal::Snare},
                        {Sig::Hat, Signal::Hat},     {Sig::Onset, Signal::Onset},   {Sig::Energy, Signal::Energy},
                        {Sig::Build, Signal::Build}, {Sig::Drop, Signal::Drop}};

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }
double frac(double x) { return x - std::floor(x); }

}  // namespace

float beat_pulse(double beats) { return std::isfinite(beats) && frac(beats) < 0.1 ? 1.0f : 0.0f; }
float bar_pulse(double beats) { return std::isfinite(beats) && frac(beats / 4.0) < 0.025 ? 1.0f : 0.0f; }

void ReactionFeed::update(const audio::Signals* sig, double beat_av, float dt, ::himmel::fx::Signals& out) {
    ok_ = sig != nullptr;
    const float step = std::clamp(dt, 0.0f, 0.25f) / kRestSeconds;
    for (const Map& m : kMap) {
        float& h = held_[static_cast<int>(m.to)];
        if (sig) h = clamp01(sig->v[static_cast<int>(m.from)]);
        else h = std::max(0.0f, h - step);  // glide to rest: a linear fall over 1 s from wherever it was
        out.v[static_cast<int>(m.to)] = h;
    }
    // The clock's pulses keep running with or without audio (they are the show clock).
    out.v[static_cast<int>(Signal::Beat)] = beat_pulse(beat_av);
    out.v[static_cast<int>(Signal::Bar)] = bar_pulse(beat_av);
    out.v[static_cast<int>(Signal::None)] = 0.0f;
}

ClockLead midi_lead(const audio::ClockSource& c, uint64_t now_us) {
    ClockLead l;
    const bool fresh = c.last_tick_us != 0 && now_us + 50000 >= c.last_tick_us &&
                       (now_us < c.last_tick_us || now_us - c.last_tick_us < 500000);
    if (c.kind != static_cast<uint8_t>(audio::ClockKind::Midi) || c.run != static_cast<uint8_t>(audio::ClockRun::Running) ||
        !fresh || !(c.confidence >= 0.5f) || !(c.bpm >= 20.0f && c.bpm <= 300.0f) || !std::isfinite(c.beat))
        return l;
    l.active = true;
    l.phase = c.phase_valid != 0;
    l.bpm = c.bpm;
    const double age_s = (static_cast<double>(now_us) - static_cast<double>(c.time_us)) / 1e6;
    l.beat = c.beat + std::clamp(age_s, -0.1, 0.5) * c.bpm / 60.0;
    return l;
}

// ---- per-knob reactions (D131) ----------------------------------------------

void react_empty_row(protocol::SlotReactions& row) {
    ::himmel::fx::EffectMeta none{};
    none.params = 0;
    for (int p = 0; p < protocol::kReactParams; ++p) ::himmel::fx::default_entry(none, p, row.p[p]);
}

void react_defaults(const ::himmel::fx::Library& lib, protocol::SlotReactions* out, int count) {
    for (int id = 0; id < count; ++id) {
        if (id < lib.count())
            for (int p = 0; p < protocol::kReactParams; ++p) ::himmel::fx::default_entry(lib.meta(id), p, out[id].p[p]);
        else
            react_empty_row(out[id]);
    }
}

protocol::Reject react_check(const protocol::Command& c) {
    using protocol::Reject;
    if (!protocol::react_args_ok(c.i)) return Reject::BadValue;
    for (float f : c.f)
        if (!std::isfinite(f)) return Reject::BadValue;
    const protocol::ReactOp op = protocol::unpack_react(c.i).op;
    if (c.param == protocol::kNone && op != protocol::ReactOp::Default && op != protocol::ReactOp::Off)
        return Reject::BadTarget;
    return Reject::None;
}

void react_apply(protocol::SlotReactions& row, const protocol::Command& c, const protocol::SlotReactions& defaults) {
    using namespace protocol;
    const ReactArgs a = unpack_react(c.i);
    const int p0 = c.param == kNone ? 0 : c.param;
    const int p1 = c.param == kNone ? kReactParams : c.param + 1;
    for (int p = p0; p < p1 && p < kReactParams; ++p) {
        ReactionEntry& e = row.p[p];
        const float env = e.env, value = e.value;
        const auto custom = [&] { e.source = static_cast<uint8_t>(ReactSource::Custom); };
        switch (a.op) {
            case ReactOp::Default: e = defaults.p[p]; break;
            case ReactOp::Off: e.source = static_cast<uint8_t>(ReactSource::Off); break;
            case ReactOp::Set:
                custom();
                e.signal = static_cast<uint8_t>(a.signal);
                e.mode = static_cast<uint8_t>(a.mode);
                e.curve = static_cast<uint8_t>(a.curve);
                e.quantise_beats = kReactQuantiseBeats[static_cast<int>(a.quantise)];
                e.hold_beats = static_cast<float>(a.hold_q) * 0.25f;
                e.amount = std::clamp(c.f[0], -1.0f, 1.0f);
                e.attack_ms = std::clamp(c.f[1], 0.0f, kReactMaxMs);
                e.release_ms = std::clamp(c.f[2], 0.0f, kReactMaxMs);
                e.threshold = std::clamp(c.f[3], 0.0f, 1.0f);
                break;
            case ReactOp::Amount: {
                custom();
                const float v = (c.flags & kRelative) ? e.amount + c.f[0] : c.f[0];
                e.amount = std::clamp(v, -1.0f, 1.0f);
                break;
            }
            case ReactOp::Signal:
                // GUESS: a knob with no reaction yet starts at amount 0.5 (ADD, 10 / 200 ms, linear); owner to tune.
                if (e.signal == static_cast<uint8_t>(ReactSignal::None) && e.amount == 0.0f) e.amount = 0.5f;
                custom();
                e.signal = static_cast<uint8_t>(a.signal);
                break;
            case ReactOp::Range:
                if (e.source == static_cast<uint8_t>(ReactSource::Default)) custom();
                if (c.f[0] < 0.0f) {
                    e.range_lo = defaults.p[p].range_lo;
                    e.range_hi = defaults.p[p].range_hi;
                    e.flags = static_cast<uint8_t>(e.flags & ~kReactRangeSet);
                } else {
                    e.range_lo = std::clamp(c.f[0], 0.0f, 1.0f);
                    e.range_hi = std::clamp(c.f[1], 0.0f, 1.0f);
                    e.flags = static_cast<uint8_t>(e.flags | kReactRangeSet);
                }
                break;
            default: break;
        }
        e.env = env, e.value = value;
    }
}

}  // namespace himmel::engine
