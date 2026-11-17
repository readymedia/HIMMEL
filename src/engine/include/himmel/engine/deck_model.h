// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The slice's show model: 2 decks x 4 layers, FX slots on layer / deck / master,
// crossfader, TAKE / CUT / black / focus (docs/interfaces/command-protocol.md §4-5).
//
// Pure logic: no GPU, no Windows, no heap, no locks. It runs in the render
// loop; every command from every producer goes through submit(), which
// rate-limits, validates (refuse or clamp), applies and acks. The render
// process reads live() / transport() to draw and writes them into
// Local\HimmelState.
//
// M2 clip transport (design/clips-and-playback.md §2-3): the model owns one
// Playhead per layer plus the HIT layer, the beat clock, cue points per clip,
// trigger styles, beat snap, autopilot, fades and layer transitions. tick()
// advances everything once per frame; the renderer only asks frame(slot).
// Unit tests: tests/engine/deck_tests.cpp, tests/engine/transport_tests.cpp.
#pragma once

#include <cstdint>

#include "himmel/engine/output_model.h"
#include "himmel/engine/transport.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/state.h"

namespace himmel::engine {

inline constexpr uint32_t kDefaultTakeMs = 2000;  // slice default (command-protocol.md §4)
inline constexpr uint32_t kMaxTakeMs = 60000;     // GUESS: longer TAKEs are clamped (owner: is 60 s enough?)

// Layer slots: 0..7 = deck * 4 + layer, 8 = the HIT layer (clips-and-playback.md §3.3).
inline constexpr int kHitSlot = protocol::kDecks * protocol::kLayersPerDeck;
inline constexpr int kLayerSlots = kHitSlot + 1;
inline constexpr int slot_of(int deck, int layer) { return deck * protocol::kLayersPerDeck + layer; }

// Defaults for a layer's transport settings (status fields zeroed).
void default_transport(protocol::LayerTransport& t, int slot);

class DeckModel {
public:
    // `catalog` is the engine's catalog; it must outlive the model. Ids are checked against it.
    explicit DeckModel(const protocol::Catalog& catalog);

    // Back to the empty show (every layer empty, crossfader on A, no FX).
    void reset();

    // The whole path for one command from producer slot `producer` (0..7):
    // rate limit, validate, apply, then ack (acked_seq) or count the refusal.
    // Returns Reject::None when the command was applied (possibly clamped).
    protocol::Reject submit(int producer, const protocol::Command& c, uint64_t now_us);

    // Validation only (§5 steps 1-4); no side effects.
    protocol::Reject validate(const protocol::Command& c) const;

    // M6: the output commands (types 70..73) go to this model (validated, rate limited and
    // acked like every command). Without one they are refused as UnknownType.
    void set_outputs(OutputModel* outputs) { outputs_ = outputs; }

    // A confirmed SystemQuit (74, f[0] = 1) was applied: the producer slot that sent it
    // (kProducers = local), -1 = none. Sticky: the renderer reports it in its status block
    // (EngineStatus::quit_request) and the supervisor stops every process.
    int quit_producer() const { return quit_producer_; }

    // Once per frame: advance a running TAKE (lands -> program_deck flips), the
    // beat clock, every playhead, fades, transitions, waiting launches, autopilot.
    void tick(uint64_t now_us);

    // ---- feedback from the renderer (not commands) ----
    void set_position(int deck, int layer, float pos01);
    void set_missing(int deck, int layer, bool missing);
    void set_missing_slot(int slot, bool missing);
    void set_frame(uint64_t frame, uint64_t time_us, float fps);
    // `now_us` (M6): a producer that goes quiet lets go of a held IDENTIFY.
    void set_producer_alive(int producer, bool alive, uint64_t now_us = 0);
    void set_epoch(uint32_t epoch) { live_.epoch = epoch; }

    // Bumped on every jump of a layer's playhead (LayerJump, cue jump, Restart,
    // a new clip); jump_to() = where to (0..1 of the clip). The renderer uses it
    // to measure jump -> screen latency.
    uint32_t jump_gen(int deck, int layer) const { return jump_gen_[slot_of(deck, layer)]; }
    float jump_to(int deck, int layer) const { return jump_to_[slot_of(deck, layer)]; }
    uint32_t jump_gen_slot(int slot) const { return jump_gen_[slot]; }

    // ---- transport, per slot (0..7 = deck * 4 + layer, kHitSlot = HIT) ----
    const protocol::LayerState& layer(int slot) const;
    const protocol::LayerTransport& layer_transport(int slot) const;
    int64_t frame(int slot) const { return ph_[slot].frame(); }     // the frame to show, -1 = none
    int direction(int slot) const { return ph_[slot].direction(); } // read-ahead direction
    // The frames a jump can land on (cue points, else the 16 equal slices of
    // in..out): the decode worker keeps them decoded so a jump shows next frame.
    void warm_frames(int slot, int64_t out[protocol::kCuePoints]) const;
    // Bumped when a FADE transition starts: the renderer keeps the old picture.
    uint32_t transition_gen(int slot) const { return trans_gen_[slot]; }
    const protocol::TransportSection& transport() const { return transport_; }
    double beat(uint64_t t_us) const { return clock_.beat(t_us); }
    double bpm() const { return clock_.bpm(); }
    // An external clock leads (MIDI clock from himmel_audio, M8): once per frame
    // before tick(). While `active`, the clock follows it (BeatClock::follow), Tap and
    // Bpm commands are acked but ignored, and ClockState::source = 2.
    void follow_clock(bool active, double bpm, double beat, bool phase, uint64_t now_us, double dt_s);
    bool clock_external() const { return clock_external_; }

