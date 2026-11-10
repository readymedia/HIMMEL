// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the MK2 controller core. No hardware needed.
// Run: build\tests\...\mk2_tests.exe   (or ctest)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "himmel/mk2/input.h"
#include "himmel/mk2/map.h"
#include "himmel/mk2/output.h"
#include "himmel/mk2/pages.h"
#include "himmel/mk2/shared.h"

using namespace himmel::mk2;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

static void test_map_file_matches_default() {
    std::printf("map file vs built-in layout\n");
    std::string err;
    auto m = load_map(std::string(HIMMEL_SOURCE_DIR) + "/maschine_mk2_map.json", &err);
    CHECK(m.has_value());
    if (!m) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    const Map d = default_map();
    CHECK(m->verified);
    for (int i = 0; i < kButtons; ++i) CHECK(m->buttons[i].byte == d.buttons[i].byte && m->buttons[i].bit == d.buttons[i].bit);
    for (int k = 0; k < kKnobs; ++k) CHECK(m->knobs[k].byte == d.knobs[k].byte && m->knobs[k].word);
    CHECK(m->encoder.byte == 7 && !m->encoder.word && m->encoder.low_nibble);
    for (int p = 0; p < kPads; ++p) CHECK(m->pad_hw_id[p] == d.pad_hw_id[p]);
    CHECK(m->pad_hw_id[0] == 12 && m->pad_hw_id[15] == 3);  // flipped rows
    CHECK(m->noise_floor[11] == 136);                       // worn Pad 12
}

static void test_bad_map_is_rejected() {
    std::printf("bad map file\n");
    std::string err;
    CHECK(!load_map("does/not/exist.json", &err).has_value());
    CHECK(!err.empty());
}

static void test_controls() {
    std::printf("controls report: buttons, knobs, encoder\n");
    ControlsParser p(default_map());
    uint8_t r[kControlsPayload] = {};
    r[8] = 995 & 0xFF;  // Knob 1 = 995
    r[9] = 995 >> 8;
    r[7] = 15;          // encoder at 15
    EventBatch b;
    p.parse(r, 1, b);
    CHECK(b.count == 0);  // first report = baseline
    r[4] |= 1 << 4;       // Play down
    b = {};
    p.parse(r, 2, b);
    CHECK(b.count == 1 && b.items[0].type == EventType::ButtonDown && b.items[0].id == static_cast<int>(Button::Play));
    r[4] = 0;
    b = {};
    p.parse(r, 3, b);
    CHECK(b.count == 1 && b.items[0].type == EventType::ButtonUp);
    r[8] = 3;  // Knob 1 wraps 995 -> 3: +8
    r[9] = 0;
    r[7] = 0;  // encoder wraps 15 -> 0: +1
    b = {};
    p.parse(r, 4, b);
    CHECK(b.count == 2);
    CHECK(b.items[0].type == EventType::KnobDelta && b.items[0].id == 0 && b.items[0].value == 8);
    CHECK(b.items[1].type == EventType::EncoderDelta && b.items[1].value == 1);
    r[8] = 250;  // backwards
    r[9] = 0;
    b = {};
    p.parse(r, 5, b);
    CHECK(b.count == 1 && b.items[0].value == 247);
    r[8] = 240;
    b = {};
    p.parse(r, 6, b);
    CHECK(b.count == 1 && b.items[0].value == -10);
}

static void test_pad_report() {
    std::printf("pad report parsing\n");
    uint8_t r[kPadsPayload] = {};
    for (int id = 0; id < 16; ++id) {
        const uint16_t w = static_cast<uint16_t>((id << 12) | (id * 100));
        r[2 * id] = w & 0xFF;
        r[2 * id + 1] = w >> 8;
    }
    std::array<uint16_t, kPads> p{};
    parse_pad_report(r, p);
    for (int id = 0; id < 16; ++id) CHECK(p[id] == id * 100);
}

