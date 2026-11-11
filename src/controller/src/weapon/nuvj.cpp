// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// NuVJ codec, map, decoder, LCD and driver session (see nuvj.h). Port of tools/nuvj_sim.py;
// weapon_tests.cpp checks the bytes against the Python output.
#include "himmel/weapon/nuvj.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace himmel::weapon::nuvj {

// ---------------------------------------------------------------- feedback codec
MidiMsg feedback(uint8_t cmd, const uint8_t* data, size_t n) {
    MidiMsg m;
    if (cmd == kCmdContrastStore || n > 127 || n + 10 > MidiMsg::kCap) return m;  // never store settings in the controller
    m.b[0] = 0xF0;
    std::memcpy(m.b + 1, kHeader, 5);
    m.b[6] = cmd;
    m.b[7] = 0x00;
    m.b[8] = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) m.b[9 + i] = data[i] & 0x7F;
    m.b[9 + n] = 0xF7;
    m.len = static_cast<uint16_t>(10 + n);
    return m;
}

bool parse_feedback(const uint8_t* msg, size_t n, uint8_t& cmd, const uint8_t*& data, size_t& len) {
    if (n < 10 || msg[0] != 0xF0 || msg[n - 1] != 0xF7 || std::memcmp(msg + 1, kHeader, 5) != 0) return false;
    cmd = msg[6];
    data = msg + 9;
    len = n - 10;
    return true;
}

void text_bytes(std::string_view text, uint8_t out[8]) {
    std::string t(text);
    if (t.size() > 8) t = t.substr(0, 7) + ".";
    for (int i = 0; i < 8; ++i) {
        const char c = i < static_cast<int>(t.size()) ? t[static_cast<size_t>(i)] : ' ';
        out[i] = static_cast<uint8_t>(c >= 32 && c < 127 ? c : '?');
    }
}

