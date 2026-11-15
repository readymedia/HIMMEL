// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The slice's show model: 2 decks x 4 layers, FX slots on layer / deck / master,
// crossfader, TAKE / CUT / black / focus (docs/interfaces/command-protocol.md §4-5).
//
// Pure logic: no GPU, no Windows, no heap, no locks. It runs in the render
// loop; every command from every producer goes through submit(), which
// rate-limits, validates (refuse or clamp), applies and acks. The render
// process reads live() to draw and writes it into Local\HimmelState.
// Unit tests: tests/engine/deck_tests.cpp.
#pragma once

#include <cstdint>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/state.h"

namespace himmel::engine {

inline constexpr uint32_t kDefaultTakeMs = 2000;  // slice default (command-protocol.md §4)
inline constexpr uint32_t kMaxTakeMs = 60000;     // GUESS: longer TAKEs are clamped (owner: is 60 s enough?)

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

    // Once per frame: advance a running TAKE (lands -> program_deck flips).
    void tick(uint64_t now_us);

    // ---- feedback from the renderer (not commands) ----
    void set_position(int deck, int layer, float pos01);
    void set_missing(int deck, int layer, bool missing);
    void set_frame(uint64_t frame, uint64_t time_us, float fps);
    void set_producer_alive(int producer, bool alive);
    void set_epoch(uint32_t epoch) { live_.epoch = epoch; }

    // A jump the renderer must do on that layer's play head: when jump_gen()
    // changes, seek to jump_to() (0..1). LayerJump, LayerPlay Restart and
    // LayerClip bump it.
    uint32_t jump_gen(int deck, int layer) const { return jump_gen_[deck][layer]; }
    float jump_to(int deck, int layer) const { return jump_to_[deck][layer]; }

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
    // Returns the number of layers whose clip could not be found by name.
    int restore(const protocol::LiveState& old_live, const protocol::Catalog& old_catalog);

    const protocol::LiveState& live() const { return live_; }
    const protocol::Catalog& catalog() const { return catalog_; }
    uint32_t default_take_ms = kDefaultTakeMs;

private:
    protocol::Reject apply(const protocol::Command& c, uint64_t now_us);
    protocol::FxSlotState* fx_slot(const protocol::Command& c);
    const protocol::FxSlotState* fx_slot(const protocol::Command& c) const;
    void set_crossfader(float v);
    void land_take();
    void reset_fx(protocol::FxSlotState& s, int effect);

    const protocol::Catalog& catalog_;
    protocol::LiveState live_{};
    bool dirty_ = true;
    uint32_t jump_gen_[protocol::kDecks][protocol::kLayersPerDeck] = {};
    float jump_to_[protocol::kDecks][protocol::kLayersPerDeck] = {};
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
