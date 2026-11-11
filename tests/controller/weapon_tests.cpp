// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the generic weapon drivers. No hardware needed.
// The expected bytes come from the Python reference (tools/lightpad_sim.py,
// tools/lightpad_littlefoot.py, tools/nuvj_sim.py), which was checked on the
// owner's Lightpad M, LUMI Keys and NuVJ.
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "himmel/weapon/apc20.h"
#include "himmel/weapon/blocks.h"
#include "himmel/weapon/blocks_session.h"
#include "himmel/weapon/driver.h"
#include "himmel/weapon/midi.h"
#include "himmel/weapon/nuvj.h"
#include "himmel/weapon/shared.h"

using namespace himmel::weapon;
namespace bl = himmel::weapon::blocks;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += k[d[i] >> 4], s += k[d[i] & 15];
    return s;
}
static std::string hex(const MidiMsg& m) { return hex(m.b, m.len); }
static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return v;
}

static const char* kSourceDir = HIMMEL_SOURCE_DIR;

// ---------------------------------------------------------------- BLOCKS
static void test_bits() {
    std::printf("BLOCKS bit packing\n");
    bl::BitWriter w;
    const uint32_t vals[][2] = {{5, 3}, {0xABC, 12}, {1, 1}, {0xDEADBEEF, 32}, {77, 7}, {0x3FF, 10}};
    for (auto& v : vals) w.put(v[0], static_cast<int>(v[1]));
    bool all7 = true;
    for (size_t i = 0; i < w.nbytes(); ++i) all7 &= w.bytes()[i] < 0x80;
    CHECK(all7);
    bl::BitReader r(w.bytes(), w.nbytes());
    for (auto& v : vals) {
        const uint32_t mask = v[1] == 32 ? 0xFFFFFFFFu : (1u << v[1]) - 1;
        CHECK(r.get(static_cast<int>(v[1])) == (v[0] & mask));
    }
}

static void test_host_messages() {
    std::printf("BLOCKS host messages (bytes as the Python reference)\n");
    CHECK(hex(bl::host_command(bl::kCmdPing)) == "f0002110770001030063f7");
    CHECK(hex(bl::host_config_set(13, 127)) == "f000211077001050611f000000006af7");
    CHECK(hex(bl::host_config_request(13)) == "f000211077001001000000001a005ff7");
    CHECK(hex(bl::serial_request()) == "f0002110783ff7");
    uint8_t idx = 0;
    const uint8_t* body = nullptr;
    size_t blen = 0;
    const MidiMsg p = bl::host_command(bl::kCmdPing);
    CHECK(bl::unframe(p.b, p.len, idx, body, blen) && idx == 0);
    MidiMsg bad = p;
    bad.b[7] ^= 1;  // a flipped bit: the checksum catches it
    CHECK(!bl::unframe(bad.b, bad.len, idx, body, blen));
}

static void test_littlefoot() {
    std::printf("LittleFoot renderer (bytes as the Python reference, checked on the blocks)\n");
    CHECK(bl::function_id("repaint/v") == 28557);
    CHECK(bl::function_id("fillPixel/viii") == -15861);
    CHECK(bl::function_id("makeARGB/iiiii") == 16259);
    const auto prog = bl::build_bitmap_program();
    CHECK(prog.size() == 117);
    CHECK(hex(prog.data(), prog.size()) ==
          "2659750004000000c201648c1a0069b62000a04428008d6f30000b078e7e05000c120c07d2ce05010b120c07d2ce05010a01100d0f"
          "26110d0f280d05130d10220d0b20400d032d0d06140d10220d0520400d022d0d05150d1022400d032d0eff0007833f070bc20c2010"
          "0ee1002436023200080500");
    const auto keys = bl::build_key_program();
    CHECK(hex(keys.data(), keys.size()) ==
          "b18d7900040000003000648c1a0069b62500a0442d008d6f35000b078e7e0b0c07a38105000c120c07d2ce05010b120c07d2ce05010a"
          "01100d1826110d18280d05130d10220d0b20400d032d0d06140d10220d0520400d022d0d05150d1022400d032d0eff0007833f070bc2"
          "0c20100d182436023700080500");
    bl::ProgramInfo info;
    CHECK(bl::parse_program(prog.data(), prog.size(), info) && info.size == 117 && info.heap == 450 && info.functions == 4);
    auto broken = prog;
    broken[50] ^= 1;
    CHECK(!bl::parse_program(broken.data(), broken.size(), info));
}

