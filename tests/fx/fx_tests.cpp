// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// fx_tests.exe - effect metadata, every shader compiles, PRESS / scope / reaction
// resolution, and the optional flash cap (D65): with the cap on, strobe and
// flash never exceed it. No GPU needed (D3DCompile only).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/fx/fx.h"
#include "himmel/fx/signals.h"

using namespace himmel::fx;
namespace proto = himmel::protocol;

static int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

namespace {

proto::FxSlotState slot_of(const EffectMeta& m, int id) {
    proto::FxSlotState s{};
    s.effect = static_cast<int16_t>(id);
    s.mix = 1.0f;
    for (int p = 0; p < m.params; ++p) s.param[p] = m.param[p].def;
    return s;
}

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void test_library() {
    const Library& lib = builtin();
    Library fresh;
    std::string err;
    CHECK(fresh.load_builtin(&err));
    if (!err.empty()) std::printf("  load: %s\n", err.c_str());
    CHECK(lib.count() == himmel::engine::fx::kEffectCount);
    // The enum in fx_catalog.h follows fx/catalog.json (append only).
    const char* order[] = {"hue_sat",   "invert",    "mirror",    "kaleido",  "rgb_split",  "rotozoom",  "strobe",
                           "pixelate",  "bright_contrast", "colorize", "posterize", "tile", "zoom_pulse", "wave",
                           "shake",     "blur",      "glow",      "trails",   "luma_key",   "gen_solid", "gen_noise",
                           "gen_lines", "gen_rings", "gen_shapes", "gen_ticker"};
    static_assert(sizeof(order) / sizeof(order[0]) == static_cast<size_t>(himmel::engine::fx::kEffectCount));
    for (int i = 0; i < lib.count() && i < himmel::engine::fx::kEffectCount; ++i) CHECK(std::strcmp(lib.meta(i).id, order[i]) == 0);
    CHECK(lib.find("strobe") == static_cast<int>(himmel::engine::fx::Effect::Strobe));
    CHECK(lib.find("gen_ticker") == static_cast<int>(himmel::engine::fx::Effect::GenTicker));

    int effects = 0, generators = 0, tier1 = 0;
    for (int i = 0; i < lib.count(); ++i) {
        const EffectMeta& m = lib.meta(i);
        std::string e;
        CHECK(Library::validate(m, &e));
        (m.kind == Kind::Generator ? generators : effects)++;
        tier1 += m.tier == 1;
        // D88: a default reaction on a real signal and a real param.
        CHECK(m.reaction.signal != Signal::None && m.reaction.param >= 0 && m.reaction.param < m.params);
        CHECK(m.main[0] >= 0 && m.main[0] < m.params && m.main[1] >= 0 && m.main[1] < m.params);
        CHECK(m.find_preset("subtle") >= 0 && m.find_preset("extreme") >= 0);
        CHECK(m.press.param >= 0 && m.press.param < m.params);
        CHECK(m.help[0][0] != 0);
        CHECK(lib.hlsl(i)[0] != 0);
        CHECK(m.passes == 1);
    }
    CHECK(effects >= 16 + 2);  // the 16 tier-1 effects + trails + luma key (plus pixelate)
    CHECK(generators == 6);
    CHECK(tier1 == lib.count());
    CHECK(lib.meta(lib.find("trails")).state == State::Feedback);
    CHECK(lib.meta(lib.find("strobe")).flash.mode == FlashMode::Rate);
    CHECK(lib.meta(lib.find("invert")).flash.mode == FlashMode::Level);
    CHECK(!(lib.meta(lib.find("luma_key")).levels & level_bit(Level::Master)));

    // Catalog for the state block.
    auto cat = std::make_unique<proto::Catalog>();
    std::memset(cat.get(), 0, sizeof(proto::Catalog));
    himmel::engine::fx::fill_catalog(*cat);
    CHECK(cat->effect_count == static_cast<uint32_t>(lib.count()));
    CHECK(std::strcmp(cat->effect[6].name, "Strobe") == 0);
    CHECK(cat->effect[6].param_count == 4 && std::strcmp(cat->effect[6].param_name[0], "RATE") == 0);
    CHECK(approx(cat->effect[3].param_default[0], lib.meta(3).param[0].def));
    CHECK(cat->effect[19].reserved[0] == 1 && cat->effect[0].reserved[0] == 0);
}

void test_validation() {
    EffectMeta m;
    std::string err;
    const char* good = R"({"id":"t","name":"T","params":[{"id":"a","name":"A","default":0.5},{"id":"b","name":"B","default":0.2}],
        "main":["a","b"],"reaction":{"signal":"kick","param":"a","amount":0.5},"press":{"param":"a","lo":0,"hi":1},
        "presets":[{"name":"subtle","values":{"a":0.1}},{"name":"extreme","values":{"a":0.9}}],"help":["x"]})";
    CHECK(Library::parse(good, m, &err));
    CHECK(m.preset[0].v[1] == 0.2f);  // a value the preset leaves out = the default
    std::string bad = good;
    auto replaced = [&](const char* a, const char* b) {
        std::string s = good;
        const size_t at = s.find(a);
        if (at != std::string::npos) s.replace(at, std::strlen(a), b);
        return s;
    };
    CHECK(!Library::parse(replaced(R"("reaction":{"signal":"kick","param":"a","amount":0.5},)", "").c_str(), m, &err));
    CHECK(err.find("reaction") != std::string::npos);
    CHECK(!Library::parse(replaced(R"("signal":"kick")", R"("signal":"wobble")").c_str(), m, &err));
    CHECK(!Library::parse(replaced(R"("amount":0.5)", R"("amount":0)").c_str(), m, &err));
    CHECK(!Library::parse(replaced(R"({"name":"extreme","values":{"a":0.9}})", R"({"name":"wild","values":{"a":0.9}})").c_str(), m, &err));
    CHECK(!Library::parse(replaced(R"("default":0.5)", R"("default":5)").c_str(), m, &err));
    CHECK(!Library::parse(replaced(R"("main":["a","b"])", R"("main":["a","a"])").c_str(), m, &err));
    CHECK(!Library::parse(replaced(R"("press":{"param":"a","lo":0,"hi":1})", R"("press":{"param":"zz","lo":0,"hi":1})").c_str(), m, &err));
    CHECK(!Library::parse("{nope", m, &err));
    CHECK(!Library::parse(replaced(R"("help":["x"])", R"("help":[])").c_str(), m, &err));
}

