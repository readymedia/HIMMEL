// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Music -> reactions (M8, AUDIO track, docs/interfaces/audio-block.md §5).
//
// ReactionFeed turns the audio block's signals + the show clock into the
// himmel::fx::Signals every effect's reaction listens to. Pure: no Windows,
// no heap, no locks; it runs once per frame in the render loop.
// AudioLink (reactions_link.cpp) maps Local\HimmelAudio at start-up and
// copies a section per frame (seqlock, no waiting).
#pragma once

#include <cstdint>

#include "himmel/audio/block.h"
#include "himmel/engine/shm_block.h"
#include "himmel/fx/fx.h"

namespace himmel::engine {

class ReactionFeed {
public:
    static constexpr float kRestSeconds = 1.0f;  // no audio: the signals glide to 0 over 1 s (music.md §5)

    // `sig`: a fresh copy of the block's signals, or nullptr when there is no
    // audio (himmel_audio dead / hung, input lost). `beat_av`: the show clock
    // in beats at now + AV offset. `dt`: seconds since the last frame.
    void update(const audio::Signals* sig, double beat_av, float dt, ::himmel::fx::Signals& out);
    bool audio_ok() const { return ok_; }

private:
    float held_[::himmel::fx::kSignals] = {};
    bool ok_ = false;
};

// Beat / bar pulses from the show clock: 1 for the first 10 % of each beat / bar.
float beat_pulse(double beats);
float bar_pulse(double beats);

// Which external clock leads (audio-block.md §4, v1: MIDI only).
struct ClockLead {
    bool active = false;   // follow it
    bool phase = false;    // also glide the phase (a Start / SPP was seen)
    double bpm = 0, beat = 0;
};
// MIDI leads while it runs, is fresh (< 500 ms since a tick) and confidence >= 0.5.
// `beat` is extrapolated to now_us.
ClockLead midi_lead(const audio::ClockSource& c, uint64_t now_us);

// The renderer's view of Local\HimmelAudio (Windows; start-up maps it, frames only copy).
class AudioLink {
public:
    // Start-up only: maps the block (creates it zeroed if himmel_audio is not up yet).
    bool open();
    // Once per frame: copies the signals and MIDI clock. Returns false (and leaves
    // the outputs alone) when the block is not valid, himmel_audio's heartbeat is
    // older than 250 ms, the input is not running, or the signals are older than 500 ms.
    bool read(uint64_t now_us, audio::Signals& sig, audio::ClockSource& midi);
    // The input as last seen (for the status / log; refreshed by read() every ~0.5 s).
    const audio::InputStatus& input() const { return input_; }
    bool alive() const { return alive_; }

private:
    ShmBlock shm_;
    audio::InputStatus input_{};
    audio::Signals last_{}, scratch_{};
    bool have_last_ = false;
    uint64_t next_input_us_ = 0;
    bool alive_ = false;
};

}  // namespace himmel::engine
