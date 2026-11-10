// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Simulated MK2 link: the raw USB reports of a pretend MK2 over UDP on loopback.
//
// A simulator (tools/mk2_live.py in Simulate mode) sends the same input reports
// the hardware sends (0x01 controls, 0x20 pads, one report per datagram) to
// 127.0.0.1:<port>. The controller sends its LED (0x80-0x82) and screen
// (0xE0/0xE1) reports back to whoever sent the last datagram. Everything above
// this link (parsing, pad tracking, events, LEDs, screens) is the same code as
// for the real device.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace himmel::mk2 {

inline constexpr uint16_t kSimDefaultPort = 8766;

class SimLink {
public:
    SimLink() = default;
    ~SimLink();
    SimLink(const SimLink&) = delete;
    SimLink& operator=(const SimLink&) = delete;

    // Binds 127.0.0.1:port. Safe to call again once bound.
    bool bind(uint16_t port, std::string& why);
    bool bound() const { return sock_ != kInvalid; }
    // One datagram into buf. Returns its length, 0 on timeout (or a datagram we
    // ignore), -1 on a socket error. Remembers the sender as the peer.
    int recv(uint8_t* buf, size_t cap, int timeout_ms);
    // Sends one report to the peer. Never blocks; a lost datagram is not an
    // error (the pad-stream heartbeat notices a simulator that went away).
    void send(const uint8_t* data, size_t len);
    bool has_peer() const { return peer_port_.load(std::memory_order_acquire) != 0; }
    // True once after datagrams start coming from a different sender (the
    // simulator was restarted): the session must resend every LED and strip.
    bool take_peer_changed() { return peer_changed_.exchange(false, std::memory_order_acq_rel); }

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};
    uintptr_t sock_ = kInvalid;  // SOCKET, kept out of the header to avoid <winsock2.h>
    bool wsa_ = false;
    std::atomic<uint16_t> peer_port_{0};  // network byte order; peer is always 127.0.0.1
    std::atomic<bool> peer_changed_{false};
};

}  // namespace himmel::mk2