static void apply_all(const std::vector<bl::DataPacket>& pk, uint8_t* data, size_t n) {
    for (const auto& p : pk) {
        uint8_t idx;
        const uint8_t* body;
        size_t blen;
        if (!bl::unframe(p.msg.b, p.msg.len, idx, body, blen)) continue;
        bl::BitReader r(body, blen);
        CHECK(r.get(7) == bl::kHostData);
        uint16_t pkt = 0;
        bl::apply_data_change(r, data, n, pkt);
        CHECK(pkt == p.index);
    }
}

static void test_data_changes() {
    std::printf("sharedDataChange packets (bytes as the Python reference)\n");
    constexpr int G = bl::kGrid;
    uint8_t rgb[G * G][3];
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x) {
            rgb[y * G + x][0] = static_cast<uint8_t>((x * 17) % 256);
            rgb[y * G + x][1] = static_cast<uint8_t>((y * 17) % 256);
            rgb[y * G + x][2] = static_cast<uint8_t>(((x + y) * 9) % 256);
        }
    std::vector<uint8_t> zero(450, 0), target(450), dev(450, 0);
    bl::pixels_to_bytes(rgb, G * G, target.data());
    std::vector<bl::DataPacket> pk;
    bl::host_data_changes(1, zero.data(), target.data(), 450, pk);
    size_t total = 0;
    for (auto& p : pk) total += p.msg.len;
    CHECK(pk.size() == 4 && pk[0].index == 1 && pk[3].index == 4 && total == 638);
    CHECK(!pk.empty() &&
          hex(pk[0].msg) ==
              "f000211077000201004850400812082203314410480a224921181d443344484e086a0a313d115819223b23383b441760080a0c0212"
              "0c23434144184a0a4349311c1d463464504e0c6b4a413d195a19433b333c3b064802231430460406696014110c53423932182907230d"
              "315613065b62342f0c3746796e18700e230638460231480427697018114e536249321c2a47330d395813275b72382f4e3766016f1c72"
              "4e33065248222a49040043f7");
    apply_all(pk, dev.data(), dev.size());
    CHECK(dev == target);
    auto small = target;
    small[100] ^= 0xFF;
    small[101] ^= 0x0F;
    bl::host_data_changes(9, target.data(), small.data(), 450, pk);
    CHECK(pk.size() == 1 && hex(pk[0].msg) == "f000211077000209000c19566b48001af7");
    apply_all(pk, dev.data(), dev.size());
    CHECK(dev == small);
    uint8_t red[G * G][3];
    for (auto& p : red) p[0] = 255, p[1] = p[2] = 0;
    std::vector<uint8_t> fill(450);
    bl::pixels_to_bytes(red, G * G, fill.data());
    bl::host_data_changes(1022, small.data(), fill.data(), 450, pk);
    // the counter wraps 1023 -> 0 (1024 hangs a real block)
    CHECK(pk.size() == 4 && pk[0].index == 1022 && pk[1].index == 1023 && pk[2].index == 0 && pk[3].index == 1);
    CHECK(pk.size() == 4 && hex(pk[3].msg) ==
                                "f000211077000201006c7f0d640f017c11401f027823003f047047007e08600f017c11401f027823003f04704700"
                                "7e08600f017c11007c11401f027823003f047047007e08600f017c11401f027823003f047047007e08600f017c11"
                                "401f02100ef7");
    apply_all(pk, dev.data(), dev.size());
    CHECK(dev == fill);
    bl::host_data_changes(5, fill.data(), fill.data(), 450, pk);
    CHECK(pk.empty());
}

static void test_heap_sync() {
    std::printf("HeapSync: cumulative ACKs, resends, wrap\n");
    bl::HeapSync s;
    std::vector<uint8_t> t(bl::kDataBlock, 0);
    const auto first = s.start(t.data(), 1000);  // unknown block: every byte is written
    CHECK(!first.empty() && s.busy());
    CHECK(s.on_ack(first.back().index, 3000));  // the last ACK covers every packet before it
    CHECK(!s.busy() && s.frames == 1);
    CHECK(s.start(t.data(), 4000).empty());     // nothing changed: nothing to send
    t[700] = 0x55;
    const auto p = s.start(t.data(), 5000);
    CHECK(p.size() == 1 && s.busy());
    CHECK(s.due(5000 + 100000).empty());        // not late yet
    CHECK(s.due(5000 + 300000).size() == 1 && s.resends == 1);
    CHECK(!s.on_ack(static_cast<uint16_t>(p[0].index + 5), 6000));  // an ACK for something else
    CHECK(s.on_ack(p[0].index, 6000) && s.frames == 2);
}

