// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// OutputModel (M6): canvas layout (span / mirror / presets / blend), layer places,
// black per output, IDENTIFY fades, the output commands through DeckModel (validate,
// ack, refuse), restore, and display matching by identity. No GPU, no Windows.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/output_model.h"

using namespace himmel::protocol;
using himmel::engine::DeckModel;
using himmel::engine::OutputModel;
using himmel::engine::OutputSetup;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static Command cmd(CommandType t, uint8_t slot = kNone, int32_t i = 0, uint8_t param = kNone) {
    Command c = make(t);
    c.slot = slot;
    c.i = i;
    c.param = param;
    return c;
}
static Command place(uint8_t deck, uint8_t layer, Place p, uint8_t out = 0, float x = 0, float y = 0, float w = 0,
                     float h = 0) {
    Command c = make(CommandType::LayerPlace, deck, layer, static_cast<int32_t>(p));
    c.param = out;
    c.f[0] = x, c.f[1] = y, c.f[2] = w, c.f[3] = h;
    return c;
}
static Command config(OutputParam p, int32_t i, uint8_t slot = kNone, float f = 0) {
    Command c = cmd(CommandType::OutputConfig, slot, i, static_cast<uint8_t>(p));
    c.f[0] = f;
    return c;
}

// canvas uv -> clip uv, as the shader does it
static void map(const float x[4], float u, float v, float& cu, float& cv) {
    cu = (u - 0.5f) * x[0] + 0.5f + x[2];
    cv = (v - 0.5f) * x[1] + 0.5f + x[3];
}

static void test_layout() {
    OutputSetup s;
    s.count = 3;
    s.preset = ResolutionPreset::P1080;
    OutputModel m(s);
    CHECK(m.canvas_w() == 5760 && m.canvas_h() == 1080);
    CHECK(m.region(0).x == 0 && m.region(1).x == 1920 && m.region(2).x == 3840);
    CHECK(near(m.region(1).uv[0], 1.0f / 3) && near(m.region(1).uv[2], 1.0f / 3) && near(m.region(1).uv[3], 1.0f));
    CHECK(m.region(3).w == 0);
    CHECK(m.layout_dirty());
    m.clear_layout_dirty();

    // Every preset.
    const struct { ResolutionPreset p; uint32_t w, h; } presets[] = {
        {ResolutionPreset::P720, 1280, 720}, {ResolutionPreset::P1080, 1920, 1080},
        {ResolutionPreset::P1440, 2560, 1440}, {ResolutionPreset::P2160, 3840, 2160}};
    for (const auto& p : presets) {
        m.apply(config(OutputParam::Resolution, static_cast<int32_t>(p.p)), -1, 1);
        CHECK(m.canvas_w() == p.w * 3 && m.canvas_h() == p.h);
        CHECK(m.layout_dirty());
        m.clear_layout_dirty();
    }
    // Custom follows --size.
    m.apply(config(OutputParam::Resolution, static_cast<int32_t>(ResolutionPreset::Custom)), -1, 1);
    m.set_custom_size(640, 360);
    CHECK(m.canvas_w() == 1920 && m.canvas_h() == 360);

    // One output with its own preset: heights differ, the canvas takes the tallest.
    m.apply(config(OutputParam::Resolution, static_cast<int32_t>(ResolutionPreset::P720), 1), -1, 1);
    CHECK(m.canvas_w() == 640 + 1280 + 640 && m.canvas_h() == 720);
    CHECK(m.region(1).h == 720 && m.region(0).h == 360 && near(m.region(0).uv[3], 0.5f));

    // Mirror: every output is the whole canvas.
    m.apply(config(OutputParam::Mode, static_cast<int32_t>(CanvasMode::Mirror)), -1, 1);
    CHECK(m.canvas_w() == 1280 && m.canvas_h() == 720);
    for (int o = 0; o < 3; ++o) CHECK(m.region(o).x == 0 && near(m.region(o).uv[2], 1.0f));
}

