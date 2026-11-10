// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// mk2_pages_demo.exe - the Himmel screen pages, live on the MK2.
//
// Run mk2_controller.exe first. This is a stand-in for the engine/GUI: it keeps
// a small fake show state, turns controller events into page models and writes
// the screens and LEDs through the shared block.
//
//   Control / Browse / Sampling   Perform view / library browser / audio view
//   Group A-H                     select deck          Page < / Page >  knob page
//   Knobs 1-8                     change the page's parameters (touched one inverts)
//   Master encoder                Perform: fine-tune the last knob; Browse: scroll
//   Encoder push                  Browse: load the item
//   Tempo held + encoder          BPM (big message on the right screen)
//   Play                          start / stop the beat clock
//   Shift + encoder               system menu (turn, push to pick)
//   Pads                          Audio view: pressure drives the fake meters
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "himmel/mk2/output.h"
#include "himmel/mk2/pages.h"
#include "himmel/mk2/shared.h"
#include "page_samples.h"

using namespace himmel::mk2;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

enum class View { Perform, Browse, Audio, KnobAlign };

constexpr std::array<std::string_view, 8> kMenu = {"PERFORM", "BROWSE", "AUDIO", "KNOB ALIGN", "ABOUT", "", "", ""};
constexpr int kMenuCount = 5;

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

struct Demo {
    View view = View::Perform;
    int deck = 0, page = 0;
    std::array<std::array<std::array<float, kKnobs>, 3>, 8> values{};  // deck, page, knob
    int touched = -1;
    Clock::time_point touched_t{};
    bool shift = false, tempo = false, playing = true;
    float bpm = 128.0f;
    Clock::time_point beat_origin = Clock::now();
    int browse = 0;
    bool menu = false;
    int menu_sel = 0;
    // Message on the right screen.
    char msg_title[24] = "", msg_big[32] = "", msg_small[40] = "";
    Clock::time_point msg_until{};
    // Fake audio from pad pressure.
    std::array<float, kPads> pad_p{};
    float level[2] = {0, 0}, peak[2] = {0, 0};
    std::array<float, 32> bands{};
    std::array<uint8_t, 96 * 54> thumb{};
    int thumb_for = -1;

    Demo() {
        for (int d = 0; d < 8; ++d)
            for (int p = 0; p < 3; ++p)
                for (int k = 0; k < kKnobs; ++k)
                    values[d][p][k] = samples::kKnobPages[p].params[k].bipolar ? 0.5f : 0.25f + 0.1f * ((d + k) % 5);
    }

    void message(const char* title, const char* big, const char* small, double seconds) {
        std::snprintf(msg_title, sizeof(msg_title), "%s", title);
        std::snprintf(msg_big, sizeof(msg_big), "%s", big);
        std::snprintf(msg_small, sizeof(msg_small), "%s", small);
        msg_until = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    }

    float beat_phase(Clock::time_point now) const {
        if (!playing) return -1.0f;
        return static_cast<float>(std::fmod(secs(beat_origin, now) * bpm / 60.0, 4.0));
    }

    void touch(int k, float delta) {
        float& v = values[deck][page][k];
        v = std::clamp(v + delta, 0.0f, 1.0f);
        touched = k;
        touched_t = Clock::now();
    }

    void pick_menu() {
        menu = false;
        switch (menu_sel) {
            case 0: view = View::Perform; break;
            case 1: view = View::Browse; break;
            case 2: view = View::Audio; break;
            case 3: view = View::KnobAlign; break;
            default: message("ABOUT", "HIMMEL", "MADE BY READYMEDIA.NO", 3.0); break;
        }
    }