static void test_decode_device() {
    std::printf("BLOCKS device messages (from the Python simulator)\n");
    bl::DeviceEvent e[8];
    auto dec = [&](const char* h) {
        const auto b = unhex(h);
        return bl::decode_device(b.data(), b.size(), e, 8);
    };
    CHECK(dec("f000211077400800000030020000687f050800401f04f7") == 1);
    CHECK(e[0].kind == bl::DeviceEvent::Touch && e[0].phase == 0 && e[0].index == 0 && e[0].x == 1024 &&
          e[0].y == 3071 && e[0].z == 128 && e[0].vz == 63 && e[0].t == 8);
    CHECK(dec("f0002110774009000000000400000ff7") == 1);
    CHECK(e[0].kind == bl::DeviceEvent::Button && e[0].down && e[0].id == 0 && !e[0].via_program);
    CHECK(dec("f000211077400a00000020000067f7") == 1 && e[0].kind == bl::DeviceEvent::Ack && e[0].id == 0);
    CHECK(dec("f000211077400a00000000431a00020000000000000078070000003bf7") == 1);
    CHECK(e[0].kind == bl::DeviceEvent::Config && e[0].id == 13 && e[0].value == 64 && e[0].min == 0 && e[0].max == 127);
    CHECK(dec("f000211077400a000000101020000026282164646666226669642618185818001b5af7") == 1);
    CHECK(e[0].kind == bl::DeviceEvent::Topology && e[0].battery == 22 && e[0].charging &&
          std::string(e[0].text) == "LPBHIMMELSIM0001");
    CHECK(dec("f00021107834383a42363a32303a30303a30303a30314c504248494d4d454c53494d30303031f7") == 1);
    CHECK(e[0].kind == bl::DeviceEvent::Serial && std::string(e[0].text) == "LPBHIMMELSIM0001");
    // a program event from our renderer: sendMessageToHost(1, 2, 1) = button 2 down
    bl::BitWriter w;
    w.put(1234, 32).put(bl::kDevProgramEvent, 7).put(1, 32).put(2, 32).put(1, 32);
    const MidiMsg m = bl::frame(0x40, w);
    CHECK(bl::decode_device(m.b, m.len, e, 8) == 1 && e[0].kind == bl::DeviceEvent::Button && e[0].via_program &&
          e[0].id == 2 && e[0].down);
}

static void test_sysex_assembler() {
    std::printf("SysEx assembler (WinMM hands SysEx over in pieces)\n");
    SysexAssembler a;
    std::vector<std::string> got;
    auto out = [&](const MidiMsg& m) { got.push_back(hex(m)); };
    const auto x = unhex("f0002110770001030063f7f000013f7f2003");
    const auto y = unhex("0008080000000400000cf7");
    a.feed(x.data(), 7, out);
    a.feed(x.data() + 7, x.size() - 7, out);
    a.feed(y.data(), y.size(), out);
    CHECK(got.size() == 2 && got[0] == "f0002110770001030063f7" && got[1] == "f000013f7f20030008080000000400000cf7");
}

// ---------------------------------------------------------------- NuVJ
static std::optional<Profile> nuvj_profile() {
    return load_profile(std::string(kSourceDir) + "/weapons/nuvj.weapon.json");
}

static void test_nuvj_feedback() {
    std::printf("NuVJ feedback SysEx (bytes as the Python reference)\n");
    auto p = nuvj_profile();
    CHECK(p.has_value());
    if (!p) return;
    const auto t = nuvj::led_table(*p);
    uint8_t lv[kMaxControls] = {};
    lv[p->index("A Pad 7")] = 127;
    lv[p->index("B Sync")] = 127;
    lv[p->index("BPM Tap")] = 100;
    lv[p->index("A Latch")] = 20;  // dim = off on a one-colour LED
    uint8_t leds[8];
    nuvj::led_bytes(t, lv, leds);
    CHECK(hex(nuvj::feedback(nuvj::kCmdLeds, leds, 8)) == "f000013f7f20030008080000000400000cf7");
    CHECK(t.bits.size() == 31);
    uint8_t txt[8];
    nuvj::text_bytes("BRIGHTNESS", txt);
    CHECK(std::memcmp(txt, "BRIGHTN.", 8) == 0);
    nuvj::text_bytes("HI", txt);
    CHECK(std::memcmp(txt, "HI      ", 8) == 0);
    CHECK(nuvj::feedback(nuvj::kCmdContrastStore, txt, 1).len == 0);  // Himmel never stores settings
    const MidiMsg init = nuvj::feedback(nuvj::kCmdInit, nuvj::kInitData, 4);
    CHECK(hex(init) == "f000013f7f2060000401010100f7");
    uint8_t cmd;
    const uint8_t* data;
    size_t len;
    CHECK(nuvj::parse_feedback(init.b, init.len, cmd, data, len) && cmd == 0x60 && len == 4);
}

