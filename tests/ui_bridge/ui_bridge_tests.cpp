// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_ui: JSON <-> Command conversion, validation and the HTTP helpers (no Windows, no engine).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "himmel/ui/json_codec.h"
#include "http_util.h"

using namespace himmel::protocol;
using namespace himmel::ui;
using nlohmann::json;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static const CatalogLimits kLim{14, 7};

static ParseResult P(const char* text, CatalogLimits lim = kLim) { return command_from_json(json::parse(text), lim); }

static void test_layer_commands() {
    std::printf("layer commands\n");
    auto r = P(R"({"type":"LayerClip","deck":1,"layer":2,"value":5})");
    CHECK(r.ok && r.cmd.type == CommandType::LayerClip && r.cmd.deck == 1 && r.cmd.layer == 2 && r.cmd.i == 5);
    CHECK(r.cmd.slot == kNone && r.cmd.param == kNone && r.cmd.seq == 0);
    r = P(R"({"type":"LayerClip","deck":0,"layer":0,"value":-1})");
    CHECK(r.ok && r.cmd.i == -1);
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":0,"value":14})").ok);   // not in the catalog
    CHECK(P(R"({"type":"LayerClip","deck":0,"layer":0,"value":200})", CatalogLimits{}).ok);  // unknown catalog: engine checks
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":0,"value":256})", CatalogLimits{}).ok);  // over kMaxClips
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":0,"value":-2})").ok);
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":0})").ok);
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":0,"value":1.5})").ok);
    CHECK(P(R"({"type":"LayerClip","deck":0,"layer":0,"value":3.0})").ok);
    CHECK(!P(R"({"type":"LayerClip","deck":2,"layer":0,"value":1})").ok);
    CHECK(!P(R"({"type":"LayerClip","deck":0,"layer":4,"value":1})").ok);
    CHECK(!P(R"({"type":"LayerClip","layer":0,"value":1})").ok);
    CHECK(!P(R"({"type":"LayerClip","deck":"master","layer":0,"value":1})").ok);
    r = P(R"({"type":"LayerClip","deck":"B","layer":3,"value":0})");
    CHECK(r.ok && r.cmd.deck == 1);

    r = P(R"({"type":"LayerOpacity","deck":0,"layer":1,"value":0.25})");
    CHECK(r.ok && std::fabs(r.cmd.f[0] - 0.25f) < 1e-6f && r.cmd.flags == 0);
    r = P(R"({"type":"LayerOpacity","deck":0,"layer":1,"value":1.3})");
    CHECK(r.ok && r.cmd.f[0] == 1.f);  // clamped, not refused
    r = P(R"({"type":"LayerOpacity","deck":0,"layer":1,"value":-0.1,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && std::fabs(r.cmd.f[0] + 0.1f) < 1e-6f);
    CHECK(!P(R"({"type":"LayerOpacity","deck":0,"layer":1,"value":"x"})").ok);
    CHECK(!P(R"({"type":"LayerOpacity","deck":0,"layer":1,"value":0.5,"relative":1})").ok);
    r = P(R"({"type":"LayerSpeed","deck":0,"layer":1,"value":9})");
    CHECK(r.ok && r.cmd.f[0] == 4.f);
    r = P(R"({"type":"LayerSpeed","deck":0,"layer":1,"value":-12,"relative":true})");
    CHECK(r.ok && r.cmd.f[0] == -8.f);  // a delta may span the whole range
    r = P(R"({"type":"LayerJump","deck":1,"layer":0,"value":0.5})");
    CHECK(r.ok && r.cmd.f[0] == 0.5f);
    CHECK(!P(R"({"type":"LayerJump","deck":1,"layer":0,"value":0.5,"relative":true})").ok);

    r = P(R"({"type":"LayerBlend","deck":0,"layer":0,"value":"screen"})");
    CHECK(r.ok && r.cmd.i == static_cast<int>(BlendMode::Screen));
    r = P(R"({"type":"LayerBlend","deck":0,"layer":0,"value":4})");
    CHECK(r.ok && r.cmd.i == 4);
    CHECK(!P(R"({"type":"LayerBlend","deck":0,"layer":0,"value":6})").ok);
    CHECK(!P(R"({"type":"LayerBlend","deck":0,"layer":0,"value":"Overlay"})").ok);

    r = P(R"({"type":"LayerMute","deck":0,"layer":0,"value":true})");
    CHECK(r.ok && r.cmd.i == 1);
    r = P(R"({"type":"LayerMute","deck":0,"layer":0,"value":"toggle"})");
    CHECK(r.ok && r.cmd.i == 2);
    CHECK(!P(R"({"type":"LayerMute","deck":0,"layer":0,"value":3})").ok);
    r = P(R"({"type":"LayerPlay","deck":0,"layer":0,"value":"Restart"})");
    CHECK(r.ok && r.cmd.i == static_cast<int>(PlayAction::Restart));
    CHECK(P(R"({"type":"LayerPlay","deck":0,"layer":0,"value":4})").ok);   // Release (M2)
    CHECK(P(R"({"type":"LayerPlay","deck":0,"layer":0,"value":"Stop"})").cmd.i == static_cast<int>(PlayAction::Stop));
    CHECK(!P(R"({"type":"LayerPlay","deck":0,"layer":0,"value":6})").ok);
}