void test_shaders_compile() {
    const Library& lib = builtin();
    const std::string common = Library::common_hlsl(), tail = Library::main_hlsl();
    for (int i = 0; i < lib.count(); ++i) {
        const std::string src = common + "\n" + lib.hlsl(i) + "\n" + tail;
        Microsoft::WRL::ComPtr<ID3DBlob> out, err;
        const HRESULT hr = D3DCompile(src.data(), src.size(), lib.meta(i).id, nullptr, nullptr, "ps_fx", "ps_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
        CHECK(SUCCEEDED(hr));
        if (FAILED(hr)) std::printf("  %s: %s\n", lib.meta(i).id, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    }
}

void test_steps() {
    CHECK(step_index(0.0f, 5) == 0 && step_index(0.199f, 5) == 0 && step_index(0.2f, 5) == 1);
    CHECK(step_index(1.0f, 5) == 4 && step_index(2.0f, 5) == 4 && step_index(-1.0f, 5) == 0);
    CHECK(step_index(NAN, 8) == 0);
    for (int n = 2; n <= 16; ++n)
        for (int i = 0; i < n; ++i) CHECK(step_index(step_center(i, n), n) == i);
}

void test_resolve_basics() {
    const Library& lib = builtin();
    Globals g;
    FrameCtx ctx;  // silence, no cap
    for (int id = 0; id < lib.count(); ++id) {
        const EffectMeta& m = lib.meta(id);
        proto::FxSlotState s = slot_of(m, id);
        SlotRuntime rt;
        const Resolved r = resolve(m, id, s, rt, g, ctx);
        for (int p = 0; p < m.params; ++p) {
            const float want = m.param[p].steps >= 2 ? step_center(step_index(s.param[p], m.param[p].steps), m.param[p].steps) : s.param[p];
            CHECK(approx(r.p[p], want));
        }
        CHECK(r.mix == 1.0f && r.press == 0.0f);
    }
    // Hostile slot values are clamped, never NaN.
    const int hue = lib.find("hue_sat");
    proto::FxSlotState s = slot_of(lib.meta(hue), hue);
    s.param[0] = NAN, s.param[1] = 7.0f, s.mix = -3.0f;
    SlotRuntime rt;
    const Resolved r = resolve(lib.meta(hue), hue, s, rt, g, ctx);
    CHECK(r.p[0] == 0.0f && r.p[1] == 1.0f && r.mix == 0.0f);
}

void test_press() {
    const Library& lib = builtin();
    Globals g;
    FrameCtx ctx;
    ctx.bpm = 120;
    ctx.dt = 1.0f / 60;
    const int id = lib.find("rgb_split");
    const EffectMeta& m = lib.meta(id);
    proto::FxSlotState s = slot_of(m, id);
    SlotRuntime rt;
    s.press = 255;
    Resolved r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[m.press.param], m.press.hi));
    CHECK(r.press == 1.0f);
    // Release: it falls over release_beats (0.5 beat = 0.25 s at 120 BPM), not at once.
    s.press = 0;
    r = resolve(m, id, s, rt, g, ctx);
    CHECK(r.press > 0.9f && r.p[m.press.param] > s.param[m.press.param]);
    for (int f = 0; f < 20; ++f) r = resolve(m, id, s, rt, g, ctx);  // 0.35 s
    CHECK(r.press == 0.0f && approx(r.p[m.press.param], s.param[m.press.param]));
    // Velocity sets the start: a soft press lands between the knob and hi.
    s.press = 128;
    r = resolve(m, id, s, rt, g, ctx);
    CHECK(r.p[m.press.param] > m.press.lo && r.p[m.press.param] < m.press.hi);

    // Kaleido's PRESS is stepped: segments 3 -> 12, always on a step.
    const int k = lib.find("kaleido");
    const EffectMeta& km = lib.meta(k);
    proto::FxSlotState ks = slot_of(km, k);
    SlotRuntime krt;
    for (int v = 0; v <= 255; v += 15) {
        ks.press = static_cast<uint8_t>(v);
        const Resolved kr = resolve(km, k, ks, krt, g, ctx);
        const int n = km.param[km.press.param].steps;
        CHECK(approx(kr.p[km.press.param], step_center(step_index(kr.p[km.press.param], n), n)));
    }
    ks.press = 255;
    const Resolved kr = resolve(km, k, ks, krt, g, ctx);
    CHECK(2 + step_index(kr.p[0], 15) == 12);
}

