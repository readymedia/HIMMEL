// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: the Numark NuVJ (weapon no. 5).
//
// C++ port of tools/nuvj_sim.py (Decoder, feedback codec, LED bits, LCD segments).
// Protocol: docs/nuvj-protocol.md. Checked on the owner's unit 2026-11-11.
//   Controls -> host: notes, CCs (absolute, relative), pitch bend on channels 1-3.
//   Host -> controls: F0 00 01 3F 7F 20 <cmd> 00 <len> <data> F7
//     60 init (01 01 01 00), 01 LCD text (8 ASCII), 02 LCD segments (64), 03 LEDs (8).
//     7B (store contrast in the controller) is never sent: feedback() refuses it.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "himmel/weapon/driver.h"
#include "himmel/weapon/midi.h"
#include "himmel/weapon/shared.h"

namespace himmel::weapon::nuvj {

inline constexpr uint8_t kHeader[5] = {0x00, 0x01, 0x3F, 0x7F, 0x20};  // Numark, all devices, NuVJ
enum : uint8_t {
    kCmdText = 0x01,
    kCmdSegments = 0x02,
    kCmdLeds = 0x03,
    kCmdInit = 0x60,
    kCmdContrast = 0x7A,
    kCmdContrastStore = 0x7B,  // never sent by Himmel
};
inline constexpr uint8_t kInitData[4] = {0x01, 0x01, 0x01, 0x00};
inline constexpr int kAlphaCells = 11;
inline constexpr int kRampSteps = 7;
inline constexpr int kPads = 18;  // A pads 1..9 = pad numbers 1..9, B pads = 10..18

// One feedback SysEx. len = 0 for cmd 7B (refused) or data too long.
MidiMsg feedback(uint8_t cmd, const uint8_t* data, size_t n);
bool parse_feedback(const uint8_t* msg, size_t n, uint8_t& cmd, const uint8_t*& data, size_t& len);
// 8 characters; longer text is cut to 7 + "." (as the NuVJ software did).
void text_bytes(std::string_view text, uint8_t out[8]);

// Words and boxes on the LCD (Display::marks bits). Names = keys in nuvj_map.json "lcd_segments".
enum Mark : uint32_t {
    kMarkRule = 1u << 0, kMarkBpm = 1u << 1, kMarkPreset = 1u << 2, kMarkLast = 1u << 3, kMarkData = 1u << 4,
    kMarkFull = 1u << 5, kMark16Levels = 1u << 6, kMarkBoxA = 1u << 7, kMarkBoxB = 1u << 8, kMarkBoxC = 1u << 9,
    kMarkBoxD = 1u << 10, kMarkEnter = 1u << 11, kMarkProg = 1u << 12, kMarkNote = 1u << 13, kMarkAft = 1u << 14,
    kMarkCtrl = 1u << 15, kMarkMidiCh = 1u << 16, kMarkPage = 1u << 17, kMarkProgram = 1u << 18,
    kMarkChange = 1u << 19, kMarkGlobal = 1u << 20, kMarkEdit = 1u << 21,
};
inline constexpr int kMarks = 22;
inline constexpr std::array<std::string_view, kMarks> kMarkNames = {
    "rule", "lbl_bpm", "lbl_preset", "lbl_last", "lbl_data", "lbl_full", "lbl_16levels", "box_a",
    "box_b", "box_c", "box_d", "enter", "lbl_prog", "lbl_note", "lbl_aft", "lbl_ctrl",
    "lbl_midich", "lbl_page", "lbl_program", "lbl_change", "lbl_global", "lbl_edit"};

// ---------------------------------------------------------------- the map
enum class Type : uint8_t { None, Note, Cc, Pb };
enum class Enc : uint8_t { Plain, Abs, Rel, RelAbs, Fader, Switch, Toggle };
struct Entry {
    Type type = Type::None;
    uint8_t ch = 0, num = 0;
    Enc enc = Enc::Plain;
    bool cw_neg = false;  // relative: clockwise sends the "negative" values
    uint8_t step = 2;     // relabs: 0..127 units per tick
    uint8_t nvalues = 0;
    uint8_t values[4] = {};
};
using Bits = std::vector<std::pair<uint8_t, uint8_t>>;  // (byte, mask)

struct Lcd {
    std::array<Bits, kMarks> marks;
    std::array<std::array<Bits, 7>, 3> bpm;     // digit 0..2 (left to right), segment a..g
    std::array<std::array<Bits, 7>, 2> preset;
    std::array<std::array<Bits, 16>, kAlphaCells> alpha;  // segment a1 a2 b c d1 d2 e f g1 g2 h i j k l m
    std::array<Bits, kRampSteps> last, data;
    bool preset_learned = false;
};

struct Map {
    std::vector<Entry> controls;  // by profile control index
    Lcd lcd;
    bool verified = false;
    bool from_file = false;
};
// The starting table (public forum post + mappings.xml), for a missing map file.
Map default_map(const Profile& p);
// nuvj_map.json; controls the file does not have keep the default.
std::optional<Map> load_map(const std::filesystem::path& path, const Profile& p, std::string* err = nullptr);

// ---------------------------------------------------------------- input
class Decoder {
public:
    Decoder(const Map& m, const Profile& p);
    // MIDI from the device -> Himmel events (see shared.h for the ids). Returns how many.
    int decode(const uint8_t* msg, size_t n, uint64_t t, Event* out, int cap);
    int pad_number(int control) const { return control >= 0 && control < kMaxControls ? pad_of_[control] : 0; }
    // Controls held down right now (buttons and pads), by control index.
    bool held(int control) const { return control >= 0 && control < kMaxControls && held_[control]; }

private:
    const Map& m_;
    std::array<std::array<int16_t, 128>, 16> note_{}, cc_{};
    std::array<int16_t, 16> pb_{};
    std::array<uint8_t, kMaxControls> pad_of_{};
    std::array<int16_t, kMaxControls> acc_{}, last_abs_{};
    std::array<bool, kMaxControls> held_{};
};

// ---------------------------------------------------------------- output
// 8 LED bytes from levels by control index (on when level >= 32).
struct LedTable {
    std::vector<std::array<int, 3>> bits;  // control index, byte, mask
};
LedTable led_table(const Profile& p);
void led_bytes(const LedTable& t, const uint8_t* levels, uint8_t out[8]);
// 64 bytes of segment memory for a display model.
void render_segments(const Display& d, const Lcd& lcd, uint8_t out[64]);
// The segments one 16-segment character lights (bit i = segment i in a1 a2 b c ...).
uint16_t seg16(char c);

// ---------------------------------------------------------------- the driver session
inline constexpr int kNumber = 5;  // weapon number = default slot
Header make_header(const Profile& p, bool sim);

class NuvjSession final : public Session {
public:
    NuvjSession(const Map& m, const Profile& p, SharedMemory& shm);
    bool start(MidiTransport& io, uint64_t t) override;
    void on_message(const MidiMsg& m, uint64_t t) override;
    bool tick(MidiTransport& io, uint64_t t) override;
    void let_go(MidiTransport& io) override;

private:
    bool send_fb(MidiTransport& io, uint8_t cmd, const uint8_t* data, size_t n);
    const Map& map_;
    const Profile& prof_;
    SharedMemory& shm_;
    Decoder dec_;
    LedTable leds_;
    uint64_t next_ = 0, next_full_ = 0;
    bool have_[3] = {};
    uint8_t sent_leds_[8] = {}, sent_text_[8] = {}, sent_segs_[64] = {};
    uint32_t led_seen_ = 0, disp_seen_ = 0;
    uint8_t levels_[kMaxControls] = {};
    Display disp_{};
};

}  // namespace himmel::weapon::nuvj
