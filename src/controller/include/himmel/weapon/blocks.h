// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: the ROLI BLOCKS protocol (Lightpad Block, LUMI Keys).
//
// C++ port of Himmel's Python reference (tools/lightpad_sim.py codec + HeapSync,
// tools/lightpad_littlefoot.py), which was written from reading ROLI's
// roli_blocks_basics (ISC). No ROLI code is copied. Checked on the owner's
// Lightpad M and LUMI Keys through the Python tools (2026-11-10).
//
// SysEx: F0 00 21 10 77 <index> <LSB-first bits packed into 7-bit bytes> <checksum> F7.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "himmel/weapon/midi.h"

namespace himmel::weapon::blocks {

inline constexpr uint8_t kHeader[5] = {0xF0, 0x00, 0x21, 0x10, 0x77};
inline constexpr uint8_t kSerialRequest[7] = {0xF0, 0x00, 0x21, 0x10, 0x78, 0x3F, 0xF7};
inline constexpr uint8_t kSerialHeader[5] = {0xF0, 0x00, 0x21, 0x10, 0x78};
inline constexpr size_t kDataBlock = 7200;      // padBlockProgramAndHeapSize
inline constexpr size_t kMaxPacketBits = 7 * 140;
inline constexpr int kGrid = 15;
inline constexpr uint32_t kPingIntervalUs = 400000;

// Device -> host message types
enum : uint32_t {
    kDevTopology = 0x01, kDevAck = 0x02,
    kTouchStart = 0x10, kTouchMove = 0x11, kTouchEnd = 0x12,
    kTouchStartV = 0x13, kTouchMoveV = 0x14, kTouchEndV = 0x15,
    kButtonDown = 0x20, kButtonUp = 0x21,
    kDevVersion = 0x06, kDevName = 0x07, kDevConfig = 0x18, kDevProgramEvent = 0x28,
};
// Host -> device
enum : uint32_t { kHostCommand = 0x01, kHostData = 0x02, kHostProgramEvent = 0x03, kHostConfig = 0x10 };
enum : uint32_t { kCmdBeginApi = 0, kCmdTopology = 1, kCmdEndApi = 2, kCmdPing = 3 };
// Config items (roli_BlockConfigId.h, via blocksd)
enum : uint32_t { kCfgOctave = 4, kCfgVelocitySensitivity = 10, kCfgPressureSensitivity = 13 };
inline constexpr int32_t kButtonEvent = 1;  // our renderer: sendMessageToHost(1, button, down)

// ---------------------------------------------------------------- bit packing
class BitWriter {
public:
    BitWriter& put(uint32_t value, int bits);
    size_t bits() const { return nbits_; }
    size_t nbytes() const { return (nbits_ + 6) / 7; }
    const uint8_t* bytes() const { return b_.data(); }

private:
    std::array<uint8_t, 320> b_{};
    size_t nbits_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t n) : d_(data), total_(7 * n) {}
    size_t left() const { return total_ - pos_; }
    uint32_t get(int bits);

private:
    const uint8_t* d_;
    size_t total_, pos_ = 0;
};

uint8_t checksum(const uint8_t* body, size_t n);
MidiMsg frame(uint8_t index, const BitWriter& w);
// Body of a BLOCKS SysEx (checksum checked). False if it is not one.
bool unframe(const uint8_t* msg, size_t n, uint8_t& index, const uint8_t*& body, size_t& body_len);

MidiMsg host_command(uint32_t cmd, uint8_t index = 0);
MidiMsg host_config_set(uint32_t item, int32_t value, uint8_t index = 0);
MidiMsg host_config_request(uint32_t item, uint8_t index = 0);
MidiMsg serial_request();

struct DataPacket {
    uint16_t index;  // 10-bit packet counter
    MidiMsg msg;
};
// sharedDataChange packets that turn old into new (n bytes each). Each packet starts at
// byte 0, so a resend is harmless. The counter wraps 1023 -> 0 (1024 hangs a real block).
void host_data_changes(uint16_t packet_index, const uint8_t* old_data, const uint8_t* new_data, size_t n,
                       std::vector<DataPacket>& out);