// ---------------------------------------------------------------- tables
namespace {

struct Src {
    const char* name;
    Type type;
    uint8_t num;  // pb: the channel
    Enc enc;
    std::array<uint8_t, 3> values;
    uint8_t nvalues;
};
// The starting table: a public forum post, confirmed by mappings.xml in the NuVJ software.
const Src kSource[] = {
    {"A Bank", Type::Cc, 14, Enc::Rel, {}, 0}, {"B Bank", Type::Cc, 15, Enc::Rel, {}, 0},
    {"A PMTR1", Type::Cc, 0, Enc::RelAbs, {}, 0}, {"B PMTR1", Type::Cc, 1, Enc::RelAbs, {}, 0},
    {"A PMTR2", Type::Cc, 2, Enc::RelAbs, {}, 0}, {"B PMTR2", Type::Cc, 3, Enc::RelAbs, {}, 0},
    {"A Latch", Type::Note, 8, Enc::Plain, {}, 0}, {"B Latch", Type::Note, 9, Enc::Plain, {}, 0},
    {"A Auto", Type::Note, 4, Enc::Plain, {}, 0}, {"B Auto", Type::Note, 5, Enc::Plain, {}, 0},
    {"A Sync", Type::Note, 6, Enc::Plain, {}, 0}, {"B Sync", Type::Note, 7, Enc::Plain, {}, 0},
    {"A Jog", Type::Cc, 17, Enc::Rel, {}, 0}, {"B Jog", Type::Cc, 16, Enc::Rel, {}, 0},
    {"A Scratch", Type::Note, 0, Enc::Plain, {}, 0}, {"B Scratch", Type::Note, 1, Enc::Plain, {}, 0},
    {"A Bank Push", Type::Note, 2, Enc::Plain, {}, 0}, {"B Bank Push", Type::Note, 3, Enc::Plain, {}, 0},
    {"Insta A", Type::Note, 10, Enc::Plain, {}, 0}, {"Insta B", Type::Note, 11, Enc::Plain, {}, 0},
    {"A Speed", Type::Pb, 0, Enc::Plain, {}, 0}, {"B Speed", Type::Pb, 1, Enc::Plain, {}, 0},
    {"Crossfader", Type::Pb, 2, Enc::Plain, {}, 0},
    {"FX Level", Type::Cc, 8, Enc::RelAbs, {}, 0}, {"Master PMTR1", Type::Cc, 4, Enc::RelAbs, {}, 0},
    {"Master PMTR2", Type::Cc, 5, Enc::RelAbs, {}, 0}, {"Master FX Select", Type::Cc, 9, Enc::Rel, {}, 0},
    {"Contrast", Type::Cc, 6, Enc::RelAbs, {}, 0}, {"Bright", Type::Cc, 7, Enc::RelAbs, {}, 0},
    {"Master FX", Type::Note, 30, Enc::Plain, {}, 0}, {"Insta Black", Type::Note, 31, Enc::Plain, {}, 0},
    {"BPM Tap", Type::Note, 32, Enc::Plain, {}, 0},
    {"Fader Mode", Type::Cc, 18, Enc::Switch, {1, 2, 0}, 3},
    {"Auto Fade Speed", Type::Cc, 13, Enc::Abs, {}, 0},
    {"Fader Start", Type::Note, 33, Enc::Toggle, {0, 65, 0}, 2},
    // pads 1-3 = bottom row, 7-9 = top row; deck A even notes, deck B the next odd one
    {"A Pad 1", Type::Note, 24, Enc::Plain, {}, 0}, {"B Pad 1", Type::Note, 25, Enc::Plain, {}, 0},
    {"A Pad 2", Type::Note, 26, Enc::Plain, {}, 0}, {"B Pad 2", Type::Note, 27, Enc::Plain, {}, 0},
    {"A Pad 3", Type::Note, 28, Enc::Plain, {}, 0}, {"B Pad 3", Type::Note, 29, Enc::Plain, {}, 0},
    {"A Pad 4", Type::Note, 18, Enc::Plain, {}, 0}, {"B Pad 4", Type::Note, 19, Enc::Plain, {}, 0},
    {"A Pad 5", Type::Note, 20, Enc::Plain, {}, 0}, {"B Pad 5", Type::Note, 21, Enc::Plain, {}, 0},
    {"A Pad 6", Type::Note, 22, Enc::Plain, {}, 0}, {"B Pad 6", Type::Note, 23, Enc::Plain, {}, 0},
    {"A Pad 7", Type::Note, 12, Enc::Plain, {}, 0}, {"B Pad 7", Type::Note, 13, Enc::Plain, {}, 0},
    {"A Pad 8", Type::Note, 14, Enc::Plain, {}, 0}, {"B Pad 8", Type::Note, 15, Enc::Plain, {}, 0},
    {"A Pad 9", Type::Note, 16, Enc::Plain, {}, 0}, {"B Pad 9", Type::Note, 17, Enc::Plain, {}, 0},
};

Entry entry_from(const Src& s) {
    Entry e;
    e.type = s.type;
    e.enc = s.enc;
    if (s.type == Type::Pb) e.ch = s.num;
    else e.num = s.num;
    if (s.enc == Enc::Rel) e.cw_neg = true;  // the post: clockwise reads as "slower"
    e.nvalues = s.nvalues;
    for (int i = 0; i < s.nvalues; ++i) e.values[i] = s.values[static_cast<size_t>(i)];
    return e;
}

constexpr std::array<std::string_view, 16> kSeg16Names = {"a1", "a2", "b", "c", "d1", "d2", "e", "f",
                                                          "g1", "g2", "h", "i", "j", "k", "l", "m"};
struct Glyph16 {
    char c;
    const char* segs;
};
const Glyph16 kSeg16[] = {
    {' ', ""}, {'0', "a1 a2 b c d1 d2 e f j k"}, {'1', "b c j"}, {'2', "a1 a2 b g1 g2 e d1 d2"},
    {'3', "a1 a2 b g2 c d1 d2"}, {'4', "f g1 g2 b c"}, {'5', "a1 a2 f g1 g2 c d1 d2"},
    {'6', "a1 a2 f e d1 d2 c g1 g2"}, {'7', "a1 a2 b c"}, {'8', "a1 a2 b c d1 d2 e f g1 g2"},
    {'9', "a1 a2 b c d1 d2 f g1 g2"}, {'A', "e f a1 a2 b c g1 g2"}, {'B', "a1 a2 b c d1 d2 i l g2"},
    {'C', "a1 a2 f e d1 d2"}, {'D', "a1 a2 b c d1 d2 i l"}, {'E', "a1 a2 f e d1 d2 g1"}, {'F', "a1 a2 f e g1"},
    {'G', "a1 a2 f e d1 d2 c g2"}, {'H', "f e b c g1 g2"}, {'I', "a1 a2 i l d1 d2"}, {'J', "b c d1 d2 e"},
    {'K', "f e g1 j m"}, {'L', "f e d1 d2"}, {'M', "f e b c h j"}, {'N', "f e b c h m"},
    {'O', "a1 a2 b c d1 d2 e f"}, {'P', "a1 a2 b f e g1 g2"}, {'Q', "a1 a2 b c d1 d2 e f m"},
    {'R', "a1 a2 b f e g1 g2 m"}, {'S', "a1 a2 f g1 g2 c d1 d2"}, {'T', "a1 a2 i l"}, {'U', "f e d1 d2 c b"},
    {'V', "f e k j"}, {'W', "f e b c k m"}, {'X', "h j k m"}, {'Y', "h j l"}, {'Z', "a1 a2 j k d1 d2"},
    {'-', "g1 g2"}, {'+', "g1 g2 i l"}, {'/', "j k"}, {'*', "g1 g2 h i j k l m"}, {'.', "d2"}, {'_', "d1 d2"},
    {'=', "g1 g2 d1 d2"}, {'\'', "i"}, {':', "i l"}, {'%', "a1 f g1 i j k l g2 c d2"}, {'?', "a1 a2 b g2 l"},
    {'#', "b c e f g1 g2 i l"},
};

int seg16_index(std::string_view name) {
    for (size_t i = 0; i < kSeg16Names.size(); ++i)
        if (kSeg16Names[i] == name) return static_cast<int>(i);
    return -1;
}

uint16_t parse_segs(const char* s) {
    uint16_t bits = 0;
    std::string_view v(s);
    while (!v.empty()) {
        const size_t sp = v.find(' ');
        const int i = seg16_index(v.substr(0, sp));
        if (i >= 0) bits = static_cast<uint16_t>(bits | (1u << i));
        if (sp == std::string_view::npos) break;
        v.remove_prefix(sp + 1);
    }
    return bits;
}

// 7-segment digits, as the NuVJ software encodes them.
const char* digit_segments(char c) {
    switch (c) {
        case '0': return "abcdef";
        case '1': return "bc";
        case '2': return "abged";
        case '3': return "abgcd";
        case '4': return "fgbc";
        case '5': return "afgcd";
        case '6': return "afgedc";
        case '7': return "abc";
        case '8': return "abcdefg";
        case '9': return "abcdfg";
        case '-': return "g";
        default: return "";
    }
}

Bits parse_bits(const nlohmann::json& v) {
    Bits b;
    if (!v.is_array() || v.empty()) return b;
    if (v[0].is_number()) {  // the older single [byte, mask]
        b.emplace_back(v[0].get<uint8_t>(), v[1].get<uint8_t>());
        return b;
    }
    for (const auto& x : v) b.emplace_back(x[0].get<uint8_t>(), x[1].get<uint8_t>());
    return b;
}

void or_bits(const Bits& b, uint8_t out[64]) {
    for (const auto& [byte, mask] : b)
        if (byte < 64) out[byte] = static_cast<uint8_t>(out[byte] | mask);
}

}  // namespace

