// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// mk2_monitor.exe - test client for mk2_controller.exe.
//
// Attaches to the shared block, prints every event, and drives the outputs so
// the whole loop can be checked on the device:
//   * pad hit  -> that pad lights in a colour from its velocity (blue soft .. red hard),
//                 aftertouch sets the brightness, release dims it
//   * button   -> its LED lights while held
//   * screens  -> one 512x64 page: event log, last hit, 8 knob bars over the knobs
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>

#include "himmel/mk2/output.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"

using namespace himmel::mk2;

namespace {

std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

// Knob centres on the 512-wide canvas (they sit under the screens).
constexpr int kKnobX[8] = {10, 87, 163, 240, 266, 343, 419, 496};

void velocity_colour(int v, uint8_t rgb[3]) {
    // 1..127: blue -> cyan -> green -> yellow -> red
    const float x = (v - 1) / 126.0f;
    const float r = std::clamp(x * 2.0f - 0.8f, 0.0f, 1.0f);
    const float g = x < 0.75f ? std::clamp(x * 2.0f, 0.0f, 1.0f) : std::clamp((1.0f - x) * 4.0f, 0.0f, 1.0f);
    const float b = std::clamp(1.0f - x * 2.0f, 0.0f, 1.0f);
    rgb[0] = static_cast<uint8_t>(r * 127);
    rgb[1] = static_cast<uint8_t>(g * 127);
    rgb[2] = static_cast<uint8_t>(b * 127);
}

std::string describe(const Event& e) {
    char s[96];
    switch (e.type) {
        case EventType::ButtonDown:
        case EventType::ButtonUp:
            std::snprintf(s, sizeof(s), "%s %s", e.type == EventType::ButtonDown ? "DOWN" : "UP  ",
                          std::string(kButtonNames[e.id]).c_str());
            break;
        case EventType::KnobDelta: std::snprintf(s, sizeof(s), "KNOB %d %+d", e.id + 1, e.value); break;
        case EventType::EncoderDelta: std::snprintf(s, sizeof(s), "ENCODER %+d", e.value); break;
        case EventType::PadHit: std::snprintf(s, sizeof(s), "HIT pad %d vel %d (p %d)", e.id, e.value, e.raw); break;
        case EventType::PadPressure: std::snprintf(s, sizeof(s), "PRESS pad %d %d", e.id, e.value); break;
        case EventType::PadRelease: std::snprintf(s, sizeof(s), "RELEASE pad %d", e.id); break;
        case EventType::Connected: std::snprintf(s, sizeof(s), "CONNECTED"); break;
        case EventType::Disconnected: std::snprintf(s, sizeof(s), "DISCONNECTED"); break;
        default: std::snprintf(s, sizeof(s), "%s", std::string(event_name(e.type)).c_str()); break;  // not from an MK2
    }
    return s;
}

}  // namespace