void test_reaction() {
    const Library& lib = builtin();
    Globals g;
    FrameCtx ctx;
    ctx.dt = 1.0f / 60;
    // Zoom pulse: kick -> amount (ADD). Silence = the knob; a kick lifts it, then it falls back.
    const int id = lib.find("zoom_pulse");
    const EffectMeta& m = lib.meta(id);
    proto::FxSlotState s = slot_of(m, id);
    SlotRuntime rt;
    Resolved r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[0], s.param[0]));
    ctx.sig.v[static_cast<int>(Signal::Kick)] = 1.0f;
    r = resolve(m, id, s, rt, g, ctx);  // attack 5 ms: nearly there in one frame
    CHECK(r.p[0] > s.param[0] + 0.15f);
    ctx.sig = Signals{};
    for (int f = 0; f < 60; ++f) r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[0], s.param[0], 0.01f));

    // Invert: snare -> amount -1 (TRIGGER): the edge flips it back to normal, then it releases.
    const int inv = lib.find("invert");
    const EffectMeta& im = lib.meta(inv);
    proto::FxSlotState is = slot_of(im, inv);  // AMOUNT 1 = inverted
    SlotRuntime irt;
    ctx.sig.v[static_cast<int>(Signal::Snare)] = 0.0f;
    r = resolve(im, inv, is, irt, g, ctx);
    CHECK(r.p[0] == 1.0f);
    ctx.sig.v[static_cast<int>(Signal::Snare)] = 1.0f;
    r = resolve(im, inv, is, irt, g, ctx);
    CHECK(r.p[0] < 0.1f);
    for (int f = 0; f < 60; ++f) r = resolve(im, inv, is, irt, g, ctx);  // held high: no new edge, it releases
    CHECK(r.p[0] > 0.95f);

    // The test signal moves every effect's reaction target at some point in 8 s.
    for (int e = 0; e < lib.count(); ++e) {
        const EffectMeta& em = lib.meta(e);
        proto::FxSlotState es = slot_of(em, e);
        SlotRuntime ert;
        float lo = 2, hi = -2;
        for (int f = 0; f < 480; ++f) {
            FrameCtx c;
            c.dt = 1.0f / 60;
            c.time_s = f / 60.0;
            c.beats = c.time_s * 2.0;
            c.sig = TestSignal::at(c.time_s, c.beats);
            const Resolved er = resolve(em, e, es, ert, g, c);
            lo = std::min(lo, er.p[em.reaction.param]);
            hi = std::max(hi, er.p[em.reaction.param]);
        }
        CHECK(hi - lo > 0.01f);
        if (!(hi - lo > 0.01f)) std::printf("  %s: its reaction never moved %s\n", em.id, em.param[em.reaction.param].id);
    }
    // Test signal stays in 0..1.
    for (double t = 0; t < 70; t += 0.013) {
        const Signals sg = TestSignal::at(t, t * 2.1);
        for (float v : sg.v) CHECK(v >= 0.0f && v <= 1.0f);
    }
}