// Feeds a pressure curve (ms, pressure) for one pad at the real 1.33 ms report rate.
static std::vector<Event> run_curve(PadTracker& t, int pad, const std::vector<std::pair<double, int>>& curve) {
    const Map m = default_map();
    std::vector<Event> out;
    std::array<uint16_t, kPads> pr{};
    pr[m.pad_hw_id[11]] = 136;  // Pad 12 resting noise, always present
    uint64_t base = 1000000;
    for (auto [ms, p] : curve) {
        pr[m.pad_hw_id[pad - 1]] = static_cast<uint16_t>(p);
        EventBatch b;
        t.process(pr, base + static_cast<uint64_t>(ms * 1000), b);
        for (int i = 0; i < b.count; ++i) out.push_back(b.items[i]);
    }
    return out;
}

static std::vector<std::pair<double, int>> ramp(int peak, double rise_ms, double hold_ms) {
    std::vector<std::pair<double, int>> c;
    for (double t = 0; t < rise_ms + hold_ms + 30; t += 1.333) {
        int p = t < rise_ms ? static_cast<int>(70 + (peak - 70) * t / rise_ms) : (t < rise_ms + hold_ms ? peak : 0);
        c.push_back({t, p});
    }
    return c;
}

static void test_pads() {
    std::printf("pad tracker: velocity, aftertouch, release, Pad 12 noise\n");
    PadConfig cfg;
    CHECK(pad_velocity(60, cfg) == 1);
    CHECK(pad_velocity(3300, cfg) == 127);
    CHECK(pad_velocity(2265, cfg) == 93);  // the measured medium hit (spec 7)
    CHECK(pad_velocity(337, cfg) == 18);   // the measured soft hit

    {  // hard hit: full pressure within ~1 ms
        PadTracker t(default_map(), cfg);
        auto ev = run_curve(t, 13, ramp(3300, 1.0, 40));
        int hits = 0, rel = 0, vel = 0;
        for (auto& e : ev) {
            if (e.type == EventType::PadHit) ++hits, vel = e.value;
            if (e.type == EventType::PadRelease) ++rel;
            CHECK(e.id == 13);  // Pad 12's resting 136 never fires
        }
        CHECK(hits == 1 && rel == 1);
        CHECK(vel >= 120);
    }
    {  // soft, slow press: low velocity, but still a hit, then aftertouch
        PadTracker t(default_map(), cfg);
        auto ev = run_curve(t, 5, ramp(2500, 150, 60));
        int hits = 0, vel = 0, presses = 0;
        uint64_t hit_t = 0;
        for (auto& e : ev) {
            if (e.type == EventType::PadHit) ++hits, vel = e.value, hit_t = e.t_us;
            if (e.type == EventType::PadPressure) ++presses;
        }
        CHECK(hits == 1);
        CHECK(vel < 30);
        CHECK(presses > 5);
        CHECK(hit_t > 0);
    }
    {  // ghost touch: rises a little and goes away -> nothing
        PadTracker t(default_map(), cfg);
        std::vector<std::pair<double, int>> c;
        for (double ms = 0; ms < 30; ms += 1.333) c.push_back({ms, ms < 15 ? 100 : 0});
        auto ev = run_curve(t, 2, c);
        CHECK(ev.empty());
    }
    {  // release rebound (seen on the MK2): weak bump 2-9 ms after release -> no second hit
        PadTracker t(default_map(), cfg);
        std::vector<std::pair<double, int>> c;
        for (double ms = 0; ms < 120; ms += 1.333) {
            int p = 0;
            if (ms < 40) p = 1500;                   // the real hit
            else if (ms >= 45 && ms < 52) p = 400;   // rebound after the release at 40 ms
            else if (ms >= 90 && ms < 110) p = 2000; // a real second hit 50 ms later
            c.push_back({ms, p});
        }
        auto ev = run_curve(t, 6, c);
        int hits = 0;
        for (auto& e : ev) hits += e.type == EventType::PadHit;
        CHECK(hits == 2);
    }
    {  // the hit fires ~4 ms after onset, not later
        PadTracker t(default_map(), cfg);
        auto ev = run_curve(t, 1, ramp(2400, 2.0, 50));
        uint64_t hit_t = 0;
        for (auto& e : ev)
            if (e.type == EventType::PadHit) hit_t = e.t_us;
        CHECK(hit_t > 1000000 && hit_t - 1000000 <= 5400);
    }
}