static void test_nuvj_segments() {
    std::printf("NuVJ LCD segments from the verified map (bytes as the Python reference)\n");
    auto p = nuvj_profile();
    if (!p) return;
    std::string err;
    auto m = nuvj::load_map(std::string(kSourceDir) + "/nuvj_map.json", *p, &err);
    CHECK(m.has_value());
    if (!m) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CHECK(m->lcd.preset_learned);
    Display d{};
    set_text(d.alpha, "HIMMEL VJ 5");
    set_text(d.number[0], "128");
    set_text(d.number[1], "07");
    d.bar[0] = 3;
    d.bar[1] = 5;
    d.marks = nuvj::kMarkRule | nuvj::kMarkBoxA | nuvj::kMarkBpm | nuvj::kMarkEnter;
    uint8_t segs[64];
    nuvj::render_segments(d, m->lcd, segs);
    CHECK(hex(segs, 64) ==
          "0007001f0000010010000a1f021d0a00101b010d000c01001011131011130100191808191000000000000010140200001901010000"
          "000000091b080a010a1b00");
    d.raw_mode = 1;
    d.raw[5] = 0x11;
    nuvj::render_segments(d, m->lcd, segs);
    CHECK(segs[5] == 0x11 && segs[0] == 0);
}

static void test_nuvj_decoder() {
    std::printf("NuVJ input -> Himmel events\n");
    auto p = nuvj_profile();
    if (!p) return;
    auto m = nuvj::load_map(std::string(kSourceDir) + "/nuvj_map.json", *p);
    if (!m) return;
    nuvj::Decoder dec(*m, *p);
    Event e[2];
    auto one = [&](std::initializer_list<uint8_t> b) {
        const std::vector<uint8_t> v(b);
        return dec.decode(v.data(), v.size(), 7, e, 2);
    };
    // A Pad 7 = note 12 -> pad number 7, B Pad 9 = note 17 -> pad number 18
    CHECK(one({0x90, 12, 127}) == 1 && e[0].type == EventType::PadHit && e[0].id == 7 && e[0].value == 127 &&
          e[0].reserved == p->index("A Pad 7"));
    CHECK(one({0x80, 12, 0}) == 1 && e[0].type == EventType::PadRelease && e[0].id == 7);
    CHECK(one({0x90, 17, 127}) == 1 && e[0].type == EventType::PadHit && e[0].id == 18);
    // BPM Tap = note 32
    CHECK(one({0x90, 32, 127}) == 1 && e[0].type == EventType::ButtonDown && e[0].id == p->index("BPM Tap"));
    CHECK(dec.held(p->index("BPM Tap")));
    CHECK(one({0x90, 32, 0}) == 1 && e[0].type == EventType::ButtonUp);
    // A PMTR1: endless (CC 0), 1 = clockwise one tick: +2 of 127 = +16 knob units
    CHECK(one({0xB0, 0, 1}) == 1 && e[0].type == EventType::KnobDelta && e[0].id == p->index("A PMTR1") &&
          e[0].value == 16 && e[0].raw == 66);
    CHECK(one({0xB0, 0, 127}) == 1 && e[0].value == -16 && e[0].raw == 64);
    // A Jog: CC 17, relative, clockwise = negative values on the wire
    CHECK(one({0xB0, 17, 127}) == 1 && e[0].type == EventType::EncoderDelta && e[0].id == p->index("A Jog") &&
          e[0].value == 1);
    // Crossfader: pitch bend on channel 3 (after init) and CC 12 (before init)
    CHECK(one({0xE2, 0x7F, 0x7F}) == 1 && e[0].type == EventType::Fader && e[0].id == p->index("Crossfader") &&
          e[0].value == 16383);
    CHECK(one({0xB0, 12, 0}) == 1 && e[0].type == EventType::Fader && e[0].id == p->index("Crossfader") && e[0].value == 0);
    // Fader Mode: CC 18, positions A/B/C = 1/2/0
    CHECK(one({0xB0, 18, 0}) == 1 && e[0].type == EventType::Switch && e[0].value == 2);
    // Fader Start: note 33, 0 = off, 65 = on
    CHECK(one({0x90, 33, 65}) == 1 && e[0].type == EventType::Switch && e[0].value == 1);
    // Auto Fade Speed: absolute CC 13; the first value is the baseline
    CHECK(one({0xB0, 13, 50}) == 1 && e[0].type == EventType::KnobDelta && e[0].value == 0 && e[0].raw == 50);
    CHECK(one({0xB0, 13, 60}) == 1 && e[0].value == 79 && e[0].raw == 60);
    CHECK(one({0xFE}) == 0);                 // active sensing (the simulator's keep-alive)
    CHECK(one({0x90, 99, 127}) == 0);        // not mapped
}

