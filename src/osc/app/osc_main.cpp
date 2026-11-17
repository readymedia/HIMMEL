// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_osc.exe: OSC in (M13, docs/interfaces/osc.md).
//
//   himmel_osc [--port 7000] [--allow LIST]      listen; LIST = "any" or "127.0.0.1,192.168.1.0/24"
//                                                (default: this PC only)
//   himmel_osc --status                          print Local\HimmelOsc (counters + the last messages)
//   himmel_osc --send ADDRESS [ARGS...] [--to IP:PORT]
//                                                send one OSC message (a test client): 3 = int,
//                                                0.5 = float, T / F, anything else = string
//
// UDP datagrams -> allow list -> strict parser -> address map -> commands on
// producer slot 2 (Osc) of Local\HimmelCommands. The engine validates them again.
// Started by the supervisor (himmel.exe); it can die and come back with no effect on
// the outputs: the engine just sees the producer go quiet.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)  // not in every SDK's headers
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "himmel/engine/shm_block.h"
#include "himmel/osc/osc.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/state.h"

using namespace himmel;
using namespace himmel::protocol;
using himmel::engine::ShmBlock;

namespace {

constexpr int kSlot = static_cast<int>(Producer::Osc);
std::atomic<bool> g_stop{false};

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
uint64_t unix_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}
BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

void log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::printf("%02d:%02d:%02d  himmel_osc  %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    std::fflush(stdout);
}

