// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Where the Entrancer's MIDI bytes come from: UDP 127.0.0.1:<port> (default
// 8768), sent by the simulator (tools/entrancer_live.py). The real unit's MIDI
// input (WinMM) will feed the same ring later.
//
// A receive thread owns the socket and pushes bytes into a lock-free SPSC ring;
// the render thread pops them once per frame (no locks, no allocation). The
// thread answers the sender every 250 ms with a one-line status, so the
// simulator page can show what the engine plays ("playing Himmel").
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace himmel::engine {

inline constexpr uint16_t kEntrancerPort = 8768;

class EntrancerLink {
public:
    explicit EntrancerLink(uint16_t port);
    ~EntrancerLink();
    EntrancerLink(const EntrancerLink&) = delete;
    EntrancerLink& operator=(const EntrancerLink&) = delete;

    bool ok() const { return ok_; }
    // Render thread: take up to max bytes. Returns the count.
    size_t read(uint8_t* out, size_t max);
    // Steady-clock microseconds of the last datagram (0 = never).
    uint64_t last_rx_us() const { return last_rx_us_.load(std::memory_order_relaxed); }
    // Render thread: what to report back (packed by pack_status).
    void set_status(uint64_t s) { status_.store(s, std::memory_order_relaxed); }
    static uint64_t pack_status(int program, int effect, bool on, bool hold, bool freeze, bool video_off, int bpm);

private:
    void run();
    static constexpr size_t kRing = 4096;  // power of two
    std::array<uint8_t, kRing> ring_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<uint64_t> last_rx_us_{0};
    std::atomic<uint64_t> status_{0};
    std::atomic<bool> stop_{false};
    uintptr_t sock_ = ~uintptr_t(0);
    bool ok_ = false;
    std::thread thread_;
};

}  // namespace himmel::engine
