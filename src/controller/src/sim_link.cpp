// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/mk2/sim_link.h"

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

namespace himmel::mk2 {

namespace {
SOCKET as_socket(uintptr_t s) { return static_cast<SOCKET>(s); }
}  // namespace

SimLink::~SimLink() {
    if (sock_ != kInvalid) closesocket(as_socket(sock_));
    if (wsa_) WSACleanup();
}

bool SimLink::bind(uint16_t port, std::string& why) {
    if (sock_ != kInvalid) return true;
    if (!wsa_) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            why = "WSAStartup failed";
            return false;
        }
        wsa_ = true;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        why = "socket() failed (" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    // Without this, a send to a simulator that has gone makes the next recv fail
    // with WSAECONNRESET (an ICMP "port unreachable" coming back).
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    int buf = 1 << 20;  // room for a burst of pad reports if the reader is late
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only: nothing from the network
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        why = "port " + std::to_string(port) + " is busy (another mk2_controller --sim running?)";
        closesocket(s);
        return false;
    }
    sock_ = static_cast<uintptr_t>(s);
    return true;
}

int SimLink::recv(uint8_t* buf, size_t cap, int timeout_ms) {
    if (sock_ == kInvalid) return -1;
    SOCKET s = as_socket(sock_);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int r = select(0, &rd, nullptr, nullptr, &tv);
    if (r == 0) return 0;
    if (r < 0) return -1;
    sockaddr_in from{};
    int flen = sizeof(from);
    const int n = recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
    if (n < 0) {
        const int e = WSAGetLastError();
        // Too big for buf, nothing there after all, or an ICMP echo: not fatal.
        return (e == WSAEMSGSIZE || e == WSAEWOULDBLOCK || e == WSAECONNRESET) ? 0 : -1;
    }
    if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
    const uint16_t old = peer_port_.exchange(from.sin_port, std::memory_order_acq_rel);
    if (old != 0 && old != from.sin_port) peer_changed_.store(true, std::memory_order_release);
    return n;
}

void SimLink::send(const uint8_t* data, size_t len) {
    const uint16_t port = peer_port_.load(std::memory_order_acquire);
    if (sock_ == kInvalid || port == 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(as_socket(sock_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

}  // namespace himmel::mk2
