// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the MK2 Perform mapping (M4) (src/perform/), no hardware:
// a fake CommandSink records what the mapper sends, fake LiveState / Catalog
// stand in for the engine. Mapping table: docs/agents/handoff/control.md, "Perform mapping (M4)".
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "himmel/perform/mk2_perform.h"

using namespace himmel;
using namespace himmel::protocol;
using mk2::Button;
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

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

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

// Test fixture: heap-allocated state (Catalog is ~25 KB), a mapper, a sink.
struct Rig {
    std::unique_ptr<LiveState> live = std::make_unique<LiveState>();
    std::unique_ptr<Catalog> cat = std::make_unique<Catalog>();
    std::unique_ptr<ShowStatus> show = std::make_unique<ShowStatus>();
    std::unique_ptr<TransportSection> tr = std::make_unique<TransportSection>();
    perform::Mk2Perform m;
    FakeSink sink;
    mk2::LedState leds{};
    uint8_t screens[2][mk2::kScreenBytes]{};
    uint64_t t = 1'000'000;

    explicit Rig(int clips = 40, int effects = 3) {
        std::memset(live.get(), 0, sizeof(LiveState));
        std::memset(cat.get(), 0, sizeof(Catalog));
        live->epoch = 7;
        live->program_deck = 0;  // A on air, B = PREVIEW
        live->focus_deck = kNone;
        live->focus_layer = kNone;
        for (auto& d : live->deck) {
            for (auto& l : d.layer) {
                l.clip = -1;
                l.opacity = 1.0f;
                l.speed = 1.0f;
                for (auto& f : l.fx) f.effect = -1;
            }
            for (auto& f : d.fx) f.effect = -1;
        }
        for (auto& f : live->master_fx) f.effect = -1;
        std::memset(show.get(), 0, sizeof(ShowStatus));
        std::memset(tr.get(), 0, sizeof(TransportSection));
        for (auto& d : show->deck) d.scene = -1, d.pad = -1;
        tr->clock.bpm = 120.0f;
        for (auto& d : tr->layer)
            for (auto& l : d)
                for (auto& c : l.cue) c = -1.0f;
        m.set_context(show.get(), tr.get());
        cat->clip_count = static_cast<uint32_t>(clips);
        for (int i = 0; i < clips; ++i) {
            std::snprintf(cat->clip[i].name, kNameLen, "clip%02d.mov", i);
            cat->clip[i].thumb_rgb = 0xFF8000;  // orange
        }
        cat->effect_count = static_cast<uint32_t>(effects);
        for (int i = 0; i < effects; ++i) {
            std::snprintf(cat->effect[i].name, kNameLen, "FX%d", i);
            cat->effect[i].param_count = 2;
            std::snprintf(cat->effect[i].param_name[0], kParamNameLen, "AMOUNT");
            std::snprintf(cat->effect[i].param_name[1], kParamNameLen, "SIZE");
        }
    }
    void ev(EventType type, int id, int value = 0) {
        mk2::Event e{};
        e.t_us = t;
        e.type = type;
        e.id = static_cast<uint8_t>(id);
        e.value = static_cast<int16_t>(value);
        m.on_event(e, *live, *cat, sink);
    }
    void down(Button b) { ev(EventType::ButtonDown, static_cast<int>(b)); }
    void up(Button b) { ev(EventType::ButtonUp, static_cast<int>(b)); }
    void press(Button b) {
        down(b);
        up(b);
    }
    void tick() {
        m.tick(t, *live, *cat, sink, leds, screens);
        t += 16'000;
    }
    // Plays the engine: applies the last Focus command to the state, as the next frame would.
    void apply_focus() {
        if (const Command* c = sink.last(CommandType::Focus)) {
            live->focus_deck = c->deck;
            live->focus_layer = c->layer;
        }
    }
    LayerState& layer(int d, int l) { return live->deck[d].layer[l]; }
    const uint8_t* group(int g) const { return leds.group_rgb[g][0]; }
};

// ---------------------------------------------------------------------------- focus
static void test_initial_focus() {
    std::printf("initial focus\n");
    Rig r;
    r.tick();  // engine has no focus yet: the mapper announces PREP layer 1
    CHECK(r.sink.count(CommandType::Focus) == 1);
    const Command* f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 1 && f->layer == 0);  // PREVIEW = deck B
    r.apply_focus();
    r.sink.clear();
    r.tick();
    CHECK(r.sink.got.empty());  // no repeat once the engine has it

    Rig g;  // the engine (GUI) already has a focus: adopt it, send nothing
    g.live->focus_deck = 0;  // PROGRAM deck A, layer 3
    g.live->focus_layer = 2;
    g.tick();
    CHECK(g.sink.got.empty());
    CHECK(g.group(6)[0] == 127 && g.group(6)[1] == 0);  // G (PROGRAM layer 3) bright red
}

static void test_groups_focus() {
    std::printf("groups set focus\n");
    Rig r;
    r.tick();
    r.apply_focus();
    struct {
        Button b;
        int deck, layer;
    } cases[] = {{Button::GroupA, 1, 0}, {Button::GroupB, 1, 1}, {Button::GroupC, 1, 2}, {Button::GroupD, 1, 3},
                 {Button::GroupE, 0, 0}, {Button::GroupF, 0, 1}, {Button::GroupG, 0, 2}, {Button::GroupH, 0, 3}};
    for (auto& c : cases) {
        r.sink.clear();
        r.press(c.b);
        const Command* f = r.sink.last(CommandType::Focus);
        CHECK(r.sink.got.size() == 1);
        CHECK(f && f->deck == c.deck && f->layer == c.layer);
        r.apply_focus();
    }
    // With B on air the rows keep their roles: A = PREVIEW = deck A now.
    r.live->program_deck = 1;
    r.tick();
    r.apply_focus();
    r.sink.clear();
    r.press(Button::GroupA);
    const Command* f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 0 && f->layer == 0);
    r.sink.clear();
    r.press(Button::GroupH);
    f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 1 && f->layer == 3);
}

static void test_focus_after_take() {
    std::printf("focus after a TAKE flips the decks\n");
    Rig r;
    r.tick();
    r.press(Button::GroupB);  // PREP layer 2 = deck B layer 2
    r.apply_focus();
    r.tick();
    r.sink.clear();
    // The TAKE lands: B is on air, the engine leaves its focus on deck B.
    r.live->program_deck = 1;
    r.tick();
    const Command* f = r.sink.last(CommandType::Focus);
    CHECK(r.sink.count(CommandType::Focus) == 1);
    CHECK(f && f->deck == 0 && f->layer == 1);  // still PREP layer 2, now on deck A
    r.apply_focus();
    r.sink.clear();
    r.ev(EventType::KnobDelta, 3, 100);  // knob 4 (SPEED) now acts on deck A layer 2
    const Command* o = r.sink.last(CommandType::LayerSpeed);
    CHECK(o && o->deck == 0 && o->layer == 1);
    r.sink.clear();
    r.tick();
    CHECK(r.sink.got.empty());
    CHECK(r.group(1)[1] == 127);  // B row still focused (green)

    // PLAY focus follows the role too: E -> after the flip, the new PROGRAM deck.
    r.press(Button::GroupE);  // deck A? no: PROGRAM is B now -> deck B layer 1
    f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 1 && f->layer == 0);
    r.apply_focus();
    r.tick();
    r.sink.clear();
    r.live->program_deck = 0;  // CUT back
    r.tick();
    f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 0 && f->layer == 0);
}

