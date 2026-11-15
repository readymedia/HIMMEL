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
//
// Which simulator counts (SimPeerPolicy, 2026-11-15): exactly one sender is the
// peer. A datagram from another sender is dropped while the peer is alive,
// unless that sender is brand new: then the LAST STARTED simulator wins and the
// old one is ignored from then on. A peer that has been silent for
// kSimPeerIdleUs is replaced by whoever sends next (also an ignored one), so a
// closed simulator never blocks the link. Before this, two simulators sending
// at once (e.g. a forgotten live view) made every datagram look like a "new
// simulator" and the controller reconnected many times per second.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace himmel::mk2 {

inline constexpr uint16_t kSimDefaultPort = 8766;
inline constexpr uint64_t kSimPeerIdleUs = 1'000'000;  // MK2 sims stream pads every 4 ms; the NuVJ sim only senses every 300 ms

// Pure peer selection (no sockets, unit-tested in mk2_tests). Ports are opaque
// 16-bit values (SimLink passes them in network byte order); 0 = none.
class SimPeerPolicy {
public:
    enum class Verdict : uint8_t {
        Accept,   // from the current peer
        NewPeer,  // a new peer: start a new session (full LED / screen resend)
        Drop,     // from a sender we ignore
    };
    Verdict on_datagram(uint16_t port, uint64_t now_us) {
        if (port == 0) return Verdict::Drop;
        if (port == peer_) {
            last_us_ = now_us;
            return Verdict::Accept;
        }
        const bool peer_alive = peer_ != 0 && now_us - last_us_ < kSimPeerIdleUs;
        if (peer_alive && ignored(port)) return Verdict::Drop;
        // Brand-new sender (the last one started wins), or the peer went quiet.
        if (peer_alive) ignore(peer_);
        forget(port);
        peer_ = port;
        last_us_ = now_us;
        return Verdict::NewPeer;
    }
    uint16_t peer() const { return peer_; }
    bool ignored(uint16_t port) const {
        for (uint16_t p : ignored_)
            if (p == port) return true;
        return false;
    }

private:
    void ignore(uint16_t port) {
        ignored_[next_] = port;
        next_ = (next_ + 1) % kIgnored;
    }
    void forget(uint16_t port) {
        for (uint16_t& p : ignored_)
            if (p == port) p = 0;
    }
    static constexpr int kIgnored = 8;
    uint16_t peer_ = 0;
    uint64_t last_us_ = 0;
    uint16_t ignored_[kIgnored] = {};
    int next_ = 0;
};

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
    // ignore: another simulator, an ICMP reset), -1 on a socket error. The peer
    // is chosen by SimPeerPolicy (reader thread only).
    int recv(uint8_t* buf, size_t cap, int timeout_ms);
    // Sends one report to the peer. Never blocks; a lost datagram is not an
    // error (the pad-stream heartbeat notices a simulator that went away).
    void send(const uint8_t* data, size_t len);
    bool has_peer() const { return peer_port_.load(std::memory_order_acquire) != 0; }
    // True once after SimPeerPolicy picked a new peer (a simulator was started
    // or restarted): the session must resend every LED and strip.
    bool take_peer_changed() { return peer_changed_.exchange(false, std::memory_order_acq_rel); }

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};
    uintptr_t sock_ = kInvalid;  // SOCKET, kept out of the header to avoid <winsock2.h>
    bool wsa_ = false;
    std::atomic<uint16_t> peer_port_{0};  // network byte order; peer is always 127.0.0.1
    std::atomic<bool> peer_changed_{false};
    SimPeerPolicy policy_;  // recv() only
};

}  // namespace himmel::mk2
