// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/entrancer_link.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <chrono>
#include <cstdio>
#include <string_view>

#include "himmel/engine/touch_fx.h"

namespace himmel::engine {

namespace {
uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
}  // namespace

uint64_t EntrancerLink::pack_status(int program, int effect, bool on, bool hold, bool freeze, bool video_off, int bpm) {
    return static_cast<uint64_t>(program & 0xFF) | static_cast<uint64_t>(effect & 0xFF) << 8 |
           static_cast<uint64_t>(on) << 16 | static_cast<uint64_t>(hold) << 17 | static_cast<uint64_t>(freeze) << 18 |
           static_cast<uint64_t>(video_off) << 19 | static_cast<uint64_t>(bpm & 0xFFFF) << 24 | uint64_t(1) << 63;
}

EntrancerLink::EntrancerLink(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    // A datagram sent to a sender that has gone away would come back as a
    // queued WSAECONNRESET on every later recv: switch that off.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    DWORD timeout_ms = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return;
    }
    sock_ = static_cast<uintptr_t>(s);
    ok_ = true;
    thread_ = std::thread([this] { run(); });
}

EntrancerLink::~EntrancerLink() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (ok_) closesocket(static_cast<SOCKET>(sock_));
    WSACleanup();
}

size_t EntrancerLink::read(uint8_t* out, size_t max) {
    size_t n = 0;
    size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    while (t != h && n < max) out[n++] = ring_[t++ & (kRing - 1)];
    tail_.store(t, std::memory_order_release);
    return n;
}

void EntrancerLink::run() {
    const SOCKET s = static_cast<SOCKET>(sock_);
    sockaddr_in peer{};
    bool have_peer = false;
    uint64_t next_status = 0;
    uint8_t buf[512];
    while (!stop_.load()) {
        sockaddr_in from{};
        int flen = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        const uint64_t t = now_us();
        if (n > 0) {
            if (!have_peer || from.sin_port != peer.sin_port) next_status = 0;  // new simulator: answer now
            peer = from;
            have_peer = true;
            last_rx_us_.store(t, std::memory_order_relaxed);
            size_t h = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);
            for (int i = 0; i < n && h - tail < kRing; ++i) ring_[h++ & (kRing - 1)] = buf[i];  // full: drop the rest
            head_.store(h, std::memory_order_release);
        }
        if (have_peer && t >= next_status) {
            next_status = t + 250000;
            const uint64_t st = status_.load(std::memory_order_relaxed);
            char line[160];
            if (st >> 63) {
                const auto name = touchfx::info(static_cast<int>((st >> 8) & 0xFF)).name;
                std::snprintf(line, sizeof(line), "HIMMEL program=%d effect=%.*s on=%d hold=%d freeze=%d video_off=%d bpm=%d\n",
                              static_cast<int>(st & 0xFF), static_cast<int>(name.size()), name.data(),
                              static_cast<int>((st >> 16) & 1), static_cast<int>((st >> 17) & 1),
                              static_cast<int>((st >> 18) & 1), static_cast<int>((st >> 19) & 1),
                              static_cast<int>((st >> 24) & 0xFFFF));
            } else {
                std::snprintf(line, sizeof(line), "HIMMEL starting\n");
            }
            sendto(s, line, static_cast<int>(std::string_view(line).size()), 0, reinterpret_cast<sockaddr*>(&peer),
                   sizeof(peer));
        }
    }
}

}  // namespace himmel::engine
