// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/fx/signals.h"

#include <algorithm>
#include <cmath>

namespace himmel::fx {

Signals TestSignal::at(double time_s, double beats) {
    auto frac = [](double x) { return static_cast<float>(x - std::floor(x)); };
    auto put = [](Signals& s, Signal k, float v) { s.v[static_cast<int>(k)] = std::clamp(v, 0.0f, 1.0f); };
    const float t = static_cast<float>(time_s);
    const double bar_pos = std::fmod(beats, 4.0);
    const float kick = std::exp(-frac(beats) * 8.0f);
    const int beat_in_bar = static_cast<int>(std::floor(bar_pos));
    const float snare = (beat_in_bar == 1 || beat_in_bar == 3) ? std::exp(-frac(bar_pos) * 10.0f) : 0.0f;
    const float hat = 0.7f * std::exp(-frac(beats * 2.0) * 14.0f);
    const float bass = 0.6f * kick + 0.2f * (0.5f + 0.5f * std::sin(t * 0.7f));
    const float mid = 0.5f + 0.4f * std::sin(t * 1.3f);
    const float high = 0.4f + 0.3f * std::sin(t * 2.1f + 1.0f);
    Signals s;
    put(s, Signal::Kick, kick);
    put(s, Signal::Snare, snare);
    put(s, Signal::Hat, hat);
    put(s, Signal::Bass, bass);
    put(s, Signal::Mid, mid);
    put(s, Signal::High, high);
    put(s, Signal::Level, 0.5f * bass + 0.3f * mid + 0.2f * high);
    put(s, Signal::Onset, std::max(kick, std::max(snare, hat)));
    put(s, Signal::Beat, frac(beats) < 0.1f ? 1.0f : 0.0f);
    put(s, Signal::Bar, bar_pos < 0.1 ? 1.0f : 0.0f);
    put(s, Signal::Energy, 0.5f + 0.4f * std::sin(t * 6.2831853f / 32.0f));
    put(s, Signal::Build, frac(time_s / 32.0));
    put(s, Signal::Drop, std::fmod(beats, 64.0) < 1.0 ? 1.0f : 0.0f);
    return s;
}

}  // namespace himmel::fx