static void test_fx_commands() {
    std::printf("fx commands\n");
    auto r = P(R"({"type":"FxSet","deck":0,"layer":1,"slot":2,"value":3})");
    CHECK(r.ok && r.cmd.deck == 0 && r.cmd.layer == 1 && r.cmd.slot == 2 && r.cmd.i == 3 && r.cmd.param == kNone);
    r = P(R"({"type":"FxSet","deck":1,"slot":0,"value":-1})");  // deck level
    CHECK(r.ok && r.cmd.layer == kNone && r.cmd.i == -1);
    r = P(R"({"type":"FxSet","deck":"master","slot":3,"value":6})");
    CHECK(r.ok && r.cmd.deck == kMasterDeck && r.cmd.layer == kNone);
    CHECK(!P(R"({"type":"FxSet","deck":"master","layer":0,"slot":3,"value":1})").ok);
    CHECK(!P(R"({"type":"FxSet","deck":0,"layer":1,"slot":4,"value":1})").ok);
    CHECK(!P(R"({"type":"FxSet","deck":0,"layer":1,"value":1})").ok);
    CHECK(!P(R"({"type":"FxSet","deck":0,"layer":1,"slot":0,"value":7})").ok);
    r = P(R"({"type":"FxParam","deck":0,"layer":0,"slot":0,"param":7,"value":0.75})");
    CHECK(r.ok && r.cmd.param == 7 && r.cmd.f[0] == 0.75f);
    CHECK(!P(R"({"type":"FxParam","deck":0,"layer":0,"slot":0,"param":8,"value":0.75})").ok);
    CHECK(!P(R"({"type":"FxParam","deck":0,"layer":0,"slot":0,"value":0.75})").ok);
    r = P(R"({"type":"FxBypass","deck":"master","slot":1,"value":2})");
    CHECK(r.ok && r.cmd.i == 2);
    r = P(R"({"type":"FxMix","deck":1,"layer":3,"slot":1,"value":0.5})");
    CHECK(r.ok && r.cmd.f[0] == 0.5f);
}

static void test_mixer_commands() {
    std::printf("mixer commands\n");
    auto r = P(R"({"type":"Crossfader","value":0.4})");
    CHECK(r.ok && r.cmd.deck == kNone && r.cmd.f[0] == 0.4f);
    r = P(R"({"type":"Take"})");
    CHECK(r.ok && r.cmd.i == 0);
    r = P(R"({"type":"Take","value":1500})");
    CHECK(r.ok && r.cmd.i == 1500);
    r = P(R"({"type":"Take","value":999999})");
    CHECK(r.ok && r.cmd.i == 60000);
    CHECK(!P(R"({"type":"Take","value":0.5})").ok);
    r = P(R"({"type":"Cut"})");
    CHECK(r.ok && r.cmd.type == CommandType::Cut);
    r = P(R"({"type":"Black","value":2})");
    CHECK(r.ok && r.cmd.i == 2);
    CHECK(!P(R"({"type":"Black"})").ok);
    r = P(R"({"type":"Focus","deck":1,"layer":3})");
    CHECK(r.ok && r.cmd.deck == 1 && r.cmd.layer == 3);
    CHECK(!P(R"({"type":"Focus","deck":1})").ok);
    r = P(R"({"type":"cut"})");  // names are case-insensitive
    CHECK(r.ok);
}