void test_scopes() {
    const Library& lib = builtin();
    Globals g;
    FrameCtx ctx;
    const int id = lib.find("hue_sat");
    const EffectMeta& m = lib.meta(id);
    proto::FxSlotState s = slot_of(m, id);
    SlotRuntime rt;
    rt.effect = static_cast<int16_t>(id);  // the slot already holds hue_sat (a new effect resets the runtime)
    // GLOBAL: one value for every hue_sat in the show.
    rt.scope[1] = static_cast<uint8_t>(Scope::Global);
    Resolved r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[1], s.param[1]));  // nothing global yet: the slot value
    g.v[id][1] = 0.9f, g.set[id][1] = true;
    r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[1], 0.9f));
    SlotRuntime other;  // a second instance with SLOT scope keeps its own
    r = resolve(m, id, s, other, g, ctx);
    CHECK(approx(r.p[1], s.param[1]));
    // LOOK: the scene's value.
    rt.scope[0] = static_cast<uint8_t>(Scope::Look);
    rt.look[0] = 0.7f, rt.look_set[0] = true;
    r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[0], 0.7f));
    // MOVE on the reaction's param: it follows the signal over the whole range.
    rt.scope[0] = static_cast<uint8_t>(Scope::Move);
    ctx.dt = 1.0f;
    ctx.sig.v[static_cast<int>(Signal::Mid)] = 0.0f;
    r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[0], m.reaction.range_lo, 0.01f));
    ctx.sig.v[static_cast<int>(Signal::Mid)] = 1.0f;
    for (int f = 0; f < 4; ++f) r = resolve(m, id, s, rt, g, ctx);
    CHECK(approx(r.p[0], m.reaction.range_hi, 0.02f));
    // A scope the effect does not allow falls back to SLOT.
    EffectMeta m2 = m;
    m2.param[1].scopes = scope_bit(Scope::Slot);
    SlotRuntime rt2;
    rt2.scope[1] = static_cast<uint8_t>(Scope::Global);
    r = resolve(m2, id, s, rt2, g, FrameCtx{});
    CHECK(approx(r.p[1], s.param[1]));
    // A new effect in the slot resets the runtime (scopes, envelopes).
    rt.press_env = 1;
    resolve(lib.meta(1), 1, slot_of(lib.meta(1), 1), rt, g, FrameCtx{});
    CHECK(rt.effect == 1 && rt.press_env == 0 && rt.scope[0] == 0);
}