static void test_focus_from_gui() {
    std::printf("focus moved by the GUI is adopted\n");
    Rig r;
    r.tick();
    r.apply_focus();
    r.tick();
    r.sink.clear();
    r.live->focus_deck = 0;  // GUI focuses PROGRAM (A) layer 4
    r.live->focus_layer = 3;
    r.tick();
    CHECK(r.sink.got.empty());
    r.ev(EventType::KnobDelta, 3, 50);
    const Command* o = r.sink.last(CommandType::LayerSpeed);
    CHECK(o && o->deck == 0 && o->layer == 3);
    CHECK(r.group(7)[0] == 127);

    // A group press in the same frame (before the engine applies it) is not undone.
    r.sink.clear();
    r.press(Button::GroupC);
    r.ev(EventType::KnobDelta, 3, 50);  // the state still says A/3
    o = r.sink.last(CommandType::LayerSpeed);
    CHECK(o && o->deck == 1 && o->layer == 2);
}

static void test_new_epoch() {
    std::printf("engine restart (new epoch) resyncs\n");
    Rig r;
    r.tick();
    r.apply_focus();
    r.press(Button::GroupD);
    r.apply_focus();
    r.tick();
    r.sink.clear();
    r.live->epoch = 8;  // restarted engine without a focus
    r.live->focus_deck = kNone;
    r.live->focus_layer = kNone;
    r.tick();
    const Command* f = r.sink.last(CommandType::Focus);
    CHECK(f && f->deck == 1 && f->layer == 3);  // our focus is re-announced
}

// ---------------------------------------------------------------------------- wheel / take
static void test_wheel_and_take() {
    std::printf("wheel = T-bar, PLAY / push = TAKE, RESTART = CUT\n");
    Rig r;
    r.tick();
    r.sink.clear();
    r.ev(EventType::EncoderDelta, 0, 3);  // A on air: clockwise blends B in (toward 1)
    const Command* x = r.sink.last(CommandType::Crossfader);
    CHECK(x && (x->flags & kRelative) && near(x->f[0], 0.1f));
    r.ev(EventType::EncoderDelta, 0, -1);
    x = r.sink.last(CommandType::Crossfader);
    CHECK(x && near(x->f[0], -1.0f / 30.0f));
    r.live->program_deck = 1;  // B on air: clockwise now heads to A (toward 0)
    r.ev(EventType::EncoderDelta, 0, 3);
    x = r.sink.last(CommandType::Crossfader);
    CHECK(x && near(x->f[0], -0.1f));
    // 30 detents = one full T-bar move.
    float sum = 0;
    r.sink.clear();
    for (int i = 0; i < 30; ++i) r.ev(EventType::EncoderDelta, 0, 1);
    for (const auto& c : r.sink.got)
        if (c.type == CommandType::Crossfader) sum += c.f[0];
    CHECK(near(sum, -1.0f));
    // SHIFT + wheel = the SYSTEM menu (never a crossfade); PLAY below closes it and still TAKEs.
    r.sink.clear();
    r.down(Button::Shift);
    r.ev(EventType::EncoderDelta, 0, 5);
    r.up(Button::Shift);
    CHECK(r.sink.count(CommandType::Crossfader) == 0 && r.m.system_menu());

    r.sink.clear();
    r.press(Button::Play);
    const Command* t = r.sink.last(CommandType::Take);
    CHECK(t && t->i == 0);
    r.press(Button::EncoderPush);
    CHECK(r.sink.count(CommandType::Take) == 2);
    r.press(Button::Restart);
    CHECK(r.sink.count(CommandType::Cut) == 1);
}

// ---------------------------------------------------------------------------- helpers
static void hit(Rig& r, int pad, int vel = 100) { r.ev(EventType::PadHit, pad, vel); }
static void lift(Rig& r, int pad) { r.ev(EventType::PadRelease, pad); }
static void page_to(Rig& r, int n) {  // 0 PLAY .. 4 MIX
    r.down(Button::Shift);
    r.press(Button::PageLeft);  // SHIFT + PAGE = jump to PLAY
    r.up(Button::Shift);
    for (int i = 0; i < n; ++i) r.press(Button::PageRight);
}
static Rig& ready(Rig& r) {
    r.tick();
    r.apply_focus();  // PREP layer 1 = deck B layer 1
    r.sink.clear();
    return r;
}
static bool flash_is(const Rig& r, const char* s) { return std::strcmp(r.m.flash(), s) == 0; }

// ---------------------------------------------------------------------------- knobs
static void test_play_page() {
    std::printf("PLAY page: SCROLL, DICE, FX amount, SPEED\n");
    Rig r(10, 3);
    ready(r);
    CHECK(r.m.page() == perform::Mk2Perform::Page::Play);
    r.ev(EventType::KnobDelta, 0, 50);  // under one step: nothing yet
    CHECK(r.sink.got.empty());
    r.ev(EventType::KnobDelta, 0, 50);  // one step: the first clip into the empty layer
    const Command* c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == 0);
    r.layer(1, 0).clip = 5;
    r.ev(EventType::KnobDelta, 0, -100);
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->i == 4);
    r.layer(1, 0).clip = 9;
    r.ev(EventType::KnobDelta, 0, 200);  // two steps, wraps
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->i == 1);

    r.ev(EventType::KnobDelta, 1, 150);
    c = r.sink.last(CommandType::Dice);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->param == static_cast<uint8_t>(DiceVerb::Roll));
    r.ev(EventType::KnobDelta, 1, -150);
    c = r.sink.last(CommandType::Dice);
    CHECK(c && c->param == static_cast<uint8_t>(DiceVerb::Back));
    r.down(Button::All);  // ALL = the whole deck
    r.ev(EventType::KnobDelta, 1, 150);
    r.up(Button::All);
    c = r.sink.last(CommandType::Dice);
    CHECK(c && c->layer == kNone);

    r.sink.clear();
    r.ev(EventType::KnobDelta, 2, 100);  // no effect: nothing
    CHECK(r.sink.got.empty());
    r.layer(1, 0).fx[0].effect = 1;
    r.ev(EventType::KnobDelta, 2, 100);
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && (c->flags & kRelative) && near(c->f[0], 0.1f));
    r.ev(EventType::KnobDelta, 3, -250);
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && c->deck == 1 && (c->flags & kRelative) && near(c->f[0], -0.5f));
    r.down(Button::Shift);  // fine
    r.ev(EventType::KnobDelta, 3, 100);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && near(c->f[0], 0.02f));
}

