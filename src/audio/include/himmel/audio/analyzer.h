// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The analysis inside himmel_audio (docs/interfaces/audio-block.md §2-3):
// mono samples in, one Signals snapshot per hop (~5 ms) out.
//
// Pure: no Windows, no threads. All memory is inside the object (no heap
// after construction), so feeding it never allocates. Unit tests:
// tests/audio/audio_tests.cpp.
#pragma once

#include <cstdint>

#include "himmel/audio/block.h"

namespace himmel::audio {

// RBJ biquad, direct form I (state per instance).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    static Biquad lowpass(float sr, float hz, float q = 0.7071f);
    static Biquad highpass(float sr, float hz, float q = 0.7071f);
    float run(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
};

struct AnalyzerSettings {
    float gain_db = 0;           // manual gain
    bool auto_gain = true;
    float gate_db = -60;         // raw RMS below this = silence
    float onset_sensitivity = 0.5f;  // 0..1: 0 = only hard hits, 1 = everything
};

// One drum's onset detector: the rise of the band's envelope (dB) over the
// last two hops (~10 ms), so a decaying hit or a held note never re-fires.
struct OnsetDetector {
    float h1 = -120, h2 = -120;  // the band in dB one and two hops ago
    float peak_db = -120;        // slow peak (for the strength and a floor)
    double last_hit_s = -1e9;
    float env = 0;               // the published envelope
    uint32_t count = 0;
    float rise(float db) const { return db - (h1 < h2 ? h1 : h2); }
    void update_ref(float db, float hop_s, float peak_release_s);
};

class Analyzer {
public:
    explicit Analyzer(float sample_rate = 48000.0f, const AnalyzerSettings& s = {});
    void reset(float sample_rate);           // new device: everything back to rest
    void set_settings(const AnalyzerSettings& s) { settings_ = s; }
    const AnalyzerSettings& settings() const { return settings_; }

    // Feeds up to n samples; stops early when a hop completes. Advances x / n.
    // Returns true when a hop completed: out() holds the new snapshot.
    bool feed(const float*& x, int& n);
    // A hop of silence (input lost / loopback with nothing playing): the
    // signals release towards rest at their normal speed.
    void feed_silence_hop();

    const Signals& out() const { return out_; }
    int hop_samples() const { return hop_; }
    float sample_rate() const { return sr_; }
    float hop_seconds() const { return static_cast<float>(hop_) / sr_; }
    // Seconds of audio analysed so far.
    double seconds() const { return static_cast<double>(hops_) * hop_seconds(); }

private:
    void sample(float x);
    void finish_hop();
    void spectrum();

    AnalyzerSettings settings_;
    float sr_ = 48000;
    int hop_ = 240;
    int fill_ = 0;
    uint64_t hops_ = 0;

    // per-sample filters
    Biquad bass_lp_, mid_hp_, mid_lp_, high_hp_;
    Biquad kick_hp_, kick_lp_, kick_lp2_, sbody_hp_, sbody_lp_, snoise_hp_, snoise_lp_, hat_hp_;
    // hop accumulators (sum of squares)
    double acc_raw_ = 0, acc_bass_ = 0, acc_mid_ = 0, acc_high_ = 0;
    float env_kick_ = 0, env_sbody_ = 0, env_snoise_ = 0, env_hat_ = 0;
    float rel_kick_ = 0, rel_snare_ = 0, rel_hat_ = 0;
    float peak_raw_ = 0;

    // gain / gate
    float auto_gain_db_ = 0;
    float avg_raw_db_ = -120;
    float open_s_ = 0;           // seconds the gate has been open since the input opened
    bool gate_open_ = false;
    float clip_hold_s_ = 0;

    // smoothed outputs
    float level_ = 0;
    float band_env_[3] = {};
    float band_peak_[3] = {};
    float energy_db_ = -120, energy_peak_db_ = -120;
    OnsetDetector kick_, snare_body_, snare_noise_, hat_;
    bool snare_pending_ = false, hat_pending_ = false;  // an unclear onset waits one hop

    // waveform + spectrum
    static constexpr int kFft = 1024;
    float ring_[kFft] = {};
    int ring_pos_ = 0;
    float window_[kFft] = {};
    float re_[kFft] = {}, im_[kFft] = {};
    int band_lo_[kSpectrumBands] = {}, band_hi_[kSpectrumBands] = {};
    float spec_env_[kSpectrumBands] = {};
    float spec_peak_db_ = -120;

    Signals out_{};
};

// In-place radix-2 FFT (n a power of two). Exposed for the tests.
void fft(float* re, float* im, int n);

// Linear amplitude -> dBFS (floor -120).
float to_db(float lin);

}  // namespace himmel::audio
