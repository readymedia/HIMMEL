// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// MIDI clock tracker (docs/interfaces/audio-block.md §4): real-time bytes with
// their arrival times in, tempo + beat position + confidence out.
// 24 ticks per beat; Start (FA) = beat 0, Continue (FB), Stop (FC), Song
// Position Pointer (F2, in 16ths). Pure: no Windows, no heap.
#pragma once

#include <cstdint>

#include "himmel/audio/block.h"

namespace himmel::audio {

class MidiClock {
public:
    static constexpr int kPpq = 24;
    static constexpr uint64_t kTimeoutUs = 500000;  // no tick for this long = Absent

    // One MIDI message (status byte + up to 2 data bytes) at `t_us`.
    void message(uint8_t status, uint8_t d1, uint8_t d2, uint64_t t_us);
    // The source as it is at `now_us` (beat extrapolated from the last tick,
    // never past the next one).
    ClockSource sample(uint64_t now_us) const;
    void reset() { *this = MidiClock{}; }

    float bpm() const { return bpm_; }
    bool phase_valid() const { return phase_valid_; }

private:
    void tick(uint64_t t_us);
    uint64_t last_tick_us_ = 0;
    int64_t position_ = 0;       // the last tick that played (beat 0 = tick 0; -1 = before the first after Start)
    bool running_ = false;       // between Start / Continue and Stop
    bool phase_valid_ = false;   // a Start or SPP was seen
    uint32_t ticks_ = 0;
    float intervals_[kPpq] = {};
    int n_intervals_ = 0, next_interval_ = 0;
    float bpm_ = 0, confidence_ = 0;
};

}  // namespace himmel::audio