static void test_other_pages() {
    std::printf("CLIP / LAYER / FX / MIX pages\n");
    Rig r(10, 3);
    ready(r);
    r.layer(1, 0).clip = 2;
    r.press(Button::PageRight);
    CHECK(r.m.page() == perform::Mk2Perform::Page::Clip);
    r.ev(EventType::KnobDelta, 0, 100);
    const Command* c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::In) && (c->flags & kRelative) && near(c->f[0], 0.1f));
    r.ev(EventType::KnobDelta, 1, -100);
    c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::Out) && near(c->f[0], -0.1f));
    r.tr->layer[1][0].mode = static_cast<uint8_t>(PlayMode::Scratch);
    r.ev(EventType::KnobDelta, 2, 100);  // MODE steps and wraps
    c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::Mode) && !(c->flags & kRelative) && c->i == 0);

    r.press(Button::PageRight);  // LAYER
    r.sink.clear();
    r.ev(EventType::KnobDelta, 0, 100);
    c = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && c->deck == 1 && c->layer == 0 && (c->flags & kRelative) && near(c->f[0], 0.1f));
    r.ev(EventType::KnobDelta, 1, 100);
    c = r.sink.last(CommandType::LayerBlend);
    CHECK(c && c->i == 1);
    r.ev(EventType::KnobDelta, 2, 100);
    c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::FadeIn) && near(c->f[0], 0.5f));
    r.ev(EventType::KnobDelta, 3, 100);
    c = r.sink.last(CommandType::LayerSet);
    CHECK(c && c->param == static_cast<uint8_t>(LayerParam::FadeOut));

    r.press(Button::PageRight);  // FX
    r.ev(EventType::KnobDelta, 0, 100);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && c->i == 0);
    r.sink.clear();
    r.ev(EventType::KnobDelta, 1, 100);  // no effect yet in the state: no params
    CHECK(r.sink.count(CommandType::FxParam) == 0);
    r.layer(1, 0).fx[0].effect = 0;
    r.ev(EventType::KnobDelta, 1, 100);
    r.ev(EventType::KnobDelta, 2, 100);
    CHECK(r.sink.count(CommandType::FxParam) == 2 && r.sink.last(CommandType::FxParam)->param == 1);
    r.ev(EventType::KnobDelta, 3, 100);
    CHECK(r.sink.last(CommandType::FxMix) != nullptr);

    r.press(Button::PageRight);  // MIX
    CHECK(r.m.page() == perform::Mk2Perform::Page::Mix);
    r.press(Button::PageRight);  // clamped
    CHECK(r.m.page() == perform::Mk2Perform::Page::Mix);
    r.ev(EventType::KnobDelta, 0, 50);  // TAKE time: 2000 (default) + 250
    r.sink.clear();
    r.press(Button::Play);
    c = r.sink.last(CommandType::Take);
    CHECK(c && c->i == 2250);
    r.ev(EventType::KnobDelta, 1, 100);
    c = r.sink.last(CommandType::Clock);
    CHECK(c && c->param == static_cast<uint8_t>(ClockParam::Bpm) && (c->flags & kRelative) && near(c->f[0], 4.0f));
    r.ev(EventType::KnobDelta, 2, 100);
    c = r.sink.last(CommandType::Clock);
    CHECK(c && c->param == static_cast<uint8_t>(ClockParam::Grid) && c->i == 1);
    r.down(Button::Shift);
    r.press(Button::PageLeft);  // SHIFT + PAGE = back to PLAY
    r.up(Button::Shift);
    CHECK(r.m.page() == perform::Mk2Perform::Page::Play);
}

static void test_macros() {
    std::printf("knobs 5-8 = the PROGRAM scene's macros, SHIFT + 8 = master effect\n");
    Rig r(10, 3);
    ready(r);
    for (int k = 4; k < 8; ++k) r.ev(EventType::KnobDelta, k, 100);
    CHECK(r.sink.count(CommandType::MacroValue) == 4);
    bool ok = true;
    for (int k = 0; k < 4; ++k) {
        const Command& c = r.sink.got[static_cast<size_t>(k)];
        ok = ok && c.type == CommandType::MacroValue && c.slot == k && (c.flags & kRelative) && near(c.f[0], 0.1f);
    }
    CHECK(ok);
    // The same on every page and in PLAY focus.
    page_to(r, 3);
    r.press(Button::GroupE);
    r.sink.clear();
    r.ev(EventType::KnobDelta, 6, -100);
    const Command* c = r.sink.last(CommandType::MacroValue);
    CHECK(c && c->slot == 2 && near(c->f[0], -0.1f));
    r.sink.clear();
    r.down(Button::Shift);
    r.ev(EventType::KnobDelta, 7, 60);
    CHECK(r.sink.got.empty());
    r.ev(EventType::KnobDelta, 7, 60);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->layer == kNone && c->slot == 0 && c->i == 0);
    r.up(Button::Shift);
}

// ---------------------------------------------------------------------------- pads
static void test_clip_pads() {
    std::printf("CLIPS pads trigger clips on the focus, paging on MASTER < >\n");
    Rig r(40);
    ready(r);
    hit(r, 1, 127);
    const Command* c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == 0 && near(c->f[0], 1.0f) && !(c->flags & kRelease));
    const Command* o = r.sink.last(CommandType::LayerOpacity);
    CHECK(o && !(o->flags & kRelative) && near(o->f[0], 1.0f));
    lift(r, 1);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && (c->flags & kRelease) && c->i == 0);
    hit(r, 16, 1);  // softest hit = the floor opacity
    o = r.sink.last(CommandType::LayerOpacity);
    CHECK(r.sink.last(CommandType::LayerTrigger)->i == 15 && o && near(o->f[0], 0.2f));
    lift(r, 16);
    r.press(Button::MasterRight);
    hit(r, 3);
    CHECK(r.sink.last(CommandType::LayerTrigger)->i == 18);
    lift(r, 3);
    r.press(Button::MasterRight);
    r.press(Button::MasterRight);  // clamped at page 3
    r.sink.clear();
    hit(r, 9);  // clip 40 does not exist
    lift(r, 9);
    CHECK(r.sink.got.empty());
    r.press(Button::MasterLeft);
    r.press(Button::MasterLeft);
    r.press(Button::MasterLeft);  // clamped at page 1
    hit(r, 2);
    CHECK(r.sink.last(CommandType::LayerTrigger)->i == 1);
    lift(r, 2);
    // PLAY focus: PROGRAM layer, the opacity is left alone.
    r.press(Button::GroupG);
    r.sink.clear();
    hit(r, 5, 100);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 0 && c->layer == 2 && c->i == 4);
    CHECK(r.sink.count(CommandType::LayerOpacity) == 0);
    lift(r, 5);

    Rig e(0);  // empty catalog: pads do nothing
    ready(e);
    hit(e, 1);
    CHECK(e.sink.got.empty());
}

