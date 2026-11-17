// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// MK2 chain test, no hardware (2026-11-16): the real Controller in sim mode on a
// private UDP port and a private shared-memory block, two pretend simulators
// (UDP sockets that send the same reports as tools/mk2_sim.py), and Mk2Perform
// on ring 0 with a tiny stand-in for the engine model.
//
//   simulator --udp--> Controller --ring 0--> Mk2Perform --commands--> fake engine
//   simulator <--udp-- Controller <--LEDs / screens (seqlock)-- Mk2Perform
//
// Regression for the owner's report of 2026-11-15 ("the simulator does not drive
// the engine"): a second simulator started later silently took the link, the
// first one's presses vanished and its panel never showed the engine's screens.
// Now the ignored simulator is told (report 0xFD), and gets the link back once
// the other one stops.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "himmel/mk2/controller.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/sim_link.h"
#include "himmel/perform/mk2_perform.h"

using namespace himmel;
using namespace himmel::protocol;
using mk2::Button;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- pretend simulator
// Streams pad reports every 4 ms and a controls report on change / every 100 ms
// (like mk2_sim.py), on its own thread, and records what comes back.
struct Sim {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in to{};
    mk2::Map map = mk2::default_map();
    std::array<uint8_t, mk2::kControlsPayload> controls{};
    std::atomic<bool> dirty{true}, run{true}, sending{true};
    std::atomic<int> hello{0}, busy{0}, led80{0}, led81{0}, led82{0}, strips{0};
    std::atomic<int> group_led_lit{0};
    std::thread th;
    CRITICAL_SECTION cs;

    explicit Sim(uint16_t port) {
        InitializeCriticalSection(&cs);
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);  // SIO_UDP_CONNRESET
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int k = 0; k < mk2::kKnobs; ++k) set_knob(k, 500);
        th = std::thread([this] { loop(); });
    }
    ~Sim() {
        run = false;
        th.join();
        closesocket(s);
        DeleteCriticalSection(&cs);
    }
    void set_knob(int k, int v) {
        const auto& p = map.knobs[k];
        controls[p.byte] = static_cast<uint8_t>(v & 0xFF);
        controls[p.byte + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    int knob(int k) const { return controls[map.knobs[k].byte] | (controls[map.knobs[k].byte + 1] << 8); }
    void press(Button b, bool down) {
        EnterCriticalSection(&cs);
        const auto& p = map.buttons[static_cast<int>(b)];
        if (down) controls[p.byte] = static_cast<uint8_t>(controls[p.byte] | (1 << p.bit));
        else controls[p.byte] = static_cast<uint8_t>(controls[p.byte] & ~(1 << p.bit));
        dirty = true;
        LeaveCriticalSection(&cs);
    }
    void turn(int k, int delta) {
        EnterCriticalSection(&cs);
        set_knob(k, (knob(k) + delta + 1000) % 1000);
        dirty = true;
        LeaveCriticalSection(&cs);
    }
    void wheel(int detents) {
        EnterCriticalSection(&cs);
        auto& b = controls[map.encoder.byte];
        b = static_cast<uint8_t>((b & 0xF0) | (((b & 0x0F) + detents + 16) & 0x0F));
        dirty = true;
        LeaveCriticalSection(&cs);
    }
    void send(const uint8_t* d, int n) { sendto(s, reinterpret_cast<const char*>(d), n, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)); }
    void loop() {
        uint64_t next_pad = 0, next_ctl = 0;
        uint8_t buf[512];
        while (run) {
            const uint64_t t = now_us();
            if (sending) {
                if (t >= next_pad) {
                    uint8_t r[1 + mk2::kPadsPayload] = {0x20};
                    for (int id = 0; id < 16; ++id) {
                        const uint16_t w = static_cast<uint16_t>(id << 12);
                        r[1 + 2 * id] = w & 0xFF, r[2 + 2 * id] = static_cast<uint8_t>(w >> 8);
                    }
                    send(r, sizeof(r));
                    next_pad = t + 4000;
                }
                if (dirty || t >= next_ctl) {
                    uint8_t r[1 + mk2::kControlsPayload] = {0x01};
                    EnterCriticalSection(&cs);
                    std::memcpy(r + 1, controls.data(), controls.size());
                    dirty = false;
                    LeaveCriticalSection(&cs);
                    send(r, sizeof(r));
                    next_ctl = t + 100000;
                }
            }
            for (;;) {
                const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) break;
                switch (buf[0]) {
                    case mk2::kSimHelloReport: ++hello; break;
                    case mk2::kSimBusyReport: ++busy; break;
                    case 0x80: ++led80; break;
                    case 0x81: {
                        ++led81;
                        int lit = 0;
                        for (int i = 1; i < n; ++i) lit += buf[i] != 0;
                        group_led_lit = lit;
                        break;
                    }
                    case 0x82: ++led82; break;
                    case 0xE0:
                    case 0xE1: ++strips; break;
                    default: break;
                }
            }
            sleep_ms(1);
        }
    }
};

