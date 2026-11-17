// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// OSC in (M13, docs/interfaces/osc.md): a strict OSC 1.0 parser, the 0.1 address
// map (/himmel/...) -> protocol commands, the allow list, and the shared status
// block "Local\HimmelOsc" with the last messages.
//
// Pure logic (no sockets, no Windows): himmel_osc.exe (app/osc_main.cpp) does the
// UDP and the command ring; tests/osc/osc_tests.cpp fuzzes everything here.
// Rule: garbage never becomes a command. The parser refuses anything malformed,
// the mapper only knows the addresses in the table, checks every argument, and
// builds commands with fixed targets; the engine validates them once more.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/shm.h"

namespace himmel::osc {

inline constexpr uint16_t kDefaultPort = 7000;   // GUESS: the common default (timeline-and-remote.md §6)
inline constexpr size_t kMaxPacket = 4096;       // larger datagrams are refused whole
inline constexpr int kMaxArgs = 8;
inline constexpr int kMaxAddress = 64;
inline constexpr int kMaxString = 64;
inline constexpr int kMaxBundleDepth = 4;
inline constexpr int kMaxMessagesPerPacket = 32;  // elements of a bundle (nested included)
inline constexpr int kMaxCommandsPerMessage = 2;
inline constexpr uint32_t kMaxMessagesPerSecond = 500;  // per process; the engine limits again (4000 / s)

// ---- parsing -------------------------------------------------------------------
enum class ArgType : uint8_t { Int, Float, String, True, False, Nil };
struct Arg {
    ArgType type;
    int32_t i;
    float f;
    char s[kMaxString];  // NUL-terminated, cut to fit
};
struct Message {
    char address[kMaxAddress];
    int argc;
    Arg argv[kMaxArgs];
};

enum class Parse : uint8_t {
    Ok = 0,
    Empty,         // 0 bytes
    TooBig,        // > kMaxPacket
    BadAlign,      // size not a multiple of 4
    BadAddress,    // not '/'-started, unterminated, too long, or not printable ASCII
    BadTypeTags,   // missing ',' / unterminated
    UnknownType,   // a type tag we do not take (b, h, d, t, c, r, m, [ ], ...)
    TooManyArgs,
    Truncated,     // an argument runs past the end
    BadString,     // unterminated or not printable
    BadBundle,     // "#bundle" header / element size wrong, too deep, too many elements
    Count
};
const char* parse_name(Parse p);

// One packet (a message or a bundle). The whole packet is checked first; only a
// packet without any error delivers its messages: on_message(const Message&) for
// each, in order. A bad bundle element drops the whole packet.
// Bundle time tags are ignored (everything runs at once). No allocation.
template <class F>
Parse parse_packet(const uint8_t* data, size_t n, F&& on_message);

// ---- the address map --------------------------------------------------------------
// What the mapper needs to know about the engine (read from Local\HimmelState).
struct EngineView {
    uint8_t program_deck = 0;  // 0 = A, 1 = B
    bool known = false;        // false: the state block is not there yet (deck-addressed commands wait)
};

enum class Map : uint8_t {
    Ok = 0,
    UnknownAddress,
    BadArgs,        // wrong count / type / range (NaN, inf, out of range)
    NoEngine,       // needs the engine state (PREVIEW / PROGRAM deck) and it is not there
    Unsupported,    // a 0.1 address the engine cannot do yet (/himmel/master)
    Count
};
const char* map_name(Map m);

// Turns one message into 0..kMaxCommandsPerMessage commands (seq is left 0: the sender fills it).
Map map_message(const Message& m, const EngineView& v, protocol::Command (&out)[kMaxCommandsPerMessage], int& count);

// ---- the allow list ---------------------------------------------------------------
// IPv4 networks; empty = only 127.0.0.1. "any" = everyone (Settings › Remote).
struct AllowList {
    static constexpr int kMax = 16;
    uint32_t net[kMax] = {};   // host byte order
    uint32_t mask[kMax] = {};
    int count = 0;
    bool any = false;
    // "127.0.0.1,192.168.1.0/24,10.0.0.5" or "any". False (and unchanged) on a bad entry.
    bool parse(const char* list);
    bool allows(uint32_t ipv4_host_order) const;
};
// "a.b.c.d" -> host order. False on anything else.
bool parse_ipv4(const char* s, size_t n, uint32_t& out);

// ---- the status block Local\HimmelOsc (written by himmel_osc only) ----------------------
inline constexpr const wchar_t* kOscShmName = L"Local\\HimmelOsc";
inline constexpr uint32_t kOscMagic = 0x43534F48;  // "HOSC"
inline constexpr uint32_t kOscVersion = 1;
inline constexpr int kLastMessages = 32;

enum class Verdict : uint8_t { Applied = 0, Refused = 1, Blocked = 2, Garbage = 3, RateLimited = 4, Count };
const char* verdict_name(Verdict v);

struct LastMessage {
    uint64_t unix_ms;          // wall clock
    uint32_t from_ip;          // host order
    uint16_t from_port;
    uint8_t verdict;           // Verdict
    uint8_t reason;            // Parse (Garbage), Map (Refused), else 0
    char text[96];             // "/himmel/scene 3" (address + args, cut), or "<garbage: 13 bytes>"
};

struct OscStatus {
    uint32_t pid;
    uint16_t port;             // the UDP port it listens on (0 = could not bind)
    uint16_t reserved;
    uint64_t heartbeat_us;     // steady clock
    uint64_t received, applied, refused, blocked, garbage, rate_limited, ring_full;
    char allow[128];           // the allow list as given
    uint32_t next;             // the next slot in `last` (count mod kLastMessages)
    uint32_t total;            // messages logged so far
    LastMessage last[kLastMessages];
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct OscBlock {
    protocol::BlockHeader header;      // kOscMagic, kOscVersion, sizeof(OscBlock)
    alignas(64) std::atomic<uint32_t> status_seq;
    OscStatus status;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Writes one OSC message (address + args) into out. Returns its size, or 0 if it does not fit
// or the address / a string is not printable ASCII. For tools and tests (himmel_osc --send).
size_t build_message(const char* address, const Arg* args, int argc, uint8_t* out, size_t cap);

// "/himmel/scene 3", "/himmel/macro/1 0.50", "/x \"hi\"" into out (always NUL-terminated).
void describe(const Message& m, char* out, size_t cap);

// ============================================================================ parser (inline)
namespace detail {
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
// A padded OSC string at data[pos..n): printable ASCII (32..126), NUL-terminated, padded to 4.
// Returns its length (without the NUL) and moves pos, or -1.
inline int read_string(const uint8_t* d, size_t n, size_t& pos, char* out, size_t cap) {
    size_t k = pos;
    while (k < n && d[k] != 0) {
        if (d[k] < 32 || d[k] > 126) return -1;
        ++k;
    }
    if (k >= n) return -1;  // no NUL
    const size_t len = k - pos;
    const size_t end = pos + ((len + 4) & ~size_t(3));
    if (end > n) return -1;
    for (size_t j = k; j < end; ++j)
        if (d[j] != 0) return -1;  // padding must be zeros
    if (out) {
        const size_t c = len < cap - 1 ? len : cap - 1;
        for (size_t j = 0; j < c; ++j) out[j] = static_cast<char>(d[pos + j]);
        out[c] = 0;
    }
    pos = end;
    return static_cast<int>(len);
}

template <class F>
Parse parse_one(const uint8_t* d, size_t n, F& on_message, int depth, int& messages) {
    if (n == 0) return Parse::Empty;
    if (n % 4) return Parse::BadAlign;
    if (d[0] == '#') {  // a bundle: "#bundle\0" + 8-byte time tag + (int32 size + element)*
        static const char kTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
        if (depth >= kMaxBundleDepth || n < 16) return Parse::BadBundle;
        for (int k = 0; k < 8; ++k)
            if (d[k] != static_cast<uint8_t>(kTag[k])) return Parse::BadBundle;
        size_t pos = 16;
        while (pos < n) {
            if (n - pos < 4) return Parse::BadBundle;
            const uint32_t size = be32(d + pos);
            pos += 4;
            if (size == 0 || size % 4 || size > n - pos) return Parse::BadBundle;
            const Parse r = parse_one(d + pos, size, on_message, depth + 1, messages);
            if (r != Parse::Ok) return r;
            pos += size;
        }
        return Parse::Ok;
    }
    if (++messages > kMaxMessagesPerPacket) return Parse::BadBundle;
    Message m;
    m.argc = 0;
    size_t pos = 0;
    if (d[0] != '/') return Parse::BadAddress;
    const int alen = read_string(d, n, pos, m.address, sizeof(m.address));
    if (alen < 1 || alen >= kMaxAddress) return Parse::BadAddress;
    char tags[kMaxArgs + 2];
    if (pos >= n) {  // OSC 1.0: a message without a type tag string has no arguments
        on_message(static_cast<const Message&>(m));
        return Parse::Ok;
    }
    if (d[pos] != ',') return Parse::BadTypeTags;
    size_t tpos = pos;
    const int tlen = read_string(d, n, tpos, nullptr, 0);
    if (tlen < 1) return Parse::BadTypeTags;
    if (tlen - 1 > kMaxArgs) return Parse::TooManyArgs;
    for (int k = 0; k < tlen; ++k) tags[k] = static_cast<char>(d[pos + k]);
    pos = tpos;
    for (int k = 1; k < tlen; ++k) {
        Arg& a = m.argv[m.argc++];
        a.i = 0;
        a.f = 0.0f;
        a.s[0] = 0;
        switch (tags[k]) {
            case 'i':
                if (n - pos < 4) return Parse::Truncated;
                a.type = ArgType::Int;
                a.i = static_cast<int32_t>(be32(d + pos));
                pos += 4;
                break;
            case 'f': {
                if (n - pos < 4) return Parse::Truncated;
                a.type = ArgType::Float;
                const uint32_t u = be32(d + pos);
                static_assert(sizeof(float) == 4);
                std::memcpy(&a.f, &u, 4);
                pos += 4;
                break;
            }
            case 's':
                a.type = ArgType::String;
                if (pos >= n) return Parse::Truncated;
                if (read_string(d, n, pos, a.s, sizeof(a.s)) < 0) return Parse::BadString;
                break;
            case 'T': a.type = ArgType::True; break;
            case 'F': a.type = ArgType::False; break;
            case 'N': a.type = ArgType::Nil; break;
            default: return Parse::UnknownType;
        }
    }
    if (pos != n) return Parse::Truncated;  // bytes left over: not well formed
    on_message(static_cast<const Message&>(m));
    return Parse::Ok;
}
}  // namespace detail

template <class F>
Parse parse_packet(const uint8_t* data, size_t n, F&& on_message) {
    if (n == 0) return Parse::Empty;
    if (n > kMaxPacket) return Parse::TooBig;
    int messages = 0;
    auto none = [](const Message&) {};
    const Parse r = detail::parse_one(data, n, none, 0, messages);
    if (r != Parse::Ok) return r;
    messages = 0;
    return detail::parse_one(data, n, on_message, 0, messages);
}

}  // namespace himmel::osc
