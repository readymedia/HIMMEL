// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/audio/midi_clock.h"

#include <algorithm>
#include <cmath>

namespace himmel::audio {

void MidiClock::message(uint8_t status, uint8_t d1, uint8_t d2, uint64_t t_us) {
    switch (status) {
        case 0xF8: tick(t_us); break;
        case 0xFA:  // Start: the next tick is beat 0
            running_ = true;
            phase_valid_ = true;
            position_ = -1;
            ticks_ = 0;
            break;
        case 0xFB: running_ = true; break;  // Continue from the current position
        case 0xFC: running_ = false; break; // Stop (ticks may keep coming: tempo stays known)
        case 0xF2: {                        // Song Position Pointer: 16ths = 6 ticks each
            const int spp = (d1 & 0x7F) | ((d2 & 0x7F) << 7);
            position_ = static_cast<int64_t>(spp) * 6 - 1;  // the next tick after Continue is this position
            phase_valid_ = true;
            break;
        }
        default: break;
    }
}

void MidiClock::tick(uint64_t t_us) {
    if (last_tick_us_ != 0 && t_us > last_tick_us_ && t_us - last_tick_us_ < kTimeoutUs) {
        intervals_[next_interval_] = static_cast<float>(t_us - last_tick_us_);
        next_interval_ = (next_interval_ + 1) % kPpq;
        n_intervals_ = std::min(n_intervals_ + 1, kPpq);
    } else if (last_tick_us_ != 0 && t_us - last_tick_us_ >= kTimeoutUs) {
        n_intervals_ = 0;  // after a gap the old intervals say nothing
        next_interval_ = 0;
    }
    last_tick_us_ = t_us;
    if (running_) ++position_;  // position_ = the tick that just played
    ++ticks_;
    if (n_intervals_ >= 2) {
        double sum = 0, sq = 0;
        for (int i = 0; i < n_intervals_; ++i) sum += intervals_[i];
        const double mean = sum / n_intervals_;
        for (int i = 0; i < n_intervals_; ++i) sq += (intervals_[i] - mean) * (intervals_[i] - mean);
        const double jitter = std::sqrt(sq / n_intervals_) / mean;  // relative
        bpm_ = static_cast<float>(std::clamp(60e6 / (mean * kPpq), 20.0, 300.0));
        // USB MIDI jitters ~1 ms on a ~20 ms tick: 5 % jitter is still fine.
        const float steady = static_cast<float>(std::clamp(1.0 - (jitter - 0.05) * 4.0, 0.0, 1.0));
        const float warm = std::min(1.0f, static_cast<float>(n_intervals_) / kPpq);  // a full beat of ticks
        confidence_ = steady * warm;
    }
}

ClockSource MidiClock::sample(uint64_t now_us) const {
    ClockSource s{};
    s.kind = static_cast<uint8_t>(ClockKind::Midi);
    s.last_tick_us = last_tick_us_;
    s.ticks = ticks_;
    s.time_us = now_us;
    const bool fresh = last_tick_us_ != 0 && now_us >= last_tick_us_ && now_us - last_tick_us_ < kTimeoutUs;
    if (!fresh) {
        s.run = static_cast<uint8_t>(ClockRun::Absent);
        s.bpm = bpm_;
        s.beat = static_cast<double>(position_) / kPpq;
        return s;
    }
    s.run = static_cast<uint8_t>(running_ ? ClockRun::Running : ClockRun::Stopped);
    s.phase_valid = phase_valid_ ? 1 : 0;
    s.bpm = bpm_;
    s.confidence = confidence_;
    double frac = 0;
    if (running_ && bpm_ > 0) {
        const double tick_us = 60e6 / (bpm_ * kPpq);
        frac = std::clamp(static_cast<double>(now_us - last_tick_us_) / tick_us, 0.0, 1.0);
    }
    s.beat = (static_cast<double>(position_) + frac) / kPpq;
    return s;
}

}  // namespace himmel::audio
