// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Touch FX: one effect played by one finger (docs/touch-fx-entrancer.md).
//
//   finger x  -> the effect's X parameter
//   finger y  -> the effect's Y parameter (y = 0 bottom, 1 top)
//   pressure  -> how much (30 % for a light touch, 100 % pressed hard)
//   lift      -> the effect fades out, unless Hold is on: then it keeps the
//                last position and pressure
//   depth     -> overall amount (the "FX balance"), 0..1
//
// Weapon-neutral: any touch source (MK2 pads, mouse on an output window, the
// Lightpad later) calls touch_down / touch_move / touch_up with its own source
// id; knobs call set_xy / set_depth / select. The renderer reads update().
//
// Pure logic: no Windows, no heap, no locks. Lives on the render thread for
// now (cheap: a few floats per frame).
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace himmel::engine::touchfx {

enum class Effect : uint8_t { HueTurn, Duotone, Grid, Kaleido, Bulge, Slide, Waves, Strobe, Trails };
inline constexpr int kEffects = 9;

struct EffectInfo {
    std::string_view name;     // Himmel's own name (docs/touch-fx-entrancer.md section 2)
    std::string_view x_label;  // <= 8 characters (MK2 knob label)
    std::string_view y_label;
    std::string_view mix_label;
    uint8_t tier;              // 'A' one pass, 'B' + feedback texture
    float x0, y0;              // a good starting position (knobs, Hold with no touch yet)
    bool hit;                  // no fade in / out, no smoothing (Strobe)
    float hue;                 // colour for LEDs and screens, 0..1
};
const EffectInfo& info(int effect);

// Where a touch came from. Only the source that put the finger down can lift
// it, and a lost source lifts its own finger.
enum class Source : uint8_t { None, Mk2Pads, Mouse, Lightpad };

// What the renderer reads every frame.
struct Params {
    int effect = 0;
    float x = 0.5f, y = 0.5f;  // smoothed, 0..1
    float mix = 0.0f;          // depth * envelope * pressure amount, 0..1
    bool active = false;       // mix > 0: the effect pass must run
};

class TouchFx {
public:
    static constexpr float kRampInS = 0.020f;
    static constexpr float kRampOutS = 0.060f;
    static constexpr float kSmoothS = 0.008f;          // MK2 pads, mouse
    static constexpr float kSmoothLightpadS = 0.030f;  // the block sends moves every ~28 ms
    static float smooth_s(Source s) { return s == Source::Lightpad ? kSmoothLightpadS : kSmoothS; }
    static constexpr float kMinAmount = 0.30f;  // a light touch still shows the effect
    // Lightpad z (sensitivity 127) still reaches 255 on a hard press: a soft
    // curve keeps the top of the amount range for really pressing.
    static constexpr float kLightpadCurve = 1.5f;

    // Selection (knob / encoder / keys). Wraps around.
    void select(int effect);
    void step(int delta) { select(effect_ + delta); }
    int effect() const { return effect_; }

    // Touch. x, y, pressure in 0..1.
    void touch_down(Source s, float x, float y, float pressure);
    void touch_move(Source s, float x, float y, float pressure);
    void touch_pressure(Source s, float pressure);
    void touch_up(Source s);
    void source_lost(Source s) { touch_up(s); }

    // Hold (latch): on lift keep the last position and pressure.
    void set_hold(bool on);
    bool hold() const { return hold_; }

    // Knobs: move the (latched) position, the overall depth.
    void set_xy(float x, float y);
    void set_depth(float d);
    float depth() const { return depth_; }

    // Advance by dt seconds and return what to render.
    Params update(float dt);

    bool finger_down() const { return src_ != Source::None; }
    Source source() const { return src_; }
    float target_x() const { return tx_; }
    float target_y() const { return ty_; }
    float pressure() const { return pressure_; }
    float envelope() const { return env_; }

private:
    int effect_ = 0;
    Source src_ = Source::None;
    Source last_src_ = Source::None;  // keeps its smoothing after the lift
    bool hold_ = false;
    float tx_ = 0.5f, ty_ = 0.5f;  // target position
    float x_ = 0.5f, y_ = 0.5f;    // smoothed position
    float pressure_ = 1.0f;
    float depth_ = 1.0f;
    float env_ = 0.0f;             // 0..1 fade in / out
};

// Pressure 0..1 -> amount kMinAmount..1.
float pressure_amount(float pressure);
// Raw touch pressure 0..1 -> the pressure Touch FX uses (per source curve).
float shape_pressure(Source s, float pressure);

// MK2 pad n (1..16, Pad 1 bottom-left) -> touch position at the pad's centre.
struct Point { float x, y; };
Point mk2_pad_point(int pad);

}  // namespace himmel::engine::touchfx
