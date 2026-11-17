// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the Lightpad Perform mapper (M9, src/perform/lightpad_perform.cpp), no hardware.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "himmel/perform/lightpad_perform.h"

using namespace himmel;
using namespace himmel::protocol;
using perform::LightpadPerform;
using mk2::EventType;

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

struct FakeSink : perform::CommandSink {
    std::vector<Command> got;
    bool push(const Command& c) override {
        got.push_back(c);
        return true;
    }
    void clear() { got.clear(); }
    int count(CommandType t) const {
        int n = 0;
        for (const auto& c : got) n += c.type == t;
        return n;
    }
    const Command* last(CommandType t) const {
        for (auto it = got.rbegin(); it != got.rend(); ++it)
            if (it->type == t) return &*it;
        return nullptr;
    }
};

struct Rig {
    std::unique_ptr<LiveState> live = std::make_unique<LiveState>();
    std::unique_ptr<Catalog> cat = std::make_unique<Catalog>();
    std::unique_ptr<TransportSection> tr = std::make_unique<TransportSection>();
    LightpadPerform m;
    FakeSink sink;
    uint8_t px[LightpadPerform::kPixels][3]{};
    uint64_t t = 1'000'000;

    Rig() {
        std::memset(live.get(), 0, sizeof(LiveState));
        std::memset(cat.get(), 0, sizeof(Catalog));
        std::memset(tr.get(), 0, sizeof(TransportSection));
        live->program_deck = 0;
        live->focus_deck = 1, live->focus_layer = 0;
        for (auto& d : live->deck)
            for (auto& l : d.layer) {
                l.clip = -1;
                for (auto& f : l.fx) f.effect = -1;
            }
        for (auto& f : live->master_fx) f.effect = -1;
        for (auto& d : tr->layer)
            for (auto& l : d) l.in = 0.0f, l.out = 1.0f;
        cat->clip_count = 4;
        for (int i = 0; i < 4; ++i) cat->clip[i].thumb_rgb = 0x2080FF;
        cat->effect_count = 7;
        for (int i = 0; i < 7; ++i) {
            std::snprintf(cat->effect[i].name, kNameLen, "FX%d", i);
            cat->effect[i].param_count = 2;
        }
        cat->effect[1].reserved[0] = 1;  // a generator: not a touch zone
        m.set_context(tr.get());
    }
    void ev(EventType type, int id, int x = 0, int y = 0, int z = 0) {
        mk2::Event e{};
        e.t_us = t;
        e.type = type;
        e.id = static_cast<uint8_t>(id);
        e.value = static_cast<int16_t>(x);
        e.raw = static_cast<uint16_t>(y);
        e.reserved = static_cast<uint16_t>(z);
        m.on_event(e, *live, *cat, sink);
        t += 10'000;
    }
    void mode_tap() {
        ev(EventType::ButtonDown, LightpadPerform::kModeControl);
        ev(EventType::ButtonUp, LightpadPerform::kModeControl);
    }
    void next_mode() {  // double-tap
        mode_tap();
        mode_tap();
        t += 500'000;
    }
    void tick() {
        m.tick(t, *live, *cat, sink, px);
        t += 16'000;
    }
    // Plays the engine: the last Crossfader command lands.
    void apply_xfade() {
        if (const Command* c = sink.last(CommandType::Crossfader)) live->crossfader = c->f[0];
    }
    const uint8_t* at(int col, int row) const { return px[row * LightpadPerform::kGrid + col]; }
};

static int X(float u) { return static_cast<int>(std::lround(u * 4095)); }

static void test_modes() {
    std::printf("Mode: double-tap = next mode, single tap in TOUCH = hold\n");
    Rig r;
    CHECK(r.m.mode() == LightpadPerform::Mode::Touch);
    r.mode_tap();
    CHECK(r.m.hold());
    r.t += 500'000;
    r.mode_tap();
    CHECK(!r.m.hold());
    r.t += 500'000;
    r.next_mode();
    CHECK(r.m.mode() == LightpadPerform::Mode::Mix);
    CHECK(!r.m.hold());
    r.next_mode();
    CHECK(r.m.mode() == LightpadPerform::Mode::Scrub);
    r.next_mode();
    CHECK(r.m.mode() == LightpadPerform::Mode::Touch);
    // Other weapon events are ignored.
    r.sink.clear();
    r.ev(EventType::PadHit, 3, 100);
    r.ev(EventType::ButtonDown, 0);
    CHECK(r.sink.got.empty());
}

