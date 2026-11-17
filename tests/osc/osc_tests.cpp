// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// OSC in (M13): parser, address map, allow list, and a fuzz run that proves garbage
// never becomes a command (docs/interfaces/osc.md). No sockets, no engine.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "himmel/osc/osc.h"
#include "himmel/protocol/show.h"

using namespace himmel;
using namespace himmel::protocol;
using namespace himmel::osc;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static Arg I(int v) { Arg a{}; a.type = ArgType::Int; a.i = v; return a; }
static Arg Fl(float v) { Arg a{}; a.type = ArgType::Float; a.f = v; return a; }
static Arg S(const char* v) { Arg a{}; a.type = ArgType::String; std::snprintf(a.s, sizeof(a.s), "%s", v); return a; }
static Arg T() { Arg a{}; a.type = ArgType::True; return a; }

static std::vector<uint8_t> msg(const char* addr, std::vector<Arg> args = {}) {
    std::vector<uint8_t> b(kMaxPacket);
    const size_t n = build_message(addr, args.data(), static_cast<int>(args.size()), b.data(), b.size());
    b.resize(n);
    return b;
}
static std::vector<Message> parse_all(const std::vector<uint8_t>& p, Parse* r = nullptr) {
    std::vector<Message> out;
    const Parse x = parse_packet(p.data(), p.size(), [&](const Message& m) { out.push_back(m); });
    if (r) *r = x;
    return out;
}
static std::vector<uint8_t> bundle(const std::vector<std::vector<uint8_t>>& parts) {
    std::vector<uint8_t> b = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (const auto& p : parts) {
        const uint32_t n = static_cast<uint32_t>(p.size());
        b.push_back(static_cast<uint8_t>(n >> 24)), b.push_back(static_cast<uint8_t>(n >> 16));
        b.push_back(static_cast<uint8_t>(n >> 8)), b.push_back(static_cast<uint8_t>(n));
        b.insert(b.end(), p.begin(), p.end());
    }
    return b;
}

// What the engine would accept from these commands (a strict subset of command-protocol.md §5).
static bool command_ok(const Command& c) {
    if (c.seq != 0) return false;  // the sender numbers them
    if (!std::isfinite(c.f[0]) || !std::isfinite(c.f[1]) || !std::isfinite(c.f[2]) || !std::isfinite(c.f[3])) return false;
    switch (c.type) {
        case CommandType::SceneRecall:
            return c.deck < kDecks && ((c.i >= 0 && c.i < kScenePads) || c.i == kSceneNext || c.i == kScenePrev) &&
                   c.slot == kNone && (c.f[0] == 0.0f || c.f[0] == -1.0f);
        case CommandType::ShowSelect: return c.param == static_cast<uint8_t>(SelectWhat::Song) && c.i >= 0 && c.i < 64 && !(c.flags & kRelative);
        case CommandType::Undo: return c.i == 0 || c.i == 1;
        case CommandType::Take: return c.i >= 0 && c.i <= 60000;
        case CommandType::Cut: return true;
        case CommandType::Black: return c.i >= 0 && c.i <= 2;
        case CommandType::Crossfader: return c.f[0] >= 0.0f && c.f[0] <= 1.0f && !(c.flags & kRelative);
        case CommandType::MacroValue: return c.slot < 4 && c.f[0] >= 0.0f && c.f[0] <= 1.0f && !(c.flags & kRelative);
        case CommandType::Clock:
            return (c.param == static_cast<uint8_t>(ClockParam::Bpm) && c.f[0] >= 20.0f && c.f[0] <= 300.0f) ||
                   c.param == static_cast<uint8_t>(ClockParam::Tap);
        default: return false;  // OSC never sends anything else
    }
}

static Map map1(const char* addr, std::vector<Arg> args, Command* out1 = nullptr, int* count = nullptr, bool engine = true) {
    auto p = msg(addr, args);
    auto ms = parse_all(p);
    if (ms.size() != 1) return Map::Count;
    EngineView v;
    v.known = engine;
    v.program_deck = 1;  // B on air
    Command out[kMaxCommandsPerMessage];
    int n = 0;
    const Map r = map_message(ms[0], v, out, n);
    if (out1 && n) *out1 = out[0];
    if (count) *count = n;
    return r;
}