    void on_event(const Event& e) {
        switch (e.type) {
            case EventType::ButtonDown:
            case EventType::ButtonUp: {
                const bool down = e.type == EventType::ButtonDown;
                const auto b = static_cast<Button>(e.id);
                if (b == Button::Shift) shift = down;
                if (b == Button::Tempo) {
                    tempo = down;
                    if (down) show_tempo();
                }
                if (!down) break;
                if (b == Button::Control) view = View::Perform, menu = false;
                if (b == Button::Browse) view = View::Browse, menu = false;
                if (b == Button::Sampling) view = View::Audio, menu = false;
                if (b == Button::PageLeft) page = (page + 2) % 3, touched = -1;
                if (b == Button::PageRight) page = (page + 1) % 3, touched = -1;
                if (b >= Button::GroupA && b <= Button::GroupH) deck = e.id - static_cast<int>(Button::GroupA), touched = -1;
                if (b == Button::Play) {
                    playing = !playing;
                    beat_origin = Clock::now();
                }
                if (b == Button::EncoderPush) {
                    if (menu) pick_menu();
                    else if (view == View::Browse) {
                        char n[40], t[10];
                        samples::library_item(browse, n, sizeof(n), t, sizeof(t));
                        message("LOADED TO DECK", n + 3, "PREVIEW ONLY - NO ENGINE YET", 1.5);
                    }
                }
                break;
            }
            case EventType::KnobDelta:
                if (view == View::KnobAlign) touched = e.id, touched_t = Clock::now();
                else touch(e.id, e.value / 1000.0f);
                break;
            case EventType::EncoderDelta:
                if (shift && !menu) {
                    menu = true;
                    menu_sel = std::clamp(static_cast<int>(view), 0, kMenuCount - 1);
                } else if (menu) {
                    menu_sel = std::clamp(menu_sel + e.value, 0, kMenuCount - 1);
                } else if (tempo) {
                    bpm = std::clamp(bpm + e.value * (shift ? 0.1f : 1.0f), 40.0f, 240.0f);
                    show_tempo();
                } else if (view == View::Browse) {
                    browse = std::clamp(browse + e.value, 0, samples::kLibraryCount - 1);
                } else if (view == View::Perform && touched >= 0) {
                    touch(touched, e.value * 0.01f);
                }
                break;
            case EventType::PadHit:
            case EventType::PadPressure: pad_p[e.id - 1] = e.type == EventType::PadHit ? e.value / 127.0f : e.value / 3300.0f; break;
            case EventType::PadRelease: pad_p[e.id - 1] = 0; break;
            case EventType::Connected: message("MK2", "CONNECTED", "", 1.0); break;
            default: break;
        }
    }

    void show_tempo() {
        char big[16];
        std::snprintf(big, sizeof(big), "%.1f BPM", bpm);
        message("TEMPO", big, "TURN = BPM  SHIFT = FINE", 1.2);
    }

    void tick_audio(float dt) {
        // Pads 1-16 -> 32 bands (low pads = low bands), smoothed; levels = sum.
        float sum = 0;
        for (int i = 0; i < 32; ++i) {
            const float target = std::clamp(pad_p[i / 2] * (0.8f + 0.2f * ((i & 1) ? 1.0f : 0.7f)), 0.0f, 1.0f);
            bands[i] += (target - bands[i]) * std::min(1.0f, dt * (target > bands[i] ? 30.0f : 6.0f));
            sum += bands[i];
        }
        for (int ch = 0; ch < 2; ++ch) {
            const float target = std::clamp(sum / (ch ? 7.0f : 6.0f), 0.0f, 1.0f);
            level[ch] += (target - level[ch]) * std::min(1.0f, dt * (target > level[ch] ? 40.0f : 5.0f));
            peak[ch] = std::max(level[ch], peak[ch] - dt * 0.4f);
        }
    }

