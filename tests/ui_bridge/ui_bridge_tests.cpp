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
    CHECK(!P(R"({"type":"LayerPlay","deck":0,"layer":0,"value":4})").ok);
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
    test_http();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
