// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the NuVJ Perform mapper (M10, src/perform/nuvj_perform.cpp), no hardware.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "himmel/perform/nuvj_perform.h"

using namespace himmel;
using namespace himmel::protocol;
using perform::NuvjPerform;
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

// Control indices (weapons/nuvj.weapon.json).
enum { A_BANK = 0, A_PMTR1 = 1, A_LATCH = 3, A_AUTO = 4, A_SYNC = 5, A_JOG = 15, A_SCRATCH = 16, B_JOG = 32, B_SCRATCH = 33,
       INSTA_A = 34, INSTA_B = 35, FX_LEVEL = 36, MASTER_FX = 37, MFX_SELECT = 38, INSTA_BLACK = 40, BPM_TAP = 43,
       SPEED_A = 45, XFADER = 47, AUTO_FADE = 49, FADER_START = 50, BANK_PUSH_A = 51 };

struct Rig {
    std::unique_ptr<LiveState> live = std::make_unique<LiveState>();
    std::unique_ptr<Catalog> cat = std::make_unique<Catalog>();
    std::unique_ptr<TransportSection> tr = std::make_unique<TransportSection>();
    NuvjPerform m;
    FakeSink sink;
    uint8_t leds[128]{};
    perform::NuvjLcd lcd;
    uint64_t t = 1'000'000;

    Rig() {
        std::memset(live.get(), 0, sizeof(LiveState));
        std::memset(cat.get(), 0, sizeof(Catalog));
        std::memset(tr.get(), 0, sizeof(TransportSection));
        live->program_deck = 0;
        live->focus_deck = kNone, live->focus_layer = kNone;
        for (auto& d : live->deck)
            for (auto& l : d.layer) {
                l.clip = -1;
                l.speed = 1.0f;
                l.playing = 1;
                for (auto& f : l.fx) f.effect = -1;
            }
        for (auto& f : live->master_fx) f.effect = -1;
        tr->clock.bpm = 120.0f;
        cat->clip_count = 20;
        for (int i = 0; i < 20; ++i) std::snprintf(cat->clip[i].name, kNameLen, "clip%02d.mov", i);
        cat->effect_count = 3;
        for (int i = 0; i < 3; ++i) cat->effect[i].param_count = 2;
        m.set_context(tr.get());
    }
    void ev(EventType type, int id, int value = 0, int raw = 0) {
        mk2::Event e{};
        e.t_us = t;
        e.type = type;
        e.id = static_cast<uint8_t>(id);
        e.value = static_cast<int16_t>(value);
        e.raw = static_cast<uint16_t>(raw);
        m.on_event(e, *live, *cat, sink);
        t += 10'000;
    }
    void press(int c) {
        ev(EventType::ButtonDown, c);
        ev(EventType::ButtonUp, c);
    }
    void tick(uint64_t dt = 16'000) {
        m.tick(t, *live, *cat, sink, leds, lcd);
        t += dt;
    }
    void fader(int c, float x) { ev(EventType::Fader, c, static_cast<int>(std::lround(x * 16383))); }
};

