// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The engine's built-in effects and generators. Since M5 the source of truth is
// the fx/ folder (one effect.json + one .hlsl per effect, order in
// fx/catalog.json), parsed by himmel::fx (src/engine/fx/,
// docs/interfaces/effect-metadata.md). This header keeps the ids as names for
// code and tests; fx_tests checks that they match fx/catalog.json.
//
// The effect id is the index in the catalog: APPEND ONLY.
#pragma once

#include <cstdint>

#include "himmel/protocol/state.h"

namespace himmel::engine::fx {

enum class Effect : int {
    HueSat = 0,
    Invert = 1,
    Mirror = 2,
    Kaleido = 3,
    RgbSplit = 4,
    RotoZoom = 5,
    Strobe = 6,
    Pixelate = 7,
    BrightContrast = 8,
    Colorize = 9,
    Posterize = 10,
    Tile = 11,
    ZoomPulse = 12,
    Wave = 13,
    Shake = 14,
    Blur = 15,
    Glow = 16,
    Trails = 17,
    LumaKey = 18,
    // generators
    GenSolid = 19,
    GenNoise = 20,
    GenLines = 21,
    GenRings = 22,
    GenShapes = 23,
    GenTicker = 24,
    Count
};
inline constexpr int kEffectCount = static_cast<int>(Effect::Count);

// Fills catalog.effect / effect_count from fx/ (start-up, not the frame loop).
void fill_catalog(protocol::Catalog& catalog);

}  // namespace himmel::engine::fx