static void test_touch() {
    std::printf("TOUCH: X = effect, Y = amount, pressure = PRESS, finger 2 = param 2, hold\n");
    Rig r;
    r.ev(EventType::TouchStart, 0, X(0.05f), X(0.25f), 128);  // zone 1 of 5 (effect 0), 3/4 up
    const Command* c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->slot == 1 && c->i == 0);
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && c->slot == 1 && near(c->f[0], 0.75f, 0.01f));
    c = r.sink.last(CommandType::FxPress);
    CHECK(c && near(c->f[0], 128 / 255.0f));
    r.ev(EventType::TouchMove, 0, X(0.30f), X(0.25f), 128);  // zone 2 skips the generator: effect 2
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == 2);
    r.sink.clear();
    r.ev(EventType::TouchMove, 0, X(0.30f), X(0.25f), 129);  // tiny change: nothing
    CHECK(r.sink.got.empty());
    r.ev(EventType::TouchStart, 1, X(0.8f), X(0.5f), 50);  // second finger: param 2
    c = r.sink.last(CommandType::FxParam);
    CHECK(c && c->param == 1 && near(c->f[0], 0.8f, 0.01f));
    r.ev(EventType::TouchEnd, 1, X(0.8f), X(0.5f), 0);
    CHECK(r.sink.count(CommandType::FxSet) == 0);  // one finger still down
    r.ev(EventType::TouchEnd, 0, X(0.3f), X(0.25f), 0);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1);  // lift = off
    // Hold: the effect stays after the lift, PRESS goes to 0; tap again = off.
    r.mode_tap();
    r.t += 500'000;
    r.sink.clear();
    r.ev(EventType::TouchStart, 0, X(0.95f), X(0.0f), 200);
    r.ev(EventType::TouchEnd, 0, X(0.95f), X(0.0f), 0);
    CHECK(r.sink.count(CommandType::FxSet) == 1);
    c = r.sink.last(CommandType::FxPress);
    CHECK(c && near(c->f[0], 0.0f));
    r.mode_tap();
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1 && !r.m.hold());
    // LEDs: the border is red (PROGRAM).
    r.tick();
    CHECK(r.at(7, 0)[0] > 0 && r.at(7, 0)[1] == 0);
}

static void test_mix_takeover() {
    std::printf("MIX: soft takeover against the wheel, TAKE / CUT rows\n");
    Rig r;
    r.next_mode();
    CHECK(r.m.mode() == LightpadPerform::Mode::Mix);
    r.live->crossfader = 0.5f;
    r.sink.clear();
    // A finger far from the fader does not move it.
    r.ev(EventType::TouchStart, 0, X(0.1f), X(0.5f), 80);
    r.ev(EventType::TouchMove, 0, X(0.2f), X(0.5f), 80);
    CHECK(!r.m.picked() && r.sink.count(CommandType::Crossfader) == 0);
    r.tick();
    r.ev(EventType::TouchMove, 0, X(0.45f), X(0.5f), 80);
    CHECK(!r.m.picked() && r.sink.count(CommandType::Crossfader) == 0);
    // It crosses the fader: caught, and from now on it drives it absolutely.
    r.ev(EventType::TouchMove, 0, X(0.6f), X(0.5f), 80);
    const Command* c = r.sink.last(CommandType::Crossfader);
    CHECK(r.m.picked() && c && !(c->flags & kRelative) && near(c->f[0], 0.6f));
    r.apply_xfade();
    r.ev(EventType::TouchMove, 0, X(0.8f), X(0.5f), 80);
    r.apply_xfade();
    CHECK(near(r.live->crossfader, 0.8f));
    // The MK2 wheel moves the fader under the finger: after the state lag the finger lets go.
    r.live->crossfader = 0.3f;
    r.tick();  // too soon after our last send
    CHECK(r.m.picked());
    r.t += 200'000;
    r.tick();
    CHECK(!r.m.picked());
    r.sink.clear();
    r.ev(EventType::TouchMove, 0, X(0.7f), X(0.5f), 80);  // still on the far side: nothing
    CHECK(r.sink.count(CommandType::Crossfader) == 0);
    r.ev(EventType::TouchMove, 0, X(0.31f), X(0.5f), 80);  // close enough: caught again
    CHECK(r.m.picked() && r.sink.count(CommandType::Crossfader) == 1);
    r.ev(EventType::TouchEnd, 0, X(0.31f), X(0.5f), 0);
    CHECK(!r.m.picked());
    // A finger that lands on the fader catches it at once.
    r.apply_xfade();
    r.sink.clear();
    r.ev(EventType::TouchStart, 2, X(r.live->crossfader + 0.01f), X(0.5f), 80);
    CHECK(r.m.picked() && r.sink.count(CommandType::Crossfader) == 1);
    r.ev(EventType::TouchEnd, 2, 0, 0, 0);
    // Top row = TAKE, bottom row = CUT, and they never touch the fader.
    r.sink.clear();
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.02f), 80);
    r.ev(EventType::TouchEnd, 0, X(0.5f), X(0.02f), 0);
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.98f), 80);
    r.ev(EventType::TouchEnd, 0, X(0.5f), X(0.98f), 0);
    CHECK(r.sink.count(CommandType::Take) == 1 && r.sink.count(CommandType::Cut) == 1);
    CHECK(r.sink.count(CommandType::Crossfader) == 0);
    // A TAKE running under a caught finger lets go too.
    r.live->crossfader = 0.5f;
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.5f), 80);
    CHECK(r.m.picked());
    r.live->take_active = 1;
    r.t += 200'000;
    r.tick();
    CHECK(!r.m.picked());
    r.live->take_active = 0;
    r.ev(EventType::TouchEnd, 0, 0, 0, 0);
    // LEDs: the crossfader is the white column; the top row is amber.
    r.live->crossfader = 0.0f;
    r.tick();
    CHECK(r.at(0, 7)[0] == 255 && r.at(0, 7)[1] == 255);
    CHECK(r.at(5, 0)[0] > 0 && r.at(5, 0)[1] > 0 && r.at(5, 0)[2] == 0);
    CHECK(r.at(10, 7)[1] == 40);  // deck B side = PREVIEW (A on air): green
}