static void test_parser() {
    std::printf("parser\n");
    auto p = msg("/himmel/scene", {I(3)});
    CHECK(p.size() == 16 + 4 + 4);
    Parse r;
    auto ms = parse_all(p, &r);
    CHECK(r == Parse::Ok && ms.size() == 1 && std::strcmp(ms[0].address, "/himmel/scene") == 0 && ms[0].argc == 1 &&
          ms[0].argv[0].type == ArgType::Int && ms[0].argv[0].i == 3);
    ms = parse_all(msg("/x", {Fl(0.5f), S("hello"), T()}), &r);
    CHECK(r == Parse::Ok && ms.size() == 1 && ms[0].argc == 3 && ms[0].argv[0].f == 0.5f &&
          std::strcmp(ms[0].argv[1].s, "hello") == 0 && ms[0].argv[2].type == ArgType::True);
    // No type tag string at all (old senders): no arguments.
    std::vector<uint8_t> bare = {'/', 'h', 'i', 0};
    ms = parse_all(bare, &r);
    CHECK(r == Parse::Ok && ms.size() == 1 && ms[0].argc == 0);
    // Errors.
    CHECK(parse_packet(nullptr, 0, [](const Message&) {}) == Parse::Empty);
    std::vector<uint8_t> big(kMaxPacket + 4, 0);
    big[0] = '/';
    parse_all(big, &r);
    CHECK(r == Parse::TooBig);
    std::vector<uint8_t> odd = {'/', 'a', 0};
    parse_all(odd, &r);
    CHECK(r == Parse::BadAlign);
    std::vector<uint8_t> noslash = {'a', 'b', 0, 0};
    parse_all(noslash, &r);
    CHECK(r == Parse::BadAddress);
    std::vector<uint8_t> ctl = {'/', 'a', 7, 0};
    parse_all(ctl, &r);
    CHECK(r == Parse::BadAddress);
    std::vector<uint8_t> nonul = {'/', 'a', 'b', 'c'};
    parse_all(nonul, &r);
    CHECK(r == Parse::BadAddress);
    std::vector<uint8_t> badpad = {'/', 'a', 0, 1};
    parse_all(badpad, &r);
    CHECK(r == Parse::BadAddress);
    std::vector<uint8_t> blob = {'/', 'a', 0, 0, ',', 'b', 0, 0, 0, 0, 0, 0};
    parse_all(blob, &r);
    CHECK(r == Parse::UnknownType);
    std::vector<uint8_t> trunc = {'/', 'a', 0, 0, ',', 'i', 0, 0};
    parse_all(trunc, &r);
    CHECK(r == Parse::Truncated);
    std::vector<uint8_t> extra = msg("/a", {I(1)});
    extra.insert(extra.end(), {0, 0, 0, 0});
    parse_all(extra, &r);
    CHECK(r == Parse::Truncated);
    std::vector<uint8_t> notags = {'/', 'a', 0, 0, 'x', 0, 0, 0};
    parse_all(notags, &r);
    CHECK(r == Parse::BadTypeTags);
    // Too many arguments.
    std::vector<Arg> nine(9, I(1));
    std::vector<uint8_t> m9(256);
    CHECK(build_message("/a", nine.data(), 9, m9.data(), m9.size()) == 0);  // the builder refuses too
    // Bundles: in order, nested; a bad element drops the whole packet.
    auto b = bundle({msg("/himmel/go"), msg("/himmel/take"), bundle({msg("/himmel/cut")})});
    ms = parse_all(b, &r);
    CHECK(r == Parse::Ok && ms.size() == 3 && std::strcmp(ms[2].address, "/himmel/cut") == 0);
    auto bad = bundle({msg("/himmel/go"), std::vector<uint8_t>{'x', 0, 0, 0}});
    ms = parse_all(bad, &r);
    CHECK(r == Parse::BadAddress && ms.empty());  // the first, good message was NOT delivered
    auto lie = bundle({msg("/himmel/go")});
    lie[19] = 200;  // element size larger than the packet
    ms = parse_all(lie, &r);
    CHECK(r == Parse::BadBundle && ms.empty());
    std::vector<uint8_t> deep = msg("/himmel/go");
    for (int k = 0; k < kMaxBundleDepth + 1; ++k) deep = bundle({deep});
    ms = parse_all(deep, &r);
    CHECK(r == Parse::BadBundle && ms.empty());
    std::vector<std::vector<uint8_t>> many(kMaxMessagesPerPacket + 1, msg("/himmel/go"));
    ms = parse_all(bundle(many), &r);
    CHECK(r == Parse::BadBundle && ms.empty());
}

