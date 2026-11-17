// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// OSC in (M13): the address map, the allow list and the helpers. docs/interfaces/osc.md.
#include "himmel/osc/osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "himmel/protocol/show.h"

namespace himmel::osc {

using namespace himmel::protocol;

const char* parse_name(Parse p) {
    static const char* k[] = {"ok", "empty", "too big", "not 4-byte aligned", "bad address", "bad type tags",
                              "unknown type", "too many arguments", "truncated", "bad string", "bad bundle"};
    return static_cast<size_t>(p) < sizeof(k) / sizeof(k[0]) ? k[static_cast<size_t>(p)] : "?";
}
const char* map_name(Map m) {
    static const char* k[] = {"ok", "unknown address", "bad arguments", "engine not running", "not supported yet"};
    return static_cast<size_t>(m) < sizeof(k) / sizeof(k[0]) ? k[static_cast<size_t>(m)] : "?";
}
const char* verdict_name(Verdict v) {
    static const char* k[] = {"applied", "refused", "blocked", "garbage", "rate limited"};
    return static_cast<size_t>(v) < sizeof(k) / sizeof(k[0]) ? k[static_cast<size_t>(v)] : "?";
}

namespace {

// A number argument: int or float, finite.
bool number(const Message& m, int k, float& out) {
    if (k >= m.argc) return false;
    const Arg& a = m.argv[k];
    if (a.type == ArgType::Int) out = static_cast<float>(a.i);
    else if (a.type == ArgType::Float) out = a.f;
    else return false;
    return std::isfinite(out);
}
// A whole number in [lo, hi] (an int, or a float that is a whole number, as many apps send).
bool whole(const Message& m, int k, int lo, int hi, int& out) {
    float f;
    if (!number(m, k, f) || f != std::floor(f) || f < static_cast<float>(lo) || f > static_cast<float>(hi)) return false;
    out = static_cast<int>(f);
    return true;
}
// 0..1 (a fader may overshoot by a hair: clamped within 0.001).
bool unit(const Message& m, int k, float& out) {
    if (!number(m, k, out) || out < -0.001f || out > 1.001f) return false;
    out = out < 0.0f ? 0.0f : out > 1.0f ? 1.0f : out;
    return true;
}
// On / off: int 0 / 1, float 0 / 1, T / F.
bool onoff(const Message& m, int k, bool& out) {
    if (k >= m.argc) return false;
    const Arg& a = m.argv[k];
    if (a.type == ArgType::True || a.type == ArgType::False) return out = a.type == ArgType::True, true;
    int v;
    if (!whole(m, k, 0, 1, v)) return false;
    out = v == 1;
    return true;
}
bool is(const Message& m, const char* addr) { return std::strcmp(m.address, addr) == 0; }

}  // namespace

Map map_message(const Message& m, const EngineView& v, Command (&out)[kMaxCommandsPerMessage], int& count) {
    count = 0;
    const uint8_t prog = v.program_deck < kDecks ? v.program_deck : 0;
    const uint8_t prev = static_cast<uint8_t>(prog ^ 1);
    auto emit = [&](const Command& c) { out[count++] = c; };
    auto needs_engine = [&] { return !v.known; };
    if (std::strncmp(m.address, "/himmel/", 8) != 0) return Map::UnknownAddress;

    // ---- scenes (SHOW, command-protocol.md §4b) ----
    if (is(m, "/himmel/go") || is(m, "/himmel/back") || is(m, "/himmel/go/preview")) {
        if (m.argc != 0) return Map::BadArgs;
        if (needs_engine()) return Map::NoEngine;
        const bool preview = is(m, "/himmel/go/preview");
        emit(make(CommandType::SceneRecall, preview ? prev : prog, kNone, is(m, "/himmel/back") ? kScenePrev : kSceneNext,
                  preview ? 0.0f : -1.0f));
        return Map::Ok;
    }
    if (is(m, "/himmel/scene") || is(m, "/himmel/scene/preview")) {
        int n;
        if (m.argc != 1 || !whole(m, 0, 1, kScenePads, n)) return Map::BadArgs;
        if (needs_engine()) return Map::NoEngine;
        const bool preview = is(m, "/himmel/scene/preview");
        // Live: on the beat with the scene's own transition; PREVIEW: a cut, at once.
        emit(make(CommandType::SceneRecall, preview ? prev : prog, kNone, n - 1, preview ? 0.0f : -1.0f));
        return Map::Ok;
    }
    if (is(m, "/himmel/song")) {
        if (m.argc != 1) return Map::BadArgs;
        if (m.argv[0].type == ArgType::String) return Map::Unsupported;  // GUESS: by name later (needs the song list)
        int n;
        if (!whole(m, 0, 1, 64, n)) return Map::BadArgs;
        Command c = make(CommandType::ShowSelect, kNone, kNone, n - 1);
        c.param = static_cast<uint8_t>(SelectWhat::Song);
        emit(c);
        return Map::Ok;
    }
    if (is(m, "/himmel/undo") || is(m, "/himmel/redo")) {
        if (m.argc != 0) return Map::BadArgs;
        emit(make(CommandType::Undo, kNone, kNone, is(m, "/himmel/redo") ? 1 : 0));
        return Map::Ok;
    }
    // ---- the mixer ----
    if (is(m, "/himmel/take")) {
        int ms = 0;  // 0 = the show's default
        if (m.argc > 1 || (m.argc == 1 && !whole(m, 0, 0, 60000, ms))) return Map::BadArgs;
        emit(make(CommandType::Take, kNone, kNone, ms));
        return Map::Ok;
    }
    if (is(m, "/himmel/cut")) {
        if (m.argc != 0) return Map::BadArgs;
        emit(make(CommandType::Cut));
        return Map::Ok;
    }
    if (is(m, "/himmel/black")) {
        bool on = false;
        if (m.argc > 1 || (m.argc == 1 && !onoff(m, 0, on))) return Map::BadArgs;
        emit(make(CommandType::Black, kNone, kNone, m.argc == 0 ? kToggle : on ? kOn : kOff));
        return Map::Ok;
    }
    if (is(m, "/himmel/xfade")) {
        float x;
        if (m.argc != 1 || !unit(m, 0, x)) return Map::BadArgs;
        emit(make(CommandType::Crossfader, kNone, kNone, 0, x));
        return Map::Ok;
    }
    if (is(m, "/himmel/master")) {
        float x;
        if (m.argc != 1 || !unit(m, 0, x)) return Map::BadArgs;
        return Map::Unsupported;  // the engine has no master dimmer yet (ENGINE)
    }
    // ---- macros: /himmel/macro/1..4 (always the PROGRAM scene) ----
    if (std::strncmp(m.address, "/himmel/macro/", 14) == 0) {
        const char d = m.address[14];
        if (d < '1' || d > '4' || m.address[15] != 0) return Map::UnknownAddress;
        float x;
        if (m.argc != 1 || !unit(m, 0, x)) return Map::BadArgs;
        Command c = make(CommandType::MacroValue, kNone, kNone, 0, x);
        c.slot = static_cast<uint8_t>(d - '1');
        emit(c);
        return Map::Ok;
    }
    // ---- the clock ----
    if (is(m, "/himmel/bpm")) {
        float b;
        if (m.argc != 1 || !number(m, 0, b) || b < 20.0f || b > 300.0f) return Map::BadArgs;
        Command c = make(CommandType::Clock, kNone, kNone, 0, b);
        c.param = static_cast<uint8_t>(ClockParam::Bpm);
        emit(c);
        return Map::Ok;
    }
    if (is(m, "/himmel/beat")) {
        if (m.argc != 0) return Map::BadArgs;
        Command c = make(CommandType::Clock);  // GUESS: a beat pulse = a tap (the tap lands on a beat)
        c.param = static_cast<uint8_t>(ClockParam::Tap);
        emit(c);
        return Map::Ok;
    }
    return Map::UnknownAddress;
}

// ============================================================================ allow list
bool parse_ipv4(const char* s, size_t n, uint32_t& out) {
    uint32_t v = 0;
    int parts = 0;
    size_t k = 0;
    while (parts < 4) {
        if (k >= n || s[k] < '0' || s[k] > '9') return false;
        uint32_t p = 0;
        int digits = 0;
        while (k < n && s[k] >= '0' && s[k] <= '9') {
            p = p * 10 + static_cast<uint32_t>(s[k] - '0');
            if (++digits > 3 || p > 255) return false;
            ++k;
        }
        v = (v << 8) | p;
        if (++parts < 4) {
            if (k >= n || s[k] != '.') return false;
            ++k;
        }
    }
    if (k != n) return false;
    out = v;
    return true;
}

bool AllowList::parse(const char* list) {
    AllowList a;
    if (!list) return false;
    const size_t n = std::strlen(list);
    if (n == 0) return false;
    if (n == 3 && std::strcmp(list, "any") == 0) {
        a.any = true;
        *this = a;
        return true;
    }
    size_t k = 0;
    while (k < n) {
        size_t e = k;
        while (e < n && list[e] != ',') ++e;
        size_t b = k, f = e;
        while (b < f && list[b] == ' ') ++b;
        while (f > b && list[f - 1] == ' ') --f;
        if (b == f) return false;
        size_t slash = b;
        while (slash < f && list[slash] != '/') ++slash;
        uint32_t ip;
        if (!parse_ipv4(list + b, slash - b, ip)) return false;
        int bits = 32;
        if (slash < f) {
            bits = 0;
            if (slash + 1 == f || f - slash - 1 > 2) return false;
            for (size_t j = slash + 1; j < f; ++j) {
                if (list[j] < '0' || list[j] > '9') return false;
                bits = bits * 10 + (list[j] - '0');
            }
            if (bits > 32) return false;
        }
        if (a.count >= kMax) return false;
        const uint32_t mk = bits == 0 ? 0u : 0xFFFFFFFFu << (32 - bits);
        a.net[a.count] = ip & mk;
        a.mask[a.count] = mk;
        ++a.count;
        k = e + 1;
        if (e == n - 1) return false;  // a trailing comma
    }
    *this = a;
    return true;
}

bool AllowList::allows(uint32_t ip) const {
    if (any) return true;
    if (count == 0) return ip == 0x7F000001u;  // only this PC
    for (int k = 0; k < count; ++k)
        if ((ip & mask[k]) == net[k]) return true;
    return false;
}

// ============================================================================ build
size_t build_message(const char* address, const Arg* args, int argc, uint8_t* out, size_t cap) {
    size_t n = 0;
    auto str = [&](const char* s) {
        size_t len = 0;
        for (; s[len]; ++len)
            if (static_cast<unsigned char>(s[len]) < 32 || static_cast<unsigned char>(s[len]) > 126) return false;
        const size_t padded = (len + 4) & ~size_t(3);
        if (n + padded > cap) return false;
        std::memcpy(out + n, s, len);
        std::memset(out + n + len, 0, padded - len);
        n += padded;
        return true;
    };
    auto be = [&](uint32_t v) {
        if (n + 4 > cap) return false;
        out[n] = static_cast<uint8_t>(v >> 24), out[n + 1] = static_cast<uint8_t>(v >> 16);
        out[n + 2] = static_cast<uint8_t>(v >> 8), out[n + 3] = static_cast<uint8_t>(v);
        n += 4;
        return true;
    };
    if (!address || address[0] != '/' || argc < 0 || argc > kMaxArgs || !str(address)) return 0;
    char tags[kMaxArgs + 2] = {','};
    for (int k = 0; k < argc; ++k) {
        static const char kTag[] = {'i', 'f', 's', 'T', 'F', 'N'};
        tags[k + 1] = kTag[static_cast<int>(args[k].type)];
    }
    tags[argc + 1] = 0;
    if (!str(tags)) return 0;
    for (int k = 0; k < argc; ++k) {
        const Arg& a = args[k];
        uint32_t u;
        switch (a.type) {
            case ArgType::Int:
                if (!be(static_cast<uint32_t>(a.i))) return 0;
                break;
            case ArgType::Float:
                std::memcpy(&u, &a.f, 4);
                if (!be(u)) return 0;
                break;
            case ArgType::String:
                if (!str(a.s)) return 0;
                break;
            default: break;
        }
    }
    return n;
}

// ============================================================================ describe
void describe(const Message& m, char* out, size_t cap) {
    if (!cap) return;
    size_t n = 0;
    auto put = [&](const char* s) {
        for (; *s && n + 1 < cap; ++s) out[n++] = *s;
    };
    put(m.address);
    char b[48];
    for (int k = 0; k < m.argc; ++k) {
        const Arg& a = m.argv[k];
        switch (a.type) {
            case ArgType::Int: std::snprintf(b, sizeof(b), " %d", a.i); break;
            case ArgType::Float: std::snprintf(b, sizeof(b), " %.3g", static_cast<double>(a.f)); break;
            case ArgType::String: std::snprintf(b, sizeof(b), " \"%.40s\"", a.s); break;
            case ArgType::True: std::snprintf(b, sizeof(b), " T"); break;
            case ArgType::False: std::snprintf(b, sizeof(b), " F"); break;
            default: std::snprintf(b, sizeof(b), " N"); break;
        }
        put(b);
    }
    out[n] = 0;
}

}  // namespace himmel::osc