static void test_pads_banks() {
    std::printf("pads = the deck's bank of 9, BANK turns, push = dice\n");
    Rig r;
    r.ev(EventType::PadHit, 1, 127);  // deck A pad 1 -> clip 0 on A layer 1
    const Command* c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 0 && c->layer == 0 && c->i == 0);
    r.ev(EventType::PadRelease, 1);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && (c->flags & kRelease));
    r.ev(EventType::PadHit, 12, 127);  // deck B pad 3 -> clip 2 on B
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 1 && c->i == 2);
    r.ev(EventType::PadRelease, 12);
    r.ev(EventType::EncoderDelta, A_BANK, 1);
    CHECK(r.m.bank(0) == 1);
    r.ev(EventType::EncoderDelta, A_BANK, 5);
    r.ev(EventType::EncoderDelta, A_BANK, 5);
    CHECK(r.m.bank(0) == 2);  // 20 clips = 3 banks of 9: clamped
    r.ev(EventType::PadHit, 2, 127);  // bank 3 pad 2 = clip 19
    CHECK(r.sink.last(CommandType::LayerTrigger)->i == 19);
    r.ev(EventType::PadRelease, 2);
    r.sink.clear();
    r.ev(EventType::PadHit, 3, 127);  // clip 20 does not exist
    CHECK(r.sink.count(CommandType::LayerTrigger) == 0);
    r.ev(EventType::PadRelease, 3);
    // The selected layer follows the engine's focus on that deck.
    r.live->focus_deck = 1, r.live->focus_layer = 2;
    r.ev(EventType::PadHit, 10, 127);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 1 && c->layer == 2);
    r.ev(EventType::PadRelease, 10);
    r.press(BANK_PUSH_A);
    c = r.sink.last(CommandType::Dice);
    CHECK(c && c->deck == 0 && c->layer == kNone && c->param == static_cast<uint8_t>(DiceVerb::Roll));
    // LATCH blocks the dice; AUTO = roll, a long AUTO = mutate.
    r.press(A_LATCH);
    CHECK(r.m.latched(0));
    r.sink.clear();
    r.press(A_AUTO);
    r.press(BANK_PUSH_A);
    CHECK(r.sink.count(CommandType::Dice) == 0);
    r.press(A_LATCH);
    r.press(A_AUTO);
    CHECK(r.sink.last(CommandType::Dice)->param == static_cast<uint8_t>(DiceVerb::Roll));
    r.ev(EventType::ButtonDown, A_AUTO);
    r.t += 800'000;
    r.ev(EventType::ButtonUp, A_AUTO);
    CHECK(r.sink.last(CommandType::Dice)->param == static_cast<uint8_t>(DiceVerb::Mutate));
    // LEDs: a pad whose clip is on the deck is lit.
    r.live->deck[1].layer[0].clip = 2;  // B pad 3 (bank 1)
    r.tick();
    CHECK(r.leds[31] == 127 && r.leds[29] == 0);  // B Pad 3 = control 31, B Pad 1 = 29
}

static void test_crossfader() {
    std::printf("crossfader decides PROGRAM, soft takeover, INSTA, FADER START\n");
    Rig r;
    r.live->crossfader = 0.0f;
    r.fader(XFADER, 0.5f);  // far from the engine's fader, never crossed: nothing yet
    CHECK(!r.m.picked() && r.sink.count(CommandType::Crossfader) == 0);
    r.fader(XFADER, 0.01f);  // crosses / reaches it: caught
    r.fader(XFADER, 0.3f);
    const Command* c = r.sink.last(CommandType::Crossfader);
    CHECK(r.m.picked() && c && near(c->f[0], 0.3f, 0.001f) && !(c->flags & kRelative));
    r.live->crossfader = 0.3f;
    r.tick();
    // The MK2 wheel moves it: the fader lets go until it catches up again.
    r.live->crossfader = 0.7f;
    r.t += 200'000;
    r.tick();
    CHECK(!r.m.picked());
    r.sink.clear();
    r.fader(XFADER, 0.4f);
    CHECK(r.sink.count(CommandType::Crossfader) == 0);
    r.fader(XFADER, 0.71f);
    CHECK(r.m.picked() && r.sink.count(CommandType::Crossfader) == 1);
    // INSTA B: A on air -> TAKE at the AUTO FADE speed; INSTA A while A is on air = nothing.
    r.ev(EventType::KnobDelta, AUTO_FADE, 0, 64);  // raw 64 of 127 = about 4 s
    CHECK(r.m.fade_ms() == 4000);
    r.sink.clear();
    r.press(INSTA_A);
    CHECK(r.sink.got.empty());
    r.press(INSTA_B);
    c = r.sink.last(CommandType::Take);
    CHECK(c && c->i == 4000);
    r.ev(EventType::KnobDelta, AUTO_FADE, 0, 0);  // INSTANT = a cut
    r.press(INSTA_B);
    CHECK(r.sink.count(CommandType::Cut) == 1);
    r.press(INSTA_BLACK);
    CHECK(r.sink.last(CommandType::Black)->i == kToggle);
    // FADER START: leaving the A end restarts deck B's clip.
    Rig f;
    f.live->deck[1].layer[0].clip = 4;
    f.ev(EventType::Switch, FADER_START, 1);
    f.fader(XFADER, 0.0f);
    f.fader(XFADER, 0.1f);
    c = f.sink.last(CommandType::LayerPlay);
    CHECK(c && c->deck == 1 && c->i == static_cast<int32_t>(PlayAction::Restart));
    // LEDs: INSTA A / B show the deck on air.
    r.tick();
    CHECK(r.leds[INSTA_A] == 127 && r.leds[INSTA_B] == 0);
}

