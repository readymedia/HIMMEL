// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// APC20 wire table, decoder, LED levels and driver session (see apc20.h). Port of
// tools/apc20_sim.py; weapon_tests.cpp checks it against weapons/apc20.weapon.json.
#include "himmel/weapon/apc20.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

namespace himmel::weapon::apc20 {

MidiMsg mode_sysex(uint8_t mode, uint8_t dev) {
    // The last three bytes are the host's version (Ableton sends its own); 1.0.0 for Himmel.
    const uint8_t b[12] = {0xF0, kManufacturer, static_cast<uint8_t>(dev & 0x7F), kModel, 0x60, 0x00, 0x04,
                           static_cast<uint8_t>(mode & 0x7F), 0x01, 0x00, 0x00, 0xF7};
    MidiMsg m;
    std::memcpy(m.b, b, sizeof(b));
    m.len = sizeof(b);
    return m;
}

bool parse_identity(const uint8_t* msg, size_t n, uint8_t& dev, bool& is_apc20) {
    if (n < 15 || msg[0] != 0xF0 || msg[1] != 0x7E || msg[3] != 0x06 || msg[4] != 0x02) return false;
    is_apc20 = msg[5] == kManufacturer && msg[6] == kModel;
    dev = msg[13] & 0x7F;  // Ableton's script reads the device id here
    return true;
}

int note_mode_note(int track, int scene) {
    const int half = track / 4, col = track % 4, row_up = (kRows - 1) - scene;
    return 36 + half * 20 + row_up * 4 + col;
}

int pad_number(int track, int scene) { return ((kRows - 1) - scene) * kCols + track + 1; }

Profile builtin_profile() {
    Profile p;
    p.id = "apc20";
    p.name = "APC20";
    p.path = "(built in)";
    auto add = [&](const std::string& n, const char* kind) {
        p.names.push_back(n);
        p.kinds.push_back(kind);
    };
    for (int r = 1; r <= kRows; ++r) {
        for (int t = 1; t <= kCols; ++t) add("Clip " + std::to_string(t) + "." + std::to_string(r), "button");
        add("Scene " + std::to_string(r), "button");
    }
    for (int t = 1; t <= kCols; ++t) add("Stop " + std::to_string(t), "button");
    add("Shift", "button");
    for (int t = 1; t <= kCols; ++t) add("Select " + std::to_string(t), "button");
    add("Note Mode", "button");
    for (const char* row : {"Activator", "Solo", "Arm"})
        for (int t = 1; t <= kCols; ++t) add(std::string(row) + " " + std::to_string(t), "button");
    add("Cue Level", "encoder");
    for (int t = 1; t <= kCols; ++t) add("Fader " + std::to_string(t), "fader");
    add("Master", "fader");
    return p;
}

// ---------------------------------------------------------------- the map
namespace {
// "Clip 3.2" -> 3, 2; "Fader 8" -> 8. False if the name is not prefix + number (+ "." + number).
bool parse_num(const std::string& name, const std::string& prefix, int& a, int* b = nullptr) {
    if (name.rfind(prefix, 0) != 0) return false;
    const char* p = name.data() + prefix.size();
    const char* end = name.data() + name.size();
    auto r = std::from_chars(p, end, a);
    if (r.ec != std::errc() || r.ptr == p) return false;
    if (!b) return r.ptr == end;
    if (r.ptr == end || *r.ptr != '.') return false;
    auto r2 = std::from_chars(r.ptr + 1, end, *b);
    return r2.ec == std::errc() && r2.ptr == end;
}
}  // namespace

Map default_map(const Profile& p) {
    Map m;
    m.count = std::min(p.count(), kMaxControls);
    for (int i = 0; i < m.count; ++i) {
        const std::string& n = p.names[static_cast<size_t>(i)];
        Entry e;
        int a = 0, b = 0;
        auto note = [&](int ch, int num, Palette led) {
            e.kind = Kind::Button, e.cc = false, e.ch = static_cast<uint8_t>(ch), e.num = static_cast<uint8_t>(num), e.led = led;
        };
        auto cc = [&](Kind k, int ch, int num) {
            e.kind = k, e.cc = true, e.ch = static_cast<uint8_t>(ch), e.num = static_cast<uint8_t>(num);
        };
        auto track = [](int t) { return t >= 1 && t <= kCols; };
        if (parse_num(n, "Clip ", a, &b) && track(a) && b >= 1 && b <= kRows) {
            note(a - 1, 52 + b, Palette::Tri);
            e.pad = static_cast<uint8_t>(pad_number(a - 1, b - 1));
        } else if (parse_num(n, "Scene ", a) && a >= 1 && a <= kRows) note(0, 81 + a, Palette::Mono);
        else if (parse_num(n, "Stop ", a) && track(a)) note(a - 1, 52, Palette::Mono);
        else if (n == "Shift") note(0, 81, Palette::None);
        else if (parse_num(n, "Select ", a) && track(a)) note(a - 1, 51, Palette::Mono);
        else if (n == "Note Mode") note(0, 80, Palette::Mono);
        else if (parse_num(n, "Activator ", a) && track(a)) note(a - 1, 50, Palette::Mono);
        else if (parse_num(n, "Solo ", a) && track(a)) note(a - 1, 49, Palette::Mono);
        else if (parse_num(n, "Arm ", a) && track(a)) note(a - 1, 48, Palette::Mono);
        else if (n == "Cue Level") cc(Kind::Encoder, 0, 47);
        else if (parse_num(n, "Fader ", a) && track(a)) cc(Kind::Fader, a - 1, 7);
        else if (n == "Master") cc(Kind::Fader, 0, 14);
        m.controls[static_cast<size_t>(i)] = e;
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
        if (j.value("format", "") != "himmel-apc20-map/1") {
            if (err) *err = path.string() + " is not an APC20 map";
            return std::nullopt;
        }
        Map m = default_map(p);
        m.from_file = true;
        if (j.contains("controls") && j["controls"].is_object()) {
            for (int i = 0; i < m.count; ++i) {
                auto it = j["controls"].find(p.names[static_cast<size_t>(i)]);
                if (it == j["controls"].end() || !it->is_object()) continue;
                Entry& e = m.controls[static_cast<size_t>(i)];
                e.cc = it->value("type", e.cc ? "cc" : "note") == "cc";
                e.ch = static_cast<uint8_t>(it->value("ch", static_cast<int>(e.ch)) & 15);
                e.num = static_cast<uint8_t>(it->value("num", static_cast<int>(e.num)) & 127);
            }
        }
        std::set<std::string> seen;
        if (j.contains("seen") && j["seen"].is_array())
            for (const auto& s : j["seen"])
                if (s.is_string()) seen.insert(s.get<std::string>());
        m.verified = m.count > 0;
        for (int i = 0; i < m.count; ++i) m.verified = m.verified && seen.count(p.names[static_cast<size_t>(i)]) > 0;
        return m;
    } catch (const std::exception& e) {
        if (err) *err = path.string() + ": " + e.what();
        return std::nullopt;
    }
}

// ---------------------------------------------------------------- input
Decoder::Decoder(const Map& m) : m_(m) {
    for (auto& ch : note_) ch.fill(-1);
    for (auto& ch : cc_) ch.fill(-1);
    note_mode_.fill(-1);
    for (int i = 0; i < m.count; ++i) {
        const Entry& e = m.controls[static_cast<size_t>(i)];
        if (e.kind == Kind::None) continue;
        (e.cc ? cc_ : note_)[e.ch & 15][e.num & 127] = static_cast<int16_t>(i);
        if (e.pad) {  // the grid in note mode (if the host ever puts it there)
            const int t = (e.pad - 1) % kCols, s = (kRows - 1) - (e.pad - 1) / kCols;
            note_mode_[static_cast<size_t>(note_mode_note(t, s))] = static_cast<int16_t>(i);
        }
    }
}

int Decoder::decode(const uint8_t* msg, size_t n, uint64_t t, Event* out, int cap) {
    if (n < 3 || cap < 1 || msg[0] >= 0xF0) return 0;
    const uint8_t hi = msg[0] & 0xF0, ch = msg[0] & 15;
    auto ev = [&](EventType type, int id, int value, int raw = 0, int reserved = 0) {
        out[0] = {t, type, static_cast<uint8_t>(id), static_cast<int16_t>(value), static_cast<uint16_t>(raw),
                  static_cast<uint16_t>(reserved)};
        return 1;
    };
    if (hi == 0x80 || hi == 0x90) {
        int idx = note_[ch][msg[1] & 127];
        if (idx < 0 && ch == kNoteModeCh) idx = note_mode_[msg[1] & 127];
        if (idx < 0) return 0;
        // A release may be note off (any velocity) or note on with velocity 0.
        const bool down = hi == 0x90 && msg[2] > 0;
        bool& h = held_[static_cast<size_t>(idx)];
        if (h == down) return 0;  // a repeat
        h = down;
        if (const int pad = m_.controls[static_cast<size_t>(idx)].pad)
            return down ? ev(EventType::PadHit, pad, msg[2], 0, idx) : ev(EventType::PadRelease, pad, 0, 0, idx);
        return ev(down ? EventType::ButtonDown : EventType::ButtonUp, idx, 0);
    }
    if (hi == 0xB0) {
        const int idx = cc_[ch][msg[1] & 127];
        if (idx < 0) return 0;
        const int val = msg[2] & 127;
        if (m_.controls[static_cast<size_t>(idx)].kind == Kind::Encoder) {
            const int v = val >= 64 ? val - 128 : val;  // relative two's complement
            return v ? ev(EventType::EncoderDelta, idx, v, val) : 0;
        }
        return ev(EventType::Fader, idx, val * 16383 / 127, val);
    }
    return 0;
}

// ---------------------------------------------------------------- output
uint8_t led_velocity(Palette p, uint8_t level) {
    if (p == Palette::None) return 0;
    if (level >= kLedCode + 1 && level <= kLedCode + kAmberBlink)  // an exact APC code
        return p == Palette::Tri ? static_cast<uint8_t>(level - kLedCode) : (level - kLedCode) % 2 == 0 ? 2 : 1;
    if (p == Palette::Tri) return level < 8 ? kOff : level < 64 ? kAmber : kGreen;
    return level >= 32 ? 1 : 0;
}

// ---------------------------------------------------------------- the driver session
Header make_header(const Profile& p, bool sim) {
    Header h{};
    set_text(h.weapon, "apc20");
    set_text(h.name, "APC20");
    set_text(h.profile, p.path);
    set_text(h.driver, "weapon_apc20");
    h.caps = kCapButtons | kCapPads | kCapKnobs | kCapFaders | kCapLedLevels;
    h.controls = static_cast<uint32_t>(std::min(p.count(), kMaxControls));
    h.pads = kPads;
    h.simulated = sim ? 1 : 0;
    std::memset(h.role, kRoleNone, sizeof(h.role));
    std::memset(h.knob_slot, kNone, sizeof(h.knob_slot));
    std::memset(h.pad_control, kNone, sizeof(h.pad_control));
    auto role = [&](const std::string& name, Role r, uint8_t slot = kNone) {
        const int i = p.index(name);
        if (i < 0 || i >= kMaxControls) return;
        h.role[i] = r;
        h.knob_slot[i] = slot;
    };
    // The engine skeleton (until Himmel has real mapping pages): the 8 track faders are the
    // 8 knob slots, the transport row plays / restarts / browses / pages, Note Mode = mirror.
    for (int k = 0; k < kCols; ++k) role("Fader " + std::to_string(k + 1), kRoleKnob, static_cast<uint8_t>(k));
    role("Cue Level", kRoleBrowse);
    role("Select 1", kRolePlay), role("Select 2", kRoleRestart);
    role("Select 5", kRoleBrowsePrev), role("Select 6", kRoleBrowseNext);
    role("Select 7", kRolePagePrev), role("Select 8", kRolePageNext);
    role("Note Mode", kRoleMirror);
    for (int t = 0; t < kCols; ++t)
        for (int s = 0; s < kRows; ++s) {
            const int i = p.index("Clip " + std::to_string(t + 1) + "." + std::to_string(s + 1));
            if (i >= 0 && i < kMaxControls) h.pad_control[pad_number(t, s) - 1] = static_cast<uint8_t>(i);
        }
    return h;
}

Apc20Session::Apc20Session(const Map& m, SharedMemory& shm) : map_(m), shm_(shm), dec_(m) {}

bool Apc20Session::send(MidiTransport& io, const uint8_t* b, size_t n) {
    if (!io.send(b, n)) return false;
    shm_.get()->status.msgs_out.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Apc20Session::start(MidiTransport& io, uint64_t t) {
    state_ = State::Identity;
    got_identity_ = false;
    dev_ = 0;
    identity_deadline_ = t + 300000;  // no answer in 0.3 s: device id 0
    next_ = next_hello_ = t;
    next_full_ = t + 5000000;
    led_seen_ = 0xFFFFFFFF;
    sent_.fill(-1);
    return send(io, kIdentityRequest, sizeof(kIdentityRequest));
}

void Apc20Session::on_message(const MidiMsg& m, uint64_t t) {
    if (m.sysex()) {
        uint8_t dev = 0;
        bool is_apc20 = false;
        if (parse_identity(m.b, m.len, dev, is_apc20) && !got_identity_) {
            got_identity_ = true;
            dev_ = dev;
            shm_.set_message(is_apc20 ? "APC20 identity: device id " + std::to_string(dev)
                                      : "identity reply from another model (expected APC20)");
        }
        return;
    }
    Event e[1];
    if (dec_.decode(m.b, m.len, t, e, 1)) shm_.publish(e[0]);
}

bool Apc20Session::tick(MidiTransport& io, uint64_t t) {
    if (io.simulated() && t >= next_hello_) {  // the simulator's page knows the engine is there (never sent to hardware)
        next_hello_ = t + 250000;
        const uint8_t hello = 0xFE;
        if (!send(io, &hello, 1)) return false;
    }
    if (state_ == State::Identity) {
        if (!got_identity_ && t < identity_deadline_) return true;
        // Ableton mode: the host owns the LEDs. Sent after every (re)connect.
        const MidiMsg mm = mode_sysex(kModeAbleton, dev_);
        if (!send(io, mm.b, mm.len)) return false;
        state_ = State::Run;
        sent_.fill(-1);
        next_ = t;
    }
    if (t < next_) return true;
    next_ = t + 33000;  // ~30 Hz
    auto& out = shm_.get()->outputs;
    if (mk2::seq_peek(out.led_seq) != 0) {
        if (mk2::seq_peek(out.led_seq) != led_seen_) {
            uint8_t lv[kMaxControls];
            if (const uint32_t r = mk2::seq_read(out.led_seq, out.led, lv)) {
                led_seen_ = r - 1;
                std::memcpy(levels_.data(), lv, sizeof(lv));
            }
        }
    } else {  // until the engine writes LEDs, a button lights while held
        for (int i = 0; i < kMaxControls; ++i) levels_[static_cast<size_t>(i)] = dec_.held(i) ? 127 : 0;
    }
    if (t >= next_full_) {  // everything again every 5 s (an APC that was power-cycled catches up)
        next_full_ = t + 5000000;
        sent_.fill(-1);
    }
    bool any = false;
    for (int i = 0; i < map_.count; ++i) {
        const Entry& e = map_.controls[static_cast<size_t>(i)];
        if (e.led == Palette::None || e.cc) continue;
        const uint8_t v = led_velocity(e.led, levels_[static_cast<size_t>(i)]);
        if (sent_[static_cast<size_t>(i)] == v) continue;
        const uint8_t b[3] = {static_cast<uint8_t>(0x90 | (e.ch & 15)), static_cast<uint8_t>(e.num & 127), v};
        if (!send(io, b, 3)) return false;
        sent_[static_cast<size_t>(i)] = v;
        any = true;
    }
    if (any) shm_.get()->status.led_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Apc20Session::let_go(MidiTransport& io) {
    // LEDs off, then back to generic mode (the power-on state).
    for (int i = 0; i < map_.count; ++i) {
        const Entry& e = map_.controls[static_cast<size_t>(i)];
        if (e.led == Palette::None || e.cc) continue;
        const uint8_t b[3] = {static_cast<uint8_t>(0x90 | (e.ch & 15)), static_cast<uint8_t>(e.num & 127), 0};
        send(io, b, 3);
    }
    const MidiMsg mm = mode_sysex(kModeGeneric, dev_);
    send(io, mm.b, mm.len);
}

}  // namespace himmel::weapon::apc20