uint16_t seg16(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& g : kSeg16)
        if (g.c == c) return parse_segs(g.segs);
    return parse_segs("a1 a2 b g2 l");  // '?'
}

// ---------------------------------------------------------------- the map
Map default_map(const Profile& p) {
    Map m;
    m.controls.assign(static_cast<size_t>(p.count()), Entry{});
    for (const auto& s : kSource) {
        const int i = p.index(s.name);
        if (i >= 0) m.controls[static_cast<size_t>(i)] = entry_from(s);
    }
    return m;
}

std::optional<Map> load_map(const std::filesystem::path& path, const Profile& p, std::string* err) {
    try {
        std::ifstream f(path);
        if (!f) {
            if (err) *err = "cannot open " + path.string();
            return std::nullopt;
        }
        const auto j = nlohmann::json::parse(f);
        Map m = default_map(p);
        m.verified = j.value("verified", false);
        m.from_file = true;
        const auto& cs = j.at("controls");
        for (int i = 0; i < p.count(); ++i) {
            auto it = cs.find(p.names[static_cast<size_t>(i)]);
            if (it == cs.end()) continue;  // a control added later keeps the forum guess
            const auto& c = *it;
            Entry e;
            const std::string type = c.contains("type") && c["type"].is_string() ? c["type"].get<std::string>() : "";
            e.type = type == "note" ? Type::Note : type == "cc" ? Type::Cc : type == "pb" ? Type::Pb : Type::None;
            e.ch = static_cast<uint8_t>(c.value("ch", 0) & 15);
            if (c.contains("num") && c["num"].is_number()) e.num = static_cast<uint8_t>(c["num"].get<int>() & 127);
            const std::string enc = c.contains("enc") && c["enc"].is_string() ? c["enc"].get<std::string>() : "";
            e.enc = enc == "abs" ? Enc::Abs : enc == "rel" ? Enc::Rel : enc == "relabs" ? Enc::RelAbs
                  : enc == "fader" ? Enc::Fader : enc == "switch" ? Enc::Switch : enc == "toggle" ? Enc::Toggle
                  : (e.type == Type::Cc ? Enc::Abs : Enc::Plain);
            e.cw_neg = c.value("cw", "pos") == "neg";
            e.step = static_cast<uint8_t>(c.value("step", 2));
            if (c.contains("values") && c["values"].is_array())
                for (const auto& v : c["values"])
                    if (e.nvalues < 4) e.values[e.nvalues++] = static_cast<uint8_t>(v.get<int>() & 127);
            m.controls[static_cast<size_t>(i)] = e;
        }
        if (j.contains("lcd_segments")) {
            for (const auto& [key, v] : j["lcd_segments"].items()) {
                const Bits b = parse_bits(v);
                for (int k = 0; k < kMarks; ++k)
                    if (kMarkNames[static_cast<size_t>(k)] == key) m.lcd.marks[static_cast<size_t>(k)] = b;
                const size_t d1 = key.find('.');
                if (d1 == std::string::npos) continue;
                const std::string head = key.substr(0, d1);
                const size_t d2 = key.find('.', d1 + 1);
                const int idx = std::atoi(key.substr(d1 + 1, d2 == std::string::npos ? std::string::npos : d2 - d1 - 1).c_str());
                const std::string seg = d2 == std::string::npos ? "" : key.substr(d2 + 1);
                if ((head == "bpm" || head == "preset") && seg.size() == 1 && seg[0] >= 'a' && seg[0] <= 'g') {
                    if (head == "bpm" && idx >= 0 && idx < 3) m.lcd.bpm[static_cast<size_t>(idx)][static_cast<size_t>(seg[0] - 'a')] = b;
                    if (head == "preset" && idx >= 0 && idx < 2) {
                        m.lcd.preset[static_cast<size_t>(idx)][static_cast<size_t>(seg[0] - 'a')] = b;
                        m.lcd.preset_learned = true;
                    }
                } else if (head == "alpha" && idx >= 0 && idx < kAlphaCells) {
                    const int s = seg16_index(seg);
                    if (s >= 0) m.lcd.alpha[static_cast<size_t>(idx)][static_cast<size_t>(s)] = b;
                } else if ((head == "last" || head == "data") && idx >= 0 && idx < kRampSteps) {
                    (head == "last" ? m.lcd.last : m.lcd.data)[static_cast<size_t>(idx)] = b;
                }
            }
        }
        return m;
    } catch (const std::exception& e) {
        if (err) *err = path.string() + ": " + e.what();
        return std::nullopt;
    }
}