static void test_pad_targets() {
    std::printf("pad targets: hold pad + group / ALL / SOLO / SELECT, SOLO + pad = HIT\n");
    Rig r(20);
    ready(r);
    hit(r, 6);
    r.sink.clear();
    r.press(Button::GroupB);  // pad 6 -> LAYER 2, the focus stays
    CHECK(r.sink.got.empty());
    CHECK(r.m.pad_target(6) == static_cast<uint8_t>(PadTarget::Layer2));
    CHECK(flash_is(r, "PAD 6 > LAYER 2"));
    lift(r, 6);
    CHECK(r.sink.got.empty());  // a pad used to set a target does not release a clip
    hit(r, 6);
    const Command* c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == 1 && c->layer == 1 && c->i == 5);
    r.press(Button::All);  // held pad + ALL
    lift(r, 6);
    r.sink.clear();
    hit(r, 6);
    CHECK(r.sink.count(CommandType::LayerTrigger) == 4);
    lift(r, 6);
    // A target names a layer of the focus side: PLAY focus = the PROGRAM deck.
    r.press(Button::GroupE);
    r.sink.clear();
    hit(r, 6);
    CHECK(r.sink.count(CommandType::LayerTrigger) == 4 && r.sink.last(CommandType::LayerTrigger)->deck == 0);
    r.press(Button::Select);  // back to SELECTED
    lift(r, 6);
    CHECK(r.m.pad_target(6) == static_cast<uint8_t>(PadTarget::Selected));
    // SOLO + pad = HIT from either focus.
    r.sink.clear();
    r.down(Button::Solo);
    hit(r, 3, 64);
    r.up(Button::Solo);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == kHitDeck && c->layer == 0 && c->i == 2 && c->f[0] > 0.4f && c->f[0] < 0.6f);
    lift(r, 3);
    c = r.sink.last(CommandType::LayerTrigger);
    CHECK(c && c->deck == kHitDeck && (c->flags & kRelease));
    // Hold pad + SOLO = the pad's target is HIT.
    hit(r, 4);
    r.press(Button::Solo);
    lift(r, 4);
    CHECK(r.m.pad_target(4) == static_cast<uint8_t>(PadTarget::Hit));
    // SHIFT + clip pad in PREP = load and TAKE. (FREE has no MK2 gesture in 0.1.)
    r.press(Button::GroupA);
    r.apply_focus();
    r.sink.clear();
    r.down(Button::Shift);
    hit(r, 8);
    r.up(Button::Shift);
    lift(r, 8);
    CHECK(r.sink.count(CommandType::LayerTrigger) >= 1 && r.sink.count(CommandType::Take) == 1);
}

static void test_pressure() {
    std::printf("pressure: PRESS once per tick, only on change\n");
    Rig r(20, 3);
    ready(r);
    r.layer(1, 0).fx[0].effect = 2;
    hit(r, 1);
    r.ev(EventType::PadPressure, 1, 2000);
    r.ev(EventType::PadPressure, 1, 2048);
    CHECK(r.sink.count(CommandType::FxPress) == 0);  // nothing before the tick
    r.tick();
    const Command* c = r.sink.last(CommandType::FxPress);
    CHECK(r.sink.count(CommandType::FxPress) == 1 && c && c->deck == 1 && c->layer == 0 && c->slot == 0 && near(c->f[0], 2048 / 4095.0f));
    r.ev(EventType::PadPressure, 1, 2060);  // below the deadband
    r.tick();
    CHECK(r.sink.count(CommandType::FxPress) == 1);
    r.ev(EventType::PadPressure, 1, 3000);
    r.tick();
    CHECK(r.sink.count(CommandType::FxPress) == 2);
    lift(r, 1);
    c = r.sink.last(CommandType::FxPress);
    CHECK(r.sink.count(CommandType::FxPress) == 3 && c && near(c->f[0], 0.0f));
    // No effect on the layer: no PRESS.
    r.layer(1, 0).fx[0].effect = -1;
    r.sink.clear();
    hit(r, 2);
    r.ev(EventType::PadPressure, 2, 3000);
    r.tick();
    lift(r, 2);
    CHECK(r.sink.count(CommandType::FxPress) == 0);
}

static void test_slice() {
    std::printf("SLICE: PATTERN twice, jump, pressure = beat loop, REC / ERASE + pad\n");
    Rig r(20);
    ready(r);
    r.press(Button::Pattern);
    CHECK(r.m.pad_mode() == perform::Mk2Perform::PadMode::Slice);
    hit(r, 3);  // no clip on the layer: nothing
    lift(r, 3);
    CHECK(r.sink.got.empty());
    r.layer(1, 0).clip = 4;
    hit(r, 3);
    const Command* c = r.sink.last(CommandType::LayerCue);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 2 && c->i == static_cast<int32_t>(CueAction::Jump));
    r.ev(EventType::PadPressure, 3, 4000);
    r.tick();
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && c->f[0] > 0.0f && c->f[0] <= 0.25f);
    r.ev(EventType::PadPressure, 3, 600);
    r.tick();
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 0.0f));
    r.ev(EventType::PadPressure, 3, 1500);
    r.tick();
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 1.0f));
    lift(r, 3);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 0.0f));
    r.down(Button::Rec);
    hit(r, 5);
    lift(r, 5);
    r.up(Button::Rec);
    c = r.sink.last(CommandType::LayerCue);
    CHECK(c && c->slot == 4 && c->i == static_cast<int32_t>(CueAction::Set) && c->f[0] < 0.0f);
    CHECK(r.sink.count(CommandType::SceneSave) == 0);  // REC used with a pad: no save on release
    r.down(Button::Erase);
    hit(r, 5);
    lift(r, 5);
    r.up(Button::Erase);
    c = r.sink.last(CommandType::LayerCue);
    CHECK(c && c->i == static_cast<int32_t>(CueAction::Clear));
    CHECK(r.sink.count(CommandType::Undo) == 0);
    // LEDs: a set cue is brighter than an unset one.
    r.tr->layer[1][0].cue[0] = 0.25f;
    r.tick();
    CHECK(r.leds.pad_rgb[0][0] > r.leds.pad_rgb[1][0]);
    r.press(Button::Pattern);
    CHECK(r.m.pad_mode() == perform::Mk2Perform::PadMode::Clips);
}

