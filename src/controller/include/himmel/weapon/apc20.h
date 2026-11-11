// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: the Akai APC20 (weapon no. 9, a secondary weapon).
//
// C++ port of tools/apc20_sim.py (wire table, decoder, mode SysEx). Protocol:
// weapons/README.md "Weapon no. 9" (from Ableton's APC20 script and Akai's
// Quickstart Guide; not yet checked on the owner's unit).
//   Controls -> host: note on/off (buttons), CC 7 / CC 14 (faders), CC 47 relative (Cue Level).
//   Host -> controls: identity request, F0 47 <dev> 7B 60 00 04 <mode> <maj> <min> <bug> F7,
//   and note on to a control's own note = its LED (grid 0..6 colours, others 0..2).
//   Nothing else is ever sent.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "himmel/weapon/driver.h"
#include "himmel/weapon/midi.h"
#include "himmel/weapon/shared.h"

namespace himmel::weapon::apc20 {

inline constexpr int kNumber = 9;  // weapon number = default slot
inline constexpr uint8_t kManufacturer = 0x47;  // Akai
inline constexpr uint8_t kModel = 0x7B;         // APC20 (APC40 = 0x73)
enum Mode : uint8_t { kModeGeneric = 0x40, kModeAbleton = 0x41, kModeAbletonAlt = 0x42, kModeNote = 0x43 };
inline constexpr uint8_t kIdentityRequest[6] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
inline constexpr uint8_t kNoteModeCh = 9;  // note mode: the grid plays notes 36..75 on channel 10
inline constexpr int kCols = 8, kRows = 5, kPads = kCols * kRows;
inline constexpr int kControls = 97;

// Grid LED velocities (bi-colour LEDs; the device blinks by itself).
enum Tri : uint8_t { kOff = 0, kGreen = 1, kGreenBlink = 2, kRed = 3, kRedBlink = 4, kAmber = 5, kAmberBlink = 6 };

// The mode message. dev = the device id from the identity reply (0 if none came).
MidiMsg mode_sysex(uint8_t mode, uint8_t dev);
// An identity reply: true and dev set; is_apc20 = manufacturer 47, model 7B.
bool parse_identity(const uint8_t* msg, size_t n, uint8_t& dev, bool& is_apc20);
// Note mode (Quickstart Guide): two 4 x 5 halves, C1 (36) bottom-left .. D#3 (75) top-right.
// track 0..7, scene 0..4 (0 = top row).
int note_mode_note(int track, int scene);
// Pad number of a grid button: 1 = bottom-left, 40 = top-right (as the MK2's Pad 1).
int pad_number(int track, int scene);

// The control list of weapons/apc20.weapon.json, in its order (used when the file is missing,
// and by the tests to check the file).
Profile builtin_profile();

// ---------------------------------------------------------------- the map
enum class Kind : uint8_t { None, Button, Fader, Encoder };
enum class Palette : uint8_t { None, Tri, Mono };
struct Entry {
    Kind kind = Kind::None;
    bool cc = false;  // false = note
    uint8_t ch = 0, num = 0;
    Palette led = Palette::None;  // LEDs answer on the control's own note/channel
    uint8_t pad = 0;              // grid: pad number 1..40
};
struct Map {
    std::array<Entry, kMaxControls> controls{};  // by profile control index
    int count = 0;
    bool verified = false;   // every control seen on the owner's unit
    bool from_file = false;
};
// The table from the protocol facts, by control name.
Map default_map(const Profile& p);
// apc20_map.json (learned in tools/apc20_sim.py): its "controls" override the table.
std::optional<Map> load_map(const std::filesystem::path& path, const Profile& p, std::string* err = nullptr);

// ---------------------------------------------------------------- input
class Decoder {
public:
    explicit Decoder(const Map& m);
    // MIDI from the device -> Himmel events (shared.h ids). Returns how many (0 or 1).
    int decode(const uint8_t* msg, size_t n, uint64_t t, Event* out, int cap);
    bool held(int control) const { return control >= 0 && control < kMaxControls && held_[static_cast<size_t>(control)]; }

private:
    const Map& m_;
    std::array<std::array<int16_t, 128>, 16> note_{}, cc_{};
    std::array<int16_t, 128> note_mode_{};
    std::array<bool, kMaxControls> held_{};
};

// ---------------------------------------------------------------- output
// The engine's LED level (Outputs::led, 0..127) -> the APC velocity.
//   Levels as for every weapon: grid 0..7 off, 8..63 amber, 64..127 green;
//   other LEDs 0..31 off (the engine's "dim" 20), 32..127 on.
//   Exact APC codes: kLedCode + 1..6 (121..126) = green, green blink, red, red blink,
//   amber, amber blink on the grid; on the other LEDs an even code blinks, an odd one is on.
inline constexpr uint8_t kLedCode = 120;
uint8_t led_velocity(Palette p, uint8_t level);

// ---------------------------------------------------------------- the driver session
Header make_header(const Profile& p, bool sim);

class Apc20Session final : public Session {
public:
    Apc20Session(const Map& m, SharedMemory& shm);
    bool start(MidiTransport& io, uint64_t t) override;
    void on_message(const MidiMsg& m, uint64_t t) override;
    bool tick(MidiTransport& io, uint64_t t) override;
    void let_go(MidiTransport& io) override;

private:
    bool send(MidiTransport& io, const uint8_t* b, size_t n);
    const Map& map_;
    SharedMemory& shm_;
    Decoder dec_;
    enum class State { Identity, Run } state_ = State::Identity;
    bool got_identity_ = false;
    uint8_t dev_ = 0;
    uint64_t identity_deadline_ = 0, next_ = 0, next_full_ = 0, next_hello_ = 0;
    uint32_t led_seen_ = 0;
    std::array<uint8_t, kMaxControls> levels_{};
    std::array<int16_t, kMaxControls> sent_{};  // -1 = unknown: send
};

}  // namespace himmel::weapon::apc20
