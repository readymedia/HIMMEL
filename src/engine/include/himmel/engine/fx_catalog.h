// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The slice's built-in effects: one precompiled uber-shader (render/slice_shaders.h),
// any effect in any FX slot (layer, deck, master). Pure data, no GPU.
//
// The effect id is the index in this table and in Catalog.effect. The shader
// switches on the same index (keep them in step: append only).
#pragma once

#include <cstdint>

#include "himmel/protocol/state.h"

namespace himmel::engine::fx {

enum class Effect : int {
    HueSat = 0,    // HUE turn, SAT 0..2x, BRIGHT -/+, CONTRAST 0.25..4x
    Invert = 1,    // AMOUNT, LUMA (0 = invert RGB, 1 = invert brightness only)
    Mirror = 2,    // MODE (left / right / top / bottom / quad), CENTER
    Kaleido = 3,   // SEGMENTS 2..16, TURN, ZOOM
    RgbSplit = 4,  // AMOUNT, ANGLE
    RotoZoom = 5,  // ZOOM 0.25..4x, ANGLE, SPIN (centre = still), TILE (0 clamp, 1 repeat)
    Strobe = 6,    // RATE (4..1/4 beats), COLOR (black .. white), DUTY
    Pixelate = 7,  // SIZE 1..64 px, POSTER (levels, 0 = off)
    Count
};
inline constexpr int kEffectCount = static_cast<int>(Effect::Count);

struct EffectDef {
    const char* name;
    int params;
    const char* param_name[4];
    float param_default[4];
};

// Table in fx_catalog.cpp.
const EffectDef& effect_def(int id);

// Fills catalog.effect / effect_count from the table (start-up, not the frame loop).
void fill_catalog(protocol::Catalog& catalog);

}  // namespace himmel::engine::fx
