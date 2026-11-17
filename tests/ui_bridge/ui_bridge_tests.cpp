// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_ui: JSON <-> Command conversion, validation and the HTTP helpers (no Windows, no engine).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "himmel/ui/json_codec.h"
#include "himmel/ui/panels_json.h"
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
    CHECK(!P(R"({"type":"LayerSet","deck":0,"layer":1,"param":30,"value":1})").ok);
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

// ---- M11 part 2: show / output commands and the panel JSON ----
static void test_panel_commands() {
    using namespace himmel::protocol;
    // SceneRecall
    auto r = P(R"({"type":"SceneRecall","deck":"B","value":3})");
    CHECK(r.ok && r.cmd.type == CommandType::SceneRecall && r.cmd.deck == 1 && r.cmd.i == 3 && r.cmd.slot == kNone);
    CHECK(r.cmd.f[0] == -1.0f);   // default: the scene's own transition
    r = P(R"({"type":"SceneRecall","deck":"A","value":"next","f":0})");
    CHECK(r.ok && r.cmd.i == kSceneNext && r.cmd.f[0] == 0.0f);
    r = P(R"({"type":"SceneRecall","deck":"A","value":"prev","slot":2})");
    CHECK(r.ok && r.cmd.i == kScenePrev && r.cmd.slot == 2);
    CHECK(!P(R"({"type":"SceneRecall","deck":"A","value":16})").ok);
    CHECK(!P(R"({"type":"SceneRecall","value":1})").ok);
    CHECK(!P(R"({"type":"SceneRecall","deck":"hit","value":1})").ok);
    // SceneSave
    r = P(R"({"type":"SceneSave","deck":"A"})");
    CHECK(r.ok && r.cmd.i == -1 && !(r.cmd.flags & kVariation));
    r = P(R"({"type":"SceneSave","deck":1,"value":5,"variation":true})");
    CHECK(r.ok && r.cmd.deck == 1 && r.cmd.i == 5 && (r.cmd.flags & kVariation));
    CHECK(!P(R"({"type":"SceneSave","deck":0,"variation":"yes"})").ok);
    // Undo
    CHECK(P(R"({"type":"Undo"})").ok && P(R"({"type":"Undo"})").cmd.i == 0);
    CHECK(P(R"({"type":"Undo","value":"redo"})").cmd.i == 1);
    CHECK(!P(R"({"type":"Undo","value":2})").ok);
    // SceneReset
    r = P(R"({"type":"SceneReset","deck":"A","layer":2,"f":0.5})");
    CHECK(r.ok && r.cmd.layer == 2 && r.cmd.f[0] == 0.5f);
    CHECK(P(R"({"type":"SceneReset","deck":"A"})").cmd.layer == kNone);
    CHECK(!P(R"({"type":"SceneReset","deck":"A","f":-1})").ok);
    // Dice
    r = P(R"({"type":"Dice","param":"Mutate","deck":"A","f":0.4})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(DiceVerb::Mutate) && r.cmd.f[0] == 0.4f && r.cmd.layer == kNone);
    CHECK(!P(R"({"type":"Dice","param":"Shake","deck":"A"})").ok);
    CHECK(!P(R"({"type":"Dice","deck":"A"})").ok);
    // Locks
    r = P(R"({"type":"Locks","param":"Toggle","value":["Clip","Fx"]})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(LockOp::Toggle) && r.cmd.i == (kLockClip | kLockFx));
    CHECK(P(R"({"type":"Locks","param":"Clear","value":"all"})").cmd.i == static_cast<int32_t>(kLockAll));
    CHECK(P(R"({"type":"Locks","param":"Set","value":4})").cmd.i == 4);
    CHECK(!P(R"({"type":"Locks","param":"Set","value":["Banana"]})").ok);
    CHECK(!P(R"({"type":"Locks","param":"Set","value":256})").ok);
    // Project + the text handshake
    r = P(R"({"type":"Project","param":"Save"})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(ProjectAction::Save) && !r.needs_text);
    r = P(R"({"type":"Project","param":"SaveAs","text":"My show"})");
    CHECK(r.ok && r.needs_text && r.text == "My show");
    CHECK(!P(R"({"type":"Project","param":"SaveAs"})").ok);
    CHECK(!P(R"({"type":"Project","param":"Save","text":"x"})").ok);
    r = P(R"({"type":"Project","param":"Open","slot":2})");
    CHECK(r.ok && r.cmd.slot == 2 && !r.needs_text);
    r = P(R"({"type":"Project","param":"Open","text":"C:\\shows\\a.himmel"})");
    CHECK(r.ok && r.cmd.slot == kNone && r.needs_text);
    CHECK(!P(R"({"type":"Project","param":"Open"})").ok);
    CHECK(!P(R"({"type":"Project","param":"Open","slot":1,"text":"x"})").ok);
    CHECK(!P(R"({"type":"Project","param":"Open","slot":10})").ok);
    CHECK(P(R"({"type":"Project","param":"New","value":1})").cmd.i == 1);
    CHECK(!P(R"({"type":"Project","param":"New","value":2})").ok);
    CHECK(P(R"({"type":"Project","param":"Export"})").ok);
    CHECK(!P(R"({"type":"Project","param":"SaveAs","text":""})").ok);
    CHECK(!P(R"({"type":"Project","param":"SaveAs","text":"a\nb"})").ok);
    {
        json j = {{"type", "Project"}, {"param", "SaveAs"}, {"text", std::string(kPathLen, 'x')}};
        CHECK(!command_from_json(j, kLim).ok);
    }
    // ShowSelect
    r = P(R"({"type":"ShowSelect","param":"Bank","value":1,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && r.cmd.i == 1);
    CHECK(P(R"({"type":"ShowSelect","param":"Bank","value":15})").ok);
    CHECK(!P(R"({"type":"ShowSelect","param":"Bank","value":16})").ok);
    CHECK(P(R"({"type":"ShowSelect","param":"UseSetlist","value":"toggle"})").cmd.i == 2);
    CHECK(!P(R"({"type":"ShowSelect","param":"UseSetlist","value":1,"relative":true})").ok);
    // KitSet
    r = P(R"({"type":"KitSet","deck":"A","slot":3,"param":"Clip","layer":"Layer2","value":5})");
    CHECK(r.ok && r.cmd.slot == 3 && r.cmd.param == static_cast<uint8_t>(PadKind::Clip) &&
          r.cmd.layer == static_cast<uint8_t>(PadTarget::Layer2) && r.cmd.i == 5);
    r = P(R"({"type":"KitSet","deck":"A","slot":0,"param":"Action","value":"Freeze"})");
    CHECK(r.ok && r.cmd.i == static_cast<int32_t>(PadAction::Freeze) && r.cmd.layer == 0);
    CHECK(P(R"({"type":"KitSet","deck":"A","slot":0,"param":"Empty"})").ok);
    CHECK(!P(R"({"type":"KitSet","deck":"A","slot":16,"param":"Clip","value":1})").ok);
    // MacroSet / MacroValue
    r = P(R"({"type":"MacroSet","deck":"A","slot":1,"param":0,"layer":2,"value":17,"f":[0.2,0.8]})");
    CHECK(r.ok && r.cmd.slot == 1 && r.cmd.param == 0 && r.cmd.layer == 2 && r.cmd.i == 17 && r.cmd.f[1] == 0.8f);
    CHECK(P(R"({"type":"MacroSet","deck":"A","slot":1,"param":0,"value":3})").cmd.f[1] == 1.0f);
    r = P(R"({"type":"MacroSet","deck":"A","slot":2,"text":"WOBBLE"})");
    CHECK(r.ok && r.cmd.param == kNone && r.needs_text);
    CHECK(P(R"({"type":"MacroSet","deck":"A","slot":2,"param":1,"clear":true})").ok);
    CHECK(!P(R"({"type":"MacroSet","deck":"A","slot":2})").ok);
    r = P(R"({"type":"MacroValue","slot":3,"f":1.7})");
    CHECK(r.ok && r.cmd.f[0] == 1.0f);
    r = P(R"({"type":"MacroValue","slot":0,"f":-0.1,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && r.cmd.f[0] == -0.1f);
    CHECK(!P(R"({"type":"MacroValue","slot":4,"f":0.5})").ok);
    // SongEdit
    r = P(R"({"type":"SongEdit","param":"RenameScene","slot":4,"text":"Drop"})");
    CHECK(r.ok && r.cmd.slot == 4 && r.needs_text && r.text == "Drop");
    CHECK(!P(R"({"type":"SongEdit","param":"RenameScene","slot":4})").ok);
    r = P(R"({"type":"SongEdit","param":"Bpm","value":2,"f":128})");
    CHECK(r.ok && r.cmd.i == 2 && r.cmd.f[0] == 128.0f);
    CHECK(!P(R"({"type":"SongEdit","param":"Bpm","value":2,"f":500})").ok);
    CHECK(P(R"({"type":"SongEdit","param":"New"})").ok);
    CHECK(P(R"({"type":"SongEdit","param":"AddScene","deck":"B","value":0})").cmd.deck == 1);
    CHECK(!P(R"({"type":"SongEdit","param":"AddScene","value":0})").ok);
    // Outputs
    r = P(R"({"type":"OutputBlack","slot":1,"value":"toggle"})");
    CHECK(r.ok && r.cmd.slot == 1 && r.cmd.i == 2);
    CHECK(P(R"({"type":"OutputBlack","value":1})").cmd.slot == kNone);
    CHECK(!P(R"({"type":"OutputBlack","slot":8,"value":1})").ok);
    CHECK(P(R"({"type":"Identify","value":"5s"})").cmd.i == 1);
    CHECK(P(R"({"type":"Identify","value":"Hold"})").cmd.i == 2);
    CHECK(!P(R"({"type":"Identify"})").ok);
    r = P(R"({"type":"LayerPlace","deck":"A","layer":0,"value":"Out","param":1})");
    CHECK(r.ok && r.cmd.i == static_cast<int32_t>(Place::Out) && r.cmd.param == 1);
    r = P(R"({"type":"LayerPlace","deck":"hit","value":"Free","f":[0.1,0.2,0.5,0.5]})");
    CHECK(r.ok && r.cmd.deck == kHitDeck && r.cmd.layer == 0 && r.cmd.f[3] == 0.5f);
    CHECK(!P(R"({"type":"LayerPlace","deck":"A","layer":0,"value":"Free","f":[0,0,0,0.5]})").ok);
    CHECK(!P(R"({"type":"LayerPlace","deck":"A","layer":0,"value":"Out"})").ok);
    CHECK(!P(R"({"type":"LayerPlace","deck":"A","layer":0,"value":"Fit","param":1})").ok);
    r = P(R"({"type":"OutputConfig","param":"Resolution","value":"P1440"})");
    CHECK(r.ok && r.cmd.i == static_cast<int32_t>(ResolutionPreset::P1440) && r.cmd.slot == kNone);
    CHECK(P(R"({"type":"OutputConfig","param":"Refresh","value":"Hz50"})").cmd.i == 1);
    CHECK(P(R"({"type":"OutputConfig","param":"BlendGamma","f":9})").cmd.f[0] == 3.0f);
    CHECK(P(R"({"type":"OutputConfig","param":"BlendWidth","value":200})").cmd.i == 200);
    CHECK(!P(R"({"type":"OutputConfig","param":"BlendWidth","value":2000})").ok);
    CHECK(!P(R"({"type":"OutputConfig","param":"SimLost","value":1})").ok);
    CHECK(P(R"({"type":"OutputConfig","param":"SimLost","slot":1,"value":1})").ok);
    CHECK(!P(R"({"type":"OutputConfig","param":"Zoom","value":1})").ok);
    // Names
    CHECK(std::strcmp(command_type_name(CommandType::SceneRecall), "SceneRecall") == 0);
    CHECK(std::strcmp(command_type_name(CommandType::OutputConfig), "OutputConfig") == 0);
    const json names = panels_names();
    CHECK(names["dice"].size() == 5 && names["presets"][2] == "P1440" && names["locks"][0] == "Clip");
    // Existing commands are untouched by the panel parser.
    CHECK(P(R"({"type":"Cut"})").ok);
    CHECK(!P(R"({"type":"Nonsense"})").ok);
}

static void test_panel_json() {
    using namespace himmel::protocol;
    auto st = std::make_unique<ShowStatus>();
    auto fl = std::make_unique<FilesStatus>();
    std::strcpy(st->project, "Club night");
    st->dirty = 1;
    st->pad[2].used = 1;
    std::strcpy(st->pad[2].name, "Intro");
    st->pad[2].on_deck = 2;
    st->pad[2].thumb_rgb = 0x00ff8800;
    st->deck[0].scene = 4;
    st->deck[0].variation = 'b';
    st->locks = kLockClip | kLockMood;
    st->undo_depth = 3;
    st->acked_seq[1] = 77;
    st->rejected[1] = 2;
    st->last_reject[1] = static_cast<uint8_t>(Reject::BadValue);
    std::strcpy(st->message, "Saved");
    std::strcpy(fl->path, "C:\\shows\\Club night.himmel");
    fl->recent_count = 1;
    std::strcpy(fl->recent[0].name, "Club night");
    fl->missing_count = 12;   // more than kMissingShown
    std::strcpy(fl->missing[0], "sunset");
    json j = show_to_json(*st, *fl, 1);
    CHECK(j["present"] == true && j["project"] == "Club night" && j["dirty"] == true);
    CHECK(j["pads"].size() == 16 && j["pads"][2]["name"] == "Intro" && j["pads"][2]["on"][1] == true &&
          j["pads"][2]["rgb"] == 0xff8800 && j["pads"][0]["used"] == false && !j["pads"][0].contains("name"));
    CHECK(j["decks"][0]["scene"] == 4 && j["decks"][0]["variation"] == "b" && j["decks"][1]["variation"] == "");
    CHECK(j["locks"].size() == 2 && j["locks"][1] == "Mood");
    CHECK(j["acked"] == 77 && j["rejected"] == 2 && j["last_reject"] == "BadValue");
    CHECK(j["files"]["recent"].size() == 1 && j["files"]["missing_count"] == 12 && j["files"]["missing"].size() == 8);
    CHECK(!j.contains("time_us"));

    auto os = std::make_unique<OutputsSection>();
    os->count = 2;
    os->mode = 1;
    os->out[0].state = static_cast<uint8_t>(OutputState::Ok);
    std::strcpy(os->out[0].name, "DELL U2720Q");
    std::strcpy(os->out[0].id.maker, "DEL");
    os->out[1].state = static_cast<uint8_t>(OutputState::Lost);
    os->out[1].black = 1;
    os->out[1].preset = static_cast<uint8_t>(ResolutionPreset::P2160);
    os->identify_ms_left = 4321;
    os->layer[8].place = static_cast<uint8_t>(Place::Free);
    os->layer[8].rect[2] = 0.5f;
    j = outputs_to_json(*os);
    CHECK(j["outputs"].size() == 2 && j["mode"] == "Mirror");
    CHECK(j["outputs"][0]["state"] == "Ok" && j["outputs"][0]["display"]["maker"] == "DEL");
    CHECK(j["outputs"][1]["state"] == "Lost" && j["outputs"][1]["black"] == true && j["outputs"][1]["preset"] == "P2160");
    CHECK(j["identify_left_ms"] == 4250 && j["places"].size() == 9 && j["places"][8]["place"] == "Free");
    os->count = 99;   // a wild count never reads past the table
    CHECK(outputs_to_json(*os)["outputs"].size() == static_cast<size_t>(kMaxOutputs));

    himmel::audio::Signals sig{};
    sig.v[static_cast<int>(himmel::audio::Sig::Bass)] = 0.5f;
    sig.kicks = 9;
    himmel::audio::InputStatus in{};
    in.state = static_cast<uint8_t>(himmel::audio::InputState::Running);
    std::strcpy(in.device, "Microphone (USB)");
    himmel::audio::ClockSource clk[himmel::audio::kClockSources]{};
    clk[0].kind = static_cast<uint8_t>(himmel::audio::ClockKind::Midi);
    clk[0].run = static_cast<uint8_t>(himmel::audio::ClockRun::Running);
    clk[0].bpm = 120.0f;
    clk[0].beat = 4.0;
    clk[0].time_us = 1000000;
    himmel::audio::AudioConfig cfg{};
    j = audio_to_json(sig, in, clk, cfg, 0, 1500000);   // 0.5 s later at 120 BPM = +1 beat
    CHECK(j["sig"]["bass"] == 0.5 && j["hits"][0] == 9 && j["input"]["state"] == "Running");
    CHECK(j["clock"].size() == 1 && j["clock"][0]["kind"] == "Midi" && j["clock"][0]["beat"] == 5.0);
    CHECK(j["spectrum"].size() == 32);

    std::string err;
    CHECK(audio_config_from_json(json::parse(R"({"device":"Mic","mode":"Loopback","gain_db":99,"channel_count":2})"), cfg, err));
    CHECK(std::strcmp(cfg.device, "Mic") == 0 && cfg.mode == 1 && cfg.gain_db == 48.0f && cfg.channel_count == 2);
    himmel::audio::AudioConfig before = cfg;
    CHECK(!audio_config_from_json(json::parse(R"({"mode":"Stereo"})"), cfg, err) && cfg.mode == before.mode);
    CHECK(!audio_config_from_json(json::parse(R"({"volume":3})"), cfg, err));
    CHECK(!audio_config_from_json(json{{"device", std::string(200, 'x')}}, cfg, err));

    const json dev = parse_audio_list(
        "Capture endpoints (mic / line in):\r\n    Mic A (Maker)  (2 ch, 48000 Hz)\r\n  * Mic B  (1 ch, 44100 Hz)\r\n"
        "Render endpoints (for --loopback):\r\n  * Speakers (X)  (2 ch, 48000 Hz)\r\nMIDI inputs (for --midi-clock):\r\n"
        "    ROLI Lightpad BLOCK\r\n");
    CHECK(dev["capture"].size() == 2 && dev["capture"][0]["name"] == "Mic A (Maker)" && dev["capture"][0]["channels"] == 2);
    CHECK(dev["capture"][1]["default"] == true && dev["capture"][1]["rate"] == 44100);
    CHECK(dev["render"].size() == 1 && dev["midi"].size() == 1 && dev["midi"][0] == "ROLI Lightpad BLOCK");

    auto oscs = std::make_unique<himmel::osc::OscStatus>();
    oscs->port = 7000;
    oscs->total = 3;
    oscs->next = 3;
    std::strcpy(oscs->last[0].text, "/himmel/scene 1");
    std::strcpy(oscs->last[2].text, "/himmel/take");
    oscs->last[2].from_ip = 0x7F000001;
    oscs->last[2].verdict = static_cast<uint8_t>(himmel::osc::Verdict::Blocked);
    j = osc_to_json(*oscs);
    CHECK(j["last"].size() == 3 && j["last"][0]["text"] == "/himmel/take" && j["last"][0]["verdict"] == "Blocked" &&
          j["last"][0]["from"] == "127.0.0.1:0" && j["last"][2]["text"] == "/himmel/scene 1");

    LibraryCounts lc;
    lc.queued = 4;
    lc.catalog_full = 2;
    himmel::library::IngestJob job{};
    std::strcpy(job.name, "sunset.mp4");
    job.progress = 1.7f;
    j = library_block_to_json(lc, job);
    CHECK(j["queued"] == 4 && j["catalog_full"] == 2 && j["job"]["name"] == "sunset.mp4" && j["job"]["progress"] == 1.0);
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

static void test_part3() {
    using nlohmann::json;
    auto c = downsample_curve({0.1f, 0.2f, 0.4f, 0.1f, 0.0f, 0.2f}, 3);
    CHECK(c.size() == 3 && c[0] == 0.5f && c[1] == 1.0f && c[2] == 0.5f);
    CHECK(downsample_curve({}, 8).empty() && downsample_curve({1.0f}, 0).empty());
    CHECK(downsample_curve({0.3f, 0.6f}, 8).size() == 2);
    auto z = downsample_curve({0.0f, 0.0f}, 2);
    CHECK(z.size() == 2 && z[0] == 0.0f);
    auto nan = downsample_curve({std::numeric_limits<float>::quiet_NaN(), 2.0f}, 1);
    CHECK(nan.size() == 1 && nan[0] == 1.0f);

    CHECK(!show_doc_for_page(json::object())["ok"].get<bool>());
    CHECK(!show_doc_for_page(json{{"format", "other"}})["ok"].get<bool>());
    json doc = {{"format", "himmel-show/1"}, {"kind", "document"}, {"name", "My Show"}, {"bank", 2},
                {"use_setlist", true}, {"live", {{"decks", json::array()}}},
                {"banks", json::array({json{{"index", 2}, {"pads", json::array({nullptr, json{{"name", "S2"}}})}}, 7})},
                {"setlist", json::array({json{{"name", "Intro"}, {"bpm", 120}, {"scenes", json::array()}}})}};
    json j = show_doc_for_page(doc);
    CHECK(j["ok"].get<bool>() && j["name"] == "My Show" && j["bank"] == 2 && j["use_setlist"] == true);
    CHECK(!j.contains("live") && j["banks"].size() == 1 && j["banks"][0]["index"] == 2);
    CHECK(j["banks"][0]["pads"][1]["name"] == "S2" && j["setlist"][0]["bpm"] == 120);

    // The exact commands gui/build.js sends (the BUILD view), deck as a number.
    using namespace himmel::protocol;
    auto r = P(R"({"type":"KitSet","deck":1,"slot":12,"param":"Clip","layer":"Layer1","value":4})");
    CHECK(r.ok && r.cmd.type == CommandType::KitSet && r.cmd.deck == 1 && r.cmd.slot == 12 &&
          r.cmd.param == static_cast<uint8_t>(PadKind::Clip) && r.cmd.layer == static_cast<uint8_t>(PadTarget::Layer1) && r.cmd.i == 4);
    r = P(R"({"type":"KitSet","deck":0,"slot":2,"param":"Action","value":"Black"})");
    CHECK(r.ok && r.cmd.i == static_cast<int32_t>(PadAction::Black) && r.cmd.layer == 0);
    r = P(R"({"type":"KitSet","deck":0,"slot":3,"param":"Action","value":"NextScene"})");
    CHECK(r.ok && r.cmd.i == static_cast<int32_t>(PadAction::NextScene));
    r = P(R"({"type":"KitSet","deck":0,"slot":5,"param":"Empty"})");
    CHECK(r.ok && r.cmd.param == static_cast<uint8_t>(PadKind::Empty));
    r = P(R"({"type":"KitSet","deck":0,"slot":0,"param":"Clip","layer":"Hit","value":2})");
    CHECK(r.ok && r.cmd.layer == static_cast<uint8_t>(PadTarget::Hit));
    r = P(R"({"type":"MacroSet","deck":1,"slot":2,"param":0,"value":17,"f":[0.2,0.9]})");   // deck FX (no layer)
    CHECK(r.ok && r.cmd.layer == kNone && r.cmd.param == 0 && r.cmd.i == 17 && r.cmd.f[0] == 0.2f && r.cmd.f[1] == 0.9f);
    r = P(R"({"type":"MacroSet","deck":1,"slot":2,"param":1,"layer":3,"value":0,"f":[1,0]})");
    CHECK(r.ok && r.cmd.layer == 3 && r.cmd.f[0] == 1.0f && r.cmd.f[1] == 0.0f);
    r = P(R"({"type":"MacroSet","deck":1,"slot":2,"param":1,"clear":true})");
    CHECK(r.ok && (r.cmd.flags & kClear));
    r = P(R"({"type":"MacroSet","deck":1,"slot":0,"text":"INTENSITY"})");
    CHECK(r.ok && r.needs_text && r.text == "INTENSITY" && r.cmd.param == kNone);
    r = P(R"({"type":"SongEdit","param":"New"})");
    CHECK(r.ok && r.cmd.i == 0 && !r.needs_text);
    r = P(R"({"type":"SongEdit","param":"New","text":"Warmup"})");
    CHECK(r.ok && r.needs_text);
    r = P(R"({"type":"SongEdit","param":"Move","value":2,"slot":1})");
    CHECK(r.ok && r.cmd.i == 2 && r.cmd.slot == 1);
    r = P(R"({"type":"SongEdit","param":"Bpm","value":0,"f":128})");
    CHECK(r.ok && r.cmd.f[0] == 128.0f);
    r = P(R"({"type":"SongEdit","param":"Rename","slot":0,"text":"Glass"})");
    CHECK(r.ok && r.needs_text && r.cmd.slot == 0);
    r = P(R"({"type":"SongEdit","param":"AddScene","value":0,"deck":1})");
    CHECK(r.ok && r.cmd.deck == 1 && r.cmd.i == 0);
    r = P(R"({"type":"SongEdit","param":"RemoveScene","value":0,"slot":4})");
    CHECK(r.ok && r.cmd.slot == 4);
    r = P(R"({"type":"SongEdit","param":"Delete","value":1})");
    CHECK(r.ok && r.cmd.i == 1);
    r = P(R"({"type":"SceneRecall","deck":1,"value":4,"f":0})");
    CHECK(r.ok && r.cmd.deck == 1 && r.cmd.f[0] == 0.0f);
    r = P(R"({"type":"ShowSelect","param":"Song","value":2})");
    CHECK(r.ok && r.cmd.i == 2 && !(r.cmd.flags & kRelative));
    r = P(R"({"type":"ShowSelect","param":"UseSetlist","value":0})");
    CHECK(r.ok && r.cmd.i == 0);
    r = P(R"({"type":"OutputConfig","param":"BlendWidth","value":128})");
    CHECK(r.ok && r.cmd.i == 128);
    r = P(R"({"type":"Project","param":"New","value":3})");
    CHECK(r.ok && r.cmd.i == 3);

    // FxReact (AUDIO D131, command 40): the reaction editor's commands.
    r = P(R"({"type":"FxReact","deck":1,"layer":2,"slot":0,"param":1,"op":"Set","signal":"Kick","mode":"Trigger","curve":"Soft","quantise":"Quarter","hold":4,"f":[0.6,5,300,0.2]})");
    CHECK(r.ok && r.cmd.type == CommandType::FxReact && r.cmd.deck == 1 && r.cmd.layer == 2 && r.cmd.slot == 0 && r.cmd.param == 1);
    {
        const ReactArgs a = unpack_react(r.cmd.i);
        CHECK(a.op == ReactOp::Set && a.signal == ReactSignal::Kick && a.mode == ReactMode::Trigger && a.curve == ReactCurve::Soft &&
              a.quantise == ReactQuantise::Quarter && a.hold_q == 4 && react_args_ok(r.cmd.i));
        CHECK(r.cmd.f[0] == 0.6f && r.cmd.f[1] == 5.0f && r.cmd.f[2] == 300.0f && r.cmd.f[3] == 0.2f);
    }
    r = P(R"({"type":"FxReact","deck":"master","slot":2,"param":0,"op":"Set","signal":"Bass","f":[2,9999,-5,3]})");   // clamped
    CHECK(r.ok && r.cmd.deck == kMasterDeck && r.cmd.layer == kNone && r.cmd.f[0] == 1.0f && r.cmd.f[1] == kReactMaxMs &&
          r.cmd.f[2] == 0.0f && r.cmd.f[3] == 1.0f);
    r = P(R"({"type":"FxReact","deck":0,"slot":1,"param":3,"op":"Amount","f":-0.1,"relative":true})");
    CHECK(r.ok && (r.cmd.flags & kRelative) && unpack_react(r.cmd.i).op == ReactOp::Amount && r.cmd.f[0] == -0.1f);
    r = P(R"({"type":"FxReact","deck":0,"layer":0,"slot":0,"param":0,"op":"Range","f":[0.8,0.2]})");
    CHECK(r.ok && r.cmd.f[0] == 0.8f && r.cmd.f[1] == 0.2f);
    r = P(R"({"type":"FxReact","deck":0,"layer":0,"slot":0,"param":0,"op":"Range","f":[-1,0]})");
    CHECK(r.ok && r.cmd.f[0] < 0);
    r = P(R"({"type":"FxReact","deck":0,"layer":0,"slot":0,"op":"Default"})");
    CHECK(r.ok && r.cmd.param == kNone && unpack_react(r.cmd.i).op == ReactOp::Default);
    r = P(R"({"type":"FxReact","deck":0,"layer":0,"slot":0,"param":2,"op":"Off"})");
    CHECK(r.ok && r.cmd.param == 2);
    r = P(R"({"type":"FxReact","deck":0,"layer":0,"slot":0,"param":2,"op":"Signal","signal":"Drop"})");
    CHECK(r.ok && unpack_react(r.cmd.i).signal == ReactSignal::Drop);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"op":"Set","signal":"Kick","f":[1,1,1,1]})").ok);   // Set needs a knob
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":4,"op":"Off"})").ok);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Set","signal":"Kick","f":[1,1]})").ok);   // 4 numbers
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Signal"})").ok);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Set","signal":"Wobble","f":[1,1,1,1]})").ok);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Set","hold":17,"f":[1,1,1,1]})").ok);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Range","f":[0.1,0.2],"relative":true})").ok);
    CHECK(!P(R"({"type":"FxReact","deck":0,"slot":0,"param":0,"op":"Nope"})").ok);
    {
        json cj = command_to_json(P(R"({"type":"FxReact","deck":1,"layer":2,"slot":0,"param":1,"op":"Set","signal":"Kick","f":[0.6,5,300,0.2]})").cmd);
        CHECK(cj["op"] == "Set" && cj["signal"] == "Kick" && cj["param"] == 1 && cj["slot"] == 0);
    }
    {
        auto rs = std::make_unique<ReactionSection>();
        std::memset(rs.get(), 0, sizeof(ReactionSection));
        rs->gen = 7;
        ReactionEntry& e = rs->layer[1][2][0].p[1];
        e.source = static_cast<uint8_t>(ReactSource::Custom);
        e.signal = static_cast<uint8_t>(ReactSignal::Kick);
        e.amount = 0.6f;
        e.range_hi = 1.0f;
        e.env = 0.25f;
        e.value = 0.5f;
        json rj = react_to_json(*rs, false);
        CHECK(rj["gen"] == 7 && rj["layer"].size() == 2 && rj["layer"][1].size() == 4 && rj["layer"][1][2].size() == 4);
        CHECK(rj["layer"][1][2][0][1]["signal"] == "Kick" && rj["layer"][1][2][0][1]["source"] == "Custom" &&
              rj["layer"][1][2][0][1]["amount"] == 0.6);
        CHECK(rj["deck"][0].size() == 4 && rj["master"].size() == 4 && rj["master"][0][0]["signal"] == "None");
        json lj = react_to_json(*rs, true);
        CHECK(lj["layer"][1][2][0][1][0] == 0.25 && lj["layer"][1][2][0][1][1] == 0.5);
    }

    // SystemQuit (74): only "value": 1 passes.
    r = P(R"({"type":"SystemQuit","value":1})");
    CHECK(r.ok && r.cmd.type == CommandType::SystemQuit && r.cmd.f[0] == 1.0f);
    CHECK(!P(R"({"type":"SystemQuit"})").ok && !P(R"({"type":"SystemQuit","value":0.5})").ok && !P(R"({"type":"SystemQuit","value":0})").ok);

    // MEDIA ingest commands (library-db.md §10): the LIBRARY view's buttons.
    {
        namespace lib = himmel::library;
        lib::IngestCommand ic;
        std::string err;
        CHECK(ingest_command_from_json(json{{"type", "Import"}, {"path", "D:\\Clips\\Set 1"}, {"watch", true}}, ic, err));
        CHECK(ic.type == static_cast<uint16_t>(lib::IngestCommandType::Import) && (ic.flags & lib::kIngestWatch) &&
              std::string(ic.path) == "D:\\Clips\\Set 1" && ic.clip == 0);
        CHECK(ingest_command_from_json(json{{"type", "rescan"}}, ic, err) && ic.type == 61 && ic.clip == 0);
        CHECK(ingest_command_from_json(json{{"type", "Remove"}, {"clip", 42}}, ic, err) && ic.type == 62 && ic.clip == 42);
        CHECK(ingest_command_from_json(json{{"type", "Relink"}, {"clip", 7}, {"path", "\\\\nas\\v\\a.mov"}, {"force", true}}, ic, err) &&
              ic.type == 63 && (ic.flags & lib::kIngestForce));
        CHECK(ingest_command_from_json(json{{"type", "Reanalyse"}, {"clip", 0}, {"thumbs", true}}, ic, err) && (ic.flags & lib::kIngestThumbs));
        CHECK(ingest_command_from_json(json{{"type", "Retry"}}, ic, err) && ic.type == 65);
        CHECK(ingest_command_from_json(json{{"type", "Mode"}, {"mode", "Hold"}}, ic, err) && ic.type == 66 && ic.i == 3);
        CHECK(ingest_command_from_json(json{{"type", "Unwatch"}, {"path", "C:/x"}}, ic, err) && ic.type == 67);
        CHECK(!ingest_command_from_json(json{{"type", "Import"}}, ic, err));                         // no path
        CHECK(!ingest_command_from_json(json{{"type", "Import"}, {"path", "relative\\x"}}, ic, err)); // not absolute
        CHECK(!ingest_command_from_json(json{{"type", "Remove"}}, ic, err));                         // no clip
        CHECK(!ingest_command_from_json(json{{"type", "Remove"}, {"clip", -3}}, ic, err));
        CHECK(!ingest_command_from_json(json{{"type", "Mode"}, {"mode", "Turbo"}}, ic, err));
        CHECK(!ingest_command_from_json(json{{"type", "Delete"}}, ic, err));
        CHECK(!ingest_command_from_json(json{{"type", "Import"}, {"path", "C:\\" + std::string(1200, 'a')}}, ic, err));
        lib::IngestReply rep{};
        rep.seq = 5;
        rep.type = 62;
        rep.status = static_cast<uint8_t>(lib::IngestStatus::BadId);
        rep.clip = 9;
        std::snprintf(rep.message, sizeof rep.message, "no clip 9");
        json rj = ingest_reply_to_json(rep);
        CHECK(rj["seq"] == 5 && rj["type"] == "Remove" && rj["status"] == "BadId" && rj["ok"] == false && rj["message"] == "no clip 9");
    }
}