// Counts rising flash edges; returns the most seen in any 1 s window [t, t + 1).
struct EdgeCounter {
    std::deque<double> edges;
    int worst = 0, total = 0;
    void edge(double t) {
        edges.push_back(t);
        ++total;
        while (!edges.empty() && edges.front() <= t - 1.0) edges.pop_front();
        worst = std::max(worst, static_cast<int>(edges.size()));
    }
};

void test_flash_cap() {
    const Library& lib = builtin();
    Globals g;
    const int st = lib.find("strobe");
    const EffectMeta& m = lib.meta(st);
    // Without a cap: 1/32 beat at 120 BPM is 64 flashes / s (the knob means it).
    CHECK(approx(strobe_flash_hz(m, 1.0f, 120.0f), 64.0f));
    {
        proto::FxSlotState s = slot_of(m, st);
        s.param[0] = 1.0f;
        SlotRuntime rt;
        EdgeCounter ec;
        bool prev = false;
        for (int f = 0; f < 240 * 5; ++f) {
            FrameCtx c;
            c.dt = 1.0f / 240, c.time_s = f / 240.0, c.beats = c.time_s * 2.0;
            const bool on = resolve(m, st, s, rt, g, c).gate > 0.5f;
            if (on && !prev) ec.edge(c.time_s);
            prev = on;
        }
        CHECK(ec.worst > 20);
    }
    // With the cap on: never more than the cap in any second, whatever the knob, PRESS,
    // reaction, BPM or frame timing do.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (float cap : {1.0f, 2.0f, 3.0f, 5.0f}) {
        for (float bpm : {60.0f, 90.0f, 120.0f, 128.0f, 174.0f, 200.0f, 300.0f}) {
            proto::FxSlotState s = slot_of(m, st);
            SlotRuntime rt;
            EdgeCounter ec;
            bool prev = false;
            double t = 0, beats = 0;
            for (int f = 0; f < 240 * 30; ++f) {
                const float dt = 1.0f / 240 + (u(rng) - 0.5f) * 0.004f;  // jittery frames
                t += dt;
                beats += dt * bpm / 60.0;
                if (f % 23 == 0) s.param[0] = u(rng);                               // the knob jumps around
                if (f % 31 == 0) s.param[2] = u(rng);                               // duty too
                if (f % 17 == 0) s.press = static_cast<uint8_t>(u(rng) * 255.0f);   // pad mashing
                FrameCtx c;
                c.dt = dt, c.time_s = t, c.beats = beats, c.bpm = bpm, c.flash_cap_hz = cap;
                c.sig = TestSignal::at(t, beats);
                const bool on = resolve(m, st, s, rt, g, c).gate > 0.5f;
                if (on && !prev) ec.edge(t);
                prev = on;
            }
            CHECK(ec.worst <= static_cast<int>(cap));
            CHECK(ec.total > 0);
            if (ec.worst > static_cast<int>(cap)) std::printf("  strobe cap %.0f bpm %.0f: %d in one second\n", cap, bpm, ec.worst);
        }
    }
    // A strobe already under the cap is left alone: 1 beat at 120 BPM = 2 / s with a cap of 3.
    {
        proto::FxSlotState s = slot_of(m, st);
        s.param[0] = step_center(2, 8);
        SlotRuntime rt;
        FrameCtx c;
        c.flash_cap_hz = 3;
        CHECK(approx(resolve(m, st, s, rt, g, c).p[0], step_center(2, 8)));
        s.param[0] = 1.0f;
        CHECK(2.0f * 60.0f / 60.0f / m.flash.beats[step_index(resolve(m, st, s, rt, g, c).p[0], 8)] <= 3.0f);
    }
    // Invert (a flash by level): snare triggers + random PRESS never flash more than the cap.
    const int inv = lib.find("invert");
    const EffectMeta& im = lib.meta(inv);
    for (float cap : {1.0f, 3.0f}) {
        proto::FxSlotState s = slot_of(im, inv);
        s.param[0] = 0.0f;
        SlotRuntime rt;
        EdgeCounter ec;
        bool prev = false;
        double t = 0, beats = 0;
        for (int f = 0; f < 240 * 30; ++f) {
            const float dt = 1.0f / 240;
            t += dt;
            beats += dt * 3.0;  // 180 BPM
            if (f % 13 == 0) s.press = static_cast<uint8_t>(u(rng) > 0.5f ? 255 : 0);
            FrameCtx c;
            c.dt = dt, c.time_s = t, c.beats = beats, c.bpm = 180, c.flash_cap_hz = cap;
            c.sig = TestSignal::at(t, beats);
            c.sig.v[static_cast<int>(Signal::Snare)] = (f / 7) % 2 ? 1.0f : 0.0f;  // a snare roll, 17 hits / s
            const bool on = resolve(im, inv, s, rt, g, c).p[0] > 0.5f;
            if (on && !prev) ec.edge(t);
            prev = on;
        }
        CHECK(ec.worst <= static_cast<int>(cap));
        CHECK(ec.total > 0);
    }
}

