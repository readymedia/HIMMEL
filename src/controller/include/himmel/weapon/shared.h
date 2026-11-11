// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: the shared-memory interface of one weapon slot.
//
// One named block "Local\HimmelWeapon<N>" per slot (N = 1..kMaxSlots; by default
// the weapon's number: 2 Lightpad, 3 LUMI Keys, 5 NuVJ). It has the same parts as
// "Local\HimmelMk2" (himmel/mk2/shared.h), so the engine reads every weapon the
// same way:
//   * Header  - which weapon this is (id, name, profile), what it can do
//   * Status  - written by the driver process (connected, heartbeat, counters)
//   * Outputs - written by ONE app writer (the engine) with a sequence lock,
//               read by the driver: LED levels per control, RGB pixels, a display
//   * Rings   - the same lock-free SPSC event rings of 16-byte mk2::Event.
//               Ring 0 belongs to the engine, as on the MK2.
// Event ids (docs/architecture.md, weapons/README.md "Events"):
//   ButtonDown/Up, KnobDelta, EncoderDelta, Fader, Switch: id = control index in
//   the weapon's profile (weapons/<id>.weapon.json, "controls" order).
//   PadHit/PadPressure/PadRelease: id = pad number 1..n (the weapon's own pad
//   numbering, Pad 1 = bottom-left as on the MK2), reserved = control index.
//   TouchStart/Move/End: id = finger, value = x, raw = y (0..4095), reserved = z | strike << 8.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"

