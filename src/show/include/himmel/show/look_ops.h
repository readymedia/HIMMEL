// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Looks -> commands (M3): recall, morph, undo and reset all come down to "make
// deck D look like this", expressed as ordinary commands for the deck model,
// so they pass the same validation as everything else. Only what differs is
// sent. Pure logic, no heap: it runs in the frame loop.
#pragma once

#include <cstdint>

#include "himmel/show/model.h"

namespace himmel::show {

// Reactions (D131): per knob at most Set + Range; one deck has (4 layers + deck FX) x 4 slots x 4 knobs.
inline constexpr int kLookReactCommandsMax = (kLayers + 1) * proto::kFxSlots * proto::kReactParams * 2;  // 160
inline constexpr int kMasterCommandsMax = proto::kFxSlots * (3 + proto::kFxParams + proto::kReactParams * 2);
// > an undo of both decks and the master with every FX slot, setting and reaction (~1100).
inline constexpr int kMaxListCommands = 2048;
static_assert(2 * (proto::kLookCommandsMax + kLookReactCommandsMax) + kMasterCommandsMax <= kMaxListCommands);

struct CommandList {
    proto::Command c[kMaxListCommands];
    int n = 0;
    bool overflow = false;
    void clear() { n = 0, overflow = false; }
    void push(const proto::Command& x) {
        if (n < kMaxListCommands) c[n++] = x;
        else overflow = true;
    }
};

// What the diff may touch (a morph sends clips once, at its middle).
enum DiffPart : uint32_t {
    kDiffClips = 1u << 0,
    kDiffMix = 1u << 1,        // opacity, blend, mute, speed
    kDiffTransport = 1u << 2,  // LayerSet settings
    kDiffFx = 1u << 3,         // effect, params, mix, bypass, knob reactions (D131)
    kDiffAll = 0xFu
};

// Commands that turn `cur` (deck `deck` as it is now) into `target`. A layer
// mask (bit n = layer n) limits it to some layers; deck FX are included when
// `with_deck_fx` is set. Clip ids must be valid for `cat` (-1 = empty).
// The knob reactions (FxReact) come last, after every FxSet (which resets a
// slot's reactions to Default), so the look itself is never held up by them.
void diff_look(const Look& cur, const Look& target, int deck, const proto::Catalog& cat, CommandList& out,
               uint32_t parts = kDiffAll, uint32_t layer_mask = 0xF, bool with_deck_fx = true);

// The master FX slots (undo covers them).
void diff_master(const proto::FxSlotState cur[proto::kFxSlots], const ReactRow cur_react[proto::kFxSlots],
                 const proto::FxSlotState target[proto::kFxSlots], const ReactRow target_react[proto::kFxSlots],
                 const proto::Catalog& cat, CommandList& out);

// The FxReact commands that turn one slot's reactions `cur` into `want`
// (`effect_changed` = an FxSet goes first, so `cur` counts as all Default).
void diff_react(const ReactRow& cur, const ReactRow& want, bool effect_changed, int effect, uint8_t deck,
                uint8_t layer, uint8_t slot, const proto::Catalog& cat, CommandList& out);

// The look at morph position t (0 = a, 1 = b): numbers glide where both sides
// have the same thing (the same effect in a slot, a clip on the layer), the
// rest switches at t >= 0.5 (presets-and-shuffle.md §6); reactions switch too.
void lerp_look(const Look& a, const Look& b, float t, Look& out);

// Bind the clip names of a look to catalog ids (case-insensitive). Returns the
// number of named clips the catalog does not have (they stay empty, name kept).
int bind_clips(Look& look, const proto::Catalog& cat);
int find_clip(const proto::Catalog& cat, const char* name);     // -1 = none
int find_effect(const proto::Catalog& cat, const char* name);   // -1 = none

}  // namespace himmel::show
