// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// DeckModel: validation (refuse vs clamp), apply, TAKE / CUT, rate limit, acks,
// restore by name. No GPU, no Windows.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"

using namespace himmel::protocol;
using himmel::engine::DeckModel;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static std::unique_ptr<Catalog> make_catalog(int clips) {
    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    c->clip_count = static_cast<uint32_t>(clips);
    for (int i = 0; i < clips; ++i) std::snprintf(c->clip[i].name, kNameLen, "clip%02d", i);
    himmel::engine::fx::fill_catalog(*c);
    return c;
}

constexpr int kTool = static_cast<int>(Producer::Tool);
constexpr int kGui = static_cast<int>(Producer::Gui);
static uint32_t g_seq = 0;
static Reject send(DeckModel& m, Command c, uint64_t t = 1000, int producer = kTool) {
    c.seq = ++g_seq;
    return m.submit(producer, c, t);
}

static void test_defaults() {
    std::printf("defaults\n");
    auto cat = make_catalog(4);
    DeckModel m(*cat);
    const LiveState& s = m.live();
    CHECK(s.crossfader == 0.0f && s.program_deck == 0 && !s.take_active && !s.black);
    CHECK(s.take_ms == 2000);
    for (const auto& d : s.deck)
        for (const auto& l : d.layer) CHECK(l.clip == -1 && l.opacity == 1.0f && l.speed == 1.0f && l.playing == 1);
    CHECK(s.master_fx[0].effect == -1 && s.master_fx[3].mix == 1.0f);
}

static void test_layer_commands() {
    std::printf("layer commands\n");
    auto cat = make_catalog(4);
    DeckModel m(*cat);
    CHECK(send(m, make(CommandType::LayerClip, 1, 3, 2)) == Reject::None);
    CHECK(m.live().deck[1].layer[3].clip == 2);
    const uint32_t g0 = m.jump_gen(1, 3);
    CHECK(send(m, make(CommandType::LayerClip, 1, 3, -1)) == Reject::None);
    CHECK(m.live().deck[1].layer[3].clip == -1 && m.jump_gen(1, 3) == g0 + 1);
    CHECK(send(m, make(CommandType::LayerClip, 1, 3, 4)) == Reject::BadId);   // only 4 clips
    CHECK(send(m, make(CommandType::LayerClip, 1, 3, -2)) == Reject::BadId);
    CHECK(send(m, make(CommandType::LayerClip, 2, 0, 0)) == Reject::BadTarget);
    CHECK(send(m, make(CommandType::LayerClip, 0, 4, 0)) == Reject::BadTarget);
    CHECK(send(m, make(CommandType::LayerClip, kNone, 0, 0)) == Reject::BadTarget);

    // Clamp, not refuse.
    CHECK(send(m, make(CommandType::LayerOpacity, 0, 1, 0, 1.3f)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].opacity == 1.0f);
    CHECK(send(m, make(CommandType::LayerOpacity, 0, 1, 0, -0.4f, kRelative)) == Reject::None);
    CHECK(std::fabs(m.live().deck[0].layer[1].opacity - 0.6f) < 1e-6f);
    CHECK(send(m, make(CommandType::LayerOpacity, 0, 1, 0, std::numeric_limits<float>::quiet_NaN())) == Reject::BadValue);
    CHECK(send(m, make(CommandType::LayerOpacity, 0, 1, 0, std::numeric_limits<float>::infinity())) == Reject::BadValue);
    CHECK(std::fabs(m.live().deck[0].layer[1].opacity - 0.6f) < 1e-6f);  // unchanged by refusals

    CHECK(send(m, make(CommandType::LayerSpeed, 0, 0, 0, 9.0f)) == Reject::None);
    CHECK(m.live().deck[0].layer[0].speed == 4.0f);
    CHECK(send(m, make(CommandType::LayerSpeed, 0, 0, 0, -10.0f, kRelative)) == Reject::None);
    CHECK(m.live().deck[0].layer[0].speed == -4.0f);

    CHECK(send(m, make(CommandType::LayerBlend, 0, 0, static_cast<int>(BlendMode::Difference))) == Reject::None);
    CHECK(m.live().deck[0].layer[0].blend == static_cast<uint8_t>(BlendMode::Difference));
    CHECK(send(m, make(CommandType::LayerBlend, 0, 0, static_cast<int>(BlendMode::Count))) == Reject::BadValue);
    CHECK(send(m, make(CommandType::LayerBlend, 0, 0, -1)) == Reject::BadValue);

    CHECK(send(m, make(CommandType::LayerMute, 0, 2, kToggle)) == Reject::None && m.live().deck[0].layer[2].mute == 1);
    CHECK(send(m, make(CommandType::LayerMute, 0, 2, kToggle)) == Reject::None && m.live().deck[0].layer[2].mute == 0);
    CHECK(send(m, make(CommandType::LayerMute, 0, 2, kOn)) == Reject::None && m.live().deck[0].layer[2].mute == 1);
    CHECK(send(m, make(CommandType::LayerMute, 0, 2, 3)) == Reject::BadValue);

    CHECK(send(m, make(CommandType::LayerPlay, 0, 2, static_cast<int>(PlayAction::Pause))) == Reject::None);
    CHECK(m.live().deck[0].layer[2].playing == 0);
    CHECK(send(m, make(CommandType::LayerPlay, 0, 2, static_cast<int>(PlayAction::Toggle))) == Reject::None);
    CHECK(m.live().deck[0].layer[2].playing == 1);
    const uint32_t g1 = m.jump_gen(0, 2);
    CHECK(send(m, make(CommandType::LayerPlay, 0, 2, static_cast<int>(PlayAction::Restart))) == Reject::None);
    CHECK(m.jump_gen(0, 2) == g1 + 1 && m.jump_to(0, 2) == 0.0f);
    CHECK(send(m, make(CommandType::LayerPlay, 0, 2, static_cast<int32_t>(PlayAction::Count))) == Reject::BadValue);

    CHECK(send(m, make(CommandType::LayerJump, 0, 2, 0, 1.7f)) == Reject::None);
    CHECK(m.jump_to(0, 2) == 1.0f && m.jump_gen(0, 2) == g1 + 2);
}