static void test_map() {
    std::printf("address map\n");
    Command c{};
    int n = 0;
    CHECK(map1("/himmel/go", {}, &c, &n) == Map::Ok && n == 1 && c.type == CommandType::SceneRecall && c.deck == 1 &&
          c.i == kSceneNext && c.f[0] < 0.0f);
    CHECK(map1("/himmel/back", {}, &c) == Map::Ok && c.i == kScenePrev && c.deck == 1);
    CHECK(map1("/himmel/go/preview", {}, &c) == Map::Ok && c.deck == 0 && c.i == kSceneNext && c.f[0] == 0.0f);
    CHECK(map1("/himmel/scene", {I(3)}, &c) == Map::Ok && c.deck == 1 && c.i == 2 && c.f[0] < 0.0f);
    CHECK(map1("/himmel/scene", {Fl(16.0f)}, &c) == Map::Ok && c.i == 15);  // a whole float is fine
    CHECK(map1("/himmel/scene", {Fl(2.5f)}) == Map::BadArgs);
    CHECK(map1("/himmel/scene", {I(0)}) == Map::BadArgs);
    CHECK(map1("/himmel/scene", {I(17)}) == Map::BadArgs);
    CHECK(map1("/himmel/scene", {S("3")}) == Map::BadArgs);
    CHECK(map1("/himmel/scene", {}) == Map::BadArgs);
    CHECK(map1("/himmel/scene", {I(1), I(2)}) == Map::BadArgs);
    CHECK(map1("/himmel/scene/preview", {I(1)}, &c) == Map::Ok && c.deck == 0 && c.i == 0 && c.f[0] == 0.0f);
    CHECK(map1("/himmel/scene", {I(3)}, nullptr, nullptr, false) == Map::NoEngine);
    CHECK(map1("/himmel/go", {}, nullptr, nullptr, false) == Map::NoEngine);
    CHECK(map1("/himmel/song", {I(2)}, &c) == Map::Ok && c.type == CommandType::ShowSelect && c.i == 1 &&
          c.param == static_cast<uint8_t>(SelectWhat::Song));
    CHECK(map1("/himmel/song", {S("Intro")}) == Map::Unsupported);
    CHECK(map1("/himmel/take", {}, &c) == Map::Ok && c.type == CommandType::Take && c.i == 0);
    CHECK(map1("/himmel/take", {I(1500)}, &c) == Map::Ok && c.i == 1500);
    CHECK(map1("/himmel/take", {I(-5)}) == Map::BadArgs);
    CHECK(map1("/himmel/cut", {}, &c) == Map::Ok && c.type == CommandType::Cut);
    CHECK(map1("/himmel/cut", {I(1)}) == Map::BadArgs);
    CHECK(map1("/himmel/black", {I(1)}, &c) == Map::Ok && c.type == CommandType::Black && c.i == kOn);
    CHECK(map1("/himmel/black", {Fl(0.0f)}, &c) == Map::Ok && c.i == kOff);
    CHECK(map1("/himmel/black", {T()}, &c) == Map::Ok && c.i == kOn);
    CHECK(map1("/himmel/black", {}, &c) == Map::Ok && c.i == kToggle);
    CHECK(map1("/himmel/black", {I(2)}) == Map::BadArgs);
    CHECK(map1("/himmel/macro/1", {Fl(0.25f)}, &c) == Map::Ok && c.type == CommandType::MacroValue && c.slot == 0 &&
          c.f[0] == 0.25f && !(c.flags & kRelative));
    CHECK(map1("/himmel/macro/4", {Fl(1.0005f)}, &c) == Map::Ok && c.slot == 3 && c.f[0] == 1.0f);
    CHECK(map1("/himmel/macro/4", {Fl(1.5f)}) == Map::BadArgs);
    CHECK(map1("/himmel/macro/5", {Fl(0.5f)}) == Map::UnknownAddress);
    CHECK(map1("/himmel/macro/12", {Fl(0.5f)}) == Map::UnknownAddress);
    CHECK(map1("/himmel/macro/", {Fl(0.5f)}) == Map::UnknownAddress);
    CHECK(map1("/himmel/macro/1", {Fl(std::nanf(""))}) == Map::BadArgs);
    CHECK(map1("/himmel/macro/1", {Fl(INFINITY)}) == Map::BadArgs);
    CHECK(map1("/himmel/xfade", {Fl(0.7f)}, &c) == Map::Ok && c.type == CommandType::Crossfader && c.f[0] == 0.7f);
    CHECK(map1("/himmel/master", {Fl(0.5f)}) == Map::Unsupported);
    CHECK(map1("/himmel/bpm", {Fl(128.0f)}, &c) == Map::Ok && c.type == CommandType::Clock &&
          c.param == static_cast<uint8_t>(ClockParam::Bpm) && c.f[0] == 128.0f);
    CHECK(map1("/himmel/bpm", {I(500)}) == Map::BadArgs);
    CHECK(map1("/himmel/beat", {}, &c) == Map::Ok && c.param == static_cast<uint8_t>(ClockParam::Tap));
    CHECK(map1("/himmel/undo", {}, &c) == Map::Ok && c.type == CommandType::Undo && c.i == 0);
    CHECK(map1("/himmel/redo", {}, &c) == Map::Ok && c.i == 1);
    CHECK(map1("/himmel/GO", {}) == Map::UnknownAddress);   // case-sensitive
    CHECK(map1("/himmel/go/", {}) == Map::UnknownAddress);
    CHECK(map1("/himmel/*", {}) == Map::UnknownAddress);    // no patterns in 0.1
    CHECK(map1("/other/go", {}) == Map::UnknownAddress);
    CHECK(map1("/himmel", {}) == Map::UnknownAddress);
}