static void test_led_reports() {
    std::printf("LED reports\n");
    const Map m = default_map();
    LedState s{};
    s.pad_rgb[0][0] = 100;   // Pad 1 red -> hardware id 12
    s.pad_rgb[12][2] = 200;  // Pad 13 blue (clamped to 127) -> id 0
    std::array<uint8_t, 49> pr{};
    build_pad_led_report(s, m, pr);
    CHECK(pr[0] == 0x80);
    CHECK(pr[1 + 12 * 3] == 100);
    CHECK(pr[1 + 0 * 3 + 2] == 127);
    s.group_rgb[7][1][1] = 50;  // Group H right green
    s.transport[4] = 127;       // Play
    std::array<uint8_t, 57> gr{};
    build_group_led_report(s, gr);
    CHECK(gr[0] == 0x81 && gr[1 + 7 * 6 + 3 + 1] == 50 && gr[49 + 4] == 127);
    s.buttons[30] = 127;  // Note Repeat
    std::array<uint8_t, 32> br{};
    build_button_led_report(s, br);
    CHECK(br[0] == 0x82 && br[31] == 127);
    CHECK(led_of(Button::NoteRepeat).kind == 1 && led_of(Button::NoteRepeat).index == 30);
    CHECK(led_of(Button::Play).kind == 2 && led_of(Button::Play).index == 4);
    CHECK(led_of(Button::GroupC).kind == 3 && led_of(Button::GroupC).index == 2);
    CHECK(led_of(Button::EncoderPush).kind == 0);
}

static void test_screen() {
    std::printf("screen strips and canvas\n");
    Canvas<512> c;
    c.pixel(0, 0);
    c.pixel(256, 9);
    std::array<uint8_t, kScreenBytes> l{}, r{};
    c.split(l.data(), r.data());
    CHECK(l[0] == 0x80);           // MSB = leftmost pixel
    CHECK(r[9 * 32] == 0x80);      // x 256 -> right screen x 0, row 9
    std::array<uint8_t, kStripReportSize> rep{};
    build_strip_report(1, 1, r.data(), rep);
    const uint8_t head[9] = {0xE1, 0, 0, 8, 0, 0x20, 0, 0x08, 0};
    CHECK(std::memcmp(rep.data(), head, 9) == 0);
    CHECK(rep[9 + 1 * 32] == 0x80);  // row 9 = strip 1, row 1
    CHECK(glyph('A') && glyph('z') && !glyph('\x01'));
    Canvas<256> s;
    const int end = s.text(0, 0, "Hi", true, 2);
    CHECK(end == 24);
}

static void test_ring_and_seqlock() {
    std::printf("event ring and seqlock\n");
    static EventRing ring{};  // static: 64 KB
    ring.attach();
    Event e{};
    CHECK(!ring.pop(e));
    for (uint32_t i = 0; i < kRingSize; ++i) CHECK(ring.push({i, EventType::KnobDelta, 0, 1, 0, 0}) || i == 0xFFFFFFFF);
    CHECK(!ring.push({0, EventType::PadHit, 1, 1, 0, 0}));  // full
    CHECK(ring.dropped.load() == 1);
    CHECK(ring.pop(e) && e.t_us == 0);
    CHECK(ring.push({99, EventType::PadHit, 1, 1, 0, 0}));
    uint32_t n = 1;
    while (ring.pop(e)) ++n;
    CHECK(n == kRingSize + 1 && e.t_us == 99);

    static Outputs out{};
    LedState a{}, b{};
    a.buttons[3] = 77;
    const uint32_t before = seq_read(out.led_seq, out.leds, b);
    seq_write(out.led_seq, out.leds, a);
    const uint32_t after = seq_read(out.led_seq, out.leds, b);
    CHECK(before != 0 && after != 0 && after != before);
    CHECK(b.buttons[3] == 77);
}

