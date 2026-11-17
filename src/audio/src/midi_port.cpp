// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/audio/midi_port.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace himmel::audio {

namespace {

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string port_name(UINT i) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return {};
    char b[96] = "";
    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

void CALLBACK midi_cb(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    // Time on arrival (QPC): WinMM's own millisecond stamp is too coarse for a 20 ms tick.
    if (msg == MIM_DATA) reinterpret_cast<MidiClockPort*>(inst)->on_short(static_cast<uint32_t>(p1), now_us());
}

}  // namespace

std::vector<std::string> MidiClockPort::list() {
    std::vector<std::string> out;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) out.push_back(port_name(i));
    return out;
}

void MidiClockPort::start(const std::string& name_part) {
    stop();
    want_ = name_part;
    {
        std::lock_guard<std::mutex> lk(m_);
        clock_.reset();
    }
    if (want_.empty()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void MidiClockPort::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (handle_) {
        midiInStop(static_cast<HMIDIIN>(handle_));
        midiInReset(static_cast<HMIDIIN>(handle_));
        midiInClose(static_cast<HMIDIIN>(handle_));
        handle_ = nullptr;
    }
    open_ = false;
}

void MidiClockPort::on_short(uint32_t msg, uint64_t t_us) {
    const uint8_t st = static_cast<uint8_t>(msg & 0xFF);
    if (st < 0xF0) return;  // only system messages carry the clock
    std::lock_guard<std::mutex> lk(m_);
    clock_.message(st, static_cast<uint8_t>((msg >> 8) & 0x7F), static_cast<uint8_t>((msg >> 16) & 0x7F), t_us);
}

ClockSource MidiClockPort::sample(uint64_t now) {
    ClockSource s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s = clock_.sample(now);
    }
    std::snprintf(s.port, sizeof(s.port), "%s", open_.load() ? port_name_ : "");
    if (want_.empty()) s.kind = static_cast<uint8_t>(ClockKind::None);
    return s;
}

void MidiClockPort::run() {
    const std::string want = lower(want_);
    while (!stop_) {
        if (!handle_) {
            const UINT n = midiInGetNumDevs();
            for (UINT i = 0; i < n && !handle_; ++i) {
                const std::string name = port_name(i);
                if (lower(name).find(want) == std::string::npos) continue;
                HMIDIIN h = nullptr;
                if (midiInOpen(&h, i, reinterpret_cast<DWORD_PTR>(&midi_cb), reinterpret_cast<DWORD_PTR>(this),
                               CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
                    midiInStart(h);
                    handle_ = h;
                    std::snprintf(port_name_, sizeof(port_name_), "%s", name.c_str());
                    open_ = true;
                }
            }
        } else {
            // Still there? A pulled cable makes the port vanish from the list.
            bool present = false;
            const UINT n = midiInGetNumDevs();
            for (UINT i = 0; i < n && !present; ++i) present = port_name(i) == port_name_;
            if (!present) {
                midiInReset(static_cast<HMIDIIN>(handle_));
                midiInClose(static_cast<HMIDIIN>(handle_));
                handle_ = nullptr;
                open_ = false;
            }
        }
        for (int i = 0; i < 10 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace himmel::audio