static void test_looks() {
    std::printf("LOOKS: scenes land where the focus is\n");
    Rig r(20);
    ready(r);
    r.press(Button::Scene);
    CHECK(r.m.pad_mode() == perform::Mk2Perform::PadMode::Looks);
    hit(r, 3);  // empty pad: nothing
    lift(r, 3);
    CHECK(r.sink.got.empty());
    r.show->pad[2].used = 1;
    hit(r, 3);  // PREP: into PREVIEW (deck B), at once
    lift(r, 3);
    const Command* c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 1 && c->i == 2 && c->slot == kNone && near(c->f[0], 0.0f));
    r.down(Button::Shift);  // SHIFT in PREP = live now (cut)
    hit(r, 3);
    lift(r, 3);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 0 && near(c->f[0], 0.0f));
    r.press(Button::GroupE);  // PLAY: live, with the scene's transition
    hit(r, 3);
    lift(r, 3);
    c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 0 && c->f[0] < 0.0f);
    r.down(Button::Shift);  // SHIFT in PLAY = into PREVIEW
    hit(r, 3);
    lift(r, 3);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 1);
    // REC + pad = save the focus deck's look to that pad.
    r.sink.clear();
    r.down(Button::Rec);
    hit(r, 5);
    lift(r, 5);
    r.up(Button::Rec);
    c = r.sink.last(CommandType::SceneSave);
    CHECK(r.sink.count(CommandType::SceneSave) == 1 && c && c->deck == 0 && c->i == 4);
    // MASTER < > = bank (song with the setlist).
    r.press(Button::MasterRight);
    c = r.sink.last(CommandType::ShowSelect);
    CHECK(c && c->param == static_cast<uint8_t>(SelectWhat::Bank) && c->i == 1 && (c->flags & kRelative));
    r.show->use_setlist = 1;
    r.press(Button::MasterLeft);
    c = r.sink.last(CommandType::ShowSelect);
    CHECK(c && c->param == static_cast<uint8_t>(SelectWhat::Song) && c->i == -1);
    // LEDs: on PROGRAM red, on PREVIEW green, in the bank dim, empty dark.
    r.show->pad[0].used = 1, r.show->pad[0].on_deck = 1;  // deck A = PROGRAM
    r.show->pad[1].used = 1, r.show->pad[1].on_deck = 2;  // deck B = PREVIEW
    r.show->pad[2].on_deck = 0;
    r.show->pad[2].thumb_rgb = 0x0000FF;
    r.tick();
    const auto* p = r.leds.pad_rgb;
    CHECK(p[0][0] > 40 && p[0][1] == 0 && p[0][2] == 0);
    CHECK(p[1][1] == 127 && p[1][0] < 20);
    CHECK(p[2][2] == 24 && p[2][0] == 0);
    CHECK(p[3][0] == 0 && p[3][1] == 0 && p[3][2] == 0);
    r.down(Button::Shift);
    r.press(Button::Scene);
    r.up(Button::Shift);
    CHECK(r.m.pad_mode() == perform::Mk2Perform::PadMode::Moods);
    r.sink.clear();
    hit(r, 6);
    lift(r, 6);
    CHECK(r.sink.got.empty() && flash_is(r, "MOOD 6 (STUB)"));
}

static void test_rec_erase_enter() {
    std::printf("REC = save, ERASE = undo / reset, ENTER = GO\n");
    Rig r(20);
    ready(r);
    r.press(Button::Rec);  // PREP: the preview scene to the next free pad
    const Command* c = r.sink.last(CommandType::SceneSave);
    CHECK(c && c->deck == 1 && c->i == -1 && !(c->flags & kVariation));
    r.press(Button::GroupF);  // PLAY: a new variation of the live scene
    r.press(Button::Rec);
    c = r.sink.last(CommandType::SceneSave);
    CHECK(c && c->deck == 0 && (c->flags & kVariation));
    r.sink.clear();
    r.down(Button::Erase);
    CHECK(r.sink.got.empty());  // undo happens on release
    r.up(Button::Erase);
    c = r.sink.last(CommandType::Undo);
    CHECK(c && c->i == 0 && flash_is(r, "UNDO"));
    r.down(Button::Shift);
    r.press(Button::Erase);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::Undo);
    CHECK(c && c->i == 1);
    r.sink.clear();
    r.down(Button::Erase);
    r.press(Button::GroupB);  // reset PREVIEW layer 2
    r.up(Button::Erase);
    c = r.sink.last(CommandType::SceneReset);
    CHECK(c && c->deck == 1 && c->layer == 1);
    CHECK(r.sink.count(CommandType::Undo) == 0 && r.sink.count(CommandType::Focus) == 0);
    r.down(Button::Erase);
    r.press(Button::All);  // reset the focus deck's whole scene (PLAY focus = deck A)
    r.up(Button::Erase);
    c = r.sink.last(CommandType::SceneReset);
    CHECK(c && c->deck == 0 && c->layer == kNone);
    r.sink.clear();
    r.press(Button::Enter);  // GO: next scene live with its transition
    c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 0 && c->i == kSceneNext && c->f[0] < 0.0f);
    r.down(Button::Shift);
    r.press(Button::Enter);  // SHIFT + ENTER: next scene into PREVIEW
    r.up(Button::Shift);
    c = r.sink.last(CommandType::SceneRecall);
    CHECK(c && c->deck == 1 && c->i == kSceneNext && near(c->f[0], 0.0f));
    // The show's message flashes on the screen.
    std::snprintf(r.show->message, sizeof(r.show->message), "3c saved");
    r.show->message_seq = 5;
    r.tick();  // the first message seen is the old one: not shown
    std::snprintf(r.show->message, sizeof(r.show->message), "4b saved");
    r.show->message_seq = 6;
    r.tick();
    CHECK(flash_is(r, "4b SAVED"));
}

static void test_top_buttons() {
    std::printf("Top 1-4 = locks, Top 5-8 = FREEZE / STROBE / BLACK / ASSIST\n");
    Rig r(20, 3);
    ready(r);
    r.press(Button::Top1);
    const Command* c = r.sink.last(CommandType::Locks);
    CHECK(c && c->param == static_cast<uint8_t>(LockOp::Toggle) && c->i == static_cast<int32_t>(kLockClip));
    r.down(Button::Shift);
    r.press(Button::Top4);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::Locks);
    CHECK(c && c->i == static_cast<int32_t>(kLockMood));
    r.show->locks = kLockFx;
    r.tick();
    const mk2::LedSlot t1 = mk2::led_of(Button::Top1), t2 = mk2::led_of(Button::Top2);
    CHECK(r.leds.buttons[t1.index] == 127 && r.leds.buttons[t2.index] == 0);  // lit = free, dark = held

    r.sink.clear();
    r.down(Button::Top7);
    c = r.sink.last(CommandType::Black);
    CHECK(c && c->i == kOn);
    r.up(Button::Top7);
    c = r.sink.last(CommandType::Black);
    CHECK(c && c->i == kOff);
    r.down(Button::Shift);
    r.press(Button::Top7);  // latch
    c = r.sink.last(CommandType::Black);
    CHECK(c && c->i == kOn && r.sink.count(CommandType::Black) == 3);
    r.press(Button::Top7);  // unlatch
    r.up(Button::Shift);
    c = r.sink.last(CommandType::Black);
    CHECK(c && c->i == kOff);

    r.layer(0, 0).clip = 3;  // PROGRAM layer 1
    r.layer(0, 2).clip = 4;
    r.sink.clear();
    r.down(Button::Top5);
    CHECK(r.sink.count(CommandType::LayerPlay) == 2 && r.sink.last(CommandType::LayerPlay)->i == static_cast<int32_t>(PlayAction::Pause));
    r.up(Button::Top5);
    CHECK(r.sink.count(CommandType::LayerPlay) == 4 && r.sink.last(CommandType::LayerPlay)->i == static_cast<int32_t>(PlayAction::Play));

    r.sink.clear();
    r.press(Button::Top6);  // no effect called Strobe: nothing
    CHECK(r.sink.got.empty() && flash_is(r, "NO STROBE EFFECT"));
    std::snprintf(r.cat->effect[2].name, kNameLen, "Strobe");
    r.down(Button::Top6);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->slot == 3 && c->i == 2);
    r.up(Button::Top6);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->slot == 3 && c->i == -1);
}