static void test_blend() {
    OutputSetup s;
    s.count = 3;
    s.preset = ResolutionPreset::P1080;
    s.blend_px = 240;
    OutputModel m(s);
    CHECK(m.canvas_w() == 3 * 1920 - 2 * 240);  // engine.md §2.0: N x W - (N - 1) x O
    CHECK(m.region(1).x == 1920 - 240 && m.region(2).x == 2 * (1920 - 240));
    CHECK(m.region(0).blend_left == 0 && m.region(0).blend_right == 240);
    CHECK(m.region(1).blend_left == 240 && m.region(1).blend_right == 240);
    CHECK(m.region(2).blend_left == 240 && m.region(2).blend_right == 0);
    // Never more than half the narrowest output.
    m.apply(config(OutputParam::BlendWidth, 1024), -1, 1);
    CHECK(m.region(1).blend_left == 960);
    // Mirror has no blend; one output has none either.
    m.apply(config(OutputParam::Mode, static_cast<int32_t>(CanvasMode::Mirror)), -1, 1);
    CHECK(m.region(0).blend_left == 0 && m.region(0).blend_right == 0);
    OutputSetup one;
    one.blend_px = 200;
    OutputModel m1(one);
    CHECK(m1.canvas_w() == 1920 && m1.region(0).blend_right == 0);
    m.apply(config(OutputParam::BlendGamma, 0, kNone, 9.0f), -1, 1);
    CHECK(near(m.setup().blend_gamma, 3.0f));
}

static void test_places() {
    OutputSetup s;
    s.count = 3;
    OutputModel m(s);  // 5760 x 1080
    float x[4], cu, cv;
    // FILL ALL, 16:9 clip on a 48:9 canvas: cover = crop top / bottom; the middle maps to the middle.
    m.layer_xform(0, 1920, 1080, x);
    map(x, 0.5f, 0.5f, cu, cv);
    CHECK(near(cu, 0.5f) && near(cv, 0.5f));
    map(x, 0.0f, 0.5f, cu, cv);
    CHECK(near(cu, 0.0f));
    map(x, 0.5f, 0.0f, cu, cv);
    CHECK(near(cv, 1.0f / 3));  // only the middle third of the clip's height is seen
    // OUT 2: the clip covers exactly output 2's part.
    m.apply(place(0, 0, Place::Out, 1), 0, 1);
    m.layer_xform(0, 1920, 1080, x);
    map(x, 1.0f / 3, 0.0f, cu, cv);
    CHECK(near(cu, 0.0f) && near(cv, 0.0f));
    map(x, 2.0f / 3, 1.0f, cu, cv);
    CHECK(near(cu, 1.0f) && near(cv, 1.0f));
    map(x, 0.1f, 0.5f, cu, cv);
    CHECK(cu < 0.0f);  // outside its output: transparent
    CHECK(m.place(0).place == static_cast<uint8_t>(Place::Out) && m.place(0).out == 1);
    CHECK(near(m.place(0).rect[0], 1.0f / 3) && near(m.place(0).rect[2], 1.0f / 3));
    // FIT on the wide canvas: the whole clip height, centred; the sides are outside.
    m.apply(place(0, 1, Place::Fit), 1, 1);
    m.layer_xform(1, 1920, 1080, x);
    map(x, 0.5f, 0.0f, cu, cv);
    CHECK(near(cu, 0.5f) && near(cv, 0.0f));
    map(x, 1.0f / 3, 0.5f, cu, cv);
    CHECK(near(cu, 0.0f));
    map(x, 0.1f, 0.5f, cu, cv);
    CHECK(cu < 0.0f);
    // FIT of a tall clip: letterbox the other way.
    m.layer_xform(1, 1080, 1920, x);
    map(x, 0.5f, 0.0f, cu, cv);
    CHECK(near(cv, 0.0f));
    // FREE rect: the clip covers that rect (cover inside it).
    m.apply(place(1, 0, Place::Free, 0, 0.25f, 0.25f, 0.5f, 0.5f), 4, 1);
    m.layer_xform(4, 2880, 540, x);  // the rect is 2880 x 540: same aspect as the clip
    map(x, 0.25f, 0.25f, cu, cv);
    CHECK(near(cu, 0.0f) && near(cv, 0.0f));
    map(x, 0.75f, 0.75f, cu, cv);
    CHECK(near(cu, 1.0f) && near(cv, 1.0f));
    // HIT layer slot 8.
    m.apply(place(kHitDeck, 0, Place::Out, 2), 8, 1);
    CHECK(m.place(8).out == 2);
    // OUT n of an output that went away (count shrinks are not possible at runtime, but
    // a restored place may point past the count): whole canvas.
    OutputModel one;  // 1 output
    LayerPlaceInfo far{static_cast<uint8_t>(Place::Out), 5, {0, 0}, {0, 0, 0, 0}};
    OutputsSection old{};
    one.fill(old, 1);
    old.layer[2] = far;
    one.restore(old);
    float r[4];
    one.place_rect(2, r);
    CHECK(near(r[0], 0) && near(r[2], 1));
    // A place change bumps place_gen; a layout change too.
    const uint32_t g0 = m.place_gen();
    m.apply(place(0, 2, Place::FillAll), 2, 1);
    CHECK(m.place_gen() != g0);
}

