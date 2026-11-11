// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// weapon_monitor.exe - test client for the weapon shared blocks (like mk2_monitor).
//
//   weapon_monitor [--slot N] [--ring R] [--echo]
//   weapon_monitor --stop N      ask the driver in slot N to let go of its device and exit
//
// Without --slot it watches every slot that has a driver. Prints each event with the
// control's name from the weapon's profile, and the driver's status every 2 s.
// Reads ring 1 by default, so it can run next to the engine (ring 0).
// --echo: also writes outputs, so the LED path can be checked without the engine:
// pressed controls light their LED, pads/keys/touches light pixels, the display
// shows the last event. (Do not use --echo while the engine runs: one writer only.)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "himmel/weapon/driver.h"
#include "himmel/weapon/nuvj.h"
#include "himmel/weapon/shared.h"

using namespace himmel::weapon;

namespace {
std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

struct Watch {
    int slot = 0;
    SharedMemory shm;
    bool open = false;
    std::optional<Profile> prof;
    std::array<uint8_t, kMaxControls> led{};
    std::array<std::array<uint8_t, 3>, kMaxPixels> px{};
    std::array<float, kMaxPixels> glow{};
};

std::string control_name(const Watch& w, int id) {
    if (w.prof && id >= 0 && id < w.prof->count()) return w.prof->names[static_cast<size_t>(id)];
    return "#" + std::to_string(id);
}

std::string describe(const Watch& w, const Event& e) {
    char s[160];
    switch (e.type) {
        case EventType::ButtonDown:
        case EventType::ButtonUp:
            std::snprintf(s, sizeof(s), "%s %s", e.type == EventType::ButtonDown ? "DOWN" : "UP  ", control_name(w, e.id).c_str());
            break;
        case EventType::KnobDelta:
            std::snprintf(s, sizeof(s), "KNOB %s %+d (value %u)", control_name(w, e.id).c_str(), e.value, e.raw);
            break;
        case EventType::EncoderDelta: std::snprintf(s, sizeof(s), "ENCODER %s %+d", control_name(w, e.id).c_str(), e.value); break;
        case EventType::Fader: std::snprintf(s, sizeof(s), "FADER %s %d", control_name(w, e.id).c_str(), e.value); break;
        case EventType::Switch: std::snprintf(s, sizeof(s), "SWITCH %s pos %d", control_name(w, e.id).c_str(), e.value); break;
        case EventType::PadHit:
            std::snprintf(s, sizeof(s), "HIT pad %d vel %d%s%s", e.id, e.value, e.reserved < kMaxControls ? "  " : "",
                          e.reserved < kMaxControls ? control_name(w, e.reserved).c_str() : "");
            break;
        case EventType::PadPressure: std::snprintf(s, sizeof(s), "PRESS pad %d %d", e.id, e.value); break;
        case EventType::PadRelease: std::snprintf(s, sizeof(s), "RELEASE pad %d", e.id); break;
        case EventType::TouchStart:
        case EventType::TouchMove:
        case EventType::TouchEnd:
            std::snprintf(s, sizeof(s), "TOUCH %s finger %d x %d y %u z %u",
                          e.type == EventType::TouchStart ? "start" : e.type == EventType::TouchMove ? "move " : "end  ",
                          e.id, e.value, e.raw, e.reserved & 0xFF);
            break;
        case EventType::Connected: std::snprintf(s, sizeof(s), "CONNECTED"); break;
        case EventType::Disconnected: std::snprintf(s, sizeof(s), "DISCONNECTED"); break;
        default: std::snprintf(s, sizeof(s), "event %d", static_cast<int>(e.type)); break;
    }
    return s;
}

// --echo: a tiny stand-in for the engine.
void echo(Watch& w, const Event& e, char* last) {
    auto* b = w.shm.get();
    const Header& h = b->header;
    bool leds = false;
    if (e.type == EventType::ButtonDown || e.type == EventType::ButtonUp) {
        w.led[e.id] = e.type == EventType::ButtonDown ? 127 : 0;
        leds = true;
    }
    if (e.type == EventType::PadHit || e.type == EventType::PadRelease) {
        const int c = e.id >= 1 && e.id <= kMaxPads ? h.pad_control[e.id - 1] : kNone;
        if (c != kNone) w.led[c] = e.type == EventType::PadHit ? 127 : 0, leds = true;
        if (e.type == EventType::PadHit && h.pixel_h == 1 && e.id <= h.pixel_w) w.glow[e.id - 1] = 1.0f;
        if (e.type == EventType::PadHit && h.pixel_h == 15) {  // light the 3 x 3 zone of a Lightpad pad
            const int col = (e.id - 1) % 4, row = 3 - (e.id - 1) / 4;
            for (int y = row * 4; y < row * 4 + 3; ++y)
                for (int x = col * 4; x < col * 4 + 3; ++x) w.glow[static_cast<size_t>(y * 15 + x)] = 1.0f;
        }
    }
    if (leds) {
        himmel::mk2::seq_write(b->outputs.led_seq, b->outputs.led, *reinterpret_cast<uint8_t(*)[kMaxControls]>(w.led.data()));
    }
    if (h.caps & kCapDisplay) {
        Display d{};
        set_text(d.text, "ECHO");
        std::string a = last;
        for (char& c : a) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        set_text(d.alpha, a.substr(0, 11));
        d.marks = himmel::weapon::nuvj::kMarkRule;
        himmel::mk2::seq_write(b->outputs.display_seq, b->outputs.display, d);
    }
}

void fade_pixels(Watch& w) {
    auto* b = w.shm.get();
    const int n = b->header.pixel_w * b->header.pixel_h;
    if (!n) return;
    decltype(b->outputs.pixel) px{};
    for (int i = 0; i < n && i < kMaxPixels; ++i) {
        w.glow[static_cast<size_t>(i)] *= 0.9f;
        const auto v = static_cast<uint8_t>(255 * w.glow[static_cast<size_t>(i)]);
        px[i][0] = static_cast<uint8_t>(v / 2), px[i][1] = v, px[i][2] = std::max<uint8_t>(v, 10);
    }
    himmel::mk2::seq_write(b->outputs.pixel_seq, b->outputs.pixel, px);
}
}  // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(on_console_event, TRUE);
    int only = 0, ring = 1;
    bool do_echo = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--stop" && i + 1 < argc) {
            SharedMemory shm;
            const int slot = std::atoi(argv[++i]);
            if (!shm.open(slot)) {
                std::printf("no driver in slot %d\n", slot);
                return 1;
            }
            shm.get()->status.stop_request.store(1);
            std::printf("asked the driver in slot %d (%s) to stop\n", slot, shm.get()->header.name);
            return 0;
        }
        if (a == "--slot" && i + 1 < argc) only = std::atoi(argv[++i]);
        else if (a == "--ring" && i + 1 < argc) ring = std::clamp(std::atoi(argv[++i]), 0, kMaxConsumers - 1);
        else if (a == "--echo") do_echo = true;
    }
    std::vector<std::unique_ptr<Watch>> watches;
    for (int s = 1; s <= kMaxSlots; ++s)
        if (!only || s == only) {
            watches.push_back(std::make_unique<Watch>());
            watches.back()->slot = s;
        }
    std::printf("Watching %s (ring %d)%s. Ctrl+C to stop.\n", only ? ("slot " + std::to_string(only)).c_str() : "every weapon slot",
                ring, do_echo ? ", echoing to the outputs" : "");
    auto next_status = std::chrono::steady_clock::now();
    char last[64] = "";
    while (!g_stop.load()) {
        for (auto& wp : watches) {
            Watch& w = *wp;
            if (!w.open) {
                if (!w.shm.open(w.slot)) continue;
                w.open = true;
                w.shm.get()->rings[ring].attach();
                const Header& h = w.shm.get()->header;
                w.prof = load_profile(h.profile);
                std::printf("slot %d: %s (%s, %s)%s\n", w.slot, h.name, h.driver, h.profile, h.simulated ? " simulated" : "");
            }
            auto* b = w.shm.get();
            auto& r = b->rings[ring];
            if (!r.attached.load()) r.attach();
            Event e;
            while (r.pop(e)) {
                const std::string d = describe(w, e);
                std::snprintf(last, sizeof(last), "%s", d.c_str());
                std::printf("[%d %s] %s\n", w.slot, b->header.weapon, d.c_str());
                if (do_echo) echo(w, e, last);
            }
            if (do_echo) fade_pixels(w);
        }
        if (std::chrono::steady_clock::now() >= next_status) {
            next_status += std::chrono::seconds(2);
            for (auto& wp : watches) {
                if (!wp->open) continue;
                auto& st = wp->shm.get()->status;
                const uint64_t age = now_us() - std::min(now_us(), st.heartbeat_us.load());
                std::printf("  status slot %d: %s, heartbeat %llu ms ago, in %llu out %llu, frames %llu, acks %llu, resends %llu, sync %.1f ms, battery %d, device %s - %s\n",
                            wp->slot, st.connected.load() ? "connected" : "NOT connected",
                            static_cast<unsigned long long>(age / 1000), static_cast<unsigned long long>(st.msgs_in.load()),
                            static_cast<unsigned long long>(st.msgs_out.load()), static_cast<unsigned long long>(st.led_frames.load()),
                            static_cast<unsigned long long>(st.acks.load()), static_cast<unsigned long long>(st.resends.load()),
                            st.sync_us.load() / 1000.0, st.battery.load(), st.device, st.message);
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    for (auto& wp : watches)
        if (wp->open) wp->shm.get()->rings[ring].detach();
    return 0;
}