static void test_fx_commands() {
    std::printf("fx commands\n");
    auto cat = make_catalog(2);
    DeckModel m(*cat);
    const int hue = static_cast<int>(himmel::engine::fx::Effect::HueSat);
    const int inv = static_cast<int>(himmel::engine::fx::Effect::Invert);
    // layer, deck and master levels
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 1, 2, kNone, hue)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].effect == hue);
    CHECK(m.live().deck[0].layer[1].fx[2].param[0] == cat->effect[hue].param_default[0]);
    CHECK(m.live().deck[0].layer[1].fx[2].mix == 1.0f);
    CHECK(send(m, make_fx(CommandType::FxSet, 1, kNone, 0, kNone, inv)) == Reject::None);
    CHECK(m.live().deck[1].fx[0].effect == inv);
    CHECK(send(m, make_fx(CommandType::FxSet, kMasterDeck, kNone, 3, kNone, hue)) == Reject::None);
    CHECK(m.live().master_fx[3].effect == hue);
    // bad targets
    CHECK(send(m, make_fx(CommandType::FxSet, kMasterDeck, 0, 0, kNone, hue)) == Reject::BadTarget);
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 0, 4, kNone, hue)) == Reject::BadTarget);
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 0, kNone, kNone, hue)) == Reject::BadTarget);
    CHECK(send(m, make_fx(CommandType::FxSet, 3, kNone, 0, kNone, hue)) == Reject::BadTarget);
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 0, 0, kNone, himmel::engine::fx::kEffectCount)) == Reject::BadId);
    // params
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 2, 1, 0, 2.0f)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].param[1] == 1.0f);
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 2, 1, 0, -0.25f, kRelative)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].param[1] == 0.75f);
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 2, kFxParams, 0, 0.5f)) == Reject::BadTarget);
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 2, 5, 0, 0.5f)) == Reject::BadTarget);  // Hue/Sat has 4
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 3, 0, 0, 0.5f)) == Reject::BadTarget);  // empty slot
    CHECK(send(m, make_fx(CommandType::FxParam, 0, 1, 2, 0, 0, std::nanf(""))) == Reject::BadValue);
    // bypass, mix
    CHECK(send(m, make_fx(CommandType::FxBypass, 0, 1, 2, kNone, kToggle)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].bypass == 1);
    CHECK(send(m, make_fx(CommandType::FxMix, 0, 1, 2, kNone, 0, 0.4f)) == Reject::None);
    CHECK(std::fabs(m.live().deck[0].layer[1].fx[2].mix - 0.4f) < 1e-6f);
    CHECK(send(m, make_fx(CommandType::FxMix, 0, 1, 2, kNone, 0, 5.0f, kRelative)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].mix == 1.0f);
    // set resets params + bypass
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 1, 2, kNone, hue)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].bypass == 0);
    CHECK(m.live().deck[0].layer[1].fx[2].param[1] == cat->effect[hue].param_default[1]);
    CHECK(send(m, make_fx(CommandType::FxSet, 0, 1, 2, kNone, -1)) == Reject::None);
    CHECK(m.live().deck[0].layer[1].fx[2].effect == -1);
}