static void test_nuvj_header() {
    std::printf("NuVJ header: roles, knob slots, pads\n");
    auto p = nuvj_profile();
    if (!p) return;
    const Header h = nuvj::make_header(*p, false);
    CHECK(h.pads == 18 && h.controls == 53);
    CHECK(h.role[p->index("Crossfader")] == kRoleKnob && h.knob_slot[p->index("Crossfader")] == 7);
    CHECK(h.pad_control[0] == p->index("A Pad 1") && h.pad_control[17] == p->index("B Pad 9"));
    int slots = 0;
    for (int i = 0; i < kMaxControls; ++i) slots += h.knob_slot[i] != kNone;
    CHECK(slots == 8);
}

// ---------------------------------------------------------------- APC20 (weapon no. 9)
// Expected bytes from tools/apc20_sim.py (Ableton's APC20 script + Akai's Quickstart Guide).
static std::optional<Profile> apc20_profile() {
    return load_profile(std::string(kSourceDir) + "/weapons/apc20.weapon.json");
}

static void test_apc20_profile_and_map() {
    std::printf("APC20 profile, built-in list and wire table\n");
    auto p = apc20_profile();
    CHECK(p.has_value());
    if (!p) return;
    const Profile b = apc20::builtin_profile();
    CHECK(p->count() == apc20::kControls && b.count() == apc20::kControls);
    CHECK(p->names == b.names && p->kinds == b.kinds);
    const apc20::Map m = apc20::default_map(*p);
    // every control has a wire address, and no two share one
    bool all = true, unique = true;
    std::vector<int> used(2 * 16 * 128, 0);
    for (int i = 0; i < m.count; ++i) {
        const auto& e = m.controls[static_cast<size_t>(i)];
        all &= e.kind != apc20::Kind::None;
        unique &= ++used[static_cast<size_t>((e.cc ? 2048 : 0) + e.ch * 128 + e.num)] == 1;
    }
    CHECK(all && unique);
    auto at = [&](const char* n) { return m.controls[static_cast<size_t>(p->index(n))]; };
    CHECK(!at("Clip 3.2").cc && at("Clip 3.2").ch == 2 && at("Clip 3.2").num == 54 && at("Clip 3.2").led == apc20::Palette::Tri);
    CHECK(at("Scene 5").num == 86 && at("Shift").num == 81 && at("Shift").led == apc20::Palette::None);
    CHECK(at("Select 1").ch == 0 && at("Select 1").num == 51 && at("Note Mode").num == 80);
    CHECK(at("Activator 2").ch == 1 && at("Activator 2").num == 50 && at("Solo 2").num == 49 && at("Arm 2").num == 48);
    CHECK(at("Fader 8").cc && at("Fader 8").ch == 7 && at("Fader 8").num == 7 && at("Master").num == 14);
    CHECK(at("Cue Level").kind == apc20::Kind::Encoder && at("Cue Level").num == 47);
    CHECK(at("Clip 1.5").pad == 1 && at("Clip 8.1").pad == 40 && at("Clip 1.1").pad == 33);
    CHECK(apc20::note_mode_note(0, 4) == 36 && apc20::note_mode_note(3, 0) == 55 && apc20::note_mode_note(4, 4) == 56 &&
          apc20::note_mode_note(7, 0) == 75);
}

