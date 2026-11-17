// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// A WinMM MIDI input port that feeds a MidiClock (himmel_audio only; never
// the render loop). The port is picked by part of its name; a watcher thread
// reopens it when it comes back after an unplug.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "himmel/audio/midi_clock.h"

namespace himmel::audio {

class MidiClockPort {
public:
    MidiClockPort() = default;
    ~MidiClockPort() { stop(); }
    MidiClockPort(const MidiClockPort&) = delete;
    MidiClockPort& operator=(const MidiClockPort&) = delete;

    void start(const std::string& name_part);  // "" = off
    void stop();
    ClockSource sample(uint64_t now_us);
    bool is_open() const { return open_.load(); }
    const std::string& wanted() const { return want_; }

    static std::vector<std::string> list();

    void on_short(uint32_t msg, uint64_t t_us);  // WinMM callback thread

private:
    void run();
    std::mutex m_;
    MidiClock clock_;
    std::string want_;
    char port_name_[64] = "";
    void* handle_ = nullptr;
    std::atomic<bool> open_{false}, stop_{false};
    std::thread thread_;
};

}  // namespace himmel::audio