static void test_allow() {
    std::printf("allow list\n");
    uint32_t ip;
    CHECK(parse_ipv4("192.168.1.20", 12, ip) && ip == 0xC0A80114u);
    CHECK(!parse_ipv4("192.168.1", 9, ip));
    CHECK(!parse_ipv4("192.168.1.256", 13, ip));
    CHECK(!parse_ipv4("1.2.3.4x", 8, ip));
    CHECK(!parse_ipv4("1..3.4", 6, ip));
    CHECK(!parse_ipv4("0001.2.3.4", 10, ip));
    AllowList a;
    CHECK(a.allows(0x7F000001u) && !a.allows(0xC0A80114u));  // default: this PC only
    CHECK(a.parse("127.0.0.1, 192.168.1.0/24,10.0.0.5"));
    CHECK(a.count == 3 && a.allows(0xC0A801FEu) && !a.allows(0xC0A80201u) && a.allows(0x0A000005u) && !a.allows(0x0A000006u));
    AllowList b = a;
    CHECK(!b.parse("192.168.1.0/33") && b.count == 3);  // a bad list changes nothing
    CHECK(!b.parse("192.168.1.0/") && !b.parse("") && !b.parse("1.2.3.4,") && !b.parse(",1.2.3.4") && !b.parse("hello"));
    CHECK(b.parse("any") && b.allows(0x01020304u));
    CHECK(b.parse("0.0.0.0/0") && b.allows(0xFFFFFFFFu));
}

