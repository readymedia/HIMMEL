// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the MK2 Perform slice mapping (src/perform/), no hardware:
// a fake CommandSink records what the mapper sends, fake LiveState / Catalog
// stand in for the engine. Mapping table: docs/agents/handoff/control.md, "Slice mapping (2026-11)".
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
    r.ev(EventType::KnobDelta, 0, 100);  // knob 1 now acts on deck A layer 2
    const Command* o = r.sink.last(CommandType::LayerOpacity);
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
    r.ev(EventType::KnobDelta, 0, 50);
    const Command* o = r.sink.last(CommandType::LayerOpacity);
    CHECK(o && o->deck == 0 && o->layer == 3);
    CHECK(r.group(7)[0] == 127);

    // A group press in the same frame (before the engine applies it) is not undone.
    r.sink.clear();
    r.press(Button::GroupC);
    r.ev(EventType::KnobDelta, 0, 50);  // the state still says A/3
    o = r.sink.last(CommandType::LayerOpacity);
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
    // SHIFT + wheel = system menu (no-op in the slice).
    r.sink.clear();
    r.down(Button::Shift);
    r.ev(EventType::EncoderDelta, 0, 5);
    r.up(Button::Shift);
    CHECK(r.sink.count(CommandType::Crossfader) == 0);

    r.sink.clear();
    r.press(Button::Play);
    const Command* t = r.sink.last(CommandType::Take);
    CHECK(t && t->i == 0);
    r.press(Button::EncoderPush);
    CHECK(r.sink.count(CommandType::Take) == 2);
    r.press(Button::Restart);
    CHECK(r.sink.count(CommandType::Cut) == 1);
}

// ---------------------------------------------------------------------------- knobs
static void test_knobs() {
    std::printf("knobs 1-4 on the focus, 5-8 master FX\n");
    Rig r;
    r.tick();
    r.apply_focus();  // PREP layer 1 = deck B layer 1
    r.sink.clear();
    r.ev(EventType::KnobDelta, 0, 100);
    const Command* c = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && c->deck == 1 && c->layer == 0 && (c->flags & kRelative) && near(c->f[0], 0.1f));
    r.ev(EventType::KnobDelta, 1, -250);
    c = r.sink.last(CommandType::LayerSpeed);
    CHECK(c && c->deck == 1 && (c->flags & kRelative) && near(c->f[0], -0.5f));
    // SHIFT = fine.
    r.down(Button::Shift);
    r.ev(EventType::KnobDelta, 0, 100);
    c = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && near(c->f[0], 0.01f));
    r.up(Button::Shift);

    // Knobs 3-4 need an effect in slot 1.
    r.sink.clear();
    r.ev(EventType::KnobDelta, 2, 100);
    r.ev(EventType::KnobDelta, 3, 100);
    CHECK(r.sink.got.empty());
    r.layer(1, 0).fx[0].effect = 1;
    r.ev(EventType::KnobDelta, 2, 100);
    c = r.sink.last(CommandType::FxParam);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && c->param == 0 && near(c->f[0], 0.1f));
    r.ev(EventType::KnobDelta, 3, -100);
    c = r.sink.last(CommandType::FxParam);
    CHECK(c && c->param == 1 && near(c->f[0], -0.1f));

    // Knobs 5-8: master FX slot 1 params 1-4 (only the ones the effect has).
    r.sink.clear();
    for (int k = 4; k < 8; ++k) r.ev(EventType::KnobDelta, k, 100);
    CHECK(r.sink.got.empty());  // no master effect
    r.live->master_fx[0].effect = 0;
    for (int k = 4; k < 8; ++k) r.ev(EventType::KnobDelta, k, 100);
    CHECK(r.sink.count(CommandType::FxParam) == 2);  // the effect has 2 params
    c = r.sink.got.empty() ? nullptr : &r.sink.got[0];
    CHECK(c && c->deck == kMasterDeck && c->layer == kNone && c->slot == 0 && c->param == 0);
    c = r.sink.last(CommandType::FxParam);
    CHECK(c && c->param == 1);
    // SHIFT + knob 8 = choose the master effect, one step per 100 units.
    r.sink.clear();
    r.down(Button::Shift);
    r.ev(EventType::KnobDelta, 7, 60);
    CHECK(r.sink.got.empty());
    r.ev(EventType::KnobDelta, 7, 60);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == kMasterDeck && c->layer == kNone && c->slot == 0 && c->i == 1);
    r.ev(EventType::KnobDelta, 7, -100);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1);  // from effect 0 back to empty
    r.up(Button::Shift);
}