int main() {
    SetConsoleCtrlHandler(on_console_event, TRUE);
    SharedMemory shm;
    while (!shm.open()) {
        if (g_stop) return 0;
        std::printf("Waiting for mk2_controller.exe ...\n");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    SharedBlock* b = shm.get();
    int ring_index = kMaxConsumers - 1;  // prefer the last ring; engine/GUI take the first ones
    for (int i = kMaxConsumers - 1; i >= 0; --i)
        if (!b->rings[i].attached.load()) { ring_index = i; break; }
    EventRing& ring = b->rings[ring_index];
    ring.attach();
    std::printf("Attached to ring %d. Ctrl+C to quit.\n", ring_index);

    LedState leds{};
    std::array<int, kKnobs> knob{};      // 0..1000, from deltas
    knob.fill(500);
    int encoder = 0, last_pad = 0, last_vel = 0, hits = 0;
    std::array<int, kPads> pad_vel{};
    std::deque<std::string> log;
    Canvas<512> c;
    std::array<uint8_t, kScreenBytes> left{}, right{};
    auto t_next_draw = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        Event e;
        bool leds_changed = false;
        while (ring.pop(e)) {
            const std::string text = describe(e);
            if (e.type != EventType::PadPressure) {
                std::printf("%10.3f  %s\n", e.t_us / 1e6, text.c_str());
                log.push_front(text);
                if (log.size() > 5) log.pop_back();
            }
            switch (e.type) {
                case EventType::ButtonDown:
                case EventType::ButtonUp: {
                    const uint8_t v = e.type == EventType::ButtonDown ? 127 : 0;
                    const LedSlot slot = led_of(static_cast<Button>(e.id));
                    if (slot.kind == 1) leds.buttons[slot.index] = v;
                    if (slot.kind == 2) leds.transport[slot.index] = v;
                    if (slot.kind == 3)
                        for (auto& lr : leds.group_rgb[slot.index]) lr[0] = lr[1] = lr[2] = v;
                    leds_changed = true;
                    break;
                }
                case EventType::KnobDelta: knob[e.id] = std::clamp(knob[e.id] + e.value, 0, 1000); break;
                case EventType::EncoderDelta: encoder += e.value; break;
                case EventType::PadHit:
                    velocity_colour(e.value, leds.pad_rgb[e.id - 1]);
                    pad_vel[e.id - 1] = e.value;
                    last_pad = e.id;
                    last_vel = e.value;
                    ++hits;
                    leds_changed = true;
                    break;
                case EventType::PadPressure: {
                    uint8_t rgb[3];
                    velocity_colour(pad_vel[e.id - 1], rgb);
                    const float k = std::clamp(0.25f + e.value / 3000.0f, 0.25f, 1.0f);
                    for (int i = 0; i < 3; ++i) leds.pad_rgb[e.id - 1][i] = static_cast<uint8_t>(rgb[i] * k);
                    leds_changed = true;
                    break;
                }
                case EventType::PadRelease:
                    for (auto& ch : leds.pad_rgb[e.id - 1]) ch = static_cast<uint8_t>(ch / 8);
                    leds_changed = true;
                    break;
                default: break;
            }
        }
        if (leds_changed) seq_write(b->outputs.led_seq, b->outputs.leds, leds);

        const auto now = std::chrono::steady_clock::now();
        if (now >= t_next_draw) {
            t_next_draw = now + std::chrono::milliseconds(33);
            c.clear();
            c.fill(0, 0, 512, 8);
            char line[96];
            std::snprintf(line, sizeof(line), "MK2 MONITOR (C++)  %s  pads %llu  ctl %llu  drops %u  reconnects %llu",
                          b->status.connected.load() ? "CONNECTED" : "WAITING",
                          static_cast<unsigned long long>(b->status.pad_reports.load()),
                          static_cast<unsigned long long>(b->status.control_reports.load()),
                          ring.dropped.load(), static_cast<unsigned long long>(b->status.reconnects.load()));
            c.text(2, 0, line, false);
            int y = 11;
            for (const auto& s : log) {
                c.text(2, y, s.substr(0, 40));
                y += 8;
            }
            std::snprintf(line, sizeof(line), "PAD %d", last_pad);
            c.text(300, 11, line, true, 2);
            std::snprintf(line, sizeof(line), "VEL %d", last_vel);
            c.text(300, 29, line, true, 2);
            std::snprintf(line, sizeof(line), "HITS %d  ENC %d", hits, encoder);
            c.text(420, 11, line);
            for (int k = 0; k < kKnobs; ++k) {  // bars right above the physical knobs
                const int x0 = std::clamp(kKnobX[k] - 10, 0, 512 - 20);
                c.rect(x0, 54, 20, 10);
                c.fill(x0 + 1, 55, knob[k] * 18 / 1000, 8);
            }
            c.split(left.data(), right.data());
            seq_write_bytes(b->outputs.screen_seq[0], b->outputs.screen[0], left.data(), kScreenBytes);
            seq_write_bytes(b->outputs.screen_seq[1], b->outputs.screen[1], right.data(), kScreenBytes);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    LedState off{};
    seq_write(b->outputs.led_seq, b->outputs.leds, off);
    std::array<uint8_t, kScreenBytes> blank{};
    seq_write_bytes(b->outputs.screen_seq[0], b->outputs.screen[0], blank.data(), kScreenBytes);
    seq_write_bytes(b->outputs.screen_seq[1], b->outputs.screen[1], blank.data(), kScreenBytes);
    ring.detach();
    std::printf("Bye.\n");
    return 0;
}
