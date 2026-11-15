// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/fx_catalog.h"

#include <cstdio>
#include <cstring>

namespace himmel::engine::fx {

namespace {
// GUESS: defaults chosen so that "FxSet" alone shows a visible but tasteful change
// (owner to tune after playing; confidence medium).
constexpr EffectDef kDefs[kEffectCount] = {
    {"Hue / Sat", 4, {"HUE", "SAT", "BRIGHT", "CONTRAST"}, {0.25f, 0.5f, 0.5f, 0.5f}},
    {"Invert", 2, {"AMOUNT", "LUMA"}, {1.0f, 0.0f}},
    {"Mirror", 2, {"MODE", "CENTER"}, {0.0f, 0.5f}},
    {"Kaleido", 3, {"SEGMENTS", "TURN", "ZOOM"}, {0.3f, 0.0f, 0.5f}},
    {"RGB Split", 2, {"AMOUNT", "ANGLE"}, {0.3f, 0.0f}},
    {"RotoZoom", 4, {"ZOOM", "ANGLE", "SPIN", "TILE"}, {0.5f, 0.0f, 0.5f, 1.0f}},
    {"Strobe", 3, {"RATE", "COLOR", "DUTY"}, {0.5f, 1.0f, 0.5f}},
    {"Pixelate", 2, {"SIZE", "POSTER"}, {0.3f, 0.0f}},
};
}  // namespace

const EffectDef& effect_def(int id) { return kDefs[(id >= 0 && id < kEffectCount) ? id : 0]; }

void fill_catalog(protocol::Catalog& c) {
    c.effect_count = kEffectCount;
    for (int e = 0; e < kEffectCount; ++e) {
        protocol::EffectInfo& info = c.effect[e];
        std::memset(&info, 0, sizeof(info));
        std::snprintf(info.name, sizeof(info.name), "%s", kDefs[e].name);
        info.param_count = static_cast<uint8_t>(kDefs[e].params);
        for (int p = 0; p < kDefs[e].params; ++p) {
            std::snprintf(info.param_name[p], sizeof(info.param_name[p]), "%s", kDefs[e].param_name[p]);
            info.param_default[p] = kDefs[e].param_default[p];
        }
    }
}

}  // namespace himmel::engine::fx