// Device side of one sharedDataChange body (after the 7-bit type): applies it to data.
// Used by the tests and the C++ side of nothing else. False if the stream is broken.
bool apply_data_change(BitReader& r, uint8_t* data, size_t n, uint16_t& packet);

// ---------------------------------------------------------------- decoding the device
struct DeviceEvent {
    enum Kind : uint8_t { Serial, Topology, Ack, Touch, Button, Program, Config, Version, Name, Unknown } kind;
    uint8_t phase = 0;      // touch: 0 start, 1 move, 2 end
    uint8_t index = 0;      // touch: finger; version/name: block index
    bool down = false;      // button
    bool via_program = false;
    bool has_velocity = false;
    uint16_t x = 0, y = 0;  // touch 12 bit
    uint8_t z = 0;          // touch 8 bit
    int8_t vx = 0, vy = 0, vz = 0;
    uint32_t id = 0;        // button id, ack packet, config item, message type (unknown)
    uint32_t t = 0;         // device timestamp (ms)
    int32_t value = 0, min = 0, max = 0;  // config; program: values[0..2] in value/min/max
    uint8_t battery = 0;    // topology (first device): 0..31
    bool charging = false;
    char text[24] = {};     // serial / version / name
};
// Decodes one device -> host message (BLOCKS SysEx or serial reply). Returns how many
// events went into out (at most cap).
int decode_device(const uint8_t* msg, size_t n, DeviceEvent* out, int cap);

// ---------------------------------------------------------------- LittleFoot renderer
int16_t function_id(std::string_view signature);
uint16_t program_checksum(const uint8_t* prog, size_t n);
// Himmel's renderer: reads an RGB565 bitmap from its heap and fills every LED.
// Lightpad: 15 x 15 (450 heap bytes). keep_key_handler: setUseDefaultKeyHandler(true, false)
// (LUMI: the block plays the keys, we only light them).
std::vector<uint8_t> build_bitmap_program(int heap_bytes = kGrid * kGrid * 2, int width = kGrid, int height = kGrid,
                                          bool keep_key_handler = false);
std::vector<uint8_t> build_key_program(int keys = 24);
struct ProgramInfo {
    uint16_t size = 0, heap = 0, functions = 0;
};
bool parse_program(const uint8_t* block, size_t n, ProgramInfo& info);

// 8-bit RGB -> the renderer's RGB565 heap bytes (little endian; r = low 5 bits).
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
}
void pixels_to_bytes(const uint8_t (*rgb)[3], int count, uint8_t* out);

// ---------------------------------------------------------------- HeapSync
// Keeps the block's shared data in step with a target, like ROLI's LittleFootRemoteHeap:
// diff -> packets -> wait for the ACKs -> confirmed. ACKs are cumulative (seen on a
// Lightpad M). Packets not ACKed within 250 ms go again.
class HeapSync {
public:
    static constexpr uint64_t kResendUs = 250000;
    HeapSync() { reset(); }
    void reset();
    bool busy() const { return !inflight_.empty(); }
    // Starts a sync to target (kDataBlock bytes). Returns the packets to send now.
    const std::vector<DataPacket>& start(const uint8_t* target, uint64_t t);
    // Returns true when this ACK completed the sync.
    bool on_ack(uint16_t index, uint64_t t);
    // Packets to send again (empty if nothing is late).
    const std::vector<DataPacket>& due(uint64_t t);
    const std::array<uint8_t, kDataBlock>& target() const { return target_; }
    uint64_t frames = 0, resends = 0;
    uint32_t sync_us = 0;   // last completed sync, send -> last ACK
    size_t bytes_last = 0;

private:
    bool known_ = false;  // confirmed_ is valid
    std::array<uint8_t, kDataBlock> confirmed_{}, pending_{}, target_{};
    std::vector<DataPacket> inflight_, out_;
    uint16_t index_ = 0;
    uint64_t sent_at_ = 0, started_ = 0;
};

}  // namespace himmel::weapon::blocks