// ---------------------------------------------------------------------------- pads
static void test_pads() {
    std::printf("pads load clips into the focused layer, paging\n");
    Rig r(40);  // 3 pages: 16 + 16 + 8
    r.tick();
    r.apply_focus();
    r.sink.clear();
    r.ev(EventType::PadHit, 1, 127);
    const Command* c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == 0);
    const Command* o = r.sink.last(CommandType::LayerOpacity);
    CHECK(o && !(o->flags & kRelative) && near(o->f[0], 1.0f));
    r.ev(EventType::PadRelease, 1);
    r.ev(EventType::PadHit, 16, 1);  // softest hit = the floor opacity
    c = r.sink.last(CommandType::LayerClip);
    o = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && c->i == 15);
    CHECK(o && near(o->f[0], 0.2f));
    r.ev(EventType::PadRelease, 16);

    r.press(Button::TransportRight);
    r.ev(EventType::PadHit, 3, 64);
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->i == 18);
    r.ev(EventType::PadRelease, 3);
    r.press(Button::TransportRight);
    r.press(Button::TransportRight);  // clamped at page 3
    r.sink.clear();
    r.ev(EventType::PadHit, 8, 64);
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->i == 39);
    r.ev(EventType::PadRelease, 8);
    r.ev(EventType::PadHit, 9, 64);  // clip 40 does not exist
    r.ev(EventType::PadRelease, 9);
    CHECK(r.sink.count(CommandType::LayerClip) == 1);
    r.press(Button::TransportLeft);
    r.press(Button::TransportLeft);
    r.press(Button::TransportLeft);  // clamped at page 1
    r.sink.clear();
    r.ev(EventType::PadHit, 2, 64);
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->i == 1);
    r.ev(EventType::PadRelease, 2);

    // Pads follow the focus: PLAY layer 3 = deck A layer 3.
    r.press(Button::GroupG);
    r.sink.clear();
    r.ev(EventType::PadHit, 5, 100);
    c = r.sink.last(CommandType::LayerClip);
    CHECK(c && c->deck == 0 && c->layer == 2 && c->i == 4);

    Rig e(0);  // empty catalog: pads do nothing
    e.tick();
    e.sink.clear();
    e.ev(EventType::PadHit, 1, 100);
    CHECK(e.sink.got.empty());
}

