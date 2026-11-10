// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/touch_fx.h"

#include <algorithm>
#include <cmath>

namespace himmel::engine::touchfx {

namespace {

// Keep in the same order as Effect and as ps_touchfx in render/shaders.h.
constexpr std::array<EffectInfo, kEffects> kInfo = {{
    {"HUE TURN", "HUE", "SATURATE", "MIX", 'A', 0.25f, 0.75f, false, 0.00f},
    {"DUOTONE", "DARK", "LIGHT", "MIX", 'A', 0.65f, 0.12f, false, 0.80f},
    {"GRID", "TILES", "MIRROR", "MIX", 'A', 0.20f, 0.75f, false, 0.55f},
    {"KALEIDO", "SEGMENTS", "TURN", "MIX", 'A', 0.30f, 0.00f, false, 0.70f},
    {"BULGE", "POS X", "POS Y", "STRENGTH", 'A', 0.50f, 0.50f, false, 0.45f},
    {"SLIDE", "SHIFT X", "SHIFT Y", "MIX", 'A', 0.60f, 0.50f, false, 0.15f},
    {"WAVES", "FREQ", "HEIGHT", "MIX", 'A', 0.30f, 0.40f, false, 0.50f},
    {"STROBE", "RATE", "FLASH", "MIX", 'A', 0.50f, 0.00f, true, 0.00f},
    {"TRAILS", "DECAY", "DRIFT", "MIX", 'B', 0.70f, 0.55f, false, 0.33f},
}};

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

}  // namespace

const EffectInfo& info(int effect) {
    return kInfo[static_cast<size_t>(std::clamp(effect, 0, kEffects - 1))];
}

float pressure_amount(float pressure) { return TouchFx::kMinAmount + (1.0f - TouchFx::kMinAmount) * clamp01(pressure); }

Point mk2_pad_point(int pad) {
    const int i = std::clamp(pad, 1, 16) - 1;
    return {((i % 4) + 0.5f) / 4.0f, ((i / 4) + 0.5f) / 4.0f};
}

void TouchFx::select(int effect) {
    const int e = ((effect % kEffects) + kEffects) % kEffects;
    if (e == effect_) return;
    effect_ = e;
    if (!finger_down()) {  // a new effect starts from its own good position
        tx_ = x_ = info(e).x0;
        ty_ = y_ = info(e).y0;
    }
}

void TouchFx::touch_down(Source s, float x, float y, float pressure) {
    if (s == Source::None) return;
    const bool jump = src_ == Source::None;
    src_ = last_src_ = s;
    tx_ = clamp01(x);
    ty_ = clamp01(y);
    pressure_ = clamp01(pressure);
    if (jump && env_ <= 0.0f) x_ = tx_, y_ = ty_;  // no glide from the old position into a fresh touch
}

void TouchFx::touch_move(Source s, float x, float y, float pressure) {
    if (s != src_ || s == Source::None) return;
    tx_ = clamp01(x);
    ty_ = clamp01(y);
    pressure_ = clamp01(pressure);
}

void TouchFx::touch_pressure(Source s, float pressure) {
    if (s != src_ || s == Source::None) return;
    pressure_ = clamp01(pressure);
}

void TouchFx::touch_up(Source s) {
    if (s != src_ || s == Source::None) return;
    src_ = Source::None;
}

void TouchFx::set_hold(bool on) { hold_ = on; }

void TouchFx::set_xy(float x, float y) {
    tx_ = clamp01(x);
    ty_ = clamp01(y);
}

void TouchFx::set_depth(float d) { depth_ = clamp01(d); }

Params TouchFx::update(float dt) {
    dt = std::max(dt, 0.0f);
    const EffectInfo& fi = info(effect_);
    const bool on = finger_down() || hold_;
    if (fi.hit) {
        env_ = on ? 1.0f : 0.0f;
        x_ = tx_, y_ = ty_;
    } else {
        env_ = on ? std::min(1.0f, env_ + dt / kRampInS) : std::max(0.0f, env_ - dt / kRampOutS);
        const float k = 1.0f - std::exp(-dt / smooth_s(last_src_));
        x_ += (tx_ - x_) * k;
        y_ += (ty_ - y_) * k;
    }
    Params p;
    p.effect = effect_;
    p.x = x_;
    p.y = y_;
    p.mix = depth_ * env_ * pressure_amount(pressure_);
    p.active = p.mix > 0.001f;
    return p;
}

}  // namespace himmel::engine::touchfx