static void test_identify_and_black() {
    OutputSetup s;
    s.count = 2;
    OutputModel m(s);
    const uint64_t t = 10'000'000;
    CHECK(!m.identify_on(t));
    m.apply(cmd(CommandType::Identify, kNone, 1), -1, t);
    CHECK(near(m.identify_alpha(t), 0.0f));
    CHECK(near(m.identify_alpha(t + 250000), 0.5f));
    CHECK(near(m.identify_alpha(t + 600000), 1.0f));
    CHECK(near(m.identify_alpha(t + 4'750'000), 0.5f));  // fades out over the last 0.5 s
    CHECK(!m.identify_on(t + 5'000'001));
    CHECK(m.identify_ms_left(t + 1'000'000) == 4000);
    // Held: stays until Identify 0, then fades out.
    m.apply(cmd(CommandType::Identify, kNone, 2), -1, t + 6'000'000);
    CHECK(near(m.identify_alpha(t + 60'000'000), 1.0f));
    CHECK(m.identify_ms_left(t + 60'000'000) == 0xFFFFFFFFu);
    m.apply(cmd(CommandType::Identify, kNone, 0), -1, t + 60'000'000);
    CHECK(near(m.identify_alpha(t + 60'250'000), 0.5f));
    CHECK(!m.identify_on(t + 60'600'000));
    // Re-trigger while showing: no new fade-in.
    m.apply(cmd(CommandType::Identify, kNone, 1), -1, t + 70'000'000);
    m.apply(cmd(CommandType::Identify, kNone, 1), -1, t + 72'000'000);
    CHECK(near(m.identify_alpha(t + 72'000'000), 1.0f));

    m.apply(cmd(CommandType::OutputBlack, 1, kOn), -1, t);
    CHECK(!m.black(0) && m.black(1));
    m.apply(cmd(CommandType::OutputBlack, kNone, kToggle), -1, t);
    CHECK(m.black(0) && !m.black(1));
    m.apply(cmd(CommandType::OutputBlack, kNone, kOff), -1, t);
    CHECK(!m.black(0) && !m.black(1));
    m.apply(config(OutputParam::SimLost, 1, 1), -1, t);
    CHECK(m.sim_lost(1) && !m.sim_lost(0));
    const uint32_t ag = m.adopt_gen(0);
    m.apply(config(OutputParam::Adopt, 1, 0), -1, t);
    CHECK(m.adopt_gen(0) == ag + 1);
    const uint32_t rg = m.refresh_gen();
    m.apply(config(OutputParam::Refresh, static_cast<int32_t>(RefreshRate::Hz50)), -1, t);
    CHECK(m.refresh_gen() == rg + 1 && m.setup().refresh == RefreshRate::Hz50);
    CHECK(near(refresh_hz(RefreshRate::Hz50), 50.0f) && near(refresh_hz(RefreshRate::Hz59_94), 59.94f));
}

static std::unique_ptr<Catalog> make_catalog() {
    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    c->clip_count = 2;
    himmel::engine::fx::fill_catalog(*c);
    return c;
}