namespace himmel::weapon {

using mk2::Event;
using mk2::EventRing;
using mk2::EventType;
using mk2::now_us;

inline constexpr uint32_t kShmMagic = 0x4E504157;  // "WAPN"
inline constexpr uint32_t kShmVersion = 1;
inline constexpr int kMaxSlots = 16;
inline constexpr int kMaxConsumers = mk2::kMaxConsumers;
inline constexpr int kEngineRing = 0;
inline constexpr int kMaxControls = 128;
inline constexpr int kMaxPixels = 256;

// What a weapon can do (Header::caps).
enum Caps : uint32_t {
    kCapButtons = 1u << 0,
    kCapPads = 1u << 1,       // PadHit / PadPressure / PadRelease
    kCapTouch = 1u << 2,      // TouchStart / Move / End
    kCapKnobs = 1u << 3,      // KnobDelta / EncoderDelta
    kCapFaders = 1u << 4,     // Fader / Switch
    kCapLedLevels = 1u << 5,  // Outputs::led (per control)
    kCapPixels = 1u << 6,     // Outputs::pixel (RGB)
    kCapDisplay = 1u << 7,    // Outputs::display
};

// What a control does in the engine skeleton until Himmel has real mapping pages
// (the driver fills Header::role from the profile; weapons/README.md has the table).
enum Role : uint8_t {
    kRoleNone = 0,
    kRoleKnob,      // knob_slot[] = 0..7: the engine's 8 knob slots (KnobDelta, or Fader as absolute)
    kRoleBrowse,    // EncoderDelta / button: next (+) or previous (-) clip
    kRoleBrowsePrev,
    kRoleBrowseNext,
    kRolePageNext,  // knob page: test card -> clip -> camera
    kRolePagePrev,
    kRolePlay,      // pause / play the clip
    kRoleRestart,   // clip to the start
    kRoleMirror,    // span / mirror
};
inline constexpr uint8_t kNone = 0xFF;
inline constexpr int kMaxPads = 64;

struct Header {
    char weapon[16];     // profile id: "lightpad", "lumi", "nuvj"
    char name[32];       // "Lightpad", "LUMI Keys", "NuVJ"
    char profile[260];   // weapons/<id>.weapon.json (as the driver found it)
    char driver[32];     // "weapon_blocks", "weapon_nuvj"
    uint32_t slot;
    uint32_t caps;       // Caps bits
    uint32_t controls;   // number of controls in the profile
    uint32_t pads;       // pad numbers 1..pads
    uint16_t pixel_w;    // pixel grid (Lightpad 15 x 15, LUMI 24 x 1)
    uint16_t pixel_h;
    uint32_t simulated;  // 1 = --sim
    uint8_t role[kMaxControls];        // Role per control index
    uint8_t knob_slot[kMaxControls];   // 0..7 for kRoleKnob, else kNone
    uint8_t pad_control[kMaxPads];     // pad number - 1 -> control index (kNone = no control, e.g. a zone)
};

struct Status {
    std::atomic<uint32_t> connected;
    std::atomic<uint32_t> map_verified;
    std::atomic<uint64_t> heartbeat_us;   // driver alive (updated every ~20 ms)
    std::atomic<uint64_t> msgs_in;
    std::atomic<uint64_t> msgs_out;
    std::atomic<uint64_t> reconnects;
    std::atomic<uint64_t> led_frames;     // LED / pixel / display updates sent
    std::atomic<uint64_t> acks;           // BLOCKS: data packets confirmed
    std::atomic<uint64_t> resends;        // BLOCKS: data packets sent again
    std::atomic<int32_t> battery;         // 0..31 (BLOCKS), -1 = unknown
    std::atomic<uint32_t> sync_us;        // BLOCKS: last LED frame send -> last ACK
    std::atomic<uint32_t> stop_request;   // set by the supervisor (or weapon_monitor --stop): let go and exit
    char device[48];                      // serial or port name (ASCII)
    char message[128];                    // last notable thing (ASCII), for UIs/logs
};

// A small character / segment display (the NuVJ LCD). Model fields; the driver
// knows the glass. Unused fields stay zero.
struct Display {
    char text[16];        // dot-matrix row (NuVJ: 8 characters; longer = 7 + ".")
    char alpha[16];       // 16-segment row (NuVJ: 11 characters)
    char number[2][4];    // digit groups, right-aligned (NuVJ: [0] BPM "188", [1] PRESET)
    int8_t bar[2];        // ramps, 0..steps, -1 = off (NuVJ: LAST, DATA, 7 steps)
    uint8_t reserved[2];
    uint32_t marks;       // words and boxes lit (NuVJ: nuvj::Mark bits)
    uint32_t raw_mode;    // 1 = send raw[] as the segment memory, ignore the model
    uint8_t raw[64];
};

struct Outputs {
    std::atomic<uint32_t> led_seq;    // 0 = the engine never wrote LEDs
    uint8_t led[kMaxControls];        // 0..127 by control index (one-colour LEDs)
    std::atomic<uint32_t> pixel_seq;
    uint8_t pixel[kMaxPixels][3];     // RGB 0..255; Lightpad row-major from the top-left, LUMI by key
    std::atomic<uint32_t> display_seq;
    Display display;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas(64) is intended (no false sharing)
#endif
struct SharedBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t reserved;
    Header header;
    Status status;
    Outputs outputs;
    EventRing rings[kMaxConsumers];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// "Local\HimmelWeapon<slot>"
std::wstring shm_name(int slot);

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // Driver: create + initialise. A restarted driver keeps the engine's outputs and
    // attached rings, but resets the status and writes the header again.
    bool create(int slot, const Header& h);
    // Apps: attach to an existing block (false if it does not exist or is not a weapon block).
    bool open(int slot);
    void close();
    SharedBlock* get() const { return block_; }

    // Publish one event to every attached ring (driver side).
    void publish(const Event& e);
    void set_message(const std::string& m);

private:
    void* mapping_ = nullptr;
    SharedBlock* block_ = nullptr;
};

// Copies a string into a fixed char array (always terminated).
template <size_t N>
void set_text(char (&dst)[N], const std::string& s) {
    const size_t n = s.size() < N - 1 ? s.size() : N - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

}  // namespace himmel::weapon