// ---------------------------------------------------------------- decoder
Decoder::Decoder(const Map& m, const Profile& p) : m_(m) {
    for (auto& ch : note_) ch.fill(-1);
    for (auto& ch : cc_) ch.fill(-1);
    pb_.fill(-1);
    acc_.fill(64);
    last_abs_.fill(-1);
    for (size_t i = 0; i < m.controls.size() && i < kMaxControls; ++i) {
        const Entry& e = m.controls[i];
        if (e.type == Type::Note) note_[e.ch][e.num] = static_cast<int16_t>(i);
        else if (e.type == Type::Cc) cc_[e.ch][e.num] = static_cast<int16_t>(i);
        else if (e.type == Type::Pb) pb_[e.ch] = static_cast<int16_t>(i);
        const std::string& n = p.names[i];
        if (p.kinds[i] == "pad" && n.size() > 6 && (n[0] == 'A' || n[0] == 'B'))
            pad_of_[i] = static_cast<uint8_t>(std::atoi(n.c_str() + n.rfind(' ') + 1) + (n[0] == 'B' ? 9 : 0));
    }
    // After the host's init the NuVJ sends its faders as pitch bend on channels 1-3; before
    // it as CC 10-12 (the learned map). Both reach the same control.
    for (const auto& s : kSource)
        if (s.type == Type::Pb && pb_[s.num] < 0) pb_[s.num] = static_cast<int16_t>(p.index(s.name));
}