static void test_take_cut() {
    std::printf("take / cut / crossfader\n");
    auto cat = make_catalog(1);
    DeckModel m(*cat);
    uint64_t t = 1000000;
    CHECK(send(m, make(CommandType::Take), t) == Reject::None);
    CHECK(m.live().take_active == 1 && m.live().take_ms == 2000 && m.live().program_deck == 0);
    m.tick(t + 1000000);
    CHECK(std::fabs(m.live().crossfader - 0.5f) < 1e-3f && std::fabs(m.live().take_progress - 0.5f) < 1e-3f);
    CHECK(m.live().program_deck == 0);
    m.tick(t + 2000000);
    CHECK(!m.live().take_active && m.live().crossfader == 1.0f && m.live().program_deck == 1);
    // back again with a custom time
    t += 3000000;
    CHECK(send(m, make(CommandType::Take, kNone, kNone, 500), t) == Reject::None);
    CHECK(m.live().take_ms == 500);
    m.tick(t + 250000);
    CHECK(std::fabs(m.live().crossfader - 0.5f) < 1e-3f);
    m.tick(t + 600000);
    CHECK(m.live().crossfader == 0.0f && m.live().program_deck == 0);
    // a manual move cancels a running TAKE
    t += 1000000;
    send(m, make(CommandType::Take), t);
    m.tick(t + 500000);
    CHECK(send(m, make(CommandType::Crossfader, kNone, kNone, 0, 0.1f, kRelative), t + 500000) == Reject::None);
    CHECK(!m.live().take_active && std::fabs(m.live().crossfader - 0.35f) < 1e-3f && m.live().program_deck == 0);
    m.tick(t + 3000000);
    CHECK(std::fabs(m.live().crossfader - 0.35f) < 1e-3f);  // nothing travels any more
    // manual fader to the end flips program
    CHECK(send(m, make(CommandType::Crossfader, kNone, kNone, 0, 7.0f), t) == Reject::None);
    CHECK(m.live().crossfader == 1.0f && m.live().program_deck == 1);
    CHECK(send(m, make(CommandType::Crossfader, kNone, kNone, 0, 0.5f), t) == Reject::None);
    CHECK(m.live().program_deck == 1);  // in between: the last end stays PROGRAM
    // CUT
    CHECK(send(m, make(CommandType::Cut), t) == Reject::None);
    CHECK(m.live().crossfader == 0.0f && m.live().program_deck == 0);
    CHECK(send(m, make(CommandType::Cut), t) == Reject::None);
    CHECK(m.live().crossfader == 1.0f && m.live().program_deck == 1);
    // TAKE during TAKE lands it now (GUESS in deck_model.cpp)
    send(m, make(CommandType::Take), t);
    m.tick(t + 100000);
    send(m, make(CommandType::Take), t + 100000);
    CHECK(!m.live().take_active && m.live().crossfader == 0.0f && m.live().program_deck == 0);
    // CUT during TAKE
    send(m, make(CommandType::Take), t);
    m.tick(t + 100000);
    send(m, make(CommandType::Cut), t + 100000);
    CHECK(!m.live().take_active && m.live().crossfader == 1.0f && m.live().program_deck == 1);
    CHECK(send(m, make(CommandType::Take, kNone, kNone, -5), t) == Reject::BadValue);
    CHECK(send(m, make(CommandType::Take, kNone, kNone, 9999999), t) == Reject::None);
    CHECK(m.live().take_ms == himmel::engine::kMaxTakeMs);
    CHECK(send(m, make(CommandType::Crossfader, kNone, kNone, 0, std::nanf("")), t) == Reject::BadValue);
}

static void test_black_focus_hello() {
    std::printf("black / focus / hello\n");
    auto cat = make_catalog(1);
    DeckModel m(*cat);
    CHECK(send(m, make(CommandType::Black, kNone, kNone, kToggle)) == Reject::None && m.live().black == 1);
    CHECK(send(m, make(CommandType::Black, kNone, kNone, kOff)) == Reject::None && m.live().black == 0);
    CHECK(send(m, make(CommandType::Black, kNone, kNone, 7)) == Reject::BadValue);
    CHECK(send(m, make(CommandType::Focus, 1, 2)) == Reject::None);
    CHECK(m.live().focus_deck == 1 && m.live().focus_layer == 2);
    CHECK(send(m, make(CommandType::Focus, 1, kNone)) == Reject::BadTarget);
    CHECK(send(m, make(CommandType::Focus, kNone, kNone)) == Reject::None && m.live().focus_deck == kNone);
    CHECK(send(m, make(CommandType::Hello, kNone, kNone, 1234)) == Reject::None);
    Command bad{};
    bad.type = static_cast<CommandType>(999);
    CHECK(send(m, bad) == Reject::UnknownType);
    bad.type = CommandType::Nop;
    CHECK(send(m, bad) == Reject::UnknownType);
}