static void test_through_deck_model() {
    auto cat = make_catalog();
    auto dm = std::make_unique<DeckModel>(*cat);
    const int tool = static_cast<int>(Producer::Tool);
    uint32_t seq = 0;
    auto send = [&](Command c) {
        c.seq = ++seq;
        return dm->submit(tool, c, 1000 + seq);
    };
    // Without an output model: refused.
    CHECK(send(cmd(CommandType::OutputBlack, kNone, kOn)) == Reject::UnknownType);
    OutputSetup s;
    s.count = 2;
    OutputModel om(s);
    dm->set_outputs(&om);
    CHECK(send(cmd(CommandType::OutputBlack, 1, kOn)) == Reject::None);
    CHECK(om.black(1));
    CHECK(dm->live().acked_seq[tool] == seq);
    CHECK(send(cmd(CommandType::OutputBlack, 2, kOn)) == Reject::BadTarget);  // only 2 outputs
    CHECK(send(cmd(CommandType::OutputBlack, 0, 3)) == Reject::BadValue);
    CHECK(send(cmd(CommandType::Identify, kNone, 3)) == Reject::BadValue);
    CHECK(send(cmd(CommandType::Identify, kNone, 1)) == Reject::None);
    CHECK(send(place(0, 0, Place::Out, 1)) == Reject::None);
    CHECK(send(place(0, 0, Place::Out, 2)) == Reject::BadTarget);
    CHECK(send(place(2, 0, Place::Fit)) == Reject::BadTarget);
    CHECK(send(place(kHitDeck, 0, Place::Fit)) == Reject::None);
    CHECK(om.place(8).place == static_cast<uint8_t>(Place::Fit));
    CHECK(send(place(0, 1, Place::Free, 0, 0, 0, 0, 1)) == Reject::BadValue);  // zero width
    Command nan = place(0, 1, Place::Free, 0, 0, 0, 1, 1);
    nan.f[1] = std::nanf("");
    CHECK(send(nan) == Reject::BadValue);
    CHECK(send(place(0, 1, static_cast<Place>(9))) == Reject::BadValue);
    CHECK(send(config(OutputParam::Resolution, 9)) == Reject::BadValue);
    CHECK(send(config(OutputParam::Resolution, static_cast<int32_t>(ResolutionPreset::P720))) == Reject::None);
    CHECK(om.canvas_w() == 2560);
    CHECK(send(config(OutputParam::BlendWidth, 2000)) == Reject::BadValue);
    CHECK(send(config(OutputParam::Adopt, 1)) == Reject::BadTarget);  // needs an output
    CHECK(send(config(OutputParam::SimLost, 1, 0)) == Reject::None);
    Command bad = config(OutputParam::Mode, 0);
    bad.param = 99;
    CHECK(send(bad) == Reject::BadTarget);
    // The types between SHOW and outputs (37..69) are unknown.
    CHECK(send(cmd(static_cast<CommandType>(45))) == Reject::UnknownType);
    CHECK(send(cmd(CommandType::Count)) == Reject::UnknownType);
    CHECK(std::strcmp(himmel::engine::command_name(CommandType::LayerPlace), "place") == 0);
    // IDENTIFY held by a producer that goes quiet is let go (LEAD condition, M6).
    {
        Command held = cmd(CommandType::Identify, kNone, 2);
        held.seq = ++seq;
        CHECK(dm->submit(static_cast<int>(Producer::Gui), held, 5'000'000) == Reject::None);
        CHECK(om.identify_holder() == static_cast<int>(Producer::Gui));
        CHECK(om.identify_on(9'000'000));
        dm->set_producer_alive(tool, false, 9'000'000);  // another producer: no change
        CHECK(near(om.identify_alpha(9'600'000), 1.0f));
        dm->set_producer_alive(static_cast<int>(Producer::Gui), false, 10'000'000);
        CHECK(near(om.identify_alpha(10'250'000), 0.5f));
        CHECK(!om.identify_on(10'600'000) && om.identify_holder() == -1);
        // A timed IDENTIFY (i = 1) has no holder: a quiet producer changes nothing.
        Command timed = cmd(CommandType::Identify, kNone, 1);
        timed.seq = ++seq;
        dm->submit(static_cast<int>(Producer::Gui), timed, 20'000'000);
        dm->set_producer_alive(static_cast<int>(Producer::Gui), false, 21'000'000);
        CHECK(near(om.identify_alpha(22'000'000), 1.0f));
    }
}