static void test_bad_input() {
    std::printf("bad input\n");
    CHECK(!command_from_json(json::array(), kLim).ok);
    CHECK(!command_from_json(json(5), kLim).ok);
    CHECK(!P(R"({"deck":0})").ok);
    CHECK(!P(R"({"type":42})").ok);
    CHECK(!P(R"({"type":"Explode"})").ok);
    CHECK(!P(R"({"type":"Hello","value":1})").ok);  // only the bridge says hello
    CHECK(!P(R"({"type":"Nop"})").ok);
    CHECK(!P(R"({"type":"Take","relative":true})").ok);
    // NaN / inf cannot come from JSON text, but a json value built in code can carry them.
    json j = {{"type", "LayerOpacity"}, {"deck", 0}, {"layer", 0}, {"value", std::numeric_limits<double>::quiet_NaN()}};
    CHECK(!command_from_json(j, kLim).ok);
    j["value"] = std::numeric_limits<double>::infinity();
    CHECK(!command_from_json(j, kLim).ok);
    CHECK(!P(R"({"type":"LayerOpacity","deck":0,"layer":1e20,"value":0.5})").ok);
    auto r = P(R"({"type":"Cut","deck":null,"layer":null})");
    CHECK(r.ok);
    auto e = P(R"({"type":"LayerClip","deck":0,"layer":0,"value":99})");
    CHECK(!e.ok && e.error.find("catalog") != std::string::npos);
}

static void test_round_trip() {
    std::printf("round trip\n");
    const char* cases[] = {
        R"({"type":"LayerClip","deck":1,"layer":2,"value":5})",
        R"({"type":"LayerOpacity","deck":0,"layer":1,"value":0.5,"relative":true})",
        R"({"type":"FxParam","deck":"master","slot":2,"param":3,"value":0.25})",
        R"({"type":"FxSet","deck":1,"slot":0,"value":2})",
        R"({"type":"Take","value":1200})",
        R"({"type":"Cut"})",
        R"({"type":"Focus","deck":0,"layer":3})",
    };
    for (const char* c : cases) {
        const auto a = P(c);
        CHECK(a.ok);
        const auto b = command_from_json(command_to_json(a.cmd), kLim);
        CHECK(b.ok && std::memcmp(&a.cmd, &b.cmd, sizeof(Command)) == 0);
    }
    CHECK(std::string(command_type_name(CommandType::FxMix)) == "FxMix");
    CHECK(std::string(reject_name(static_cast<uint8_t>(Reject::BadId))) == "BadId");
    CHECK(std::string(blend_name(3)) == "Multiply");
}