// ---------------------------------------------------------------- stand-in engine
struct Sink : perform::CommandSink {
    std::vector<Command> got;
    bool push(const Command& c) override {
        got.push_back(c);
        return true;
    }
};

struct Engine {
    std::unique_ptr<LiveState> live = std::make_unique<LiveState>();
    std::unique_ptr<Catalog> cat = std::make_unique<Catalog>();
    perform::Mk2Perform perform;
    Sink sink;
    mk2::LedState leds{};
    uint8_t screens[2][mk2::kScreenBytes]{};
    int focus_cmds = 0, xfade_cmds = 0, opacity_cmds = 0, events = 0;

    Engine() {
        std::memset(live.get(), 0, sizeof(LiveState));
        std::memset(cat.get(), 0, sizeof(Catalog));
        live->epoch = 1;
        live->focus_deck = 0, live->focus_layer = 0;
        for (auto& d : live->deck)
            for (auto& l : d.layer) {
                l.clip = 0;
                l.opacity = 1.0f;
                l.speed = 1.0f;
                for (auto& f : l.fx) f.effect = -1;
            }
        for (auto& f : live->master_fx) f.effect = -1;
        cat->clip_count = 4;
        for (int i = 0; i < 4; ++i) std::snprintf(cat->clip[i].name, kNameLen, "clip%d", i);
    }
    // What the real model does for the commands this test uses.
    void apply() {
        for (const Command& c : sink.got) {
            if (c.type == CommandType::Focus) {
                ++focus_cmds;
                live->focus_deck = c.deck, live->focus_layer = c.layer;
            } else if (c.type == CommandType::Crossfader) {
                ++xfade_cmds;
                live->crossfader = std::fmin(1.0f, std::fmax(0.0f, live->crossfader + c.f[0]));
            } else if (c.type == CommandType::LayerOpacity) {
                ++opacity_cmds;
                auto& o = live->deck[c.deck].layer[c.layer].opacity;
                o = std::fmin(1.0f, std::fmax(0.0f, o + c.f[0]));
            }
        }
        sink.got.clear();
    }
    // One frame, like himmel_render's loop: drain ring 0, apply, tick, publish the outputs.
    void frame(mk2::SharedBlock* b) {
        auto& ring = b->rings[0];
        mk2::Event e;
        while (ring.pop(e)) {
            ++events;
            perform.on_event(e, *live, *cat, sink);
        }
        apply();
        perform.tick(now_us(), *live, *cat, sink, leds, screens);
        apply();
        mk2::seq_write(b->outputs.led_seq, b->outputs.leds, leds);
        for (int s = 0; s < 2; ++s) mk2::seq_write_bytes(b->outputs.screen_seq[s], b->outputs.screen[s], screens[s], mk2::kScreenBytes);
    }
    // Runs frames at ~100 Hz until pred() or the timeout.
    template <class P>
    bool run_until(mk2::SharedBlock* b, P pred, int timeout_ms = 2000) {
        const uint64_t end = now_us() + static_cast<uint64_t>(timeout_ms) * 1000;
        while (now_us() < end) {
            frame(b);
            if (pred()) return true;
            sleep_ms(10);
        }
        frame(b);
        return pred();
    }
};