static void test_scrub() {
    std::printf("SCRUB: X = playhead of the focused layer, lift = let go\n");
    Rig r;
    r.next_mode();
    r.next_mode();
    CHECK(r.m.mode() == LightpadPerform::Mode::Scrub);
    r.sink.clear();
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.5f), 80);  // no clip on the focused layer: nothing
    r.ev(EventType::TouchEnd, 0, X(0.5f), X(0.5f), 0);
    CHECK(r.sink.got.empty());
    r.live->deck[1].layer[0].clip = 2;
    r.tr->layer[1][0].in = 0.2f, r.tr->layer[1][0].out = 0.6f;
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.5f), 80);
    const Command* c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == 1 && !(c->flags & kRelative) && near(c->f[0], 0.4f));
    r.ev(EventType::TouchMove, 0, X(1.0f), X(0.5f), 80);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && near(c->f[0], 0.6f));
    CHECK(r.sink.count(CommandType::FxPress) == 0);  // no effect on the layer
    r.ev(EventType::TouchEnd, 0, X(1.0f), X(0.5f), 0);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->i == 0);
    // With an effect, pressure = PRESS.
    r.live->deck[1].layer[0].fx[0].effect = 3;
    r.sink.clear();
    r.ev(EventType::TouchStart, 0, X(0.1f), X(0.5f), 200);
    c = r.sink.last(CommandType::FxPress);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && c->f[0] > 0.7f);
    // Leaving the mode with a finger down lets go of the playhead.
    r.next_mode();
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->i == 0 && r.m.mode() == LightpadPerform::Mode::Touch);
}

static void test_reset() {
    std::printf("reset / reconnect\n");
    Rig r;
    r.next_mode();
    r.live->crossfader = 0.5f;
    r.ev(EventType::TouchStart, 0, X(0.5f), X(0.5f), 80);
    CHECK(r.m.picked());
    r.ev(EventType::Disconnected, 0);
    CHECK(!r.m.picked() && r.m.mode() == LightpadPerform::Mode::Mix);
    r.sink.clear();
    r.ev(EventType::TouchMove, 0, X(0.7f), X(0.5f), 80);  // a stale move: ignored
    CHECK(r.sink.got.empty());
    bool ok = true;  // seq runs 1, 2, 3 ...
    Rig s;
    s.ev(EventType::TouchStart, 0, X(0.1f), X(0.1f), 100);
    for (size_t i = 0; i < s.sink.got.size(); ++i) ok = ok && s.sink.got[i].seq == i + 1;
    CHECK(ok && !s.sink.got.empty());
}

int main() {
    test_modes();
    test_touch();
    test_mix_takeover();
    test_scrub();
    test_reset();
    std::printf("\nlightpad_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