static void test_state_json() {
    std::printf("state json\n");
    auto s = std::make_unique<LiveState>();
    std::memset(s.get(), 0, sizeof(LiveState));
    s->epoch = 7;
    s->crossfader = 0.33333f;
    s->program_deck = 1;
    s->focus_deck = kNone;
    s->focus_layer = kNone;
    s->deck[1].layer[2].clip = 4;
    s->deck[1].layer[2].opacity = 0.5f;
    s->deck[1].layer[2].fx[1].effect = 2;
    s->deck[1].layer[2].fx[1].param[0] = 0.125f;
    s->master_fx[0].effect = -1;
    s->deck[1].layer[2].fx[0].effect = -1;
    s->producer_alive[1] = 1;
    s->last_reject[1] = static_cast<uint8_t>(Reject::BadValue);
    s->frame = 1234;
    const json j = live_to_json(*s);
    CHECK(j["epoch"] == 7);
    CHECK(j["crossfader"].get<double>() == 0.3333);
    CHECK(j["program_deck"] == 1);
    CHECK(j["focus_deck"] == -1 && j["focus_layer"] == -1);
    CHECK(j["deck"].size() == 2 && j["deck"][1]["layer"].size() == 4);
    CHECK(j["deck"][1]["layer"][2]["clip"] == 4);
    CHECK(j["deck"][1]["layer"][2]["fx"][1]["effect"] == 2);
    CHECK(j["deck"][1]["layer"][2]["fx"][1]["param"].size() == kFxParams);
    CHECK(j["deck"][1]["layer"][2]["fx"][1]["param"][0].get<double>() == 0.125);
    CHECK(!j["deck"][1]["layer"][2]["fx"][0].contains("param"));  // empty slots are short
    CHECK(j["master_fx"].size() == kFxSlots);
    CHECK(j["producer"][1]["alive"] == true && j["producer"][1]["last_reject"] == "BadValue");
    CHECK(!j.contains("frame"));  // frame / time / fps live in the bridge event
    s->frame = 9999;
    CHECK(live_to_json(*s).dump() == j.dump());

    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    c->clip_count = 2;
    std::snprintf(c->clip[0].name, kNameLen, "Ocean.mov");
    std::memset(c->clip[1].name, 'x', kNameLen);  // no NUL: must stay inside the field
    c->clip[0].thumb_rgb = 0x112233;
    c->effect_count = 1;
    std::snprintf(c->effect[0].name, kNameLen, "Hue");
    c->effect[0].param_count = 2;
    std::snprintf(c->effect[0].param_name[0], kParamNameLen, "Turn");
    std::snprintf(c->effect[0].param_name[1], kParamNameLen, "Speed");
    c->effect[0].param_default[1] = 0.5f;
    const json cj = catalog_to_json(*c);
    CHECK(cj["clips"].size() == 2 && cj["clips"][0]["name"] == "Ocean.mov" && cj["clips"][0]["thumb_rgb"] == 0x112233);
    CHECK(cj["clips"][1]["name"].get<std::string>().size() == static_cast<size_t>(kNameLen));
    CHECK(cj["effects"][0]["params"].size() == 2 && cj["effects"][0]["params"][1] == "Speed");
    CHECK(cj["effects"][0]["defaults"][1].get<double>() == 0.5);
    c->clip_count = 100000;  // a broken count never reads past the table
    CHECK(catalog_to_json(*c)["clips"].size() == static_cast<size_t>(kMaxClips));
}