static void test_fx_pads() {
    std::printf("FX PADS: momentary master effects, SHIFT = latch\n");
    Rig r(20, 3);
    ready(r);
    r.press(Button::PadMode);
    CHECK(r.m.pad_mode() == perform::Mk2Perform::PadMode::FxPads);
    hit(r, 2, 127);
    const Command* c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->slot == 2 && c->i == 1);
    c = r.sink.last(CommandType::FxMix);
    CHECK(c && c->slot == 2 && near(c->f[0], 1.0f));
    lift(r, 2);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1);
    r.down(Button::Shift);
    hit(r, 1);
    lift(r, 1);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == 0);  // latched: still on
    r.down(Button::Shift);
    hit(r, 1);
    lift(r, 1);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1);
    r.sink.clear();
    hit(r, 9);  // no effect 8
    lift(r, 9);
    CHECK(r.sink.got.empty());
}

static void test_double_tap_and_wheel() {
    std::printf("double-tap a group, TEMPO / SELECT + wheel, NOTE REPEAT, dice buttons\n");
    Rig r(20);
    ready(r);
    r.press(Button::GroupB);
    r.apply_focus();
    r.press(Button::GroupB);  // within 350 ms: the same layer on the other side
    const Command* c = r.sink.last(CommandType::Focus);
    CHECK(c && c->deck == 0 && c->layer == 1);
    r.apply_focus();
    r.t += 500'000;
    r.press(Button::GroupB);  // later: a single press again
    r.t += 500'000;
    c = r.sink.last(CommandType::Focus);
    CHECK(c && c->deck == 1 && c->layer == 1);
    r.apply_focus();

    r.sink.clear();
    r.down(Button::Tempo);
    r.ev(EventType::EncoderDelta, 0, 2);
    r.up(Button::Tempo);
    c = r.sink.last(CommandType::Clock);
    CHECK(c && c->param == static_cast<uint8_t>(ClockParam::Bpm) && near(c->f[0], 1.0f));
    CHECK(r.sink.count(CommandType::Clock) == 1 && r.sink.count(CommandType::Crossfader) == 0);
    r.press(Button::Tempo);  // alone = tap
    c = r.sink.last(CommandType::Clock);
    CHECK(c && c->param == static_cast<uint8_t>(ClockParam::Tap));

    r.layer(1, 1).clip = 3;
    r.sink.clear();
    r.down(Button::Select);
    r.ev(EventType::EncoderDelta, 0, -2);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->deck == 1 && c->layer == 1 && c->i == 1 && (c->flags & kRelative) && near(c->f[0], -0.1f));
    r.tr->layer[1][1].scratching = 1;
    r.up(Button::Select);
    c = r.sink.last(CommandType::LayerScratch);
    CHECK(c && c->i == 0);
    CHECK(r.sink.count(CommandType::Crossfader) == 0);

    r.layer(0, 0).clip = 1;  // PROGRAM layer 1
    r.sink.clear();
    r.down(Button::NoteRepeat);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && c->deck == 0 && c->layer == 0 && near(c->f[0], 0.25f) && r.sink.count(CommandType::LayerBeatLoop) == 1);
    r.up(Button::NoteRepeat);
    c = r.sink.last(CommandType::LayerBeatLoop);
    CHECK(c && near(c->f[0], 0.0f));

    r.press(Button::TransportLeft);
    c = r.sink.last(CommandType::Dice);
    CHECK(c && c->deck == 1 && c->layer == 1 && c->param == static_cast<uint8_t>(DiceVerb::Back));
    r.press(Button::TransportRight);
    CHECK(r.sink.last(CommandType::Dice)->param == static_cast<uint8_t>(DiceVerb::Forward));
    r.down(Button::Shift);
    r.press(Button::TransportRight);
    r.up(Button::Shift);
    CHECK(r.sink.last(CommandType::Dice)->param == static_cast<uint8_t>(DiceVerb::Mutate));
    r.press(Button::Grid);
    c = r.sink.last(CommandType::Clock);
    CHECK(c && c->param == static_cast<uint8_t>(ClockParam::Grid) && c->i == static_cast<int32_t>(GridDiv::Beat));
}

static void test_pad_leds() {
    std::printf("pad LEDs in CLIPS\n");
    Rig r(20);
    r.cat->clip[3].thumb_rgb = 0x000000;  // black placeholder -> grey
    ready(r);
    r.layer(1, 0).clip = 2;  // PREVIEW (focused) plays clip 2
    r.layer(0, 3).clip = 5;  // PROGRAM plays clip 5
    r.tick();
    const auto* p = r.leds.pad_rgb;
    CHECK(p[2][1] == 127 && p[2][0] < 50);                  // on PREVIEW: green-tinted bright
    CHECK(p[5][0] > 30 && p[5][1] == 0 && p[5][2] == 0);    // on PROGRAM: red
    CHECK(p[0][0] == 16 && p[0][1] == 8 && p[0][2] == 0);   // in the bank: dim own colour
    CHECK(p[3][0] == 16 && p[3][1] == 16 && p[3][2] == 16); // dark placeholder shows grey
    hit(r, 1, 80);
    r.tick();
    CHECK(p[0][0] == 127 && p[0][1] == 127 && p[0][2] == 127);  // finger: white
    lift(r, 1);
    r.press(Button::MasterRight);  // page 2: clips 16-19
    r.tick();
    CHECK(p[3][0] == 16 && p[4][0] == 0 && p[4][1] == 0 && p[4][2] == 0);
    const mk2::LedSlot ml = mk2::led_of(Button::MasterLeft), mr = mk2::led_of(Button::MasterRight);
    CHECK(r.leds.buttons[ml.index] == 127 && r.leds.buttons[mr.index] == 0);
}

static void test_group_leds() {
    std::printf("group LEDs\n");
    Rig r;
    r.tick();
    r.apply_focus();     // PREP layer 1 (deck B)
    r.layer(1, 2).clip = 4;  // PREVIEW layer 3 has a clip
    r.layer(0, 1).clip = 7;  // PROGRAM layer 2 has a clip
    r.tick();
    for (int side = 0; side < 2; ++side) CHECK(r.leds.group_rgb[0][side][1] == 127);
    CHECK(r.group(0)[0] == 0 && r.group(0)[2] == 0);         // A: bright green
    CHECK(r.group(2)[1] == 28 && r.group(2)[0] == 0);        // C: dim green
    CHECK(r.group(1)[0] == 0 && r.group(1)[1] == 0);         // B: empty, off
    CHECK(r.group(5)[0] == 28 && r.group(5)[1] == 0);        // F: dim red
    CHECK(r.group(4)[0] == 0);                               // E: empty, off
    r.press(Button::GroupF);
    r.apply_focus();
    r.tick();
    CHECK(r.group(5)[0] == 127 && r.group(0)[1] == 0);
    // After the decks swap the colours stay with the rows (roles).
    r.live->program_deck = 1;
    r.tick();
    r.apply_focus();
    r.tick();
    CHECK(r.group(5)[0] == 127);            // F still the focus (PROGRAM layer 2 = deck B layer 2)
    CHECK(r.group(2)[1] == 0);              // C = deck A layer 3: empty now
    CHECK(r.group(6)[0] == 28);             // G = deck B layer 3 (clip 4), red
    CHECK(r.group(1)[1] == 28);             // B = deck A layer 2 (clip 7), green
}

