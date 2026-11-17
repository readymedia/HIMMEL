// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Dice v1 (M3, presets-and-shuffle.md §2-3): ROLL and MUTATE a look inside the
// locks, seeded and deterministic (same seed + same input = same look). Pure,
// no heap: it is cheap enough (a few µs) to run at a frame boundary.
//
// v1 knows only the catalog: clips are picked uniformly (the library's energy /
// palette / motion analysis arrives with M7, moods later), effects by kind,
// params around their defaults. ROTATE (walk saved scenes) lives in the runtime.
#pragma once

#include <cstdint>

#include "himmel/show/model.h"

namespace himmel::show {

struct DiceScope {
    int layer = -1;   // -1 = the whole deck (DECK scope), else LAYER scope 0..3
    int slot = -1;    // with a layer (or -1 = deck FX): FX SLOT scope 0..3
};

// A new look from `cur` (the deck as it is), respecting `locks` (LockBit).
void dice_roll(const Look& cur, Look& out, const proto::Catalog& cat, uint32_t locks, DiceScope scope, uint64_t seed);

// Keep the structure, nudge the numbers by +- amount (0..1, default 0.2).
void dice_mutate(const Look& cur, Look& out, const proto::Catalog& cat, uint32_t locks, DiceScope scope, uint64_t seed,
                 float amount);

// Which lock guards an effect parameter, from its name (COLOUR: hue, colour,
// sat, tint ...; MOTION: speed, rate, spin, zoom ...); 0 = none (FX lock only).
uint32_t param_lock(const char* param_name);

// splitmix64: the dice's random source (also used to make seeds).
uint64_t mix64(uint64_t x);

}  // namespace himmel::show