static void test_acks_and_rate() {
    std::printf("acks / rejects / rate limit\n");
    auto cat = make_catalog(1);
    DeckModel m(*cat);
    Command c = make(CommandType::LayerOpacity, 0, 0, 0, 0.5f);
    c.seq = 41;
    CHECK(m.submit(kGui, c, 10) == Reject::None);
    CHECK(m.live().acked_seq[kGui] == 41 && m.live().acked_seq[kTool] == 0);
    c.deck = 9;
    c.seq = 42;
    CHECK(m.submit(kGui, c, 10) == Reject::BadTarget);
    CHECK(m.live().acked_seq[kGui] == 41 && m.live().rejected[kGui] == 1);
    CHECK(m.live().last_reject[kGui] == static_cast<uint8_t>(Reject::BadTarget));
    CHECK(m.submit(8, c, 10) == Reject::BadTarget);  // no such producer: nothing counted
    // log once per second
    CHECK(m.should_log_reject(kGui, 2000000));
    CHECK(!m.should_log_reject(kGui, 2500000));
    CHECK(m.should_log_reject(kGui, 3000001));
    // rate limit: 4000 per second per producer, the rest refused; other producers unaffected
    c.deck = 0;
    const uint64_t t = 50000000;
    int ok = 0, limited = 0;
    for (int i = 0; i < 4100; ++i) {
        c.seq = 1000 + i;
        const Reject r = m.submit(kTool, c, t + i);
        ok += r == Reject::None;
        limited += r == Reject::RateLimited;
    }
    CHECK(ok == 4000 && limited == 100);
    CHECK(m.live().acked_seq[kTool] == 1000 + 3999);
    CHECK(m.submit(kGui, c, t + 5000) == Reject::None);
    CHECK(m.submit(kTool, c, t + 1000001) == Reject::None);  // next window
    // alive flags
    m.set_producer_alive(kGui, true);
    CHECK(m.live().producer_alive[kGui] == 1);
    (void)m.take_structure_dirty();
    m.set_position(0, 0, 0.3f);
    CHECK(!m.take_structure_dirty());
    m.set_missing(0, 0, true);
    CHECK(m.take_structure_dirty() && m.live().deck[0].layer[0].missing == 1);
}

static void test_restore() {
    std::printf("restore by name\n");
    auto old_cat = make_catalog(4);
    DeckModel a(*old_cat);
    send(a, make(CommandType::LayerClip, 0, 0, 1));
    send(a, make(CommandType::LayerClip, 1, 2, 3));
    send(a, make(CommandType::LayerClip, 1, 3, 0));
    send(a, make(CommandType::LayerOpacity, 1, 2, 0, 0.25f));
    send(a, make_fx(CommandType::FxSet, kMasterDeck, kNone, 1, kNone, 4));
    send(a, make_fx(CommandType::FxParam, kMasterDeck, kNone, 1, 0, 0, 0.9f));
    send(a, make(CommandType::Crossfader, kNone, kNone, 0, 1.0f));
    send(a, make(CommandType::Black, kNone, kNone, kOn));
    a.set_position(1, 2, 0.4f);

    // New media folder: clip03 is now id 0, clip01 is gone, clip00 is id 2.
    auto new_cat = make_catalog(3);
    std::snprintf(new_cat->clip[0].name, kNameLen, "clip03");
    std::snprintf(new_cat->clip[1].name, kNameLen, "other");
    std::snprintf(new_cat->clip[2].name, kNameLen, "clip00");
    DeckModel b(*new_cat);
    const int lost = b.restore(a.live(), *old_cat);
    CHECK(lost == 1);
    CHECK(b.live().deck[0].layer[0].clip == -1);
    CHECK(b.live().deck[1].layer[2].clip == 0);
    CHECK(b.live().deck[1].layer[3].clip == 2);
    CHECK(b.live().deck[1].layer[2].opacity == 0.25f);
    CHECK(std::fabs(b.jump_to(1, 2) - 0.4f) < 1e-6f);
    CHECK(b.live().master_fx[1].effect == 4 && std::fabs(b.live().master_fx[1].param[0] - 0.9f) < 1e-6f);
    CHECK(b.live().crossfader == 1.0f && b.live().program_deck == 1 && b.live().black == 1);
    // garbage in the old block never escapes
    LiveState junk{};
    std::memset(&junk, 0x7F, sizeof(junk));
    DeckModel c(*new_cat);
    c.restore(junk, *old_cat);
    CHECK(c.live().deck[0].layer[0].clip == -1);
    CHECK(c.live().crossfader >= 0.0f && c.live().crossfader <= 1.0f);
    CHECK(c.live().deck[0].layer[0].blend < static_cast<uint8_t>(BlendMode::Count));
}

int main() {
    test_defaults();
    test_layer_commands();
    test_fx_commands();
    test_take_cut();
    test_black_focus_hello();
    test_acks_and_rate();
    test_restore();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
