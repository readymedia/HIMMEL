// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: one MIDI link to a device, real (WinMM) or simulated (UDP).
//
// WinMM is part of Windows (winmm.lib), so no new dependency. Ports are
// exclusive on Windows: close the maker's software first.
//
// The simulated link is the one the Python simulators use (tools/lightpad_sim.py,
// tools/lumi_sim.py, tools/nuvj_sim.py): every MIDI message the simulated device
// sends is one UDP datagram to 127.0.0.1:<port>; whatever the driver sends back
// to that sender is MIDI into the device. Ports: Lightpad 8767, LUMI 8769, NuVJ 8774.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace himmel::weapon {

inline constexpr uint16_t kSimPortLightpad = 8767;
inline constexpr uint16_t kSimPortLumi = 8769;
inline constexpr uint16_t kSimPortNuvj = 8774;

struct MidiMsg {
    static constexpr size_t kCap = 1024;
    uint16_t len = 0;
    uint8_t b[kCap];
    bool sysex() const { return len >= 2 && b[0] == 0xF0; }
};

// Bytes of one short message that starts with this status byte (running status not used).
inline int short_len(uint8_t status) {
    if (status >= 0xF8) return 1;
    const uint8_t hi = status & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
}

// Joins SysEx that WinMM hands over in pieces: complete F0 ... F7 messages come out.
class SysexAssembler {
public:
    // Calls out(msg) for every complete message in data. Returns how many.
    template <class F>
    int feed(const uint8_t* data, size_t n, F&& out) {
        int count = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t c = data[i];
            if (c == 0xF0) {
                m_.len = 0;
                active_ = true;
            }
            if (!active_) continue;
            if (m_.len < MidiMsg::kCap) m_.b[m_.len++] = c;
            else overflow_ = true;
            if (c == 0xF7) {
                if (!overflow_) {
                    out(m_);
                    ++count;
                }
                active_ = overflow_ = false;
                m_.len = 0;
            }
        }
        return count;
    }

private:
    MidiMsg m_;
    bool active_ = false, overflow_ = false;
};

class MidiTransport {
public:
    virtual ~MidiTransport() = default;
    // Finds and opens the device. False with why set if it is not there (call again later).
    virtual bool open(std::string& why) = 0;
    virtual void close() = 0;
    // One whole message into m: 1 = got one, 0 = nothing within timeout, -1 = link lost.
    virtual int read(MidiMsg& m, int timeout_ms) = 0;
    // One message (short or SysEx). False = the device is gone.
    virtual bool send(const uint8_t* data, size_t len) = 0;
    // Still plugged in? (hardware: the port is still listed; sim: datagrams lately)
    virtual bool present() = 0;
    virtual std::string port_name() const = 0;
    virtual bool simulated() const = 0;
};

// Real device over WinMM. hints = parts of the port name (case-insensitive), e.g.
// {"Lightpad"} or {"LUMI", "USBMidi2-ACX"}; pick (optional) = a port number or a
// name part that wins over the hints.
std::unique_ptr<MidiTransport> make_winmm(std::vector<std::string> hints, std::string pick = {});
// Simulated device on 127.0.0.1:port. silent_ms = how long without a datagram
// counts as "simulator gone".
std::unique_ptr<MidiTransport> make_udp(uint16_t port, uint32_t silent_ms = 2000);

// Port names WinMM sees (for logs and --ports).
std::vector<std::string> list_midi_in();
std::vector<std::string> list_midi_out();

}  // namespace himmel::weapon