int main() {
    std::printf("MK2 chain: simulator -> controller -> ring 0 -> perform -> engine, and back\n");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    const uint16_t port = static_cast<uint16_t>(18766 + (GetCurrentProcessId() % 1000));
    const std::wstring name = L"Local\\HimmelMk2ChainTest" + std::to_wstring(GetCurrentProcessId());
    mk2::SharedMemory shm;
    CHECK(shm.create(name.c_str()));
    mk2::SharedBlock* b = shm.get();
    if (!b) return 1;
    b->rings[0].attach();

    mk2::ControllerConfig cfg;
    cfg.sim = true;
    cfg.sim_port = port;
    cfg.heartbeat_timeout_us = 1000000;  // as mk2_controller --sim
    std::vector<std::string> log;
    CRITICAL_SECTION log_cs;
    InitializeCriticalSection(&log_cs);
    std::atomic<bool> stop{false};
    mk2::Controller ctl(mk2::default_map(), cfg, b, [&](const std::string& m) {
        EnterCriticalSection(&log_cs);
        log.push_back(m);
        LeaveCriticalSection(&log_cs);
        std::printf("    controller: %s\n", m.c_str());
    });
    std::thread ctl_thread([&] { ctl.run(stop); });

    Engine eng;
    auto tap = [&](Sim& s, Button btn) {
        s.press(btn, true);
        eng.run_until(b, [] { return false; }, 40);
        s.press(btn, false);
        eng.run_until(b, [] { return false; }, 40);
    };

    {
        std::printf("one simulator\n");
        auto a = std::make_unique<Sim>(port);
        CHECK(eng.run_until(b, [&] { return b->status.connected.load() == 1; }, 3000));
        eng.run_until(b, [] { return false; }, 300);  // first controls report = baseline
        // Every group sets the focus (PREVIEW = deck B while A is on air).
        const int want_deck[8] = {1, 1, 1, 1, 0, 0, 0, 0};
        int ok = 0;
        for (int g = 0; g < 8; ++g) {
            tap(*a, static_cast<Button>(static_cast<int>(Button::GroupA) + g));
            ok += eng.run_until(b, [&] { return eng.live->focus_deck == want_deck[g] && eng.live->focus_layer == (g & 3); }, 500);
        }
        CHECK(ok == 8);
        // 24 quick presses: none lost.
        const int before = eng.focus_cmds;
        for (int i = 0; i < 24; ++i) tap(*a, i % 2 ? Button::GroupB : Button::GroupC);
        eng.run_until(b, [] { return false; }, 100);
        CHECK(eng.focus_cmds - before == 24);
        // Wheel = T-bar; on the LAYER page (PAGE > twice, M4) knob 1 = opacity of the focused layer.
        a->wheel(3);
        CHECK(eng.run_until(b, [&] { return std::fabs(eng.live->crossfader - 0.1f) < 0.01f; }));
        const float op0 = eng.live->deck[eng.live->focus_deck].layer[eng.live->focus_layer].opacity;
        tap(*a, Button::PageRight);
        tap(*a, Button::PageRight);
        a->turn(0, -250);
        CHECK(eng.run_until(b, [&] { return eng.live->deck[eng.live->focus_deck].layer[eng.live->focus_layer].opacity < op0 - 0.2f; }));
        // The engine's LEDs and screens come back to the simulator.
        CHECK(eng.run_until(b, [&] { return a->led81 > 0 && a->group_led_lit > 0 && a->strips >= 16; }));
        CHECK(a->hello > 0);
        CHECK(a->busy == 0);
        CHECK(b->rings[0].dropped.load() == 0);

        std::printf("a second simulator starts (the owner's 2026-11-15 case)\n");
        auto c = std::make_unique<Sim>(port);
        CHECK(eng.run_until(b, [&] { return c->hello > 0 && c->strips > 0; }, 3000));  // the newest wins, full resend
        CHECK(eng.run_until(b, [&] { return a->busy > 0; }, 1000));                    // the old one is told
        auto logged = [&] {  // the controller's run loop logs it within ~20 ms
            bool found = false;
            EnterCriticalSection(&log_cs);
            for (const auto& m : log) found = found || m.find("ignoring another") != std::string::npos;
            LeaveCriticalSection(&log_cs);
            return found;
        };
        CHECK(eng.run_until(b, logged, 1000));
        eng.run_until(b, [] { return false; }, 300);
        const int f0 = eng.focus_cmds;
        tap(*a, Button::GroupD);  // ignored
        eng.run_until(b, [] { return false; }, 200);
        CHECK(eng.focus_cmds == f0);
        tap(*c, Button::GroupH);  // the peer
        CHECK(eng.run_until(b, [&] { return eng.live->focus_deck == 0 && eng.live->focus_layer == 3; }));

        std::printf("the second simulator closes: the first one gets the link back\n");
        c.reset();
        const int busy0 = a->busy, strips0 = a->strips;
        CHECK(eng.run_until(b, [&] { return a->strips > strips0 + 8; }, 4000));
        eng.run_until(b, [] { return false; }, 300);
        CHECK(a->busy - busy0 <= 8);  // notices stop once it is the peer again
        tap(*a, Button::GroupA);
        CHECK(eng.run_until(b, [&] { return eng.live->focus_deck == 1 && eng.live->focus_layer == 0; }));
    }

    stop = true;
    ctl_thread.join();
    DeleteCriticalSection(&log_cs);
    WSACleanup();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