int Decoder::decode(const uint8_t* msg, size_t n, uint64_t t, Event* out, int cap) {
    if (n < 3 || cap < 1) return 0;
    const uint8_t hi = msg[0] & 0xF0, ch = msg[0] & 15;
    auto ev = [&](EventType type, int id, int value, int raw = 0, int reserved = 0) {
        out[0] = {t, type, static_cast<uint8_t>(id), static_cast<int16_t>(value), static_cast<uint16_t>(raw),
                  static_cast<uint16_t>(reserved)};
        return 1;
    };
    if (hi == 0x80 || hi == 0x90) {
        const int idx = note_[ch][msg[1] & 127];
        if (idx < 0) return 0;
        const Entry& e = m_.controls[static_cast<size_t>(idx)];
        if (e.enc == Enc::Toggle) {
            int pos = msg[2] ? 1 : 0;
            for (int k = 0; k < e.nvalues; ++k)
                if (e.values[k] == msg[2]) pos = k;
            return ev(EventType::Switch, idx, pos, msg[2]);
        }
        const bool down = hi == 0x90 && msg[2] > 0;
        if (held_[static_cast<size_t>(idx)] == down) return 0;  // a repeat
        held_[static_cast<size_t>(idx)] = down;
        if (const int pad = pad_of_[static_cast<size_t>(idx)])
            return down ? ev(EventType::PadHit, pad, msg[2], 0, idx) : ev(EventType::PadRelease, pad, 0, 0, idx);
        return ev(down ? EventType::ButtonDown : EventType::ButtonUp, idx, 0);
    }
    if (hi == 0xB0) {
        const int idx = cc_[ch][msg[1] & 127];
        if (idx < 0) return 0;
        const Entry& e = m_.controls[static_cast<size_t>(idx)];
        const int val = msg[2];
        switch (e.enc) {
            case Enc::Rel:
            case Enc::RelAbs: {
                int v = val >= 64 ? val - 128 : val;
                if (e.cw_neg) v = -v;
                if (e.enc == Enc::Rel) return ev(EventType::EncoderDelta, idx, v, val);
                int16_t& cur = acc_[static_cast<size_t>(idx)];
                cur = static_cast<int16_t>(std::clamp(cur + v * e.step, 0, 127));
                // In the MK2's knob units (1000 = the whole range), so the engine treats every knob alike.
                return ev(EventType::KnobDelta, idx, static_cast<int>(std::lround(v * e.step * 1000.0 / 127.0)), cur);
            }
            case Enc::Fader: return ev(EventType::Fader, idx, val * 16383 / 127, val);
            case Enc::Switch:
                for (int k = 0; k < e.nvalues; ++k)
                    if (e.values[k] == val) return ev(EventType::Switch, idx, k, val);
                return 0;
            default: {
                int16_t& last = last_abs_[static_cast<size_t>(idx)];
                const int delta = last < 0 ? 0 : static_cast<int>(std::lround((val - last) * 1000.0 / 127.0));
                last = static_cast<int16_t>(val);
                return ev(EventType::KnobDelta, idx, delta, val);
            }
        }
    }
    if (hi == 0xE0) {
        const int idx = pb_[ch];
        if (idx < 0) return 0;
        const int v = (msg[1] & 127) | ((msg[2] & 127) << 7);
        return ev(EventType::Fader, idx, v, v);
    }
    return 0;
}

