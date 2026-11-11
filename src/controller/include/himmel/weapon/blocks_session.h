// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: one BLOCKS device session (Lightpad Block or LUMI Keys).
// The C++ form of LocalHost / LumiHost in tools/lightpad_sim.py and tools/lumi_sim.py:
//   handshake (serial, topology, endAPIMode, beginAPIMode, config read + set),
//   wipe the program memory, upload Himmel's renderer, stream LED frames from the
//   shared block, ping every 400 ms on its own timer, restore config on let go.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "himmel/weapon/blocks.h"
#include "himmel/weapon/driver.h"

namespace himmel::weapon::blocks {

enum class Kind { Lightpad, Lumi };

inline constexpr int kLumiKeys = 24;
inline constexpr int kLumiBaseNote = 48;
inline constexpr uint64_t kNoAnswerUs = 3000000;  // no SysEx for this long in API mode: handshake again

struct DeviceSetup {
    Kind kind;
    const char* weapon;    // profile id
    const char* name;
    int number;            // weapon number = default slot
    uint16_t sim_port;
    std::vector<std::string> port_hints;
    int pixels, pixel_w, pixel_h;
    std::map<uint32_t, int32_t> want;         // config Himmel sets (restored on let go)
    std::vector<uint32_t> config_read;        // read at every handshake
};
DeviceSetup lightpad_setup();
DeviceSetup lumi_setup();

// Fills the header (caps, roles, pads) from the profile.
Header make_header(const DeviceSetup& d, const Profile& p, bool sim);

class BlocksSession final : public Session {
public:
    BlocksSession(const DeviceSetup& d, const Profile& p, SharedMemory& shm);
    bool start(MidiTransport& io, uint64_t t) override;
    void on_message(const MidiMsg& m, uint64_t t) override;
    bool tick(MidiTransport& io, uint64_t t) override;
    void let_go(MidiTransport& io) override;

private:
    enum class Stage { Clear, Program, Frames };
    bool send(MidiTransport& io, const MidiMsg& m);
    bool handshake(MidiTransport& io, uint64_t t);
    void on_device(const DeviceEvent& e, uint64_t t);
    void on_midi(const MidiMsg& m, uint64_t t);
    void button(uint32_t id, bool down, bool via_program, uint64_t t);
    void build_pixels();
    void publish(EventType type, uint8_t id, int16_t value, uint16_t raw, uint16_t reserved, uint64_t t);

    DeviceSetup d_;
    SharedMemory& shm_;
    std::vector<uint8_t> program_;
    // profile indices
    int mode_idx_ = -1;
    std::array<int, 3> button_idx_{};   // BLOCKS button id -> control index
    std::array<int, kLumiKeys> key_idx_{};
    // protocol state
    bool api_ = false;
    Stage stage_ = Stage::Clear;
    HeapSync sync_;
    uint64_t next_ping_ = 0, last_rx_ = 0, last_pic_ = 0;
    std::map<uint32_t, std::array<int32_t, 3>> config_;  // item -> value, min, max (as the block says)
    std::map<uint32_t, int32_t> original_;               // first value seen per item
    bool firmware_buttons_ = false;
    // input state
    std::array<int8_t, 32> finger_pad_{};  // Lightpad: finger -> zone pad 1..16 (0 = none)
    std::array<std::array<uint16_t, 3>, 32> fingers_{};  // x, y, z+1 (0 = up), for the idle picture
    std::array<int8_t, 16> chan_key_{};    // LUMI: MIDI channel -> key + 1
    std::array<uint8_t, kLumiKeys> key_vel_{};
    // LED frames
    uint32_t pixel_seen_ = 0xFFFFFFFF;
    std::array<uint8_t, kMaxPixels * 3> pixels_{};
    std::array<uint8_t, kDataBlock> frame_{};
};

}  // namespace himmel::weapon::blocks