static void test_transport_commands() {
    std::printf("transport commands (M2)\n");
    // LayerSet by name, enum by name and by number
    auto r = P(R"({"type":"LayerSet","deck":0,"layer":1,"param":"Mode","value":"PingPong"})");
    CHECK(r.ok && r.cmd.type == CommandType::LayerSet && r.cmd.param == static_cast<uint8_t>(LayerParam::Mode) &&
          r.cmd.i == static_cast<int>(PlayMode::PingPong));
    r = P(R"({"type":"LayerSet","deck":0,"layer":1,"param":0,"value":8})");
    CHECK(r.ok && r.cmd.i == static_cast<int>(PlayMode::Scratch));
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"param":"Mode","value":9})").ok);
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"param":"Mode","value":"Wobble"})").ok);
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"param":"Nope","value":1})").ok);
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"param":29,"value":1})").ok);
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"value":1})").ok);
    // floats clamp like the engine; relative only for floats
    r = P(R"({"type":"LayerSet","deck":1,"layer":3,"param":"Beats","value":100})");
    CHECK(r.ok && r.cmd.f[0] == 64.f);
    r = P(R"({"type":"LayerSet","deck":1,"layer":3,"param":"In","value":0.25})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(LayerParam::In) && std::fabs(r.cmd.f[0] - 0.25f) < 1e-6f);
    r = P(R"({"type":"LayerSet","deck":1,"layer":3,"param":"Glide","value":-1,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && r.cmd.f[0] == -1.f);
    CHECK(!P(R"({"type":"LayerSet","deck":1,"layer":3,"param":"Mode","value":1,"relative":true})").ok);
    // ints clamp, toggles, bools, snap -1 / "global"
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Slices","value":40})").cmd.i == 16);
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"InnerLoop","value":"toggle"})").cmd.i == 2);
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Direction","value":"backward"})").cmd.i == 1);
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Direction","value":2})").ok);
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Snap","value":-1})").cmd.i == -1);
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Snap","value":"global"})").cmd.i == -1);
    CHECK(P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Snap","value":"Bar"})").cmd.i == static_cast<int>(GridDiv::Bar));
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":0,"param":"Mode","value":-1})").ok);
    // the HIT layer
    r = P(R"({"type":"LayerSet","deck":"hit","param":"FadeOut","value":0.5})");
    CHECK(r.ok && r.cmd.deck == kHitDeck && r.cmd.layer == 0);
    CHECK(!P(R"({"type":"LayerSet","deck":"hit","layer":2,"param":"FadeOut","value":0.5})").ok);
    CHECK(P(R"({"type":"LayerOpacity","deck":"hit","layer":0,"value":0.5})").ok);
    CHECK(!P(R"({"type":"FxSet","deck":"hit","slot":0,"value":1})").ok);
    CHECK(!P(R"({"type":"Focus","deck":"hit","layer":0})").ok);
    // LayerTrigger
    r = P(R"({"type":"LayerTrigger","deck":"hit","value":3,"velocity":0.8})");
    CHECK(r.ok && r.cmd.i == 3 && std::fabs(r.cmd.f[0] - 0.8f) < 1e-6f && !(r.cmd.flags & kRelease));
    r = P(R"({"type":"LayerTrigger","deck":0,"layer":2,"release":true})");
    CHECK(r.ok && r.cmd.i == -1 && (r.cmd.flags & kRelease) && r.cmd.f[0] == 0.f);
    CHECK(!P(R"({"type":"LayerTrigger","deck":0,"layer":2,"value":14})").ok);
    CHECK(!P(R"({"type":"LayerTrigger","deck":0,"layer":2,"release":1})").ok);
    // LayerCue
    r = P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":4,"value":"Set","pos":0.25})");
    CHECK(r.ok && r.cmd.slot == 4 && r.cmd.i == static_cast<int>(CueAction::Set) && std::fabs(r.cmd.f[0] - 0.25f) < 1e-6f);
    r = P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":15,"value":"Set"})");
    CHECK(r.ok && r.cmd.f[0] < 0);  // at the playhead
    CHECK(P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":0,"value":2})").cmd.i == static_cast<int>(CueAction::Clear));
    CHECK(!P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":16,"value":"Jump"})").ok);
    CHECK(!P(R"({"type":"LayerCue","deck":0,"layer":0,"value":"Jump"})").ok);
    CHECK(!P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":1,"value":"Fly"})").ok);
    CHECK(!P(R"({"type":"LayerCue","deck":0,"layer":0,"cue":1,"value":3})").ok);
    // LayerBeatLoop, LayerScratch
    CHECK(P(R"({"type":"LayerBeatLoop","deck":0,"layer":0,"value":0})").cmd.f[0] == 0.f);
    CHECK(P(R"({"type":"LayerBeatLoop","deck":0,"layer":0,"value":0.01})").cmd.f[0] == 1.f / 16.f);
    CHECK(P(R"({"type":"LayerBeatLoop","deck":0,"layer":0,"value":99})").cmd.f[0] == 16.f);
    r = P(R"({"type":"LayerScratch","deck":1,"layer":0,"value":1,"pos":0.4})");
    CHECK(r.ok && r.cmd.i == 1 && std::fabs(r.cmd.f[0] - 0.4f) < 1e-6f);
    r = P(R"({"type":"LayerScratch","deck":1,"layer":0,"value":1,"pos":-0.5,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && r.cmd.f[0] == -0.5f);
    CHECK(!P(R"({"type":"LayerScratch","deck":1,"layer":0,"value":1,"relative":true})").ok);
    CHECK(!P(R"({"type":"LayerScratch","deck":1,"layer":0,"value":2})").ok);
    // Clock
    r = P(R"({"type":"Clock","param":"Bpm","value":128})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(ClockParam::Bpm) && r.cmd.f[0] == 128.f);
    CHECK(P(R"({"type":"Clock","param":"Bpm","value":500})").cmd.f[0] == 300.f);
    CHECK(P(R"({"type":"Clock","param":"Bpm","value":-2,"relative":true})").cmd.f[0] == -2.f);
    CHECK(P(R"({"type":"Clock","param":"Tap"})").ok);
    CHECK(P(R"({"type":"Clock","param":"Resync"})").ok);
    CHECK(P(R"({"type":"Clock","param":"Grid","value":"FourBars"})").cmd.i == static_cast<int>(GridDiv::FourBars));
    CHECK(!P(R"({"type":"Clock","param":"Grid","value":"Never"})").ok);
    CHECK(!P(R"({"type":"Clock","param":"Tap","relative":true})").ok);
    CHECK(!P(R"({"type":"Clock","param":"Warp"})").ok);
    CHECK(P(R"({"type":"Clock","param":"Nudge","value":-3})").cmd.f[0] == -1.f);
    // FxPress (M5)
    r = P(R"({"type":"FxPress","deck":0,"layer":1,"slot":2,"value":0.7})");
    CHECK(r.ok && r.cmd.type == CommandType::FxPress && r.cmd.slot == 2 && std::fabs(r.cmd.f[0] - 0.7f) < 1e-6f);
    CHECK(!P(R"({"type":"FxPress","deck":0,"layer":1,"slot":2,"value":0.7,"relative":true})").ok);

    // round trips through command_to_json
    const char* cases[] = {
        R"({"type":"LayerSet","deck":0,"layer":1,"param":"Mode","value":"Chop"})",
        R"({"type":"LayerSet","deck":"hit","param":"FadeIn","value":1.5})",
        R"({"type":"LayerSet","deck":1,"layer":0,"param":"Snap","value":-1})",
        R"({"type":"LayerSet","deck":1,"layer":0,"param":"Beats","value":-2,"relative":true})",
        R"({"type":"LayerTrigger","deck":"hit","value":2,"velocity":0.5,"release":true})",
        R"({"type":"LayerCue","deck":0,"layer":3,"cue":7,"value":"Set","pos":0.5})",
        R"({"type":"LayerCue","deck":0,"layer":3,"cue":7,"value":"Jump"})",
        R"({"type":"LayerBeatLoop","deck":0,"layer":0,"value":0.25})",
        R"({"type":"LayerScratch","deck":0,"layer":0,"value":1,"pos":0.2})",
        R"({"type":"Clock","param":"Grid","value":"Beat"})",
        R"({"type":"Clock","param":"Bpm","value":140})",
        R"({"type":"FxPress","deck":"master","slot":1,"value":0.3})",
        R"({"type":"LayerPlay","deck":0,"layer":0,"value":"Release"})",
    };
    for (const char* c : cases) {
        const auto a = P(c);
        CHECK(a.ok);
        const auto b = command_from_json(command_to_json(a.cmd), kLim);
        CHECK(b.ok && std::memcmp(&a.cmd, &b.cmd, sizeof(Command)) == 0);
        if (!b.ok) std::printf("    %s -> %s: %s\n", c, command_to_json(a.cmd).dump().c_str(), b.error.c_str());
    }

    // the transport section as JSON
    auto t = std::make_unique<TransportSection>();
    std::memset(t.get(), 0, sizeof(TransportSection));
    t->clock.bpm = 128.f;
    t->clock.grid = static_cast<uint8_t>(GridDiv::Bar);
    t->clock.beat = 10.0;
    t->clock.time_us = 1000000;
    t->layer[1][2].mode = static_cast<uint8_t>(PlayMode::Random);
    t->layer[1][2].snap = kNone;
    t->layer[1][2].out = 0.75f;
    t->layer[1][2].phase = static_cast<uint8_t>(PlayheadPhase::InnerLoop);
    for (float& q : t->layer[1][2].cue) q = -1.f;
    t->layer[1][2].cue[3] = 0.5f;
    t->hit.clip = 2;
    t->hit_transport.mode = 99;  // garbage never crashes
    const json tj = transport_to_json(*t);
    CHECK(tj["clock"]["bpm"].get<double>() == 128.0 && tj["clock"]["grid"] == "Bar");
    CHECK(tj["layer"].size() == 2 && tj["layer"][1].size() == 4);
    CHECK(tj["layer"][1][2]["mode"] == "Random" && tj["layer"][1][2]["snap"] == "Global");
    CHECK(tj["layer"][1][2]["phase"] == "InnerLoop" && tj["layer"][1][2]["out"].get<double>() == 0.75);
    CHECK(tj["layer"][1][2]["cue"].size() == 16 && tj["layer"][1][2]["cue"][3].get<double>() == 0.5 &&
          tj["layer"][1][2]["cue"][0].get<double>() == -1.0);
    CHECK(tj["hit"]["clip"] == 2 && !tj["hit"].contains("fx") && tj["hit_transport"]["mode"] == "?");
    CHECK(!tj["clock"].contains("beat"));
    const double beat = clock_to_json(t->clock, 1000000 + 500000)["beat"].get<double>();  // 0.5 s at 128 BPM
    CHECK(std::fabs(beat - (10.0 + 128.0 / 120.0)) < 1e-3);
    const json names = transport_names();
    CHECK(names["params"].size() == static_cast<size_t>(LayerParam::Count));
    CHECK(names["params"][0]["name"] == "Mode" && names["params"][0]["values"].size() == static_cast<size_t>(PlayMode::Count));
    CHECK(names["play"].size() == static_cast<size_t>(PlayAction::Count));
    // every LayerSet name the page gets is accepted again
    for (const auto& p : names["params"]) {
        json cmd = {{"type", "LayerSet"}, {"deck", 0}, {"layer", 0}, {"param", p["name"]}};
        if (p["kind"] == "enum") cmd["value"] = p["values"].back();
        else if (p["kind"] == "float") cmd["value"] = p["lo"];
        else cmd["value"] = 1;
        CHECK(command_from_json(cmd, kLim).ok);
    }
    // the press field in the live view
    auto s = std::make_unique<LiveState>();
    std::memset(s.get(), 0, sizeof(LiveState));
    s->master_fx[0].effect = 1;
    s->master_fx[0].press = 255;
    CHECK(live_to_json(*s)["master_fx"][0]["press"].get<double>() == 1.0);
    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    c->effect_count = 2;
    c->effect[1].reserved[0] = 1;
    const json cj = catalog_to_json(*c);
    CHECK(cj["effects"][0]["kind"] == "effect" && cj["effects"][1]["kind"] == "generator");
}

static void test_http() {
    std::printf("http helpers\n");
    HttpRequest r;
    CHECK(parse_http_head("GET /preview/2?since=42 HTTP/1.1\r\nHost: 127.0.0.1:8790\r\nConnection: keep-alive\r\n", r));
    CHECK(r.method == "GET" && r.path == "/preview/2" && r.query == "since=42" && r.keep_alive);
    CHECK(query_param(r.query, "since") == "42" && query_param(r.query, "x").empty());
    CHECK(query_param("a=1&since=7&b", "b").empty() && query_param("a=1&since=7", "since") == "7");
    CHECK(parse_http_head("POST /cmd HTTP/1.1\r\nhost: localhost:8790\r\nContent-Type: Application/JSON\r\n"
                          "content-length: 17\r\nConnection: close\r\n",
                          r));
    CHECK(r.method == "POST" && r.content_length == 17 && !r.keep_alive && r.content_type == "application/json");
    CHECK(parse_http_head("GET / HTTP/1.0\r\n", r) && !r.keep_alive);
    CHECK(!parse_http_head("GARBAGE\r\n", r));
    CHECK(!parse_http_head("GET nopath HTTP/1.1\r\n", r));
    CHECK(!parse_http_head("POST /cmd HTTP/1.1\r\nContent-Length: abc\r\n", r));
    CHECK(safe_static_path("/app.js") && safe_static_path("/img/a-b_c.png"));
    CHECK(!safe_static_path("/../secret") && !safe_static_path("/a\\b") && !safe_static_path("/a%2e") &&
          !safe_static_path("//x") && !safe_static_path("x"));
    CHECK(std::string(content_type_for("/x.js")).find("javascript") != std::string::npos);
    CHECK(host_is_local("127.0.0.1:8790") && host_is_local("localhost") && host_is_local("[::1]:8790"));
    CHECK(!host_is_local("evil.example:8790") && !host_is_local("") && !host_is_local("127.0.0.1.evil.example"));
    CHECK(origin_is_local("") && origin_is_local("http://127.0.0.1:8790") && origin_is_local("http://localhost:8790"));
    CHECK(!origin_is_local("http://evil.example") && !origin_is_local("http://localhost.evil.example") &&
          !origin_is_local("null"));
}

int main() {
    test_layer_commands();
    test_fx_commands();
    test_mixer_commands();
    test_bad_input();
    test_round_trip();
    test_state_json();
    test_transport_commands();
    test_http();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
