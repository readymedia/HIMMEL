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

}  // namespace himmel::engine
