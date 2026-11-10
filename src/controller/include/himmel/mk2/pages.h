// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Maschine MK2 screen pages: the real Himmel layouts for the two 256x64 1-bit
// screens, drawn as one 512x64 canvas.
//
// Pure drawing code: no Windows calls, no heap allocation, no locks. The app
// (engine or GUI) fills a small model struct, calls draw_*(), splits the canvas
// and writes the two screens through the shared block (seq_write_bytes). The
// controller only sends the strips that changed.
//
// Layout rules (CLAUDE.md, docs/mk2-screen-fun.md):
//   * a 9-row inverted header per screen: page name left, state right;
//   * knob labels/values sit over the PHYSICAL knob centres (kKnobX), which are
//     not on an even 64-px grid; each knob owns the column between the
//     midpoints to its neighbours (kKnobColumn);
//   * the touched knob / selected item is inverted;
//   * meters are simple bars with a peak tick.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "himmel/mk2/output.h"
#include "himmel/mk2/types.h"

namespace himmel::mk2::ui {

using Screen = Canvas<2 * kScreenW>;  // 512 x 64, both screens

inline constexpr int kHeaderH = 9;
inline constexpr int kRowH = 9;
inline constexpr int kListRows = (kScreenH - kHeaderH - 1) / kRowH;  // 6

// Knob centres on the 512-wide canvas (from a photo, 2026-11-09). Check them on
// the device with draw_knob_align() and adjust here if a line is off.
inline constexpr std::array<int, kKnobs> kKnobX = {10, 87, 163, 240, 266, 343, 419, 496};

struct Column {
    int x0, x1;  // [x0, x1) on the 512 canvas
    constexpr int width() const { return x1 - x0; }
};
// The column a knob owns: from the midpoint to its left neighbour to the
// midpoint to its right neighbour, never crossing the screen edge.
constexpr Column kKnobColumn(int k) {
    const int side = k / 4, first = side * 4, edge0 = side * kScreenW, edge1 = edge0 + kScreenW;
    const int x0 = k == first ? edge0 : (kKnobX[k - 1] + kKnobX[k] + 1) / 2;
    const int x1 = k == first + 3 ? edge1 : (kKnobX[k] + kKnobX[k + 1] + 1) / 2;
    return {x0, x1};
}

// Fixed-size text so models are trivially copyable (shared memory later) and
// filling them never allocates.
template <size_t N>
struct Str {
    char s[N]{};
    constexpr Str() = default;
    constexpr Str(std::string_view v) { set(v); }
    constexpr void set(std::string_view v) {
        size_t n = v.size() < N - 1 ? v.size() : N - 1;
        for (size_t i = 0; i < n; ++i) s[i] = v[i];
        s[n] = 0;
    }
    constexpr std::string_view view() const {
        size_t n = 0;
        while (n < N && s[n]) ++n;
        return {s, n};
    }
    constexpr bool empty() const { return s[0] == 0; }
};

struct Header {
    Str<44> left;   // page / deck name
    Str<24> right;  // state (BPM, mode, page number ...)
};

// ---------------------------------------------------------------- text helpers
enum class Align { Left, Center, Right };
constexpr int text_width(std::string_view s, int scale = 1) {
    return s.empty() ? 0 : static_cast<int>(s.size()) * kGlyphW * scale - scale;  // no trailing gap
}
// Draws s inside [x0, x1), aligned, cut to fit (whole characters). Returns the
// x where the text starts (or x0 if nothing fits).
int text_in(Screen& c, int x0, int x1, int y, std::string_view s, Align a = Align::Left, bool on = true, int scale = 1);
// Draws s centred over anchor_x, but kept inside [x0, x1).
int text_anchored(Screen& c, int x0, int x1, int y, int anchor_x, std::string_view s, bool on = true, int scale = 1);

// ---------------------------------------------------------------- building blocks
// side 0 = left screen, 1 = right. beat_phase >= 0 (0..4 = position in a 4/4
// bar) draws four beat boxes at the right end of the header.
void draw_header(Screen& c, int side, const Header& h, float beat_phase = -1.0f);
// Horizontal meter: level and peak 0..1 (map dB to 0..1 before calling).
void draw_meter_h(Screen& c, int x, int y, int w, int h, float level, float peak = -1.0f);
// Vertical meter, fills from the bottom.
void draw_meter_v(Screen& c, int x, int y, int w, int h, float level, float peak = -1.0f);
// Ordered (Bayer 4x4) dither of an 8-bit grey image into the canvas.
void blit_dithered(Screen& c, int x, int y, int w, int h, const uint8_t* grey, int stride);
// Clears one screen (side 0/1).
void clear_side(Screen& c, int side);

// ---------------------------------------------------------------- pages
// Label room (5x7 font, 6 px per character): knobs 1 and 5 sit near the left
// screen edges and fit 8 characters; knobs 2-4 and 6-8 fit 9 or more. Longer
// labels are cut, so keep parameter short names within that.
struct KnobSlot {
    Str<13> label;        // "FEEDBACK"; empty = unused knob
    Str<10> value;        // "82%", "1/16", "+12", "ON"
    float norm = 0.0f;    // 0..1 bar fill
    bool bipolar = false; // bar fills from the middle (pan, drift, pitch)
    bool automated = false;
};

// Perform / Control view: deck + page on the left header, transport on the
// right header, the 8 knobs of the current page over their physical knobs.
struct PerformModel {
    Header head[2];
    std::array<KnobSlot, kKnobs> knobs{};
    int touched = -1;          // knob to invert (recently turned), -1 none
    float beat_phase = -1.0f;  // right header beat boxes, < 0 hides
    bool shift = false;        // Shift held: shows SHIFT in the left header
};
void draw_perform(Screen& c, const PerformModel& m);

// Scrolling list on one screen (browser, system menu). The selected row is
// inverted and always visible; a scroll bar appears when the list is longer
// than the screen.
struct ListModel {
    static constexpr int kMax = 48;
    Header head;
    std::array<Str<40>, kMax> items{};
    std::array<Str<10>, kMax> tags{};  // right-aligned per row ("SAVED", "4:12", ">")
    int count = 0;
    int selected = 0;
};
void draw_list(Screen& c, int side, const ListModel& m);
// First visible row for a list (shared by drawing and tests).
int list_first_row(int count, int selected);

// Detail panel for the selected browser item: a few text lines and an optional
// dithered thumbnail on the right.
struct DetailModel {
    Header head;
    std::array<Str<26>, 5> lines{};
    const uint8_t* thumb = nullptr;  // 8-bit grey, thumb_w x thumb_h, may be null
    int thumb_w = 0, thumb_h = 0;
};
void draw_detail(Screen& c, int side, const DetailModel& m);

// Audio view: L/R level meters + BPM on the left, spectrum on the right.
struct AudioModel {
    Header head[2];
    float level[2] = {0, 0};  // 0..1
    float peak[2] = {0, 0};
    std::array<float, 32> bands{};  // 0..1
    int band_count = 32;            // 8, 16 or 32
    float bpm = 0.0f;               // 0 = unknown
    float beat_phase = -1.0f;
    bool beat_lock = false;
};
void draw_audio(Screen& c, const AudioModel& m);

// Big message on one screen (Tempo latch, "SCENE SAVED", errors).
void draw_message(Screen& c, int side, std::string_view title, std::string_view big, std::string_view subtitle = {});

// Calibration: a vertical line over each knob centre and the column bounds.
// Turn a knob: if its line is not right above it, fix kKnobX.
void draw_knob_align(Screen& c, int highlight = -1);

}  // namespace himmel::mk2::ui