static void test_mute_and_panic() {
    std::printf("MUTE, SHIFT + group, SHIFT + MUTE = PANIC\n");
    Rig r(10, 3);
    ready(r);
    r.press(Button::Mute);
    const Command* c = r.sink.last(CommandType::LayerMute);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == kToggle);
    r.sink.clear();
    r.down(Button::Shift);
    r.press(Button::GroupH);
    r.press(Button::Mute);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::LayerMute);
    CHECK(c && c->deck == 0 && c->layer == 3);
    CHECK(r.sink.count(CommandType::Focus) == 0);
    c = r.sink.last(CommandType::Black);
    CHECK(c && c->i == kOn);
}

static void test_reset_and_seq() {
    std::printf("reset() and seq numbers\n");
    Rig r;
    ready(r);
    r.down(Button::Shift);
    hit(r, 4, 90);
    r.m.reset();  // MK2 reconnected: Shift and the held pad are forgotten
    r.tick();
    const mk2::LedSlot sh = mk2::led_of(Button::Shift);
    CHECK(r.leds.transport[sh.index] == 0);
    CHECK(!(r.leds.pad_rgb[3][0] == 127 && r.leds.pad_rgb[3][1] == 127));
    r.sink.clear();
    r.ev(EventType::KnobDelta, 3, 100);  // not fine any more
    const Command* c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && near(c->f[0], 0.2f));
    r.down(Button::Shift);
    r.ev(EventType::Connected, 0);  // a Connected event resets too
    r.ev(EventType::KnobDelta, 3, 100);
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && near(c->f[0], 0.2f));

    Rig s;  // seq runs 1, 2, 3 ... over every command
    s.tick();
    s.press(Button::Play);
    s.press(Button::Restart);
    s.press(Button::Enter);
    bool ok = !s.sink.got.empty();
    for (size_t i = 0; i < s.sink.got.size(); ++i) ok = ok && s.sink.got[i].seq == i + 1;
    CHECK(ok);
}

// ---------------------------------------------------------------------------- screens
static int lit(const uint8_t* screen, int x0, int y0, int w, int h) {
    int n = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) n += (screen[y * 32 + (x >> 3)] >> (7 - (x & 7))) & 1;
    return n;
}

static void test_screens() {
    std::printf("screens: left PREVIEW, right PROGRAM, focus header inverted\n");
    Rig r;
    ready(r);
    r.layer(1, 0).clip = 3;
    r.layer(1, 0).opacity = 0.5f;
    r.tick();
    CHECK(lit(r.screens[0], 0, 0, 256, 1) == 256);  // PREP focus: the left header is inverted
    CHECK(lit(r.screens[1], 0, 0, 256, 1) == 0);
    CHECK(lit(r.screens[1], 0, 8, 256, 1) == 256);  // underline instead
    CHECK(lit(r.screens[0], 0, 10, 48, 9) > 0);
    const int bar_px = lit(r.screens[0], 3, 21, 40, 3);
    CHECK(bar_px > 0 && bar_px < 40 * 3);
    CHECK(lit(r.screens[1], 3, 21, 40, 3) == 0);
    CHECK(lit(r.screens[0], 8, 62, 5, 1) == 5);
    CHECK(lit(r.screens[1], 238, 62, 5, 1) == 5);
    r.live->crossfader = 0.5f;  // TAKE bar on the right fills with the crossfader
    r.tick();
    const int half = lit(r.screens[1], 31, 28, 128, 5);
    r.live->crossfader = 0.0f;
    r.tick();
    const int none = lit(r.screens[1], 31, 28, 128, 5);
    CHECK(half > none + 200);
    r.press(Button::GroupE);  // PLAY focus flips the inversion
    r.apply_focus();
    r.tick();
    CHECK(lit(r.screens[0], 0, 0, 256, 1) == 0);
    CHECK(lit(r.screens[1], 0, 0, 256, 1) == 256);
    r.ev(EventType::KnobDelta, 5, 10);  // a turned knob inverts its column for a moment
    r.tick();
    const int hot = lit(r.screens[1], 90, 36, 20, 20);
    for (int i = 0; i < 60; ++i) r.tick();
    const int cold = lit(r.screens[1], 90, 36, 20, 20);
    CHECK(hot > cold + 200);
    // A flash message inverts the left row 3; it goes away after 1.5 s.
    r.press(Button::Erase);
    r.tick();
    const int flash = lit(r.screens[0], 0, 26, 256, 9);
    for (int i = 0; i < 100; ++i) r.tick();
    CHECK(flash > lit(r.screens[0], 0, 26, 256, 9) + 500);
}


static void test_browse() {
    std::printf("BROWSE: the catalog on the screens, wheel / knob 1 scroll, push / ENTER load\n");
    Rig r(40, 3);
    ready(r);
    r.layer(1, 0).clip = 5;  // the focused layer (PREVIEW B1) plays clip 5
    r.press(Button::Browse);
    CHECK(r.m.browsing() && r.m.browse_selected() == 5);
    r.ev(EventType::EncoderDelta, 0, 3);  // the wheel scrolls; nothing is crossfaded
    CHECK(r.m.browse_selected() == 8 && r.sink.count(CommandType::Crossfader) == 0);
    r.ev(EventType::EncoderDelta, 0, -100);
    CHECK(r.m.browse_selected() == 0);
    r.ev(EventType::EncoderDelta, 0, 500);
    CHECK(r.m.browse_selected() == 39);  // clamped at the end
    r.ev(EventType::KnobDelta, 0, -100);  // knob 1: one step per 50 units
    CHECK(r.m.browse_selected() == 37);
    r.sink.clear();
    r.press(Button::EncoderPush);  // push = load into the focused layer, no TAKE
    const Command* c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == 37 && r.sink.count(CommandType::Take) == 0);
    CHECK(flash_is(r, "LOADED > B1"));
    r.press(Button::GroupF);  // a group still sets the target: PROGRAM layer 2
    r.press(Button::Enter);   // ENTER loads too (no GO while browsing)
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->deck == 0 && c->layer == 1 && r.sink.count(CommandType::SceneRecall) == 0);
    r.press(Button::Play);    // PLAY is still TAKE
    CHECK(r.sink.count(CommandType::Take) == 1);
    // Screens: the list on the left, the detail on the right; the BROWSE LED is lit.
    r.m.set_library(12);
    r.tick();
    const mk2::LedSlot br = mk2::led_of(Button::Browse);
    CHECK(br.kind == 1 && r.leds.buttons[br.index] == 127);
    CHECK(lit(r.screens[0], 0, 10, 256, 54) > 100 && lit(r.screens[1], 0, 10, 256, 54) > 100);
    r.press(Button::Browse);  // back to PERFORM: the wheel is the T-bar again
    CHECK(!r.m.browsing());
    r.ev(EventType::EncoderDelta, 0, 1);
    CHECK(r.sink.count(CommandType::Crossfader) == 1);
    Rig e(0);  // empty catalog: nothing to load
    ready(e);
    e.press(Button::Browse);
    e.press(Button::EncoderPush);
    e.tick();
    CHECK(e.sink.count(CommandType::LayerClip) == 0);
}