static void test_wheels() {
    std::printf("wheels: BEND / SCRATCH / FX WHEEL, let go after 120 ms\n");
    Rig r;
    r.live->deck[0].layer[0].clip = 3;
    r.live->deck[0].layer[0].speed = 1.0f;
    CHECK(r.m.wheel(0) == NuvjPerform::Wheel::Bend);
    r.ev(EventType::EncoderDelta, A_JOG, 5);  // BEND: +10 %
    const Command* c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && c->deck == 0 && near(c->f[0], 1.1f) && !(c->flags & kRelative));
    r.tick();  // still turning
    r.t += 200'000;
    r.tick();  // let go: back to normal speed
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && near(c->f[0], 1.0f));
    // BEND on a paused clip scrubs.
    r.live->deck[0].layer[0].playing = 0;
    r.sink.clear();
    r.ev(EventType::EncoderDelta, A_JOG, -2);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->i == 1 && (c->flags & kRelative) && near(c->f[0], -0.06f));
    r.t += 200'000;
    r.tick();
    CHECK(r.sink.last(CommandType::LayerScratch)->i == 0);
    r.live->deck[0].layer[0].playing = 1;
    // SCRATCH: drag the playhead; let go = play on.
    r.press(A_SCRATCH);
    CHECK(r.m.wheel(0) == NuvjPerform::Wheel::Scratch);
    r.sink.clear();
    r.ev(EventType::EncoderDelta, A_JOG, 3);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->i == 1 && near(c->f[0], 0.09f));
    r.t += 200'000;
    r.tick();
    CHECK(r.sink.last(CommandType::LayerScratch)->i == 0);
    r.tick();
    CHECK(r.leds[A_SCRATCH] == 127);
    // FX WHEEL: sweep the main effect's mix, springs back.
    r.press(A_SCRATCH);
    CHECK(r.m.wheel(0) == NuvjPerform::Wheel::FxWheel);
    r.live->deck[0].layer[0].fx[0].effect = 1;
    r.live->deck[0].layer[0].fx[0].mix = 0.5f;
    r.sink.clear();
    r.ev(EventType::EncoderDelta, A_JOG, 4);
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && (c->flags & kRelative) && near(c->f[0], 0.16f));
    r.t += 200'000;
    r.tick();
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && !(c->flags & kRelative) && near(c->f[0], 0.5f));
    r.press(A_SCRATCH);
    CHECK(r.m.wheel(0) == NuvjPerform::Wheel::Bend);
    // Deck B's wheel is its own.
    CHECK(r.m.wheel(1) == NuvjPerform::Wheel::Bend);
    r.press(B_SCRATCH);
    CHECK(r.m.wheel(1) == NuvjPerform::Wheel::Scratch && r.m.wheel(0) == NuvjPerform::Wheel::Bend);
}