std::string ip_text(uint32_t ip) {
    char b[20];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

struct Link {
    ShmBlock cmd_shm, state_shm, osc_shm;
    CommandBlock* cmd = nullptr;
    StateBlock* state = nullptr;
    osc::OscBlock* blk = nullptr;
    osc::OscStatus st{};
    uint32_t seq = 0;
    uint64_t next_try_us = 0, next_hello_us = 0, next_status_us = 0;
    bool status_dirty = true;

    void poll(uint64_t t) {
        if (t >= next_try_us) {
            next_try_us = t + 1000000;
            if (!cmd) {
                const auto r = cmd_shm.open_or_create(kCommandShmName, sizeof(CommandBlock));
                if (r != ShmBlock::Result::Failed) {
                    auto* c = static_cast<CommandBlock*>(cmd_shm.data());
                    if (r == ShmBlock::Result::Created)
                        engine::write_header(c->header, kCommandMagic, kCommandVersion, sizeof(CommandBlock));
                    if (engine::header_ok(c->header, kCommandMagic, kCommandVersion, sizeof(CommandBlock))) {
                        cmd = c;
                        cmd->producers[kSlot].pid.store(GetCurrentProcessId());
                        cmd->producers[kSlot].heartbeat_us.store(t);
                        log("commands: producer slot %d (Osc)", kSlot);
                    } else {
                        cmd_shm.close();
                    }
                }
            }
            if (!state && state_shm.open_existing(kStateShmName, sizeof(StateBlock))) {
                auto* s = static_cast<StateBlock*>(state_shm.data());
                if (engine::header_ok(s->header, kStateMagic, kStateVersion, sizeof(StateBlock))) state = s;
                else state_shm.close();
            }
        }
        if (cmd && t >= next_hello_us) {  // alive while idle (command-protocol.md §3)
            next_hello_us = t + 1000000;
            push(make(CommandType::Hello, kNone, kNone, static_cast<int32_t>(GetCurrentProcessId())), t);
        }
        st.heartbeat_us = t;
        if (blk && (status_dirty || t >= next_status_us)) {
            seq_write(blk->status_seq, blk->status, st);
            status_dirty = false;
            next_status_us = t + 250000;
        }
    }
    bool push(Command c, uint64_t t) {
        if (!cmd) return false;
        c.seq = ++seq;
        cmd->producers[kSlot].heartbeat_us.store(t);
        if (!cmd->rings[kSlot].push(c)) {
            ++st.ring_full;
            return false;
        }
        return true;
    }
    osc::EngineView view() const {
        osc::EngineView v;
        LiveState* l = nullptr;
        static LiveState copy;  // ~3 KB: not on the stack of every packet
        if (state && seq_read(state->live_seq, state->live, copy)) l = &copy;
        if (l) v.program_deck = l->program_deck < kDecks ? l->program_deck : 0, v.known = true;
        return v;
    }
    void note(uint32_t ip, uint16_t port, osc::Verdict v, uint8_t reason, const char* text) {
        osc::LastMessage& m = st.last[st.next % osc::kLastMessages];
        m.unix_ms = unix_ms();
        m.from_ip = ip;
        m.from_port = port;
        m.verdict = static_cast<uint8_t>(v);
        m.reason = reason;
        std::snprintf(m.text, sizeof(m.text), "%s", text);
        st.next = (st.next + 1) % osc::kLastMessages;
        ++st.total;
        status_dirty = true;
    }
};

int print_status() {
    ShmBlock shm;
    if (!shm.open_existing(osc::kOscShmName, sizeof(osc::OscBlock))) return std::printf("himmel_osc is not running\n"), 2;
    auto* b = static_cast<osc::OscBlock*>(shm.data());
    if (!engine::header_ok(b->header, osc::kOscMagic, osc::kOscVersion, sizeof(osc::OscBlock)))
        return std::printf("Local\\HimmelOsc: wrong version\n"), 2;
    static osc::OscStatus s;
    if (!seq_read(b->status_seq, b->status, s)) return std::printf("busy, try again\n"), 2;
    std::printf("himmel_osc pid %u  udp %u  allow \"%s\"\n", s.pid, s.port, s.allow);
    std::printf("received %llu  applied %llu  refused %llu  blocked %llu  garbage %llu  rate limited %llu  ring full %llu\n",
                (unsigned long long)s.received, (unsigned long long)s.applied, (unsigned long long)s.refused,
                (unsigned long long)s.blocked, (unsigned long long)s.garbage, (unsigned long long)s.rate_limited,
                (unsigned long long)s.ring_full);
    const uint32_t n = s.total < osc::kLastMessages ? s.total : osc::kLastMessages;
    for (uint32_t k = 0; k < n; ++k) {  // newest first
        const osc::LastMessage& m = s.last[(s.next + osc::kLastMessages - 1 - k) % osc::kLastMessages];
        const char* why = m.verdict == static_cast<uint8_t>(osc::Verdict::Garbage)   ? osc::parse_name(static_cast<osc::Parse>(m.reason))
                          : m.verdict == static_cast<uint8_t>(osc::Verdict::Refused) ? osc::map_name(static_cast<osc::Map>(m.reason))
                                                                                      : "";
        std::printf("  %-12s %-15s %-40s %s\n", osc::verdict_name(static_cast<osc::Verdict>(m.verdict)),
                    ip_text(m.from_ip).c_str(), m.text, why);
    }
    return 0;
}

int send_one(int argc, char** argv) {
    std::string to = "127.0.0.1:7000";
    std::vector<osc::Arg> args;
    const char* address = nullptr;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) {
            to = argv[++i];
            continue;
        }
        if (!address) {
            address = argv[i];
            continue;
        }
        osc::Arg x{};
        char* end = nullptr;
        const long iv = std::strtol(argv[i], &end, 10);
        if (a == "T") x.type = osc::ArgType::True;
        else if (a == "F") x.type = osc::ArgType::False;
        else if (end && *end == 0 && !a.empty()) x.type = osc::ArgType::Int, x.i = static_cast<int32_t>(iv);
        else {
            const float fv = std::strtof(argv[i], &end);
            if (end && *end == 0 && !a.empty()) x.type = osc::ArgType::Float, x.f = fv;
            else x.type = osc::ArgType::String, std::snprintf(x.s, sizeof(x.s), "%s", argv[i]);
        }
        args.push_back(x);
    }
    if (!address) return std::printf("usage: himmel_osc --send ADDRESS [ARGS...] [--to IP:PORT]\n"), 1;
    uint8_t pkt[osc::kMaxPacket];
    const size_t n = osc::build_message(address, args.data(), static_cast<int>(args.size()), pkt, sizeof(pkt));
    if (!n) return std::printf("cannot build that message\n"), 1;
    const size_t colon = to.rfind(':');
    uint32_t ip;
    if (colon == std::string::npos || !osc::parse_ipv4(to.c_str(), colon, ip)) return std::printf("bad --to\n"), 1;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(std::atoi(to.c_str() + colon + 1)));
    a.sin_addr.s_addr = htonl(ip);
    const int r = sendto(s, reinterpret_cast<const char*>(pkt), static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    closesocket(s);
    WSACleanup();
    return r == static_cast<int>(n) ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--status") == 0) return print_status();
    if (argc > 1 && std::strcmp(argv[1], "--send") == 0) return send_one(argc, argv);
    uint16_t port = osc::kDefaultPort;
    std::string allow_text = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--allow" && i + 1 < argc) allow_text = argv[++i];
        else return std::printf("unknown argument %s (see the top of src/osc/app/osc_main.cpp)\n", argv[i]), 1;
    }
    osc::AllowList allow;
    if (!allow.parse(allow_text.c_str())) return std::printf("bad --allow \"%s\"\n", allow_text.c_str()), 1;
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    Link link;
    link.st.pid = GetCurrentProcessId();
    std::snprintf(link.st.allow, sizeof(link.st.allow), "%s", allow_text.c_str());
    {
        const auto r = link.osc_shm.open_or_create(osc::kOscShmName, sizeof(osc::OscBlock));
        if (r != ShmBlock::Result::Failed) {
            link.blk = static_cast<osc::OscBlock*>(link.osc_shm.data());
            engine::write_header(link.blk->header, osc::kOscMagic, osc::kOscVersion, sizeof(osc::OscBlock));
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return log("winsock failed"), 3;
    SOCKET sock = INVALID_SOCKET;
    uint64_t next_bind_us = 0;
    uint64_t window_us = 0;
    uint32_t window_count = 0;
    log("starting: udp %u, allow \"%s\"", port, allow_text.c_str());
    while (!g_stop) {
        const uint64_t t = now_us();
        if (sock == INVALID_SOCKET && t >= next_bind_us) {
            next_bind_us = t + 2000000;
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL off = FALSE;
            DWORD got = 0;
            WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
            DWORD timeout = 100;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            // Only this PC can reach us unless the allow list names other machines.
            a.sin_addr.s_addr = htonl(allow.any || allow.count > 1 || (allow.count == 1 && allow.net[0] != 0x7F000001u)
                                          ? INADDR_ANY
                                          : INADDR_LOOPBACK);
            if (bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                log("cannot listen on udp %u (error %d) - retrying every 2 s", port, WSAGetLastError());
                closesocket(sock);
                sock = INVALID_SOCKET;
                link.st.port = 0;
            } else {
                link.st.port = port;
                link.status_dirty = true;
                log("listening on udp %u", port);
            }
        }
        link.poll(t);
        if (sock == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }
        static uint8_t buf[65536];
        sockaddr_in from{};
        int flen = sizeof(from);
        const int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) continue;  // timeout (or a reset we switched off)
        const uint64_t t2 = now_us();
        const uint32_t ip = ntohl(from.sin_addr.s_addr);
        const uint16_t fport = ntohs(from.sin_port);
        ++link.st.received;
        if (!allow.allows(ip)) {
            ++link.st.blocked;
            char b[96];
            std::snprintf(b, sizeof(b), "<%d bytes from a sender not on the allow list>", n);
            link.note(ip, fport, osc::Verdict::Blocked, 0, b);
            continue;
        }
        if (t2 - window_us >= 1000000) window_us = t2, window_count = 0;
        // Check the whole packet first; garbage never reaches the mapper.
        const osc::Parse p = osc::parse_packet(buf, static_cast<size_t>(n), [](const osc::Message&) {});
        if (p != osc::Parse::Ok) {
            ++link.st.garbage;
            char b[96];
            std::snprintf(b, sizeof(b), "<garbage: %d bytes>", n);
            link.note(ip, fport, osc::Verdict::Garbage, static_cast<uint8_t>(p), b);
            continue;
        }
        const osc::EngineView view = link.view();
        osc::parse_packet(buf, static_cast<size_t>(n), [&](const osc::Message& m) {
            char text[96];
            osc::describe(m, text, sizeof(text));
            if (++window_count > osc::kMaxMessagesPerSecond) {
                ++link.st.rate_limited;
                if (window_count == osc::kMaxMessagesPerSecond + 1) link.note(ip, fport, osc::Verdict::RateLimited, 0, text);
                return;
            }
            Command out[osc::kMaxCommandsPerMessage];
            int count = 0;
            const osc::Map r = osc::map_message(m, view, out, count);
            if (r != osc::Map::Ok) {
                ++link.st.refused;
                link.note(ip, fport, osc::Verdict::Refused, static_cast<uint8_t>(r), text);
                return;
            }
            bool ok = true;
            for (int k = 0; k < count; ++k) ok = link.push(out[k], t2) && ok;
            if (ok) ++link.st.applied;
            link.note(ip, fport, ok ? osc::Verdict::Applied : osc::Verdict::Refused,
                      ok ? 0 : static_cast<uint8_t>(osc::Map::NoEngine), text);
        });
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    WSACleanup();
    if (link.cmd) link.cmd->producers[kSlot].pid.store(0);
    log("stopped");
    return 0;
}
