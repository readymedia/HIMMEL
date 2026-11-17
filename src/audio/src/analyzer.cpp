// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/audio/analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::audio {

namespace {

constexpr float kPi = 3.14159265358979f;

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }

// One-pole follower with separate attack / release time constants (seconds).
float follow(float env, float target, float attack_s, float release_s, float dt) {
    const float tau = target > env ? attack_s : release_s;
    if (tau <= 0.0f) return target;
    return env + (target - env) * (1.0f - std::exp(-dt / tau));
}

float one_pole(float y, float x, float tau_s, float dt) { return y + (x - y) * (1.0f - std::exp(-dt / tau_s)); }

// Onset release times (the time the envelope takes to fall to ~5 %): music.md §3.
constexpr float kKickRelease = 0.150f, kSnareRelease = 0.150f, kHatRelease = 0.060f;
constexpr float kKickRefractory = 0.100f, kSnareRefractory = 0.100f, kHatRefractory = 0.050f;

}  // namespace

float to_db(float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; }

// ---------------------------------------------------------------- biquads (RBJ cookbook)
Biquad Biquad::lowpass(float sr, float hz, float q) {
    const float w = 2.0f * kPi * std::min(hz, sr * 0.45f) / sr;
    const float c = std::cos(w), a = std::sin(w) / (2.0f * q), a0 = 1.0f + a;
    Biquad f;
    f.b0 = (1.0f - c) / 2.0f / a0;
    f.b1 = (1.0f - c) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0f * c / a0;
    f.a2 = (1.0f - a) / a0;
    return f;
}

Biquad Biquad::highpass(float sr, float hz, float q) {
    const float w = 2.0f * kPi * std::min(hz, sr * 0.45f) / sr;
    const float c = std::cos(w), a = std::sin(w) / (2.0f * q), a0 = 1.0f + a;
    Biquad f;
    f.b0 = (1.0f + c) / 2.0f / a0;
    f.b1 = -(1.0f + c) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0f * c / a0;
    f.a2 = (1.0f - a) / a0;
    return f;
}

