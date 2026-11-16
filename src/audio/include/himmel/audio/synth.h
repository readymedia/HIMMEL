// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Synthetic drums for the tests and `himmel_audio --selftest` (no device):
// a four-on-the-floor kick, snare on 2 and 4, closed hats on the eighths,
// optional bass line and room noise. Deterministic (own PRNG).
#pragma once

#include <cmath>
#include <cstdint>

namespace himmel::audio {

struct DrumSynth {
    float sr = 48000;
    float bpm = 120;
    float kick_gain = 0.8f, snare_gain = 0.45f, hat_gain = 0.25f;
    float bass_gain = 0.0f;      // a sustained 55 Hz bass under it (false-kick test)
    float noise_gain = 0.0f;     // room noise
    float level = 1.0f;          // overall (mic far away: 0.02)
    bool kick = true, snare = true, hats = true;
    uint32_t rng = 12345;
    double t = 0;                // seconds

    float noise() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 8388608.0f - 1.0f;
    }
    // Onset times (seconds) of beat `b` (kicks on every beat).
    double beat_time(int b) const { return b * 60.0 / bpm; }

    float next() {
        const double beat_len = 60.0 / bpm;
        const double pos = t / beat_len;
        const int beat = static_cast<int>(std::floor(pos));
        const double in_beat = (pos - beat) * beat_len;         // seconds since the beat
        const double in_eighth = std::fmod(t, beat_len / 2.0);  // seconds since the eighth
        float s = 0.0f;
        if (kick) {
            const double f = 50.0 + 90.0 * std::exp(-in_beat * 40.0);  // pitch drop
            const double ph = 2.0 * 3.14159265358979 * (50.0 * in_beat + 90.0 * (1.0 - std::exp(-in_beat * 40.0)) / 40.0);
            (void)f;
            s += kick_gain * static_cast<float>(std::sin(ph) * std::exp(-in_beat * 12.0));
        }
        const float n = noise();
        if (snare && (beat % 4 == 1 || beat % 4 == 3)) {
            const float env = static_cast<float>(std::exp(-in_beat * 25.0));
            s += snare_gain * env * (0.6f * n + 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 200.0 * in_beat)));
        }
        if (hats) {
            hp_ = 0.5f * (n - prev_n_) + 0.5f * hp_;  // crude high-pass noise
            prev_n_ = n;
            s += hat_gain * static_cast<float>(std::exp(-in_eighth * 80.0)) * hp_ * 2.0f;
        }
        if (bass_gain > 0.0f) s += bass_gain * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 55.0 * t));
        if (noise_gain > 0.0f) s += noise_gain * noise();
        t += 1.0 / sr;
        return s * level;
    }

private:
    float hp_ = 0.0f, prev_n_ = 0.0f;
};

}  // namespace himmel::audio