static void test_restore() {
    OutputSetup s;
    s.count = 3;
    OutputModel a(s);
    a.apply(cmd(CommandType::OutputBlack, 2, kOn), -1, 1);
    a.apply(place(1, 3, Place::Out, 2), 7, 1);
    OutputsSection sec{};
    a.fill(sec, 1);
    CHECK(sec.count == 3 && sec.canvas_w == 5760 && sec.out[2].black == 1 && sec.config_by_command == 0);
    CHECK(sec.out[1].canvas_x == 1920 && sec.out[1].width == 1920);
    OutputModel b(s);
    b.restore(sec);
    CHECK(b.black(2) && !b.black(0));
    CHECK(b.place(7).place == static_cast<uint8_t>(Place::Out) && b.place(7).out == 2);
    // Config changed by a command comes back; otherwise the arguments win.
    OutputSetup s720 = s;
    s720.preset = ResolutionPreset::P720;
    OutputModel c(s720);
    c.restore(sec);
    CHECK(c.canvas_w() == 3 * 1280);  // not changed by a command: --resolution 720p wins
    a.apply(config(OutputParam::Mode, static_cast<int32_t>(CanvasMode::Mirror)), -1, 1);
    a.fill(sec, 1);
    CHECK(sec.config_by_command == 1);
    c.restore(sec);
    CHECK(c.setup().mode == CanvasMode::Mirror && c.canvas_w() == 1920);
}

static DisplayId id(const char* maker, uint16_t product, uint32_t serial, uint32_t connector, const char* text = "") {
    DisplayId d{};
    strncpy_s(d.maker, maker, _TRUNCATE);
    d.product = product;
    d.serial = serial;
    strncpy_s(d.serial_text, text, _TRUNCATE);
    d.connector = connector;
    d.technology = 10;
    return d;
}

static void test_identity() {
    const DisplayId p1 = id("EPS", 0x1234, 111, 0), p2 = id("EPS", 0x1234, 222, 1), lg = id("GSM", 0x5b, 0, 2);
    CHECK(himmel::engine::same_display(p1, id("EPS", 0x1234, 111, 5)));  // same serial, other connector
    CHECK(!himmel::engine::same_display(p1, p2));
    CHECK(himmel::engine::same_display(lg, id("GSM", 0x5b, 0, 2)));      // no serial: same connector
    CHECK(!himmel::engine::same_display(lg, id("GSM", 0x5b, 0, 3)));
    CHECK(himmel::engine::same_display(id("DEL", 1, 0, 0, "ABC123"), id("DEL", 1, 0, 4, "ABC123")));
    CHECK(!himmel::engine::same_display(DisplayId{}, DisplayId{}));

    int got[3];
    bool other[3];
    // First start: unknown slots take displays left to right.
    {
        const DisplayId slots[3] = {};
        const DisplayId disp[3] = {p2, lg, p1};
        const int order[3] = {2, 0, 1};  // p1 is leftmost
        himmel::engine::match_displays(slots, 3, disp, order, 3, got, other);
        CHECK(got[0] == 2 && got[1] == 0 && got[2] == 1);
        CHECK(!other[0] && !other[1] && !other[2]);
    }
    // Windows reorders the displays after a hot-plug: the slots follow identity.
    {
        const DisplayId slots[3] = {p1, p2, lg};
        const DisplayId disp[3] = {lg, p1, p2};
        const int order[3] = {0, 1, 2};
        himmel::engine::match_displays(slots, 3, disp, order, 3, got, other);
        CHECK(got[0] == 1 && got[1] == 2 && got[2] == 0);
    }
    // OUT 2 pulled: slot 2 lost, the others stay.
    {
        const DisplayId slots[3] = {p1, p2, lg};
        const DisplayId disp[2] = {lg, p1};
        const int order[2] = {1, 0};
        himmel::engine::match_displays(slots, 3, disp, order, 2, got, other);
        CHECK(got[0] == 1 && got[1] == -1 && got[2] == 0);
        CHECK(!other[1]);  // nothing free to offer
    }
    // A different projector in its place: ask (Other), never take it silently.
    {
        const DisplayId slots[3] = {p1, p2, lg};
        const DisplayId disp[3] = {lg, p1, id("NEC", 7, 99, 1)};
        const int order[3] = {1, 2, 0};
        himmel::engine::match_displays(slots, 3, disp, order, 3, got, other);
        CHECK(got[1] == -1 && other[1] && !other[0]);
    }
    // More slots than displays on a first start.
    {
        const DisplayId slots[3] = {};
        const DisplayId disp[1] = {p1};
        himmel::engine::match_displays(slots, 3, disp, nullptr, 1, got, other);
        CHECK(got[0] == 0 && got[1] == -1 && got[2] == -1 && !other[1]);
    }
}

int main() {
    test_layout();
    test_blend();
    test_places();
    test_identify_and_black();
    test_through_deck_model();
    test_restore();
    test_identity();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