static void test_apc20_messages() {
    std::printf("APC20 identity and mode SysEx\n");
    CHECK(hex(apc20::mode_sysex(apc20::kModeAbleton, 0)) == "f047007b60000441010000f7");
    CHECK(hex(apc20::mode_sysex(apc20::kModeNote, 3)) == "f047037b60000443010000f7");
    // identity reply as the simulator sends it (version 1.9 in bytes 9..12, device id 3 at byte 13)
    const auto r = unhex("f07e030602477b001900010009030000000000000000000000000000000000000000f7");
    uint8_t dev = 0;
    bool is = false;
    CHECK(apc20::parse_identity(r.data(), r.size(), dev, is) && is && dev == 3);
    CHECK(!apc20::parse_identity(apc20::kIdentityRequest, sizeof(apc20::kIdentityRequest), dev, is));
}

static void test_apc20_decoder() {
    std::printf("APC20 input -> Himmel events\n");
    auto p = apc20_profile();
    if (!p) return;
    const apc20::Map m = apc20::default_map(*p);
    apc20::Decoder dec(m);
    Event e[1];
    auto one = [&](std::initializer_list<uint8_t> b) {
        const std::vector<uint8_t> v(b);
        return dec.decode(v.data(), v.size(), 7, e, 1);
    };
    // Clip 1.5 (bottom-left) = ch1 note 57 -> pad 1; release as note off with velocity 127
    CHECK(one({0x90, 57, 127}) == 1 && e[0].type == EventType::PadHit && e[0].id == 1 && e[0].value == 127 &&
          e[0].reserved == p->index("Clip 1.5"));
    CHECK(one({0x80, 57, 127}) == 1 && e[0].type == EventType::PadRelease && e[0].id == 1);
    // Clip 8.1 (top-right) = ch8 note 53 -> pad 40; release as note on velocity 0
    CHECK(one({0x97, 53, 127}) == 1 && e[0].type == EventType::PadHit && e[0].id == 40);
    CHECK(one({0x97, 53, 0}) == 1 && e[0].type == EventType::PadRelease);
    // Shift, Scene 1, Arm 4
    CHECK(one({0x90, 81, 127}) == 1 && e[0].type == EventType::ButtonDown && e[0].id == p->index("Shift"));
    CHECK(dec.held(p->index("Shift")));
    CHECK(one({0x90, 81, 127}) == 0);  // a repeat
    CHECK(one({0x80, 81, 127}) == 1 && e[0].type == EventType::ButtonUp);
    CHECK(one({0x90, 82, 127}) == 1 && e[0].id == p->index("Scene 1"));
    CHECK(one({0x93, 48, 127}) == 1 && e[0].id == p->index("Arm 4"));
    // Fader 3 = ch3 CC 7; master = CC 14
    CHECK(one({0xB2, 7, 127}) == 1 && e[0].type == EventType::Fader && e[0].id == p->index("Fader 3") &&
          e[0].value == 16383 && e[0].raw == 127);
    CHECK(one({0xB0, 14, 0}) == 1 && e[0].type == EventType::Fader && e[0].id == p->index("Master") && e[0].value == 0);
    // Cue Level: CC 47 relative two's complement
    CHECK(one({0xB0, 47, 1}) == 1 && e[0].type == EventType::EncoderDelta && e[0].value == 1);
    CHECK(one({0xB0, 47, 125}) == 1 && e[0].value == -3);
    // note mode: channel 10 note 36 = the bottom-left grid button
    CHECK(one({0x99, 36, 127}) == 1 && e[0].type == EventType::PadHit && e[0].id == 1);
    CHECK(one({0xFE}) == 0);
    CHECK(one({0x90, 99, 127}) == 0);
}