// ---------------------------------------------------------------- FFT
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(re[i], re[j]), std::swap(im[i], im[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr, im[b] = im[a] - ti;
                re[a] += tr, im[a] += ti;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// ---------------------------------------------------------------- onset detector
void OnsetDetector::update_ref(float db, float hop_s, float peak_release_s) {
    h2 = h1;
    h1 = db;
    // The slow peak falls 20 dB over `peak_release_s`.
    peak_db = std::max(db, peak_db - hop_s * 20.0f / peak_release_s);
}

// ---------------------------------------------------------------- analyzer
Analyzer::Analyzer(float sample_rate, const AnalyzerSettings& s) : settings_(s) { reset(sample_rate); }

void Analyzer::reset(float sample_rate) {
    const AnalyzerSettings keep = settings_;
    // Everything back to rest (keeps the settings). The object is plain data.
    sr_ = sample_rate > 1000.0f ? sample_rate : 48000.0f;
    hop_ = std::max(16, static_cast<int>(std::lround(sr_ * 0.005f)));
    fill_ = 0;
    hops_ = 0;
    bass_lp_ = Biquad::lowpass(sr_, 150.0f);
    mid_hp_ = Biquad::highpass(sr_, 150.0f);
    mid_lp_ = Biquad::lowpass(sr_, 2500.0f);
    high_hp_ = Biquad::highpass(sr_, 2500.0f);
    kick_hp_ = Biquad::highpass(sr_, 40.0f);
    kick_lp_ = Biquad::lowpass(sr_, 120.0f);
    kick_lp2_ = Biquad::lowpass(sr_, 120.0f);
    sbody_hp_ = Biquad::highpass(sr_, 150.0f);
    sbody_lp_ = Biquad::lowpass(sr_, 400.0f);
    snoise_hp_ = Biquad::highpass(sr_, 2000.0f);
    snoise_lp_ = Biquad::lowpass(sr_, 6000.0f);
    hat_hp_ = Biquad::highpass(sr_, 7000.0f);
    acc_raw_ = acc_bass_ = acc_mid_ = acc_high_ = 0;
    env_kick_ = env_sbody_ = env_snoise_ = env_hat_ = 0;
    rel_kick_ = std::exp(-1.0f / (0.050f * sr_));   // per-sample release: 50 ms
    rel_snare_ = std::exp(-1.0f / (0.020f * sr_));  // 20 ms
    rel_hat_ = std::exp(-1.0f / (0.010f * sr_));    // 10 ms
    peak_raw_ = 0;
    auto_gain_db_ = 0;
    open_s_ = 0;
    avg_raw_db_ = -120;
    gate_open_ = false;
    clip_hold_s_ = 0;
    level_ = 0;
    for (int i = 0; i < 3; ++i) band_env_[i] = band_peak_[i] = 0;
    energy_db_ = energy_peak_db_ = -120;
    kick_ = snare_body_ = snare_noise_ = hat_ = OnsetDetector{};
    snare_pending_ = hat_pending_ = false;
    std::memset(ring_, 0, sizeof(ring_));
    ring_pos_ = 0;
    for (int i = 0; i < kFft; ++i) window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / (kFft - 1));
    // 32 log bands, 40 Hz .. 16 kHz, as FFT bin ranges (at least one bin each).
    const float bin_hz = sr_ / kFft;
    for (int k = 0; k < kSpectrumBands; ++k) {
        const float f0 = 40.0f * std::pow(400.0f, static_cast<float>(k) / kSpectrumBands);
        const float f1 = 40.0f * std::pow(400.0f, static_cast<float>(k + 1) / kSpectrumBands);
        int lo = std::max(1, static_cast<int>(std::floor(f0 / bin_hz)));
        int hi = std::max(lo, static_cast<int>(std::ceil(f1 / bin_hz)) - 1);
        lo = std::min(lo, kFft / 2 - 1), hi = std::min(hi, kFft / 2 - 1);
        band_lo_[k] = lo, band_hi_[k] = hi;
        spec_env_[k] = 0;
    }
    spec_peak_db_ = -120;
    out_ = Signals{};
    out_.gain_db = keep.gain_db;
    out_.rms_db = out_.peak_db = -120;
    out_.gated = 1;
    settings_ = keep;
}

void Analyzer::sample(float x) {
    if (!std::isfinite(x)) x = 0.0f;
    acc_raw_ += static_cast<double>(x) * x;
    peak_raw_ = std::max(peak_raw_, std::fabs(x));
    ring_[ring_pos_] = x;
    ring_pos_ = (ring_pos_ + 1) & (kFft - 1);
    const float b = bass_lp_.run(x);
    const float m = mid_lp_.run(mid_hp_.run(x));
    const float h = high_hp_.run(x);
    const float k = kick_lp_.run(kick_hp_.run(x));
    const float sb = sbody_lp_.run(sbody_hp_.run(x));
    const float sn = snoise_lp_.run(snoise_hp_.run(x));
    const float hh = hat_hp_.run(x);
    acc_bass_ += b * b, acc_mid_ += m * m, acc_high_ += h * h;
    // Onset bands: peak envelopes (instant attack, short release), so a low
    // tone's ripple inside a 5 ms hop never looks like a new hit.
    env_kick_ = std::max(std::fabs(k), env_kick_ * rel_kick_);
    env_sbody_ = std::max(std::fabs(sb), env_sbody_ * rel_snare_);
    env_snoise_ = std::max(std::fabs(sn), env_snoise_ * rel_snare_);
    env_hat_ = std::max(std::fabs(hh), env_hat_ * rel_hat_);
}

bool Analyzer::feed(const float*& x, int& n) {
    while (n > 0) {
        sample(*x++);
        --n;
        if (++fill_ >= hop_) {
            fill_ = 0;
            finish_hop();
            return true;
        }
    }
    return false;
}

void Analyzer::feed_silence_hop() {
    for (int i = fill_; i < hop_; ++i) sample(0.0f);
    fill_ = 0;
    finish_hop();
}

void Analyzer::finish_hop() {
    const float hs = hop_seconds();
    const float inv = 1.0f / static_cast<float>(hop_);
    auto rms = [&](double acc) { return std::sqrt(static_cast<float>(acc) * inv); };
    const float raw_rms = rms(acc_raw_);
    const float raw_db = to_db(raw_rms);
    const float peak_db = to_db(peak_raw_);
    const float r_bass = rms(acc_bass_), r_mid = rms(acc_mid_), r_high = rms(acc_high_);
    const float d_kick = to_db(env_kick_), d_sbody = to_db(env_sbody_), d_snoise = to_db(env_snoise_), d_hat = to_db(env_hat_);
    acc_raw_ = acc_bass_ = acc_mid_ = acc_high_ = 0;
    clip_hold_s_ = peak_raw_ >= 0.99f ? 0.5f : std::max(0.0f, clip_hold_s_ - hs);
    peak_raw_ = 0;

    // Gate on the raw input, 3 dB hysteresis.
    const float gate = std::clamp(settings_.gate_db, -96.0f, -20.0f);
    if (gate_open_ && raw_db < gate) gate_open_ = false;
    else if (!gate_open_ && raw_db > gate + 3.0f) gate_open_ = true;

    // Auto gain: pull the average RMS towards -18 dBFS; slow up, faster down; frozen while gated.
    const float manual = std::clamp(settings_.gain_db, -24.0f, 48.0f);
    if (!settings_.auto_gain) {
        auto_gain_db_ = 0.0f;
    } else if (gate_open_) {
        avg_raw_db_ = avg_raw_db_ < -110.0f ? raw_db : one_pole(avg_raw_db_, raw_db, 3.0f, hs);
        const float target = std::clamp(-18.0f - (avg_raw_db_ + manual), -12.0f, 36.0f);
        // The first 3 s after the input opens: settle fast (0.5 s); then up 5 s, down 1.5 s.
        open_s_ += hs;
        const float up = open_s_ < 3.0f ? 0.5f : 5.0f, down = open_s_ < 3.0f ? 0.5f : 1.5f;
        auto_gain_db_ = one_pole(auto_gain_db_, target, target > auto_gain_db_ ? up : down, hs);
    }
    const float gain_db = manual + auto_gain_db_;
    const float gain = std::pow(10.0f, gain_db / 20.0f);

    // Level: gained loudness, -48 dBFS -> 0 .. -6 dBFS -> 1.
    const float lv_target = gate_open_ ? clamp01((raw_db + gain_db + 48.0f) / 42.0f) : 0.0f;
    level_ = follow(level_, lv_target, 0.005f, 0.120f, hs);

    // Bands: envelope, each against its own slow peak (30 s release).
    const float band_rms[3] = {r_bass, r_mid, r_high};
    float band_v[3];
    for (int i = 0; i < 3; ++i) {
        band_env_[i] = follow(band_env_[i], gate_open_ ? band_rms[i] : 0.0f, 0.010f, 0.150f, hs);
        band_peak_[i] = std::max(band_env_[i], band_peak_[i] * std::exp(-hs / 30.0f));
        band_v[i] = band_peak_[i] > 1e-6f ? clamp01(band_env_[i] / band_peak_[i]) : 0.0f;
    }

    // Energy: ~16 s loudness against its slow peak (falls 24 dB over 120 s).
    const float e_in = gate_open_ ? raw_db : gate - 6.0f;
    if (energy_db_ < -110.0f) energy_db_ = e_in;
    energy_db_ = one_pole(energy_db_, e_in, 16.0f, hs);
    energy_peak_db_ = std::max(energy_db_, energy_peak_db_ - hs * 24.0f / 120.0f);
    const float energy = gate_open_ || energy_db_ > gate ? clamp01(1.0f + (energy_db_ - energy_peak_db_) / 18.0f) : 0.0f;

    // Onsets: band energy in dB against the band's own recent average.
    const float sens = clamp01(settings_.onset_sensitivity);
    const float thr = 9.0f - 6.0f * sens;  // 9 dB (hard hits only) .. 3 dB (everything)
    const double now_s = static_cast<double>(hops_ + 1) * hs;
    auto strength = [](const OnsetDetector& d, float db) {
        return 0.6f + 0.4f * clamp01((db - (d.peak_db - 18.0f)) / 18.0f);
    };
    auto hit = [&](OnsetDetector& d, float db, float min_rise, float refractory) {
        return gate_open_ && d.rise(db) > min_rise && db > d.h1 && db > d.peak_db - 30.0f &&
               now_s - d.last_hit_s >= refractory;
    };
    auto fire = [&](OnsetDetector& d, float s) {
        d.env = std::max(d.env, s);
        d.last_hit_s = now_s;
        ++d.count;
    };
    auto decay = [&](OnsetDetector& d, float release) { d.env *= std::exp(-hs * 3.0f / release); };
    decay(kick_, kKickRelease);
    decay(snare_noise_, kSnareRelease);
    decay(hat_, kHatRelease);
    // A kick = the low band hits and is louder than the snare body band (a snare's body is not).
    if (hit(kick_, d_kick, thr, kKickRefractory) && d_kick > d_sbody) fire(kick_, strength(kick_, d_kick));
    // A snare = the noise band hits loud AND the body (150-400 Hz) is up near its own peak
    // (a hat has noise but little body; a kick has body but only a weak click of noise).
    // The onset hop may hold only the first samples of a hit, so a noise onset is
    // judged one hop later, on a full hop (snares come 5 ms later than kicks; a hat
    // at the same moment waits for that answer).
    auto snare_like = [&] {
        return d_sbody > snare_body_.peak_db - 12.0f && d_snoise > snare_noise_.peak_db - 12.0f &&
               d_snoise > d_sbody - 8.0f;  // GUESS: a snare's rattle is near its body; a kick + hat is 10+ dB under
    };
    const bool hat_now = hit(hat_, d_hat, thr, kHatRefractory);
    if (snare_pending_) {  // decide the onset of the last hop
        if (snare_like()) {
            fire(snare_noise_, strength(snare_noise_, d_snoise));
        } else if (hat_pending_ || hat_now) {
            fire(hat_, strength(hat_, d_hat));
        }
        snare_pending_ = hat_pending_ = false;
    } else if (hit(snare_noise_, d_snoise, thr, kSnareRefractory) && snare_body_.rise(d_sbody) > thr * 0.5f) {
        snare_pending_ = true;  // snare or hat? next hop
        hat_pending_ = hat_now;
    } else if (hat_now) {
        fire(hat_, strength(hat_, d_hat));
    }
    kick_.update_ref(d_kick, hs, 30.0f);
    snare_body_.update_ref(d_sbody, hs, 30.0f);
    snare_noise_.update_ref(d_snoise, hs, 30.0f);
    hat_.update_ref(d_hat, hs, 30.0f);

    ++hops_;
    out_.hop = hops_;
    out_.v[static_cast<int>(Sig::Level)] = level_;
    out_.v[static_cast<int>(Sig::Bass)] = band_v[0];
    out_.v[static_cast<int>(Sig::Mid)] = band_v[1];
    out_.v[static_cast<int>(Sig::High)] = band_v[2];
    out_.v[static_cast<int>(Sig::Kick)] = clamp01(kick_.env);
    out_.v[static_cast<int>(Sig::Snare)] = clamp01(snare_noise_.env);
    out_.v[static_cast<int>(Sig::Hat)] = clamp01(hat_.env);
    out_.v[static_cast<int>(Sig::Onset)] = std::max({clamp01(kick_.env), clamp01(snare_noise_.env), clamp01(hat_.env)});
    out_.v[static_cast<int>(Sig::Energy)] = energy;
    out_.v[static_cast<int>(Sig::Build)] = 0.0f;
    out_.v[static_cast<int>(Sig::Drop)] = 0.0f;
    out_.kicks = kick_.count, out_.snares = snare_noise_.count, out_.hats = hat_.count;
    out_.rms_db = raw_db;
    out_.peak_db = peak_db;
    out_.gain_db = gain_db;
    out_.gated = gate_open_ ? 0 : 1;
    out_.clipping = clip_hold_s_ > 0.0f ? 1 : 0;
    // Waveform: the newest samples after gain.
    for (int i = 0; i < kWaveform; ++i) {
        const float s = ring_[(ring_pos_ - kWaveform + i) & (kFft - 1)] * gain;
        out_.waveform[i] = std::clamp(s, -1.0f, 1.0f);
    }
    spectrum();
}

void Analyzer::spectrum() {
    const float hs = hop_seconds();
    for (int i = 0; i < kFft; ++i) {
        re_[i] = ring_[(ring_pos_ + i) & (kFft - 1)] * window_[i];
        im_[i] = 0.0f;
    }
    fft(re_, im_, kFft);
    float db[kSpectrumBands];
    float top = -120.0f;
    for (int k = 0; k < kSpectrumBands; ++k) {
        double p = 0;
        for (int b = band_lo_[k]; b <= band_hi_[k]; ++b) p += static_cast<double>(re_[b]) * re_[b] + static_cast<double>(im_[b]) * im_[b];
        p /= (band_hi_[k] - band_lo_[k] + 1);
        db[k] = p > 1e-12 ? 10.0f * static_cast<float>(std::log10(p)) : -120.0f;
        top = std::max(top, db[k]);
    }
    spec_peak_db_ = std::max(top, spec_peak_db_ - hs * 20.0f / 30.0f);
    for (int k = 0; k < kSpectrumBands; ++k) {
        const float target = gate_open_ ? clamp01(1.0f + (db[k] - spec_peak_db_) / 48.0f) : 0.0f;
        spec_env_[k] = follow(spec_env_[k], target, 0.010f, 0.150f, hs);
        out_.spectrum[k] = spec_env_[k];
    }
}

}  // namespace himmel::audio