static void test_trim_and_sync() {
    std::printf("hold pad + wheel = trim, SYNC = BPM time base, SYNC + wheel = beat loop\n");
    Rig r;
    r.live->deck[0].layer[0].clip = 0;
    r.ev(EventType::PadHit, 1, 127);
    r.sink.clear();
    r.ev(EventType::EncoderDelta, A_JOG, 2);  // trim: in and out move together
    CHECK(r.sink.count(CommandType::LayerSet) == 2 && r.sink.count(CommandType::LayerSpeed) == 0);
    const Command* c = r.sink.last(CommandType::LayerSet);
    CHECK(c && (c->flags & kRelative) && near(c->f[0], 0.01f));
    r.ev(EventType::PadRelease, 1);
    r.t += 200'000;
    r.tick();
    CHECK(r.sink.count(CommandType::LayerSpeed) == 0);  // a trim leaves the speed alone
    // SYNC tap = BPM time base on / off.
    r.sink.clear();
    r.press(A_SYNC);
    c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::TimeBase) && c->i == 1);
    r.tr->layer[0][0].time_base = 1;
    r.tick();
    CHECK(r.leds[A_SYNC] == 127);
    // SYNC held + wheel = beat loop (1 beat, then shorter); release = loop off, no time-base change.
    r.sink.clear();
    r.ev(EventType::ButtonDown, A_SYNC);
    r.ev(EventType::EncoderDelta, A_JOG, 1);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 1.0f));
    r.ev(EventType::EncoderDelta, A_JOG, -1);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 0.5f));
    r.ev(EventType::ButtonUp, A_SYNC);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 0.0f) && r.sink.count(CommandType::LayerSet) == 0);
}

static void test_knobs_master_lcd() {
    std::printf("PMTR, master FX, SPEED fader, BPM TAP, LCD\n");
    Rig r;
    r.live->deck[0].layer[0].clip = 5;
    r.ev(EventType::KnobDelta, A_PMTR1, 32, 70);  // no effect: nothing
    CHECK(r.sink.count(CommandType::FxParam) == 0);
    r.live->deck[0].layer[0].fx[0].effect = 2;
    r.ev(EventType::KnobDelta, A_PMTR1, 32, 70);
    const Command* c = r.sink.last(CommandType::FxParam);
    CHECK(c && c->deck == 0 && c->param == 0 && (c->flags & kRelative) && near(c->f[0], 0.032f));
    r.ev(EventType::EncoderDelta, MFX_SELECT, 1);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->slot == 0 && c->i == 0);
    r.live->master_fx[0].effect = 0;
    r.press(MASTER_FX);
    CHECK(r.sink.last(CommandType::FxBypass) != nullptr);
    r.ev(EventType::KnobDelta, FX_LEVEL, -100, 10);
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && c->deck == kMasterDeck && near(c->f[0], -0.1f));
    r.fader(SPEED_A, 0.75f);
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && near(c->f[0], 1.5f, 0.001f));
    r.press(BPM_TAP);
    CHECK(r.sink.last(CommandType::Clock)->param == static_cast<uint8_t>(ClockParam::Tap));
    r.tick();
    CHECK(std::strncmp(r.lcd.text, "CLIP05", 6) == 0);  // the PROGRAM deck's clip, upper case
    CHECK(std::strcmp(r.lcd.number[0], "120") == 0 && r.lcd.alpha[0] == 'A');
    r.press(A_SCRATCH);
    r.tick();
    CHECK(std::strncmp(r.lcd.text, "SCRATCH", 7) == 0);
    bool ok = true;  // seq runs 1, 2, 3 ...
    for (size_t i = 0; i < r.sink.got.size(); ++i) ok = ok && r.sink.got[i].seq == i + 1;
    CHECK(ok);
}

int main() {
    test_pads_banks();
    test_crossfader();
    test_wheels();
    test_trim_and_sync();
    test_knobs_master_lcd();
    std::printf("\nnuvj_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
