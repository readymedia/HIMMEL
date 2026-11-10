// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the Entrancer MIDI mapper (no hardware, no network).
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "himmel/engine/entrancer.h"
#include "himmel/engine/midi_in.h"

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

static void test_channel() {
    std::printf("channel filter\n");
    Mapper m;
    touchfx::TouchFx fx;
    m.set_channel(3);
    send(m, fx, {0xB0, 12, 100});            // channel 1: ignored
    CHECK(near(m.state().x, 0.5f));
    send(m, fx, {0xB2, 12, 100});            // channel 3: taken
    CHECK(near(m.state().x, 100 / 127.0f));
    m.set_channel(99);                       // out of range = all channels
    CHECK(m.channel() == 0);
    send(m, fx, {0xBF, 12, 0});
    CHECK(near(m.state().x, 0.0f));
}

static void test_midi_in_ring() {
    std::printf("MIDI input ring (no port needed)\n");
    MidiIn in("no such port for the test");
    CHECK(!in.is_open());
    in.on_short(0x7F0CB0);                   // CC 12 = 127 (WinMM packs status | d1 << 8 | d2 << 16)
    in.on_short(0x24C0);                     // program change 36: two bytes
    in.on_short(0xFE);                       // active sensing: one byte
    uint8_t buf[16] = {};
    const size_t n = in.read(buf, sizeof(buf));
    CHECK(n == 6);
    CHECK(buf[0] == 0xB0 && buf[1] == 12 && buf[2] == 127 && buf[3] == 0xC0 && buf[4] == 36 && buf[5] == 0xFE);
    Mapper m;
    touchfx::TouchFx fx;
    m.feed(buf, n, fx);
    CHECK(m.state().program == 36 && near(m.state().x, 1.0f));
    CHECK(in.read(buf, sizeof(buf)) == 0);
    for (int i = 0; i < 3000; ++i) in.on_short(0x7F0CB0);   // overflow: whole messages dropped, never half
    size_t total = 0;
    for (size_t k; (k = in.read(buf, sizeof(buf))) > 0;) total += k;
    CHECK(total % 3 == 0 && total > 0 && total <= 4096);
}

int main() {
    test_table();
    test_pad();
    test_extensions();
    test_bytes();
    test_channel();
    test_midi_in_ring();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
