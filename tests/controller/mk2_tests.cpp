// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the MK2 controller core. No hardware needed.
// Run: build\tests\...\mk2_tests.exe   (or ctest)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "himmel/mk2/controller.h"
#include "himmel/mk2/input.h"
#include "himmel/mk2/map.h"
#include "himmel/mk2/output.h"
#include "himmel/mk2/pages.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/sim_link.h"

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

// Which simulator the controller listens to (sim_link.h, SimPeerPolicy).
static void test_sim_peer_policy() {
    std::printf("sim peer policy\n");
    using V = SimPeerPolicy::Verdict;
    {  // one simulator: one session, every datagram accepted
        SimPeerPolicy p;
        uint64_t t = 1'000'000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);
        bool all = true;
        for (int i = 0; i < 1000; ++i) all = all && p.on_datagram(100, t += 4000) == V::Accept;
        CHECK(all);
        CHECK(p.on_datagram(0, t) == V::Drop);  // no sender address
    }
    {  // an old simulator (100) keeps streaming, a new one (200) starts: the new one wins once
        SimPeerPolicy p;
        uint64_t t = 1'000'000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);
        CHECK(p.on_datagram(200, t += 2000) == V::NewPeer);
        int new_peers = 0, accepted = 0, dropped = 0;
        for (int i = 0; i < 2000; ++i) {  // interleaved: no flapping
            t += 2000;
            const V v = p.on_datagram(i % 2 ? 100 : 200, t);
            new_peers += v == V::NewPeer;
            accepted += v == V::Accept;
            dropped += v == V::Drop;
        }
        CHECK(new_peers == 0);
        CHECK(accepted == 1000 && dropped == 1000);
        CHECK(p.peer() == 200 && p.ignored(100));
        // A third simulator starts: it wins too; both older ones are ignored.
        CHECK(p.on_datagram(300, t += 2000) == V::NewPeer);
        CHECK(p.on_datagram(200, t += 2000) == V::Drop);
        CHECK(p.on_datagram(100, t += 2000) == V::Drop);
        CHECK(p.on_datagram(300, t += 2000) == V::Accept);
        // The peer is closed: after the idle time the ignored one takes over (once).
        t += kSimPeerIdleUs + 1000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);
        CHECK(p.on_datagram(100, t += 4000) == V::Accept);
        CHECK(p.on_datagram(200, t += 4000) == V::Drop);  // still ignored while 100 is alive
        CHECK(p.peer() == 100 && !p.ignored(100));
    }
    {  // a restarted simulator (new port) while the old one is gone: one new session
        SimPeerPolicy p;
        uint64_t t = 1'000'000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);
        t += 2'000'000;
        CHECK(p.on_datagram(101, t) == V::NewPeer);
        CHECK(!p.ignored(100));  // it was not alive, so it is not held against it
        CHECK(p.on_datagram(101, t += 4000) == V::Accept);
    }
    {  // 2026-11-16: an ignored simulator is told so (0xFD), rate-limited, first notice flagged once
        using N = SimPeerPolicy::Notice;
        SimPeerPolicy p;
        uint64_t t = 1'000'000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);  // the owner's live view
        CHECK(p.on_datagram(200, t += 1000) == V::NewPeer);  // a forgotten simulator starts later and wins
        CHECK(p.on_dropped(200, t) == N::None);  // the peer is never told
        int first = 0, sent = 0;
        for (int i = 0; i < 500; ++i) {  // 1 s of the live view streaming every 2 ms
            t += 2000;
            p.on_datagram(200, t);
            if (p.on_datagram(100, t + 1) != V::Drop) continue;
            const N n = p.on_dropped(100, t + 1);
            first += n == N::SendFirst;
            sent += n != N::None;
        }
        CHECK(first == 1);
        CHECK(sent >= 4 && sent <= 5);  // every kSimBusyNoticeUs (250 ms)
        // The other one stops: the live view gets the link back and is no longer told.
        t += kSimPeerIdleUs + 1000;
        CHECK(p.on_datagram(100, t) == V::NewPeer);
        CHECK(p.on_dropped(100, t) == N::None);
        // Ignored again later: a fresh first notice.
        CHECK(p.on_datagram(300, t += 1000) == V::NewPeer);
        CHECK(p.on_datagram(100, t += 1000) == V::Drop);
        CHECK(p.on_dropped(100, t) == N::SendFirst);
    }
}

