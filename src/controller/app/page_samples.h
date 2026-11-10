// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Shared sample content for the page check tool and the live page demo.
// Knob pages and decks follow the mockup (docs/vj-mockup.html).
#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "himmel/mk2/pages.h"

namespace himmel::mk2::samples {

struct ParamDef {
    std::string_view label;
    bool bipolar;
};
struct KnobPageDef {
    std::string_view name;
    std::array<ParamDef, kKnobs> params;
};

inline constexpr std::array<KnobPageDef, 3> kKnobPages = {{
    {"CORE FX", {{{"FEEDBACK", false}, {"ZOOM", true}, {"KALEIDO", false}, {"RGB SPLIT", false},
                  {"GLITCH", false}, {"BLUR", false}, {"DISPLACE", false}, {"STROBE", false}}}},
    {"COLOUR", {{{"HUE", true}, {"SATURATION", false}, {"BRIGHT", false}, {"CONTRAST", true},
                 {"PALETTE", false}, {"INVERT", false}, {"POSTERISE", false}, {"OPACITY", false}}}},
    {"MOTION", {{{"SPEED", true}, {"ROTATION", true}, {"DRIFT X", true}, {"DRIFT Y", true},
                 {"GRAVITY", false}, {"SIZE", false}, {"DENSITY", false}, {"LIFETIME", false}}}},
}};

inline constexpr std::array<std::string_view, 8> kDecks = {"BACKGROUND", "PARTICLES", "RINGS", "GEOMETRY",
                                                          "TEXT", "3 CAMERAS", "CLIPS", "AUDIO"};

// Value text for a 0..1 parameter: percent, or signed percent if bipolar.
inline void format_value(float v, bool bipolar, char* out, size_t n) {
    if (bipolar) {
        const int p = static_cast<int>(std::lround((v - 0.5f) * 200.0f));
        std::snprintf(out, n, "%+d", p);
    } else {
        std::snprintf(out, n, "%d%%", static_cast<int>(std::lround(v * 100.0f)));
    }
}

inline ui::PerformModel perform(int deck, int page, const std::array<float, kKnobs>& values, float bpm, bool playing,
                                int touched, float beat_phase, bool shift) {
    ui::PerformModel m;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%c %.*s", 'A' + deck, static_cast<int>(kDecks[deck].size()), kDecks[deck].data());
    m.head[0].left.set(buf);
    std::snprintf(buf, sizeof(buf), "P%d %.*s", page + 1, static_cast<int>(kKnobPages[page].name.size()), kKnobPages[page].name.data());
    m.head[0].right.set(buf);
    std::snprintf(buf, sizeof(buf), "%s %d BPM", playing ? "PLAY" : "STOP", static_cast<int>(std::lround(bpm)));
    m.head[1].left.set(buf);
    m.head[1].right.set("MAP SPAN");
    for (int k = 0; k < kKnobs; ++k) {
        const auto& p = kKnobPages[page].params[k];
        auto& s = m.knobs[k];
        s.label.set(p.label);
        s.bipolar = p.bipolar;
        s.norm = values[k];
        format_value(values[k], p.bipolar, buf, sizeof(buf));
        s.value.set(buf);
    }
    m.touched = touched;
    m.beat_phase = beat_phase;
    m.shift = shift;
    return m;
}

// 32 library items for the browser.
inline constexpr int kLibraryCount = 32;
inline void library_item(int i, char* name, size_t nn, char* tag, size_t tn) {
    static constexpr std::array<std::string_view, 8> kNames = {"NEON TUNNEL", "INK BLOOM", "CITY RUSH", "LIQUID GOLD",
                                                               "STATIC RAIN", "ORBIT", "PAPER BIRDS", "DEEP SEA"};
    std::snprintf(name, nn, "%02d %.*s", i + 1, static_cast<int>(kNames[i % 8].size()), kNames[i % 8].data());
    std::snprintf(tag, tn, "%d:%02d", 0 + (i * 7) % 3, (i * 13) % 60);
}

inline ui::ListModel browser(int selected) {
    ui::ListModel m;
    m.head.left.set("LIBRARY  BANK 1");
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d/%d", selected + 1, kLibraryCount);
    m.head.right.set(buf);
    m.count = kLibraryCount;
    m.selected = selected;
    for (int i = 0; i < kLibraryCount; ++i) {
        char n[40], t[10];
        library_item(i, n, sizeof(n), t, sizeof(t));
        m.items[i].set(n);
        m.tags[i].set(t);
    }
    return m;
}

// Procedural 96x54 grey "thumbnail" so the dither has something to chew on.
inline void thumbnail(int seed, std::array<uint8_t, 96 * 54>& g) {
    for (int y = 0; y < 54; ++y)
        for (int x = 0; x < 96; ++x) {
            const float cx = x - 48.0f - 10.0f * std::sin(seed * 0.7f), cy = y - 27.0f;
            const float r = std::sqrt(cx * cx + cy * cy);
            const float v = 0.5f + 0.5f * std::cos(r * 0.35f - seed) * std::exp(-r / 40.0f) + x / 400.0f;
            g[y * 96 + x] = static_cast<uint8_t>(std::fmin(255.0f, std::fmax(0.0f, v * 255.0f)));
        }
}

inline ui::DetailModel detail(int selected, const std::array<uint8_t, 96 * 54>* thumb) {
    ui::DetailModel m;
    char n[40], t[10];
    library_item(selected, n, sizeof(n), t, sizeof(t));
    m.head.left.set("CLIP");
    m.head.right.set("PUSH = LOAD");
    m.lines[0].set(n + 3);
    m.lines[1].set(std::string_view("HAP Q  1920X1080"));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "LENGTH %s", t);
    m.lines[2].set(buf);
    std::snprintf(buf, sizeof(buf), "BPM %d  MOOD %s", 90 + (selected * 9) % 50, selected % 3 ? "TECHNO" : "LOUNGE");
    m.lines[3].set(buf);
    m.lines[4].set(selected % 4 == 0 ? "LOOP  * FAVOURITE" : "LOOP");
    if (thumb) {
        m.thumb = thumb->data();
        m.thumb_w = 96;
        m.thumb_h = 54;
    }
    return m;
}

inline ui::ListModel system_menu(const std::array<std::string_view, 8>& items, int count, int selected) {
    ui::ListModel m;
    m.head.left.set("HIMMEL MENU");
    m.head.right.set("PUSH = OK");
    m.count = count;
    m.selected = selected;
    for (int i = 0; i < count; ++i) m.items[i].set(items[i]);
    return m;
}

}  // namespace himmel::mk2::samples