static void test_pad_leds() {
    std::printf("pad LEDs\n");
    Rig r(20);
    r.cat->clip[2].thumb_rgb = 0x00FF00;  // green
    r.cat->clip[3].thumb_rgb = 0x000000;  // black placeholder -> grey
    r.tick();
    r.apply_focus();  // deck B layer 1
    r.layer(1, 0).clip = 2;   // focused layer plays clip 2
    r.layer(0, 3).clip = 5;   // another layer plays clip 5
    r.tick();
    const auto* p = r.leds.pad_rgb;
    CHECK(p[2][0] == 0 && p[2][1] == 127 && p[2][2] == 0);  // focused clip: full, own colour
    CHECK(p[5][0] == 56 && p[5][1] == 28 && p[5][2] == 0);  // on a deck: medium orange
    CHECK(p[0][0] == 16 && p[0][1] == 8 && p[0][2] == 0);   // in the bank: dim
    CHECK(p[3][0] == 16 && p[3][1] == 16 && p[3][2] == 16); // dark placeholder shows grey
    CHECK(p[15][0] == 16);
    r.ev(EventType::PadHit, 1, 80);
    r.tick();
    CHECK(p[0][0] == 127 && p[0][1] == 127 && p[0][2] == 127);  // finger: white
    r.ev(EventType::PadRelease, 1);
    r.press(Button::TransportRight);  // page 2: clips 16-19
    r.tick();
    CHECK(p[3][0] == 16 && p[4][0] == 0 && p[4][1] == 0 && p[4][2] == 0);  // pads 5-16 dark
    const mk2::LedSlot tl = mk2::led_of(Button::TransportLeft), tr = mk2::led_of(Button::TransportRight);
    CHECK(r.leds.transport[tl.index] == 127 && r.leds.transport[tr.index] == 0);
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

// ---------------------------------------------------------------------------- FX / mute / shift
static void test_fx_buttons() {
    std::printf("PAD MODE cycles FX slot 1, NAVIGATE bypasses, MUTE mutes\n");
    Rig r(10, 3);
    r.tick();
    r.apply_focus();  // deck B layer 1
    r.sink.clear();
    r.press(Button::PadMode);
    const Command* c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && c->i == 0);
    r.layer(1, 0).fx[0].effect = 2;
    r.press(Button::PadMode);  // last effect -> empty
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == -1);
    r.down(Button::Shift);
    r.press(Button::PadMode);  // SHIFT = previous: 2 -> 1
    r.up(Button::Shift);
    c = r.sink.last(CommandType::FxSet);
    CHECK(c && c->i == 1);
    r.press(Button::Navigate);
    c = r.sink.last(CommandType::FxBypass);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->slot == 0 && c->i == kToggle);
    r.layer(1, 0).fx[0].effect = -1;  // no effect: nothing to bypass
    r.sink.clear();
    r.press(Button::Navigate);
    CHECK(r.sink.got.empty());

    r.press(Button::Mute);
    c = r.sink.last(CommandType::LayerMute);
    CHECK(c && c->deck == 1 && c->layer == 0 && c->i == kToggle);
    // SHIFT + group = mute that layer, focus unchanged.
    r.sink.clear();
    r.down(Button::Shift);
    r.press(Button::GroupH);
    r.up(Button::Shift);
    c = r.sink.last(CommandType::LayerMute);
    CHECK(c && c->deck == 0 && c->layer == 3);
    CHECK(r.sink.count(CommandType::Focus) == 0);

    Rig none(10, 0);  // no effects in the catalog: PAD MODE does nothing
    none.tick();
    none.sink.clear();
    none.press(Button::PadMode);
    CHECK(none.sink.got.empty());

    // Button LEDs.
    const mk2::LedSlot pm = mk2::led_of(Button::PadMode), nv = mk2::led_of(Button::Navigate),
                       mu = mk2::led_of(Button::Mute), sh = mk2::led_of(Button::Shift),
                       pl = mk2::led_of(Button::Play);
    r.layer(1, 0).fx[0].effect = 1;
    r.layer(1, 0).fx[0].bypass = 1;
    r.layer(1, 0).mute = 1;
    r.down(Button::Shift);
    r.live->take_active = 1;
    r.tick();
    CHECK(pm.kind == 1 && r.leds.buttons[pm.index] == 127);
    CHECK(nv.kind == 1 && r.leds.buttons[nv.index] == 127);
    CHECK(mu.kind == 1 && r.leds.buttons[mu.index] == 127);
    CHECK(sh.kind == 2 && r.leds.transport[sh.index] == 127);
    CHECK(pl.kind == 2 && r.leds.transport[pl.index] == 127);
    r.up(Button::Shift);
    r.layer(1, 0).fx[0].bypass = 0;
    r.live->take_active = 0;
    r.tick();
    CHECK(r.leds.buttons[nv.index] == 12);
    CHECK(r.leds.transport[sh.index] == 0);
    CHECK(r.leds.transport[pl.index] == 28);
}

