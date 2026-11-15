// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/midi_in.h"

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
#include <cstring>

namespace himmel::engine {

namespace {

std::string narrow(const wchar_t* w) {
    char buf[128];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void CALLBACK midi_callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
    if (msg == MIM_DATA) reinterpret_cast<MidiIn*>(instance)->on_short(static_cast<uint32_t>(p1));
}

// Data bytes that follow a status byte (0 for real-time and unknown).
int data_bytes(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;
        case 0xF0:
            return status == 0xF1 || status == 0xF3 ? 1 : status == 0xF2 ? 2 : 0;
        default: return 2;
    }
}

}  // namespace

std::vector<std::string> MidiIn::list() {
    std::vector<std::string> out;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        out.push_back(midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname) : "");
    }
    return out;
}

MidiIn::MidiIn(std::string name_part) : want_(lower(std::move(name_part))) {
    thread_ = std::thread([this] { run(); });
}

MidiIn::~MidiIn() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    close_port();
}

void MidiIn::on_short(uint32_t msg) {
    const uint8_t b[3] = {static_cast<uint8_t>(msg), static_cast<uint8_t>(msg >> 8), static_cast<uint8_t>(msg >> 16)};
    const int n = 1 + data_bytes(b[0]);
    size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    if (h - t + n > kRing) return;  // full: drop the message whole, never half of it
    for (int i = 0; i < n; ++i) ring_[h++ & (kRing - 1)] = b[i];
    head_.store(h, std::memory_order_release);
    messages_.fetch_add(1, std::memory_order_relaxed);
}

size_t MidiIn::read(uint8_t* out, size_t max) {
    size_t n = 0;
    size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    while (t != h && n < max) out[n++] = ring_[t++ & (kRing - 1)];
    tail_.store(t, std::memory_order_release);
    return n;
}

bool MidiIn::try_open() {
    const auto ports = list();
    for (UINT i = 0; i < ports.size(); ++i) {
        if (ports[i].empty() || lower(ports[i]).find(want_) == std::string::npos) continue;
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, reinterpret_cast<DWORD_PTR>(&midi_callback), reinterpret_cast<DWORD_PTR>(this),
                       CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
            continue;  // busy (another program holds it) or gone: try the next match
        midiInStart(h);
        handle_ = h;
        strncpy_s(port_name_, sizeof(port_name_), ports[i].c_str(), _TRUNCATE);
        open_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    return false;
}

void MidiIn::close_port() {
    if (!handle_) return;
    HMIDIIN h = static_cast<HMIDIIN>(handle_);
    midiInStop(h);
    midiInReset(h);
    midiInClose(h);
    handle_ = nullptr;
    if (open_.exchange(false, std::memory_order_acq_rel)) generation_.fetch_add(1, std::memory_order_acq_rel);
}

void MidiIn::run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (!stop_.load()) {
        if (clock::now() >= next) {
            next = clock::now() + std::chrono::seconds(1);
            if (!handle_) {
                try_open();
            } else {  // still plugged in? WinMM does not always say when a USB port disappears
                bool there = false;
                for (const auto& p : list()) there |= p == port_name_;
                if (!there) close_port();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace himmel::engine