// ---------------------------------------------------------------- output
LedTable led_table(const Profile& p) {
    LedTable t;
    auto add = [&](const std::string& name, int byte, int mask) {
        const int i = p.index(name);
        if (i >= 0) t.bits.push_back({i, byte, mask});
    };
    add("A Latch", 0, 0x01), add("A Auto", 0, 0x02), add("A Sync", 0, 0x04);
    add("B Latch", 4, 0x01), add("B Auto", 4, 0x02), add("B Sync", 4, 0x04);
    add("A Scratch", 3, 0x01), add("Insta A", 3, 0x02), add("Master FX", 3, 0x04), add("Insta Black", 3, 0x08);
    add("B Scratch", 7, 0x01), add("Insta B", 7, 0x02), add("BPM Tap", 7, 0x04);
    // The software's cell 1..9 is the top-left pad first (cell 1 = PAD 7).
    const int pad_of_cell[9] = {7, 8, 9, 4, 5, 6, 1, 2, 3};
    const int cell_byte[9] = {0, 1, 1, 1, 1, 2, 2, 2, 2}, cell_bit[9] = {3, 0, 1, 2, 3, 0, 1, 2, 3};
    for (int c = 0; c < 9; ++c) {
        add("A Pad " + std::to_string(pad_of_cell[c]), cell_byte[c], 1 << cell_bit[c]);
        add("B Pad " + std::to_string(pad_of_cell[c]), cell_byte[c] + 4, 1 << cell_bit[c]);
    }
    return t;
}

void led_bytes(const LedTable& t, const uint8_t* levels, uint8_t out[8]) {
    std::memset(out, 0, 8);
    out[7] |= 0x08;  // the software always sets this bit (what it lights is not known)
    for (const auto& b : t.bits)
        if (levels[b[0]] >= 32) out[b[1]] = static_cast<uint8_t>(out[b[1]] | b[2]);
}