// GUI wave 7: the import path dialog and the queue ETA (pure helpers).
static void test_wave7() {
    using namespace himmel::ui;
    CHECK(import_kind_of("a.MOV") == "video" && import_kind_of("x.mp4") == "video" && import_kind_of("clip.webm") == "video");
    CHECK(import_kind_of("p.PNG") == "still" && import_kind_of("p.jpeg") == "still" && import_kind_of("logo.svg") == "vector");
    CHECK(import_kind_of("g.gif") == "animation" && import_kind_of("notes.txt").empty() && import_kind_of("noext").empty());
    CHECK(import_kind_of(".mov") == "video" && import_kind_of("trail.").empty());
    CHECK(parent_dir_of(R"(C:\Clips\Club)") == R"(C:\Clips)");
    CHECK(parent_dir_of(R"(C:\Clips\Club\)") == R"(C:\Clips)");
    CHECK(parent_dir_of(R"(C:\Clips)") == R"(C:\)");
    CHECK(parent_dir_of(R"(C:\)").empty() && parent_dir_of("C:").empty() && parent_dir_of("").empty());
    CHECK(parent_dir_of("D:/a/b") == "D:/a");
    CHECK(parent_dir_of(R"(\\srv\share)").empty() && parent_dir_of(R"(\\srv\share\x)") == R"(\\srv\share)");
    CHECK(eta_seconds(0, 3.0) == 0 && eta_seconds(4, 2.5) == 10 && eta_seconds(3, 0) == -1 && eta_seconds(3, -1) == -1);
    CHECK(eta_seconds(100000, 10.0) == 86400 && eta_seconds(2, 1e9) == -1);
    CHECK(url_decode("C%3A%5CClips%5CMy+Club") == R"(C:\Clips\My Club)");
    CHECK(url_decode("%E2%9C%93") == "\xE2\x9C\x93" && url_decode("100%") == "100%" && url_decode("%zz") == "%zz");

    // Library block v3 (MEDIA wave 7, library-db.md §11): the new commands and Import options.
    namespace lib = himmel::library;
    using nlohmann::json;
    lib::IngestCommand ic;
    std::string err;
    CHECK(ingest_command_from_json(json{{"type", "ClearQueue"}}, ic, err) && ic.type == 68 && ic.path[0] == 0);
    CHECK(ingest_command_from_json(json{{"type", "Library"}, {"path", R"(E:\Himmel Library)"}, {"move", true}}, ic, err) &&
          ic.type == 69 && (ic.flags & lib::kIngestMove) && std::string(ic.path) == R"(E:\Himmel Library)");
    CHECK(!ingest_command_from_json(json{{"type", "Library"}}, ic, err));   // needs a path
    CHECK(ingest_command_from_json(json{{"type", "Import"}, {"path", R"(D:\Clips)"}, {"height", 720}, {"hapq", true},
                                        {"skip_known", true}, {"dry_run", true}, {"fit", true}, {"conform_hz", 60}}, ic, err) &&
          ic.i == 720 && ic.conform_hz == 60 && (ic.flags & lib::kIngestHapQ) && (ic.flags & lib::kIngestSkipKnown) &&
          (ic.flags & lib::kIngestDryRun) && (ic.flags & lib::kIngestFit) && !(ic.flags & lib::kIngestList));
    CHECK(ingest_command_from_json(json{{"type", "Import"}, {"path", R"(C:\t\list.txt)"}, {"list", true}}, ic, err) &&
          (ic.flags & lib::kIngestList) && ic.i == 0 && ic.conform_hz == 0);
    CHECK(!ingest_command_from_json(json{{"type", "Import"}, {"path", R"(D:\Clips)"}, {"height", 100}}, ic, err));
    CHECK(!ingest_command_from_json(json{{"type", "Import"}, {"path", R"(D:\Clips)"}, {"height", 5000}}, ic, err));
    CHECK(!ingest_command_from_json(json{{"type", "Import"}, {"path", R"(D:\Clips)"}, {"conform_hz", 30}}, ic, err));
    CHECK(ingest_command_from_json(json{{"type", "Import"}, {"path", R"(D:\Clips)"}, {"height", 0}}, ic, err) && ic.i == 0);
    lib::IngestReply rep{};
    rep.seq = 9; rep.type = 60; rep.status = static_cast<uint8_t>(lib::IngestStatus::NoSpace);
    json rj = ingest_reply_to_json(rep);
    CHECK(rj["status"] == "NoSpace" && rj["ok"] == false && rj["type"] == "Import");
    rep.type = 69; rep.status = static_cast<uint8_t>(lib::IngestStatus::Busy);
    rj = ingest_reply_to_json(rep);
    CHECK(rj["status"] == "Busy" && rj["type"] == "Library");
    LibraryV3 v;
    v.stored_mode = 3; v.disk_low = 1; v.disk_free = 7300000000ull; v.disk_total = 500000000000ull; v.disk_reserve = 10000000000ull;
    v.library_bytes = 123; v.queue_bytes = 17200000000ull;
    auto prog = std::make_unique<lib::IngestProgress>();
    prog->done = 3; prog->total = 86; prog->clip = 41; prog->file_progress = 0.4567f; prog->eta_s = 1260;
    std::snprintf(prog->file, sizeof prog->file, "beach.mp4");
    std::snprintf(prog->error, sizeof prog->error, "The disk is full.");
    auto plan = std::make_unique<lib::IngestPlan>();
    plan->seq = 4; plan->producer = 1; plan->refused = 1; plan->clips = 86; plan->fit_clips = 0; plan->need_bytes = 17200000000ull;
    std::snprintf(plan->message, sizeof plan->message, "needs ~17.2 GB for 86 clips");
    auto rt = std::make_unique<lib::LibraryRoot>();
    std::snprintf(rt->path, sizeof rt->path, R"(E:\Lib)");
    rt->move_state = static_cast<uint32_t>(lib::MoveState::Copying); rt->move_progress = 0.25f;
    json lj;
    library_v3_to_json(lj, v, *prog, *plan, *rt, 1);
    CHECK(lj["stored_mode"] == "Hold" && lj["disk_low"] == true && lj["disk"]["free"] == 7300000000ull && lj["disk"]["queue_bytes"] == 17200000000ull);
    CHECK(lj["progress"]["done"] == 3 && lj["progress"]["total"] == 86 && lj["progress"]["file"] == "beach.mp4" &&
          lj["progress"]["eta_s"] == 1260 && std::fabs(lj["progress"]["file_progress"].get<double>() - 0.457) < 1e-9 &&
          lj["progress"]["error"] == "The disk is full.");
    CHECK(lj["plan"]["mine"] == true && lj["plan"]["refused"] == true && lj["plan"]["clips"] == 86 && lj["plan"]["message"] == "needs ~17.2 GB for 86 clips");
    CHECK(lj["root_info"]["path"] == R"(E:\Lib)" && lj["root_info"]["move_state"] == "Copying");
    library_v3_to_json(lj, v, *prog, *plan, *rt, 3);
    CHECK(lj["plan"]["mine"] == false);
    json nm = panels_names();
    CHECK(nm["ingest"].size() == 10 && nm["ingest"][8] == "ClearQueue" && nm["ingest"][9] == "Library");
}

int main() {
    test_layer_commands();
    test_fx_commands();
    test_mixer_commands();
    test_bad_input();
    test_round_trip();
    test_state_json();
    test_transport_commands();
    test_panel_commands();
    test_panel_json();
    test_http();
    test_part3();
    test_wave7();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