    void draw(ui::Screen& c, Clock::time_point now) {
        const float phase = beat_phase(now);
        if (touched >= 0 && secs(touched_t, now) > 1.5) touched = -1;
        switch (view) {
            case View::Perform:
                ui::draw_perform(c, samples::perform(deck, page, values[deck][page], bpm, playing, touched, phase, shift));
                break;
            case View::Browse:
                if (thumb_for != browse) samples::thumbnail(browse, thumb), thumb_for = browse;
                c.clear();
                ui::draw_list(c, 0, samples::browser(browse));
                ui::draw_detail(c, 1, samples::detail(browse, &thumb));
                break;
            case View::Audio: {
                ui::AudioModel a;
                a.head[0].left.set("AUDIO  PADS AS INPUT (DEMO)");
                a.head[1].left.set("SPECTRUM");
                a.head[1].right.set("32");
                for (int ch = 0; ch < 2; ++ch) a.level[ch] = level[ch], a.peak[ch] = peak[ch];
                a.bands = bands;
                a.bpm = bpm;
                a.beat_phase = phase;
                a.beat_lock = playing;
                ui::draw_audio(c, a);
                break;
            }
            case View::KnobAlign: ui::draw_knob_align(c, touched); break;
        }
        if (menu) ui::draw_list(c, 0, samples::system_menu(kMenu, kMenuCount, menu_sel));
        if (now < msg_until) ui::draw_message(c, 1, msg_title, msg_big, msg_small);
    }

    void leds(LedState& s, Clock::time_point now) const {
        s = {};
        for (int g = 0; g < 8; ++g)
            for (auto& lr : s.group_rgb[g])
                for (auto& ch : lr) ch = static_cast<uint8_t>(g == deck ? 127 : 12);
        auto on = [&](Button b, uint8_t v) {
            const LedSlot slot = led_of(b);
            if (slot.kind == 1) s.buttons[slot.index] = v;
            if (slot.kind == 2) s.transport[slot.index] = v;
        };
        on(Button::Control, view == View::Perform ? 127 : 10);
        on(Button::Browse, view == View::Browse ? 127 : 10);
        on(Button::Sampling, view == View::Audio ? 127 : 10);
        on(Button::PageLeft, 40);
        on(Button::PageRight, 40);
        on(Button::Tempo, tempo ? 127 : 10);
        on(Button::Shift, shift ? 127 : 10);
        const float phase = beat_phase(now);
        on(Button::Play, playing ? static_cast<uint8_t>(phase - std::floor(phase) < 0.15f ? 127 : 40) : 0);
        if (view == View::Audio)
            for (int p = 0; p < kPads; ++p) {
                const uint8_t v = static_cast<uint8_t>(std::clamp(pad_p[p], 0.0f, 1.0f) * 127);
                s.pad_rgb[p][0] = v, s.pad_rgb[p][1] = static_cast<uint8_t>(v / 2), s.pad_rgb[p][2] = static_cast<uint8_t>(127 - v / 2);
            }
    }
};

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
    int ring_index = kMaxConsumers - 1;
    for (int i = kMaxConsumers - 1; i >= 0; --i)
        if (!b->rings[i].attached.load()) { ring_index = i; break; }
    EventRing& ring = b->rings[ring_index];
    ring.attach();
    std::printf("Pages demo on ring %d. Control / Browse / Sampling switch views, Shift + encoder = menu. Ctrl+C quits.\n",
                ring_index);

    Demo demo;
    ui::Screen canvas;
    std::array<uint8_t, kScreenBytes> left{}, right{};
    LedState leds{}, last_leds{};
    bool first = true;
    auto last = Clock::now(), next_draw = last;
    while (!g_stop.load()) {
        Event e;
        while (ring.pop(e)) demo.on_event(e);
        const auto now = Clock::now();
        demo.tick_audio(static_cast<float>(secs(last, now)));
        last = now;
        demo.leds(leds, now);
        if (first || std::memcmp(&leds, &last_leds, sizeof(leds)) != 0) {
            seq_write(b->outputs.led_seq, b->outputs.leds, leds);
            last_leds = leds;
            first = false;
        }
        if (now >= next_draw) {  // ~30 fps; the controller only sends strips that changed
            next_draw = now + std::chrono::milliseconds(33);
            demo.draw(canvas, now);
            canvas.split(left.data(), right.data());
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
    return 0;
}
