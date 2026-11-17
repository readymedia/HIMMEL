// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Clip transport (M2, docs/design/clips-and-playback.md §2-3): the beat clock
// and the playhead state machine of one layer.
//
// Pure logic: no GPU, no Windows, no heap, no locks. DeckModel owns one
// Playhead per layer (+ the HIT layer) and advances them once per frame at
// the frame start, so a command and the frame it changes are the same frame.
// The decode worker only decodes the frames the playhead asks for (and keeps
// the cue frames warm), see ClipPlayer.
// Unit tests: tests/engine/transport_tests.cpp.
#pragma once

#include <cstdint>

#include "himmel/protocol/state.h"

namespace himmel::engine {

// ---------------------------------------------------------------- beat clock
// The internal clock: a tempo and a phase. Tap tempo and set BPM for now; the
// audio clock, MIDI clock and Link feed it in M8.
class BeatClock {
public:
    void reset(uint64_t now_us);
    double beat(uint64_t t_us) const;          // running beat count (beat 0 = the last resync)
    double bpm() const { return bpm_; }
    void set_bpm(double bpm, uint64_t now_us); // keeps the phase continuous
    void tap(uint64_t now_us);                 // >= 2 taps set the tempo; every tap lands on a beat
    void nudge(double beats) { anchor_beat_ += beats; }
    void resync(uint64_t now_us);              // beat 0 is now
    // Follows an external clock (MIDI clock, M8, docs/interfaces/audio-block.md §4):
    // takes its tempo and, when `phase`, glides the phase onto `beat` (bar-aligned,
    // the nearest of 4 beats) with a time constant of 2 beats. Never jumps.
    void follow(double bpm, double beat, bool phase, uint64_t now_us, double dt_s);
    uint32_t taps() const { return static_cast<uint32_t>(tap_n_); }
    bool tapped() const { return tapped_; }

    static constexpr double kMinBpm = 20.0, kMaxBpm = 300.0;
    static constexpr uint64_t kTapTimeoutUs = 2000000;  // a gap this long starts a new tap run

private:
    void anchor(uint64_t now_us);
    double bpm_ = 120.0;
    double anchor_beat_ = 0.0;
    uint64_t anchor_us_ = 0;
    uint64_t tap_us_[8] = {};
    int tap_n_ = 0;
    bool tapped_ = false;
};

// Beats of one grid step (0 = off).
double grid_beats(protocol::GridDiv g);

// ---------------------------------------------------------------- playhead
struct ClipGeom {
    int64_t frames = 0;  // frames in the clip
    double fps = 0.0;
};

struct TickIn {
    double dt = 0.0;      // seconds since the last tick
    double beat0 = 0.0;   // clock beat at the last tick
    double beat1 = 0.0;   // clock beat now
    double bpm = 120.0;
};

// What advance() reports to the model (bit set).
enum PlayheadEvent : uint32_t {
    kEvNone = 0,
    kEvLooped = 1u << 0,  // the main head wrapped (loop, ping-pong cycle, inner loop)
    kEvEnded = 1u << 1,   // a ONCE mode reached the end of the range (reported once)
};

// One layer's playhead. Positions are in frames (double) within the clip.
// The "main" head always runs by the mode's rules; while a beat loop, a
// scratch or a held REVERSE is active it is the shadow playhead, and the
// frame shown comes from that override. Letting go returns to the main head
// (catch-up) or moves the main head to where the override was (stay).
class Playhead {
public:
    // A new clip (or a restart): position in frames, timers and loops reset.
    void start(const ClipGeom& g, const protocol::LayerTransport& t, double pos, double speed_now);
    void clear();
    bool active() const { return frames_ > 0; }

    // Once per frame. `playing` = LayerState.playing, `speed_target` = LayerState.speed.
    uint32_t advance(const ClipGeom& g, const protocol::LayerTransport& t, bool playing, float speed_target,
                     const TickIn& in);

    // Jumps end a beat loop, a scratch and a held reverse (as in Resolume).
    void jump(const ClipGeom& g, const protocol::LayerTransport& t, double pos);
    void beat_loop(const ClipGeom& g, const protocol::LayerTransport& t, double beats, double bpm);
    void beat_loop_off(bool catch_up);
    void scratch_hold(const ClipGeom& g, const protocol::LayerTransport& t);
    void scratch_move(const ClipGeom& g, const protocol::LayerTransport& t, double pos);
    void scratch_release(bool snap_back);
    void reverse(bool held);
    void release_inner();            // inner loop -> outro
    void mode_changed();             // timers restart, ended flag cleared
    void refresh_inner(const ClipGeom& g, const protocol::LayerTransport& t) { set_inner_phase(g, t, main_); }

    double pos() const;              // the position shown (frames)
    double main_pos() const { return main_.pos; }
    int64_t frame() const;           // the frame shown, 0..frames-1 (-1 = none)
    int direction() const;           // +1 / -1: which way the shown position moves (read-ahead)
    double speed_now() const { return speed_now_; }
    protocol::PlayheadPhase phase(bool playing) const;
    bool beat_looping() const { return ov_ == Override::BeatLoop; }
    bool scratching() const { return ov_ == Override::Scratch; }
    double beat_loop_beats() const { return ov_beats_; }
    bool ended() const { return main_.ended; }
    uint32_t rng_next();             // the layer's seeded random source (also used by autopilot)
    void seed(uint32_t s) { rng_ = s ? s : 0x9E3779B9u; }

    // Base speed in frames per second at speed 1 (FREE: the clip's fps; BPM:
    // the range lasts `beats` beats).
    static double base_rate(const ClipGeom& g, const protocol::LayerTransport& t, double bpm);
    // The in..out range in frames, at least one frame long.
    static void range(const ClipGeom& g, const protocol::LayerTransport& t, double& a, double& b);

private:
    enum class Override : uint8_t { None, BeatLoop, Scratch, Reverse };
    struct Head {
        double pos = 0.0;
        int dir = 1;            // ping-pong flips it
        double timer = 0.0;     // seconds (FREE time base) for random / chop / step
        uint8_t inner = 0;      // PlayheadPhase Intro / InnerLoop / Outro, or 0 when the inner loop is off
        bool ended = false;
    };
    uint32_t move(const ClipGeom& g, const protocol::LayerTransport& t, Head& h, double df);
    uint32_t step_events(const protocol::LayerTransport& t, Head& h, const TickIn& in);
    void set_inner_phase(const ClipGeom& g, const protocol::LayerTransport& t, Head& h);
    double velocity(const ClipGeom& g, const protocol::LayerTransport& t, const Head& h, double bpm) const;

    int64_t frames_ = 0;
    Head main_;
    Override ov_ = Override::None;
    double ov_pos_ = 0.0, ov_start_ = 0.0, ov_len_ = 0.0, ov_elapsed_ = 0.0, ov_beats_ = 0.0;
    double speed_now_ = 1.0, glide_from_ = 1.0, glide_to_ = 1.0, glide_t_ = 0.0;
    bool glide_init_ = false;
    bool backward_ = false;
    bool landed_ = false;  // jumped this frame: the next advance() holds the landing frame
    uint32_t rng_ = 0x9E3779B9u;
};

}  // namespace himmel::engine