static void test_apc20_leds_and_header() {
    std::printf("APC20 LED levels, header\n");
    using apc20::Palette;
    CHECK(apc20::led_velocity(Palette::Tri, 0) == 0 && apc20::led_velocity(Palette::Tri, 5) == 0);
    CHECK(apc20::led_velocity(Palette::Tri, 20) == apc20::kAmber && apc20::led_velocity(Palette::Tri, 127) == apc20::kGreen);
    CHECK(apc20::led_velocity(Palette::Tri, apc20::kLedCode + apc20::kRedBlink) == apc20::kRedBlink);
    CHECK(apc20::led_velocity(Palette::Mono, 20) == 0 && apc20::led_velocity(Palette::Mono, 127) == 1);
    CHECK(apc20::led_velocity(Palette::Mono, apc20::kLedCode + 2) == 2);
    CHECK(apc20::led_velocity(Palette::None, 127) == 0);
    auto p = apc20_profile();
    if (!p) return;
    const Header h = apc20::make_header(*p, true);
    CHECK(h.pads == 40 && h.controls == 97 && h.simulated == 1);
    CHECK(h.role[p->index("Fader 1")] == kRoleKnob && h.knob_slot[p->index("Fader 1")] == 0 &&
          h.knob_slot[p->index("Fader 8")] == 7);
    CHECK(h.role[p->index("Cue Level")] == kRoleBrowse && h.role[p->index("Select 1")] == kRolePlay);
    CHECK(h.pad_control[0] == p->index("Clip 1.5") && h.pad_control[39] == p->index("Clip 8.1"));
}

// ---------------------------------------------------------------- shared block
static void test_shared_block() {
    std::printf("shared block Local\\HimmelWeapon<N>\n");
    static_assert(sizeof(Event) == 16);
    CHECK(shm_name(5) == L"Local\\HimmelWeapon5");
    // The MK2's event values never change (the MK2 path must stay as it is).
    static_assert(static_cast<int>(EventType::Disconnected) == 8 && static_cast<int>(EventType::TouchStart) == 9 &&
                  static_cast<int>(EventType::Switch) == 13);
    auto lp = load_profile(std::string(kSourceDir) + "/weapons/lightpad.weapon.json");
    auto lu = load_profile(std::string(kSourceDir) + "/weapons/lumi.weapon.json");
    CHECK(lp && lu);
    if (!lp || !lu) return;
    const Header hl = bl::make_header(bl::lumi_setup(), *lu, true);
    CHECK(hl.pads == 24 && hl.pixel_w == 24 && hl.pixel_h == 1 && hl.simulated == 1);
    CHECK(hl.pad_control[0] == lu->index("Key 1") && hl.pad_control[23] == lu->index("Key 24"));
    CHECK(hl.role[lu->index("Octave +")] == kRoleBrowseNext);

    constexpr int kSlot = 15;  // a slot no driver uses by default
    SharedMemory drv, app;
    CHECK(!app.open(kSlot));
    CHECK(drv.create(kSlot, bl::make_header(bl::lightpad_setup(), *lp, true)));
    CHECK(app.open(kSlot));
    CHECK(std::string(app.get()->header.weapon) == "lightpad" && app.get()->header.slot == kSlot);
    CHECK(app.get()->status.battery.load() == -1);
    // Events: only attached rings get them.
    drv.publish({1, EventType::TouchStart, 3, 100, 200, 50});
    Event e;
    CHECK(!app.get()->rings[0].pop(e));
    app.get()->rings[0].attach();
    drv.publish({2, EventType::TouchMove, 3, 101, 201, 60});
    CHECK(app.get()->rings[0].pop(e) && e.type == EventType::TouchMove && e.value == 101 && e.raw == 201);
    // Outputs: the engine writes, the driver reads a consistent copy.
    uint8_t px[kMaxPixels][3] = {};
    px[0][0] = 255;
    px[224][2] = 9;
    himmel::mk2::seq_write(app.get()->outputs.pixel_seq, app.get()->outputs.pixel, px);
    uint8_t back[kMaxPixels][3] = {};
    CHECK(himmel::mk2::seq_read(drv.get()->outputs.pixel_seq, drv.get()->outputs.pixel, back) != 0);
    CHECK(back[0][0] == 255 && back[224][2] == 9);
    // A restarted driver keeps the engine's outputs and attached rings.
    SharedMemory drv2;
    CHECK(drv2.create(kSlot, bl::make_header(bl::lightpad_setup(), *lp, false)));
    CHECK(drv2.get()->outputs.pixel[0][0] == 255 && drv2.get()->rings[0].attached.load() == 1);
    CHECK(drv2.get()->header.simulated == 0);
}

int main() {
    test_bits();
    test_host_messages();
    test_littlefoot();
    test_data_changes();
    test_heap_sync();
    test_decode_device();
    test_sysex_assembler();
    test_nuvj_feedback();
    test_nuvj_segments();
    test_nuvj_decoder();
    test_nuvj_header();
    test_apc20_profile_and_map();
    test_apc20_messages();
    test_apc20_decoder();
    test_apc20_leds_and_header();
    test_shared_block();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
