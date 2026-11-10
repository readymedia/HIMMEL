// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// A MIDI input port (WinMM, part of Windows: no new dependency), for a real
// MIDI weapon such as the Entrancer behind a USB-MIDI interface.
//
// * The port is picked by part of its name (case-insensitive), so it survives
//   a different USB socket or port order.
// * A watcher thread opens the port, checks every second that it is still
//   there, and reopens it when it comes back (cable pulled = closed, not dead).
// * WinMM delivers short messages on its own thread; they go into a lock-free
//   SPSC byte ring. The render thread reads it once per frame: no locks, no
//   allocation. SysEx is ignored (the Entrancer's pad never uses it).
// * Ports are exclusive on Windows: only one program can hold a port.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace himmel::engine {

class MidiIn {
public:
    explicit MidiIn(std::string name_part);
    ~MidiIn();
    MidiIn(const MidiIn&) = delete;
    MidiIn& operator=(const MidiIn&) = delete;

    // Render thread: take up to max bytes. Returns the count.
    size_t read(uint8_t* out, size_t max);
    bool is_open() const { return open_.load(std::memory_order_acquire); }
    // Changes every time the port opens or closes (log it once per change).
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    // The full name of the open port (valid while is_open()).
    const char* port_name() const { return port_name_; }
    uint64_t messages() const { return messages_.load(std::memory_order_relaxed); }

    // All MIDI input ports, in WinMM order.
    static std::vector<std::string> list();

    // Called by WinMM (its own thread). Public only for the C callback.
    void on_short(uint32_t msg);

private:
    void run();
    bool try_open();
    void close_port();
    static constexpr size_t kRing = 4096;  // power of two
    std::array<uint8_t, kRing> ring_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<bool> open_{false}, stop_{false};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint64_t> messages_{0};
    std::string want_;
    char port_name_[64] = "";
    void* handle_ = nullptr;  // HMIDIIN
    std::thread thread_;
};

}  // namespace himmel::engine