// `perform_tests --dump file.pbm` writes a sample of both screens (512x64 PBM) to look at.
static void dump(const char* path) {
    Rig r(40, 3);
    ready(r);
    r.layer(1, 0).clip = 3;
    r.layer(1, 0).opacity = 0.8f;
    r.layer(1, 0).fx[0].effect = 1;
    r.layer(1, 0).fx[0].mix = 0.6f;
    r.layer(1, 2).clip = 12;
    r.layer(1, 2).opacity = 0.4f;
    r.layer(0, 0).clip = 7;
    r.layer(0, 1).clip = 21;
    r.layer(0, 1).mute = 1;
    r.layer(0, 1).opacity = 0.6f;
    r.live->crossfader = 0.35f;
    r.show->deck[1].scene = 2, r.show->deck[1].pad = 2, r.show->deck[1].variation = 'b', r.show->deck[1].edited = 1;
    r.show->deck[0].scene = 0, r.show->deck[0].pad = 0, r.show->deck[0].variation = 'a';
    r.show->macro[0] = 0.7f, r.show->macro[1] = 0.2f, r.show->macro[2] = 0.5f, r.show->macro[3] = 1.0f;
    std::snprintf(r.show->macro_name[0], 12, "INTENSITY");
    std::snprintf(r.show->macro_name[1], 12, "COLOUR");
    std::snprintf(r.show->macro_name[2], 12, "SPACE");
    std::snprintf(r.show->macro_name[3], 12, "MASTER FX");
    r.ev(EventType::KnobDelta, 2, 10);
    r.tick();
    std::ofstream f(path, std::ios::binary);
    f << "P4\n512 64\n";
    for (int y = 0; y < 64; ++y)
        for (int side = 0; side < 2; ++side) f.write(reinterpret_cast<const char*>(r.screens[side] + y * 32), 32);
}

// ENGINE (quit): SHIFT + wheel -> SYSTEM menu; QUIT HIMMEL = push and hold the wheel 2 s.
static void test_system_quit() {
    std::printf("SYSTEM menu: QUIT HIMMEL needs a 2 s hold of the wheel push\n");
    Rig r(40, 3);
    ready(r);
    // Outside the menu the push is a TAKE, never a quit.
    r.down(Button::EncoderPush);
    for (int i = 0; i < 200; ++i) r.tick();
    r.up(Button::EncoderPush);
    CHECK(r.sink.count(CommandType::SystemQuit) == 0 && r.sink.count(CommandType::Take) == 1);
    r.sink.clear();
    // Open the menu; the wheel in it never crossfades.
    r.down(Button::Shift);
    r.ev(EventType::EncoderDelta, 0, 1);
    r.up(Button::Shift);
    CHECK(r.m.system_menu());
    r.ev(EventType::EncoderDelta, 0, 3);
    r.tick();
    CHECK(r.sink.got.empty());
    // Let go after ~1 s: cancelled, nothing sent.
    r.down(Button::EncoderPush);
    for (int i = 0; i < 60; ++i) r.tick();  // 0.96 s
    r.up(Button::EncoderPush);
    r.tick();
    CHECK(r.sink.got.empty() && flash_is(r, "QUIT CANCELLED") && r.m.system_menu());
    // Hold: nothing before 2 s, exactly one confirmed SystemQuit after.
    r.down(Button::EncoderPush);
    const uint64_t t0 = r.t;
    while (r.t < t0 + perform::Mk2Perform::kQuitHoldUs - 20'000) r.tick();
    CHECK(r.sink.count(CommandType::SystemQuit) == 0);
    for (int i = 0; i < 5; ++i) r.tick();
    const Command* q = r.sink.last(CommandType::SystemQuit);
    CHECK(q && q->f[0] == 1.0f && r.sink.count(CommandType::SystemQuit) == 1 && r.m.quit_sent());
    for (int i = 0; i < 100; ++i) r.tick();
    r.up(Button::EncoderPush);
    CHECK(r.sink.count(CommandType::SystemQuit) == 1 && r.sink.count(CommandType::Take) == 0);
    // Any other button closes the menu and does its job.
    Rig r2(40, 3);
    ready(r2);
    r2.down(Button::Shift);
    r2.ev(EventType::EncoderDelta, 0, -1);
    r2.up(Button::Shift);
    r2.down(Button::EncoderPush);
    r2.tick();
    r2.press(Button::Play);  // closes the menu, drops the hold, TAKEs
    CHECK(!r2.m.system_menu() && r2.sink.count(CommandType::Take) == 1);
    for (int i = 0; i < 200; ++i) r2.tick();
    r2.up(Button::EncoderPush);
    CHECK(r2.sink.count(CommandType::SystemQuit) == 0);
    // A reconnect closes the menu.
    r2.down(Button::Shift);
    r2.ev(EventType::EncoderDelta, 0, 1);
    r2.up(Button::Shift);
    r2.m.reset();
    CHECK(!r2.m.system_menu());
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--dump-quit") == 0) {  // the countdown screen, for a look
        Rig r(40, 3);
        ready(r);
        r.down(Button::Shift);
        r.ev(EventType::EncoderDelta, 0, 1);
        r.up(Button::Shift);
        r.down(Button::EncoderPush);
        for (int i = 0; i < 40; ++i) r.tick();
        std::ofstream f(argv[2], std::ios::binary);
        f << "P4\n512 64\n";
        for (int y = 0; y < 64; ++y)
            for (int side = 0; side < 2; ++side) f.write(reinterpret_cast<const char*>(r.screens[side] + y * 32), 32);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--dump-browse") == 0) {
        Rig r(40, 3);
        ready(r);
        r.layer(1, 0).clip = 5;
        r.layer(0, 0).clip = 6;
        r.press(Button::Browse);
        r.m.set_library(12);
        r.tick();
        std::ofstream f(argv[2], std::ios::binary);
        f << "P4\n512 64\n";
        for (int y = 0; y < 64; ++y)
            for (int side = 0; side < 2; ++side) f.write(reinterpret_cast<const char*>(r.screens[side] + y * 32), 32);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) {
        dump(argv[2]);
        return 0;
    }
    test_initial_focus();
    test_groups_focus();
    test_focus_after_take();
    test_focus_from_gui();
    test_new_epoch();
    test_wheel_and_take();
    test_play_page();
    test_other_pages();
    test_macros();
    test_clip_pads();
    test_pad_targets();
    test_pressure();
    test_slice();
    test_looks();
    test_rec_erase_enter();
    test_top_buttons();
    test_fx_pads();
    test_double_tap_and_wheel();
    test_pad_leds();
    test_group_leds();
    test_mute_and_panic();
    test_reset_and_seq();
    test_screens();
    test_browse();
    test_system_quit();
    std::printf("\nperform_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