static void test_reset_and_seq() {
    std::printf("reset() and seq numbers\n");
    Rig r;
    r.tick();
    r.apply_focus();
    r.down(Button::Shift);
    r.ev(EventType::PadHit, 4, 90);
    r.m.reset();  // MK2 reconnected: Shift and the held pad are forgotten
    r.tick();
    const mk2::LedSlot sh = mk2::led_of(Button::Shift);
    CHECK(r.leds.transport[sh.index] == 0);
    CHECK(!(r.leds.pad_rgb[3][0] == 127 && r.leds.pad_rgb[3][1] == 127));
    r.sink.clear();
    r.ev(EventType::KnobDelta, 0, 100);  // not fine any more
    const Command* c = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && near(c->f[0], 0.1f));
    // A Connected event resets too.
    r.down(Button::Shift);
    r.ev(EventType::Connected, 0);
    r.ev(EventType::KnobDelta, 0, 100);
    c = r.sink.last(CommandType::LayerOpacity);
    CHECK(c && near(c->f[0], 0.1f));

    // seq runs 1, 2, 3 ... over every command.
    Rig s;
    s.tick();
    s.press(Button::Play);
    s.press(Button::Restart);
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
    r.tick();
    r.apply_focus();
    r.layer(1, 0).clip = 3;
    r.layer(1, 0).opacity = 0.5f;
    r.tick();
    // PREP focus: the left header row 0 is fully lit, the right one is not.
    CHECK(lit(r.screens[0], 0, 0, 256, 1) == 256);
    CHECK(lit(r.screens[1], 0, 0, 256, 1) == 0);
    CHECK(lit(r.screens[1], 0, 8, 256, 1) == 256);  // underline instead
    // Layer 1 of PREVIEW has a clip: its name cell (inverted, focused) and a half bar.
    CHECK(lit(r.screens[0], 0, 10, 48, 9) > 0);
    const int bar_px = lit(r.screens[0], 3, 21, 40, 3);
    CHECK(bar_px > 0 && bar_px < 40 * 3);
    // Empty layer bars on PROGRAM: only the outline.
    CHECK(lit(r.screens[1], 3, 21, 40, 3) == 0);
    // Knob ticks over the physical knobs at the bottom.
    CHECK(lit(r.screens[0], 8, 62, 5, 1) == 5);
    CHECK(lit(r.screens[1], 238, 62, 5, 1) == 5);
    // TAKE bar on the right fills with the crossfader.
    r.live->crossfader = 0.5f;
    r.tick();
    const int half = lit(r.screens[1], 31, 28, 188, 5);
    r.live->crossfader = 0.0f;
    r.tick();
    const int none = lit(r.screens[1], 31, 28, 188, 5);
    CHECK(half > none + 300);
    // PLAY focus flips the inversion.
    r.press(Button::GroupE);
    r.apply_focus();
    r.tick();
    CHECK(lit(r.screens[0], 0, 0, 256, 1) == 0);
    CHECK(lit(r.screens[1], 0, 0, 256, 1) == 256);
    // A turned knob inverts its column for a moment, then not.
    r.ev(EventType::KnobDelta, 5, 10);
    r.tick();
    const int hot = lit(r.screens[1], 90, 36, 20, 20);
    for (int i = 0; i < 60; ++i) r.tick();
    const int cold = lit(r.screens[1], 90, 36, 20, 20);
    CHECK(hot > cold + 200);
}

// `perform_tests --dump file.pbm` writes a sample of both screens (512x64 PBM) to look at.
static void dump(const char* path) {
    Rig r(40, 3);
    r.tick();
    r.apply_focus();
    r.layer(1, 0).clip = 3;
    r.layer(1, 0).opacity = 0.8f;
    r.layer(1, 0).fx[0].effect = 1;
    r.layer(1, 0).fx[0].param[0] = 0.3f;
    r.layer(1, 0).fx[0].param[1] = 0.7f;
    r.layer(1, 2).clip = 12;
    r.layer(1, 2).opacity = 0.4f;
    r.layer(0, 0).clip = 7;
    r.layer(0, 1).clip = 21;
    r.layer(0, 1).mute = 1;
    r.layer(0, 1).opacity = 0.6f;
    r.live->master_fx[0].effect = 2;
    r.live->master_fx[0].param[0] = 0.5f;
    r.live->crossfader = 0.35f;
    r.ev(EventType::KnobDelta, 2, 10);
    r.tick();
    std::ofstream f(path, std::ios::binary);
    f << "P4\n512 64\n";
    for (int y = 0; y < 64; ++y)
        for (int side = 0; side < 2; ++side) f.write(reinterpret_cast<const char*>(r.screens[side] + y * 32), 32);
}

int main(int argc, char** argv) {
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
    test_knobs();
    test_pads();
    test_pad_leds();
    test_group_leds();
    test_fx_buttons();
    test_reset_and_seq();
    test_screens();
    std::printf("\nperform_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