    // True once after anything but position / frame stats changed.
    bool take_structure_dirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }
    // The first refusal per producer per second is logged, never more (§5.6).
    bool should_log_reject(int producer, uint64_t now_us);

    // Rebuild from a previous renderer's state (engine-state.md §2): clips and
    // effects are matched by name against the current catalog; what is not
    // found becomes empty. A running TAKE is not resumed (the crossfader stays where it was).
    // With `old_transport` (a version 2 block) the transport settings, cue points,
    // BPM and grid come back too. The HIT layer is never restored.
    // Returns the number of layers whose clip could not be found by name.
    // With `old_react` (a version 3 block) each slot whose effect came back keeps its
    // Custom / Off reactions (D131).
    int restore(const protocol::LiveState& old_live, const protocol::Catalog& old_catalog,
                const protocol::TransportSection* old_transport = nullptr,
                const protocol::ReactionSection* old_react = nullptr);

    // ---- per-knob reactions (D131, FxReact) ----
    // Start-up: each effect's Default row (engine::react_defaults), indexed by effect
    // id; must outlive the model. Without it a fresh slot's rows have signal None.
    void set_react_defaults(const protocol::SlotReactions* rows, int count);
    // The table (settings; env / value are the renderer's). gen bumps on every change.
    const protocol::ReactionSection& reactions() const { return react_; }

    const protocol::LiveState& live() const { return live_; }
    const protocol::Catalog& catalog() const { return catalog_; }
    uint32_t default_take_ms = kDefaultTakeMs;

private:
    OutputModel* outputs_ = nullptr;
    int quit_producer_ = -1;  // a confirmed SystemQuit came from this producer
    int submitting_ = -1;  // the producer of the command being applied (M6: who holds IDENTIFY)
    protocol::Reject apply(const protocol::Command& c, uint64_t now_us);
    protocol::FxSlotState* fx_slot(const protocol::Command& c);
    const protocol::FxSlotState* fx_slot(const protocol::Command& c) const;
    void set_crossfader(float v);
    void land_take();
    void reset_fx(protocol::FxSlotState& s, int effect);
    protocol::SlotReactions* react_row(const protocol::FxSlotState* s);  // nullptr: not a live slot (HIT)
    const protocol::SlotReactions& react_default_row(int effect) const;

    // ---- transport (M2) ----
    enum class PendingKind : uint8_t { None, Launch, Jump };
    struct Pending {
        PendingKind kind = PendingKind::None;
        bool legato = false;
        uint8_t cue = 0;
        int16_t clip = -1;
        float velocity = 1.0f;
        double due_beat = 0.0;
    };
    enum class FadeThen : uint8_t { Nothing, Eject, Load };
    struct Fade {
        bool active = false;
        FadeThen then = FadeThen::Nothing;
        int16_t clip = -1;     // FadeThen::Load
        float velocity = 1.0f;
        double frac = -1.0;    // start position for the load (< 0: by the start rule)
        float from = 1.0f, to = 1.0f;
        double t = 0.0, dur = 0.0;
    };
    protocol::LayerState& L(int slot);
    protocol::LayerTransport& T(int slot);
    ClipGeom geom(int clip) const;
    bool waits_for_grid(int slot) const;
    double next_grid_beat(int slot) const;
    double cue_frac(int slot, int cue) const;    // 0..1 of the clip
    void load_clip(int slot, int clip, double frac, float velocity);
    void change_clip(int slot, int clip, double frac, float velocity);
    void launch(int slot, int clip, bool legato, float velocity);
    void jump_now(int slot, double frac);
    void stop_layer(int slot, float fade_s);
    void start_fade(int slot, float from, float to, double dur, FadeThen then, int clip = -1, double frac = -1.0,
                    float velocity = 1.0f);
    void finish_fade(int slot);
    void on_ended(int slot);
    void autopilot(int slot, protocol::AutopilotMode how);
    int pick_clip(int slot, protocol::AutopilotMode how);
    protocol::Reject apply_layer_set(int slot, const protocol::Command& c);
    protocol::Reject apply_trigger(int slot, const protocol::Command& c);
    void tick_transport(uint64_t now_us);

    const protocol::Catalog& catalog_;
    protocol::LiveState live_{};
    protocol::TransportSection transport_{};
    protocol::ReactionSection react_{};
    const protocol::SlotReactions* react_defaults_ = nullptr;
    int react_default_count_ = 0;
    protocol::SlotReactions react_empty_{};
    bool dirty_ = true;
    Playhead ph_[kLayerSlots];
    BeatClock clock_;
    bool clock_external_ = false;  // M8: an external clock leads (follow_clock)
    uint64_t last_tick_us_ = 0;
    double last_beat_ = 0.0;
    Pending pending_[kLayerSlots];
    Fade fade_[kLayerSlots];
    int16_t piano_clip_[kLayerSlots] = {};
    double trans_t_[kLayerSlots] = {};
    uint32_t trans_gen_[kLayerSlots] = {};
    uint32_t jump_gen_[kLayerSlots] = {};
    float jump_to_[kLayerSlots] = {};
    float cues_[protocol::kMaxClips][protocol::kCuePoints] = {};  // per clip id; < 0 = not set
    float resume_[protocol::kMaxClips] = {};                      // StartPos::Continue, 0..1
    // TAKE
    float take_from_ = 0.f, take_to_ = 1.f;
    uint64_t take_start_us_ = 0;
    // rate limit + log limit per producer
    uint64_t rate_window_us_[protocol::kProducers] = {};
    uint32_t rate_count_[protocol::kProducers] = {};
    uint64_t log_window_us_[protocol::kProducers] = {};
};

// Short, stable names for logs and himmel_cmd.
const char* reject_name(protocol::Reject r);
const char* command_name(protocol::CommandType t);

}  // namespace himmel::engine