// The supervisor's stop path (2026-11-17): status.stop_request makes a running
// Controller let go cleanly: every LED off, the "stopped" page on both screens,
// run() returns fast. Driven end to end through a simulated MK2 on loopback UDP
// (a test port, never the live 8766) and a private SharedBlock (not Local\HimmelMk2).
static void test_stop_request() {
    std::printf("stop_request: LEDs off, stopped page, quick exit\n");
    constexpr uint16_t kTestPort = 28766;
    using clk = std::chrono::steady_clock;
    auto ms_since = [](clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); };
    static SharedBlock blk{};
    LedState lit{};
    lit.buttons[0] = 127;
    lit.pad_rgb[0][1] = 100;
    lit.group_rgb[0][0][2] = 90;
    seq_write(blk.outputs.led_seq, blk.outputs.leds, lit);
    static uint8_t scr[kScreenBytes];
    std::memset(scr, 0xAA, sizeof(scr));
    seq_write_bytes(blk.outputs.screen_seq[0], blk.outputs.screen[0], scr, kScreenBytes);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ControllerConfig cfg;
    cfg.sim = true;
    cfg.sim_port = kTestPort;
    cfg.heartbeat_timeout_us = 1000000;
    auto say = [](const std::string& m) { std::printf("    controller: %s\n", m.c_str()); };
    {
        Controller ctl(default_map(), cfg, &blk, say);
        std::atomic<bool> stop{false}, done{false};
        std::thread th([&] {
            ctl.run(stop);
            done.store(true);
        });
        SOCKET so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(so, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        DWORD tmo = 2;
        setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        sockaddr_in to = me;
        to.sin_port = htons(kTestPort);
        std::vector<std::vector<uint8_t>> got;
        uint8_t pads[1 + kPadsPayload] = {0x20};
        auto pump = [&](double max_ms, auto until) {
            const auto t0 = clk::now();
            while (ms_since(t0) < max_ms && !until()) {
                sendto(so, reinterpret_cast<const char*>(pads), sizeof(pads), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                uint8_t buf[600];
                int n;
                while ((n = recv(so, reinterpret_cast<char*>(buf), sizeof(buf), 0)) > 0) got.emplace_back(buf, buf + n);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto last = [&](uint8_t id, int y = -1) -> const std::vector<uint8_t>* {
            for (auto it = got.rbegin(); it != got.rend(); ++it)
                if (!it->empty() && (*it)[0] == id && (y < 0 || (it->size() > 3 && (*it)[3] == y))) return &*it;
            return nullptr;
        };
        pump(4000, [&] { return blk.status.connected.load() && last(0x82) && last(0xE0, 56); });
        CHECK(blk.status.connected.load() == 1);
        const auto* b0 = last(0x82);
        CHECK(b0 && b0->size() == 32 && std::count_if(b0->begin() + 1, b0->end(), [](uint8_t v) { return v != 0; }) > 0);

        got.clear();
        const auto t_stop = clk::now();
        blk.status.stop_request.store(1);
        pump(2000, [&] { return done.load(); });
        const double took = ms_since(t_stop);
        pump(30, [] { return false; });  // what is still in flight
        th.join();
        std::printf("    run() returned %.0f ms after stop_request\n", took);
        CHECK(done.load());
        CHECK(took < 500.0);
        CHECK(blk.status.connected.load() == 0);

        const LedState off{};
        std::array<uint8_t, 49> pad_r{};
        std::array<uint8_t, 57> grp_r{};
        std::array<uint8_t, 32> btn_r{};
        build_pad_led_report(off, default_map(), pad_r);
        build_group_led_report(off, grp_r);
        build_button_led_report(off, btn_r);
        const auto* p = last(0x80);
        const auto* g = last(0x81);
        const auto* b = last(0x82);
        CHECK(p && std::equal(p->begin(), p->end(), pad_r.begin(), pad_r.end()));
        CHECK(g && std::equal(g->begin(), g->end(), grp_r.begin(), grp_r.end()));
        CHECK(b && std::equal(b->begin(), b->end(), btn_r.begin(), btn_r.end()));

        static ui::Screen page;
        ui::draw_stopped(page);
        std::array<std::array<uint8_t, kScreenBytes>, 2> half{};
        page.split(half[0].data(), half[1].data());
        int strips_ok = 0;
        for (int side = 0; side < 2; ++side)
            for (int k = 0; k < kStrips; ++k) {
                std::array<uint8_t, kStripReportSize> want{};
                build_strip_report(side, k, half[side].data(), want);
                const auto* st = last(static_cast<uint8_t>(0xE0 + side), k * 8);
                if (st && std::equal(st->begin(), st->end(), want.begin(), want.end())) ++strips_ok;
            }
        CHECK(strips_ok == 2 * kStrips);
        int lit_bytes = 0;
        for (int i = 0; i < kScreenBytes; ++i) lit_bytes += half[0][i] != 0;
        CHECK(lit_bytes > 50);  // the page says something
        closesocket(so);
    }
    {
        // No MK2 (no simulator sending): a stop request still ends run() quickly.
        blk.status.stop_request.store(0);
        Controller ctl(default_map(), cfg, &blk, say);
        std::atomic<bool> stop{false};
        std::thread th([&] { ctl.run(stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto t_stop = clk::now();
        blk.status.stop_request.store(1);
        th.join();
        const double took = ms_since(t_stop);
        std::printf("    without an MK2: run() returned %.0f ms after stop_request\n", took);
        CHECK(took < 1000.0);
    }
    {
        // Ctrl+C in the controller's own console (the old path) still ends it.
        blk.status.stop_request.store(0);
        Controller ctl(default_map(), cfg, &blk, say);
        std::atomic<bool> stop{false};
        std::thread th([&] { ctl.run(stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto t_stop = clk::now();
        stop.store(true);
        th.join();
        CHECK(ms_since(t_stop) < 1000.0);
    }
    static_assert(kShmVersion == 2, "stop_request came with v2");
    WSACleanup();
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
    test_sim_peer_policy();
    test_stop_request();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