void test_deck_press() {
    auto cat = std::make_unique<proto::Catalog>();
    std::memset(cat.get(), 0, sizeof(proto::Catalog));
    himmel::engine::fx::fill_catalog(*cat);
    himmel::engine::DeckModel dm(*cat);
    using proto::CommandType;
    proto::Command c = proto::make_fx(CommandType::FxSet, 0, 1, 2, proto::kNone, 6);
    c.seq = 1;
    CHECK(dm.submit(3, c, 1000) == proto::Reject::None);
    c = proto::make_fx(CommandType::FxPress, 0, 1, 2, proto::kNone, 0, 0.5f);
    c.seq = 2;
    CHECK(dm.submit(3, c, 2000) == proto::Reject::None);
    CHECK(dm.live().deck[0].layer[1].fx[2].press == 128);
    c = proto::make_fx(CommandType::FxPress, proto::kMasterDeck, proto::kNone, 0, proto::kNone, 0, 9.0f);
    c.seq = 3;
    CHECK(dm.submit(3, c, 3000) == proto::Reject::None);
    CHECK(dm.live().master_fx[0].press == 255);
    c = proto::make_fx(CommandType::FxPress, 0, 1, 7, proto::kNone, 0, 0.5f);
    c.seq = 4;
    CHECK(dm.submit(3, c, 4000) == proto::Reject::BadTarget);
    c = proto::make_fx(CommandType::FxPress, 0, 1, 2, proto::kNone, 0, NAN);
    c.seq = 5;
    CHECK(dm.submit(3, c, 5000) == proto::Reject::BadValue);
    c = proto::make_fx(CommandType::FxSet, 0, 1, 2, proto::kNone, 7);  // a new effect starts released
    c.seq = 6;
    CHECK(dm.submit(3, c, 6000) == proto::Reject::None);
    CHECK(dm.live().deck[0].layer[1].fx[2].press == 0);
}

}  // namespace

int main() {
    test_library();
    test_validation();
    test_shaders_compile();
    test_steps();
    test_resolve_basics();
    test_press();
    test_reaction();
    test_scopes();
    test_flash_cap();
    test_deck_press();
    std::printf("fx_tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