static void test_pages() {
    std::printf("screen pages: knob columns, text, lists, dither, meters\n");
    using namespace himmel::mk2::ui;
    // Knob columns tile each screen without gaps and hold their knob centre.
    for (int side = 0; side < 2; ++side) {
        CHECK(kKnobColumn(side * 4).x0 == side * kScreenW);
        CHECK(kKnobColumn(side * 4 + 3).x1 == (side + 1) * kScreenW);
        for (int k = side * 4; k < side * 4 + 3; ++k) CHECK(kKnobColumn(k).x1 == kKnobColumn(k + 1).x0);
    }
    for (int k = 0; k < kKnobs; ++k) CHECK(kKnobColumn(k).x0 <= kKnobX[k] && kKnobX[k] < kKnobColumn(k).x1);
    // The longest labels we use must fit their column (1 px margin, 0 at the edge).
    for (int k = 0; k < kKnobs; ++k) {
        const Column col = kKnobColumn(k);
        const int w = col.width() - (col.x0 % kScreenW ? 1 : 0) - (col.x1 % kScreenW ? 1 : 0);
        CHECK(text_width("RGB SPLIT") <= w || k % 4 == 0);  // knob 1 and 5 columns fit 8 characters
        CHECK(text_width("FEEDBACK") <= w);
    }
    CHECK(text_width("A") == 5 && text_width("AB") == 11 && text_width("AB", 2) == 22);

    Screen c;
    text_in(c, 0, 30, 0, "ABCDEFGHIJ");  // 5 characters = 29 px fit, the 6th not
    bool beyond = false;
    for (int x = 30; x < 60; ++x)
        for (int y = 0; y < 7; ++y) beyond |= c.get(x, y);
    CHECK(!beyond);

    // Header stays on its own screen.
    c.clear();
    Header h;
    h.left.set("A VERY LONG DECK NAME THAT DOES NOT FIT AT ALL");
    h.right.set("P1 CORE FX");
    draw_header(c, 0, h, 1.0f);
    bool spill = false;
    for (int y = 0; y < kScreenH; ++y) spill |= c.get(kScreenW, y) || c.get(kScreenW + 5, y);
    CHECK(!spill);
    CHECK(c.get(0, 0) && c.get(255, 8));

    // Lists keep the selection visible and centred where possible.
    CHECK(list_first_row(4, 3) == 0);
    CHECK(list_first_row(32, 0) == 0);
    CHECK(list_first_row(32, 11) == 8);
    CHECK(list_first_row(32, 31) == 32 - kListRows);
    ListModel l;
    l.count = 32;
    l.selected = 11;
    for (int i = 0; i < 32; ++i) l.items[i].set("ITEM");
    c.clear();
    draw_list(c, 1, l);
    const int sel_y = kHeaderH + 1 + (11 - 8) * kRowH;
    CHECK(c.get(kScreenW + 200, sel_y + 4) && !c.get(kScreenW + 200, sel_y + kRowH + 4));
    CHECK(!c.get(200, sel_y + 4));  // left screen untouched

    // Bayer dither: mid grey lights exactly half the pixels.
    std::array<uint8_t, 16 * 16> grey;
    grey.fill(128);
    c.clear();
    blit_dithered(c, 0, 0, 16, 16, grey.data(), 16);
    int lit = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) lit += c.get(x, y);
    CHECK(lit == 128);

    // Meter: half level fills half the inner width.
    c.clear();
    draw_meter_h(c, 0, 0, 104, 8, 0.5f);
    int filled = 0;
    for (int x = 2; x < 102; ++x) filled += c.get(x, 4);
    CHECK(filled == 50);

    // Touched knob column is inverted; others are not.
    PerformModel pm;
    pm.knobs[1].label.set("ZOOM");
    pm.touched = 1;
    draw_perform(c, pm);
    CHECK(c.get(kKnobColumn(1).x0 + 1, 40));
    CHECK(!c.get(kKnobColumn(2).x0 + 1, 40));

    // Str never overflows.
    Str<5> s("HELLO WORLD");
    CHECK(s.view() == "HELL");
}

int main() {
    test_map_file_matches_default();
    test_bad_map_is_rejected();
    test_controls();
    test_pad_report();
    test_pads();
    test_led_reports();
    test_screen();
    test_ring_and_seqlock();
    test_pages();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
