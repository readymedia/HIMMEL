// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the Touch FX gesture logic (no GPU, no hardware).
#include <cmath>
#include <cstdio>

#include "himmel/engine/touch_fx.h"

using namespace himmel::engine::touchfx;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

static Params run(TouchFx& fx, float seconds, float dt = 1.0f / 60.0f) {
    Params p;
    for (float t = 0; t < seconds; t += dt) p = fx.update(dt);
    return p;
}

static void test_table() {
    std::printf("effect table\n");
    static_assert(kEffects == 9);
    for (int e = 0; e < kEffects; ++e) {
        const auto& i = info(e);
        CHECK(!i.name.empty() && i.name.size() <= 9);   // fits the MK2 knob value
        CHECK(i.x_label.size() <= 9 && i.y_label.size() <= 9);
        CHECK(i.tier == 'A' || i.tier == 'B');
        CHECK(i.x0 >= 0 && i.x0 <= 1 && i.y0 >= 0 && i.y0 <= 1);
    }
    CHECK(info(-5).name == info(0).name);                // clamped, never out of range
    CHECK(info(99).name == info(kEffects - 1).name);
    CHECK(info(static_cast<int>(Effect::Trails)).tier == 'B');
    CHECK(info(static_cast<int>(Effect::Strobe)).hit);
}

static void test_touch() {
    std::printf("touch, lift, envelope\n");
    TouchFx fx;
    CHECK(!fx.update(0.016f).active);                    // nothing until a touch
    fx.touch_down(Source::Mouse, 0.2f, 0.8f, 1.0f);
    Params p = run(fx, 0.1f);
    CHECK(p.active && near(p.mix, 1.0f));
    CHECK(near(p.x, 0.2f) && near(p.y, 0.8f));          // a fresh touch does not glide in
    fx.touch_move(Source::Mouse, 0.6f, 0.4f, 0.0f);      // light touch
    p = run(fx, 0.2f);
    CHECK(near(p.x, 0.6f) && near(p.y, 0.4f));
    CHECK(near(p.mix, TouchFx::kMinAmount));             // light touch still shows the effect
    fx.touch_up(Source::Mouse);
    p = fx.update(0.030f);
    CHECK(p.active && p.mix < TouchFx::kMinAmount);       // fading out, not cut
    p = run(fx, 0.1f);
    CHECK(!p.active);

    std::printf("ramp in\n");
    TouchFx r;
    r.touch_down(Source::Mouse, 0.5f, 0.5f, 1.0f);
    p = r.update(0.010f);
    CHECK(p.mix > 0.4f && p.mix < 0.6f);                 // half way after 10 of 20 ms
}

static void test_sources() {
    std::printf("sources\n");
    TouchFx fx;
    fx.touch_down(Source::Mk2Pads, 0.1f, 0.1f, 1.0f);
    fx.touch_move(Source::Mouse, 0.9f, 0.9f, 1.0f);      // another source cannot move it
    CHECK(near(fx.target_x(), 0.1f));
    fx.touch_up(Source::Mouse);                          // ... or lift it
    CHECK(fx.finger_down());
    fx.source_lost(Source::Mk2Pads);                     // the weapon died: finger lifted
    CHECK(!fx.finger_down());
    CHECK(!run(fx, 0.2f).active);
    fx.touch_down(Source::None, 0.5f, 0.5f, 1.0f);       // ignored
    CHECK(!fx.finger_down());
}

static void test_hold() {
    std::printf("hold\n");
    TouchFx fx;
    fx.set_hold(true);
    fx.touch_down(Source::Mouse, 0.3f, 0.7f, 0.5f);
    run(fx, 0.1f);
    fx.touch_up(Source::Mouse);
    Params p = run(fx, 1.0f);
    CHECK(p.active);                                     // stays on after the lift
    CHECK(near(p.x, 0.3f) && near(p.y, 0.7f));
    CHECK(near(p.mix, pressure_amount(0.5f)));            // keeps the last pressure
    fx.set_xy(0.9f, 0.1f);                               // knobs move the held position
    p = run(fx, 0.2f);
    CHECK(near(p.x, 0.9f) && near(p.y, 0.1f));
    fx.set_hold(false);
    CHECK(!run(fx, 0.2f).active);
}

static void test_select_depth() {
    std::printf("select, depth\n");
    TouchFx fx;
    fx.step(-1);
    CHECK(fx.effect() == kEffects - 1);                  // wraps
    fx.step(2);
    CHECK(fx.effect() == 1);
    CHECK(near(fx.target_x(), info(1).x0) && near(fx.target_y(), info(1).y0));  // starts at its good spot
    fx.touch_down(Source::Mouse, 0.4f, 0.4f, 1.0f);
    fx.select(3);                                        // while touching: position stays
    CHECK(near(fx.target_x(), 0.4f));
    fx.set_depth(0.5f);
    CHECK(near(run(fx, 0.1f).mix, 0.5f));
    fx.set_depth(0.0f);
    CHECK(!run(fx, 0.1f).active);
}

static void test_strobe_hits() {
    std::printf("strobe = hit\n");
    TouchFx fx;
    fx.select(static_cast<int>(Effect::Strobe));
    fx.touch_down(Source::Mouse, 0.5f, 1.0f, 1.0f);
    CHECK(near(fx.update(0.001f).mix, 1.0f));            // no fade in
    fx.touch_up(Source::Mouse);
    CHECK(!fx.update(0.001f).active);                    // no fade out
}

static void test_lightpad_smoothing() {
    std::printf("lightpad smoothing\n");
    TouchFx mouse, pad;
    mouse.touch_down(Source::Mouse, 0.0f, 0.0f, 1.0f);
    pad.touch_down(Source::Lightpad, 0.0f, 0.0f, 1.0f);
    mouse.update(0.016f), pad.update(0.016f);
    mouse.touch_move(Source::Mouse, 1.0f, 1.0f, 1.0f);
    pad.touch_move(Source::Lightpad, 1.0f, 1.0f, 1.0f);
    const Params m = mouse.update(0.016f), p = pad.update(0.016f);
    CHECK(m.x > 0.8f);                                   // 8 ms: almost there after one frame
    CHECK(p.x > 0.3f && p.x < 0.5f);                     // 30 ms: glides over the 28 ms gap
}

static void test_pads() {
    std::printf("MK2 pad points\n");
    const Point p1 = mk2_pad_point(1), p4 = mk2_pad_point(4), p13 = mk2_pad_point(13), p16 = mk2_pad_point(16);
    CHECK(near(p1.x, 0.125f) && near(p1.y, 0.125f));     // Pad 1 bottom-left
    CHECK(near(p4.x, 0.875f) && near(p4.y, 0.125f));
    CHECK(near(p13.x, 0.125f) && near(p13.y, 0.875f));
    CHECK(near(p16.x, 0.875f) && near(p16.y, 0.875f));   // Pad 16 top-right
}

int main() {
    test_table();
    test_touch();
    test_sources();
    test_hold();
    test_select_depth();
    test_strobe_hits();
    test_lightpad_smoothing();
    test_pads();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
