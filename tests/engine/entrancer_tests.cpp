// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the Entrancer MIDI mapper (no hardware, no network).
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "himmel/engine/entrancer.h"

using namespace himmel::engine;
using namespace himmel::engine::entrancer;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

static void send(Mapper& m, touchfx::TouchFx& fx, std::initializer_list<int> bytes) {
    std::vector<uint8_t> b;
    for (int v : bytes) b.push_back(static_cast<uint8_t>(v));
    m.feed(b.data(), b.size(), fx);
}

static void test_table() {
    std::printf("program table\n");
    for (int n = 0; n < kPrograms; ++n) {
        const Program& p = program(n);
        CHECK(p.effect < touchfx::kEffects);
        CHECK(p.fixed_y >= 0.0f && p.fixed_y <= 1.0f);
    }
    CHECK(program(-3).effect == program(0).effect);
    CHECK(program(500).effect == program(99).effect);
    CHECK(program(24).effect == static_cast<uint8_t>(touchfx::Effect::Grid) && near(program(24).fixed_y, 1.0f));  // mirror tile
    CHECK(program(29).effect == static_cast<uint8_t>(touchfx::Effect::Strobe));
    CHECK(!program(90).y_is_mix);  // vertical wave: X and Y are both wave parameters
}

static void test_pad() {
    std::printf("pad: touch, X, Y = mix\n");
    Mapper m;
    touchfx::TouchFx fx;
    send(m, fx, {0xC0, 14});                 // program 14: tiles, Y = how much
    CHECK(fx.effect() == static_cast<int>(touchfx::Effect::Grid));
    send(m, fx, {0xB0, 12, 127, 13, 64});    // running status: X right, Y middle
    send(m, fx, {0xB0, 92, 127});            // touch on
    CHECK(fx.finger_down() && fx.source() == touchfx::Source::Entrancer);
    CHECK(near(fx.target_x(), 1.0f));
    CHECK(near(fx.target_y(), program(14).fixed_y));   // the effect's Y stays put ...
    CHECK(near(fx.depth(), 64 / 127.0f));              // ... Y is the amount
    send(m, fx, {0xB0, 13, 0});
    CHECK(near(fx.depth(), 0.0f));
    send(m, fx, {0xB0, 92, 0});
    CHECK(!fx.finger_down());

    std::printf("pad: X and Y both parameters\n");
    send(m, fx, {0xC1, 90});                 // another channel works too
    send(m, fx, {0xB1, 92, 127, 12, 10, 13, 100});
    CHECK(fx.effect() == static_cast<int>(touchfx::Effect::Waves));
    CHECK(near(fx.target_x(), 10 / 127.0f) && near(fx.target_y(), 100 / 127.0f));
    CHECK(near(fx.depth(), 1.0f));
}

static void test_extensions() {
    std::printf("Himmel extensions\n");
    Mapper m;
    touchfx::TouchFx fx;
    send(m, fx, {0xC0, 90, 0xB0, 102, 0});   // FX balance fully dry
    send(m, fx, {0xB0, 92, 127});
    CHECK(near(fx.depth(), 0.0f));
    send(m, fx, {0xB0, 102, 127, 108, 0});   // wet, pressure 0
    CHECK(near(fx.depth(), 1.0f) && near(fx.pressure(), 0.0f));
    send(m, fx, {0xB0, 104, 127});
    CHECK(m.state().freeze);
    send(m, fx, {0xB0, 105, 127});           // lever on AUDIO: video untouched
    CHECK(!fx.finger_down() && m.state().video_off);
    send(m, fx, {0xB0, 105, 0});             // back on VIDEO with the finger still down
    CHECK(fx.finger_down());
    send(m, fx, {0xB0, 106, 1, 107, 12});    // 140 BPM
    CHECK(m.state().bpm == 140);
    send(m, fx, {0xB0, 107, 50});            // LSB without MSB: ignored
    CHECK(m.state().bpm == 140);
    m.lost(fx);
    CHECK(!fx.finger_down() && !m.state().freeze);
}

static void test_bytes() {
    std::printf("MIDI parsing\n");
    Mapper m;
    touchfx::TouchFx fx;
    send(m, fx, {0xFE, 0xF8, 0xB0, 0xFE, 12, 0xF8, 30});   // real-time bytes inside a message
    CHECK(near(m.state().x, 30 / 127.0f));
    send(m, fx, {0xF0, 0x42, 12, 99, 0xF7, 12, 99});        // SysEx data never counts as CCs
    CHECK(near(m.state().x, 30 / 127.0f));
    send(m, fx, {0x90, 60, 100, 0xE0, 0, 64});               // notes / pitch bend ignored
    CHECK(fx.effect() == 0);
    const uint32_t before = m.state().messages;
    send(m, fx, {0xB0, 12});                                 // half a message ...
    CHECK(m.state().messages == before);
    send(m, fx, {64});                                       // ... completed in the next datagram
    CHECK(near(m.state().x, 64 / 127.0f));
}

int main() {
    test_table();
    test_pad();
    test_extensions();
    test_bytes();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