void render_segments(const Display& d, const Lcd& lcd, uint8_t out[64]) {
    if (d.raw_mode) {
        for (int i = 0; i < 64; ++i) out[i] = d.raw[i] & 0x7F;
        return;
    }
    std::memset(out, 0, 64);
    auto str = [](const char* s, size_t cap) { return std::string(s, strnlen(s, cap)); };
    if (!lcd.preset_learned) {  // what the NuVJ software sent: its 2-digit number + two fixed marks
        out[0x2E] = out[0x36] = 1;
        std::string t = str(d.number[1], 4);
        t = std::string(t.size() < 2 ? 2 - t.size() : 0, ' ') + t;
        t = t.substr(t.size() - 2);
        const uint8_t bytes[2][2] = {{0x3D, 0x3E}, {0x3B, 0x3C}};  // tens, units
        for (int k = 0; k < 2; ++k)
            for (const char* s = digit_segments(t[static_cast<size_t>(k)]); *s; ++s) {
                // a (1,0x01) b (0,0x02) c (0,0x08) d (1,0x10) e (1,0x08) f (1,0x02) g (1,0x04)
                static const uint8_t half[7] = {1, 0, 0, 1, 1, 1, 1}, mask[7] = {0x01, 0x02, 0x08, 0x10, 0x08, 0x02, 0x04};
                const int sg = *s - 'a';
                out[bytes[k][half[sg]]] |= mask[sg];
            }
    }
    for (int k = 0; k < kMarks; ++k)
        if (d.marks & (1u << k)) or_bits(lcd.marks[static_cast<size_t>(k)], out);
    auto digits = [&](const char* s, int n, const auto& segs) {
        std::string t = str(s, 4);
        if (t.empty()) return;
        if (static_cast<int>(t.size()) < n) t = std::string(static_cast<size_t>(n) - t.size(), ' ') + t;
        t = t.substr(t.size() - static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            for (const char* sg = digit_segments(t[static_cast<size_t>(i)]); *sg; ++sg)
                or_bits(segs[static_cast<size_t>(i)][static_cast<size_t>(*sg - 'a')], out);
    };
    digits(d.number[0], 3, lcd.bpm);
    digits(d.number[1], 2, lcd.preset);
    const std::string alpha = str(d.alpha, sizeof(d.alpha));
    for (int i = 0; i < kAlphaCells && i < static_cast<int>(alpha.size()); ++i) {
        const uint16_t bits = seg16(alpha[static_cast<size_t>(i)]);
        for (int s = 0; s < 16; ++s)
            if (bits & (1u << s)) or_bits(lcd.alpha[static_cast<size_t>(i)][static_cast<size_t>(s)], out);
    }
    for (int i = 0; i < std::min<int>(d.bar[0], kRampSteps); ++i) or_bits(lcd.last[static_cast<size_t>(i)], out);
    for (int i = 0; i < std::min<int>(d.bar[1], kRampSteps); ++i) or_bits(lcd.data[static_cast<size_t>(i)], out);
}

// ---------------------------------------------------------------- the driver session
Header make_header(const Profile& p, bool sim) {
    Header h{};
    set_text(h.weapon, "nuvj");
    set_text(h.name, "NuVJ");
    set_text(h.profile, p.path);
    set_text(h.driver, "weapon_nuvj");
    h.caps = kCapButtons | kCapPads | kCapKnobs | kCapFaders | kCapLedLevels | kCapDisplay;
    h.controls = static_cast<uint32_t>(p.count());
    h.pads = kPads;
    h.simulated = sim ? 1 : 0;
    std::memset(h.role, kRoleNone, sizeof(h.role));
    std::memset(h.knob_slot, kNone, sizeof(h.knob_slot));
    std::memset(h.pad_control, kNone, sizeof(h.pad_control));
    auto role = [&](const char* name, Role r, uint8_t slot = kNone) {
        const int i = p.index(name);
        if (i < 0 || i >= kMaxControls) return;
        h.role[i] = r;
        h.knob_slot[i] = slot;
    };
    // The engine skeleton's 8 knob slots (weapons/README.md has the table).
    role("A PMTR1", kRoleKnob, 0), role("A PMTR2", kRoleKnob, 1), role("A Speed", kRoleKnob, 2);
    role("B Speed", kRoleKnob, 3), role("B PMTR1", kRoleKnob, 4), role("B PMTR2", kRoleKnob, 5);
    role("FX Level", kRoleKnob, 6), role("Crossfader", kRoleKnob, 7);
    role("A Bank", kRoleBrowse), role("B Bank", kRoleBrowse), role("Master FX Select", kRoleBrowse);
    role("A Bank Push", kRoleRestart), role("B Bank Push", kRoleRestart);
    role("BPM Tap", kRolePlay), role("Master FX", kRolePageNext), role("Insta Black", kRoleMirror);
    for (int pad = 1; pad <= 9; ++pad) {
        const int a = p.index("A Pad " + std::to_string(pad)), b = p.index("B Pad " + std::to_string(pad));
        if (a >= 0) h.pad_control[pad - 1] = static_cast<uint8_t>(a);
        if (b >= 0) h.pad_control[pad + 8] = static_cast<uint8_t>(b);
    }
    return h;
}

NuvjSession::NuvjSession(const Map& m, const Profile& p, SharedMemory& shm)
    : map_(m), prof_(p), shm_(shm), dec_(m, p), leds_(led_table(p)) {}

bool NuvjSession::send_fb(MidiTransport& io, uint8_t cmd, const uint8_t* data, size_t n) {
    const MidiMsg m = feedback(cmd, data, n);
    if (!m.len) return true;
    if (!io.send(m.b, m.len)) return false;
    shm_.get()->status.msgs_out.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool NuvjSession::start(MidiTransport& io, uint64_t t) {
    have_[0] = have_[1] = have_[2] = false;
    next_ = t;
    next_full_ = t + 2000000;
    led_seen_ = disp_seen_ = 0xFFFFFFFF;
    return send_fb(io, kCmdInit, kInitData, sizeof(kInitData));
}

void NuvjSession::on_message(const MidiMsg& m, uint64_t t) {
    if (m.sysex()) return;  // identity replies etc.: not used
    Event e[2];
    const int n = dec_.decode(m.b, m.len, t, e, 2);
    for (int i = 0; i < n; ++i) shm_.publish(e[i]);
}

bool NuvjSession::tick(MidiTransport& io, uint64_t t) {
    if (t < next_) return true;
    next_ = t + 33000;  // ~30 Hz
    auto& out = shm_.get()->outputs;
    // LEDs: the engine's levels; until it writes any, the LEDs follow the buttons held.
    if (mk2::seq_peek(out.led_seq) != 0) {
        if (mk2::seq_peek(out.led_seq) != led_seen_) {
            uint8_t lv[kMaxControls];
            if (const uint32_t r = mk2::seq_read(out.led_seq, out.led, lv)) {
                led_seen_ = r - 1;
                std::memcpy(levels_, lv, sizeof(lv));
            }
        }
    } else {
        for (int i = 0; i < kMaxControls; ++i) levels_[i] = dec_.held(i) ? 127 : 0;
    }
    if (mk2::seq_peek(out.display_seq) != 0) {
        if (mk2::seq_peek(out.display_seq) != disp_seen_) {
            Display d;
            if (const uint32_t r = mk2::seq_read(out.display_seq, out.display, d)) {
                disp_seen_ = r - 1;
                disp_ = d;
            }
        }
    } else {
        disp_ = Display{};
        set_text(disp_.text, " HIMMEL ");
        set_text(disp_.alpha, "HIMMEL VJ");
        disp_.marks = kMarkRule;
    }
    uint8_t leds[8], text[8], segs[64];
    led_bytes(leds_, levels_, leds);
    text_bytes(std::string_view(disp_.text, strnlen(disp_.text, sizeof(disp_.text))), text);
    render_segments(disp_, map_.lcd, segs);
    if (t >= next_full_) {  // everything again every 2 s (a NuVJ that reset itself catches up)
        next_full_ = t + 2000000;
        have_[0] = have_[1] = have_[2] = false;
    }
    auto& st = shm_.get()->status;
    if (!have_[0] || std::memcmp(leds, sent_leds_, 8) != 0) {
        if (!send_fb(io, kCmdLeds, leds, 8)) return false;
        std::memcpy(sent_leds_, leds, 8), have_[0] = true;
        st.led_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (!have_[1] || std::memcmp(text, sent_text_, 8) != 0) {
        if (!send_fb(io, kCmdText, text, 8)) return false;
        std::memcpy(sent_text_, text, 8), have_[1] = true;
    }
    if (!have_[2] || std::memcmp(segs, sent_segs_, 64) != 0) {
        if (!send_fb(io, kCmdSegments, segs, 64)) return false;
        std::memcpy(sent_segs_, segs, 64), have_[2] = true;
    }
    return true;
}

void NuvjSession::let_go(MidiTransport& io) {
    // LEDs off, the LCD back to the device's own start-up look (as far as we can).
    const uint8_t zero[64] = {};
    uint8_t text[8];
    text_bytes("  NuVJ", text);
    send_fb(io, kCmdLeds, zero, 8);
    send_fb(io, kCmdSegments, zero, 64);
    send_fb(io, kCmdText, text, 8);
}

}  // namespace himmel::weapon::nuvj