// The fuzz run: random bytes, mutated real messages, random well-formed messages with random
// addresses and arguments. Nothing may crash, and every command that comes out must be one the
// engine accepts (command_ok); garbage (any parse error) must produce no message at all.
static void test_fuzz() {
    std::printf("fuzz\n");
    std::mt19937 rng(20261117);
    auto rnd = [&](uint32_t n) { return n ? rng() % n : 0u; };
    const char* addrs[] = {"/himmel/go", "/himmel/back", "/himmel/go/preview", "/himmel/scene", "/himmel/scene/preview",
                           "/himmel/song", "/himmel/take", "/himmel/cut", "/himmel/black", "/himmel/macro/1",
                           "/himmel/macro/4", "/himmel/macro/9", "/himmel/xfade", "/himmel/master", "/himmel/bpm",
                           "/himmel/beat", "/himmel/undo", "/himmel/redo", "/himmel/x", "/x"};
    std::vector<std::vector<uint8_t>> seeds;
    for (const char* a : addrs) seeds.push_back(msg(a, {I(3)})), seeds.push_back(msg(a, {Fl(0.5f)})), seeds.push_back(msg(a));
    seeds.push_back(bundle({msg("/himmel/go"), msg("/himmel/macro/2", {Fl(0.3f)})}));
    long packets = 0, delivered = 0, commands = 0, bad_commands = 0, garbage_delivered = 0;
    auto run = [&](const std::vector<uint8_t>& p, bool engine) {
        ++packets;
        std::vector<Message> got;
        const Parse r = parse_packet(p.data(), p.size(), [&](const Message& m) { got.push_back(m); });
        if (r != Parse::Ok && !got.empty()) ++garbage_delivered;
        EngineView v;
        v.known = engine;
        v.program_deck = static_cast<uint8_t>(rnd(2));
        for (const Message& m : got) {
            ++delivered;
            // Every parsed message is well formed.
            if (m.address[0] != '/' || m.argc < 0 || m.argc > kMaxArgs) ++bad_commands;
            Command out[kMaxCommandsPerMessage];
            int n = -1;
            const Map mr = map_message(m, v, out, n);
            if (n < 0 || n > kMaxCommandsPerMessage || (mr != Map::Ok && n != 0)) ++bad_commands;
            for (int k = 0; k < n; ++k) {
                ++commands;
                if (!command_ok(out[k])) ++bad_commands;
            }
            char text[96];
            describe(m, text, sizeof(text));
            if (std::strlen(text) >= sizeof(text)) ++bad_commands;
        }
    };
    for (int it = 0; it < 200000; ++it) {
        std::vector<uint8_t> p;
        switch (it % 4) {
            case 0: {  // random bytes, often starting like OSC
                p.resize(rnd(64) * (rnd(4) ? 4 : 1) + rnd(3));
                for (auto& x : p) x = static_cast<uint8_t>(rnd(256));
                if (!p.empty() && rnd(2)) p[0] = rnd(2) ? '/' : '#';
                break;
            }
            case 1: {  // a real message, mutated: flipped bytes, truncated or extended
                p = seeds[rnd(static_cast<uint32_t>(seeds.size()))];
                const int flips = 1 + static_cast<int>(rnd(4));
                for (int k = 0; k < flips && !p.empty(); ++k) p[rnd(static_cast<uint32_t>(p.size()))] = static_cast<uint8_t>(rnd(256));
                if (rnd(3) == 0 && !p.empty()) p.resize(rnd(static_cast<uint32_t>(p.size())));
                if (rnd(5) == 0) p.insert(p.end(), 4, static_cast<uint8_t>(rnd(256)));
                break;
            }
            case 2: {  // well formed, random address and arguments (incl. NaN / inf / huge numbers)
                std::vector<Arg> args;
                const int argc = static_cast<int>(rnd(4));
                for (int k = 0; k < argc; ++k) {
                    switch (rnd(5)) {
                        case 0: args.push_back(I(static_cast<int>(rng()))); break;
                        case 1: args.push_back(I(static_cast<int>(rnd(20)) - 2)); break;
                        case 2: {
                            uint32_t u = rng();
                            float f;
                            std::memcpy(&f, &u, 4);
                            args.push_back(Fl(rnd(2) ? f : static_cast<float>(rnd(400)) / 100.0f - 1.0f));
                            break;
                        }
                        case 3: args.push_back(S("x")); break;
                        default: args.push_back(T()); break;
                    }
                }
                p = msg(addrs[rnd(sizeof(addrs) / sizeof(addrs[0]))], args);
                break;
            }
            default: {  // bundles of mixed good and bad elements
                std::vector<std::vector<uint8_t>> parts;
                const int k = 1 + static_cast<int>(rnd(4));
                for (int j = 0; j < k; ++j) {
                    auto e = seeds[rnd(static_cast<uint32_t>(seeds.size()))];
                    if (rnd(3) == 0 && !e.empty()) e[rnd(static_cast<uint32_t>(e.size()))] = static_cast<uint8_t>(rnd(256));
                    parts.push_back(e);
                }
                p = bundle(parts);
                break;
            }
        }
        run(p, rnd(5) != 0);
    }
    std::printf("  %ld packets, %ld messages parsed, %ld commands\n", packets, delivered, commands);
    CHECK(garbage_delivered == 0);
    CHECK(bad_commands == 0);
    CHECK(commands > 1000);  // the fuzz reached the mapper, not only the parser
}

int main() {
    test_parser();
    test_map();
    test_allow();
    test_fuzz();
    std::printf("\nosc_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
