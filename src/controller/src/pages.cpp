// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Maschine MK2 screen pages (see pages.h).
#include "himmel/mk2/pages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace himmel::mk2::ui {

namespace {

constexpr uint8_t kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// How many whole characters of s fit in w pixels.
size_t chars_that_fit(std::string_view s, int w, int scale) {
    if (w <= 0) return 0;
    const size_t n = static_cast<size_t>((w + scale) / (kGlyphW * scale));
    return std::min(n, s.size());
}

}  // namespace

int text_in(Screen& c, int x0, int x1, int y, std::string_view s, Align a, bool on, int scale) {
    s = s.substr(0, chars_that_fit(s, x1 - x0, scale));
    const int w = text_width(s, scale);
    int x = x0;
    if (a == Align::Center) x = x0 + (x1 - x0 - w) / 2;
    if (a == Align::Right) x = x1 - w;
    c.text(x, y, s, on, scale);
    return x;
}

int text_anchored(Screen& c, int x0, int x1, int y, int anchor_x, std::string_view s, bool on, int scale) {
    s = s.substr(0, chars_that_fit(s, x1 - x0, scale));
    const int w = text_width(s, scale);
    const int x = std::clamp(anchor_x - w / 2, x0, std::max(x0, x1 - w));
    c.text(x, y, s, on, scale);
    return x;
}

void clear_side(Screen& c, int side) { c.fill(side * kScreenW, 0, kScreenW, kScreenH, false); }

void draw_header(Screen& c, int side, const Header& h, float beat_phase) {
    const int x0 = side * kScreenW, x1 = x0 + kScreenW;
    c.fill(x0, 0, kScreenW, kHeaderH);
    int right_end = x1 - 2;
    if (beat_phase >= 0.0f) {  // four boxes, the current beat filled
        const int beat = static_cast<int>(std::floor(beat_phase)) % 4;
        for (int i = 0; i < 4; ++i) {
            const int bx = x1 - 2 - (4 - i) * 7 + 1;
            c.rect(bx, 1, 6, 6, false);
            if (i == beat) c.fill(bx + 1, 2, 4, 4, false);
        }
        right_end = x1 - 2 - 4 * 7 - 3;
    }
    const std::string_view r = h.right.view();
    const int rw = text_width(r);
    int left_end = right_end;
    if (!r.empty()) {
        const int rx = text_in(c, std::max(x0 + 2, right_end - rw), right_end, 1, r, Align::Right, false);
        left_end = rx - 6;
    }
    text_in(c, x0 + 2, left_end, 1, h.left.view(), Align::Left, false);
}

void draw_meter_h(Screen& c, int x, int y, int w, int h, float level, float peak) {
    c.rect(x, y, w, h);
    const int inner = w - 4;
    c.fill(x + 2, y + 2, static_cast<int>(std::lround(inner * clamp01(level))), h - 4);
    if (peak >= 0.0f) {
        const int px = x + 2 + std::min(inner - 1, static_cast<int>(std::lround(inner * clamp01(peak))));
        c.fill(px, y + 1, 1, h - 2);
    }
}

void draw_meter_v(Screen& c, int x, int y, int w, int h, float level, float peak) {
    c.rect(x, y, w, h);
    const int inner = h - 4;
    const int fill = static_cast<int>(std::lround(inner * clamp01(level)));
    c.fill(x + 2, y + h - 2 - fill, w - 4, fill);
    if (peak >= 0.0f) {
        const int py = y + h - 3 - std::min(inner - 1, static_cast<int>(std::lround(inner * clamp01(peak))));
        c.fill(x + 1, py, w - 2, 1);
    }
}

void blit_dithered(Screen& c, int x, int y, int w, int h, const uint8_t* grey, int stride) {
    if (!grey) return;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const int X = x + xx, Y = y + yy;
            const int threshold = kBayer4[(Y & 3) * 4 + (X & 3)] * 16 + 8;
            c.pixel(X, Y, grey[yy * stride + xx] > threshold);
        }
}

// ---------------------------------------------------------------- perform
namespace {

void draw_knob_slot(Screen& c, int k, const KnobSlot& s, bool hot) {
    const Column col = kKnobColumn(k);
    const int ax = kKnobX[k];
    const int x0 = col.x0 + 2, x1 = col.x1 - 2;  // bar box
    // Text may use the full column: 1 px from a neighbour, 0 px at the screen edge
    // (knob 1 and 4 have narrow columns; FEEDBACK / RGB SPLIT must still fit).
    const int tx0 = col.x0 % kScreenW == 0 ? col.x0 : col.x0 + 1;
    const int tx1 = col.x1 % kScreenW == 0 ? col.x1 : col.x1 - 1;
    // A small tick right above the physical knob, so the eye can follow it down.
    c.fill(ax - 2, 62, 5, 1);
    c.fill(ax - 1, 61, 3, 1);
    c.pixel(ax, 60);
    if (!s.label.empty()) {
        text_anchored(c, tx0, tx1, 12, ax, s.label.view());
        const std::string_view v = s.value.view();
        const bool big = text_width(v, 2) <= tx1 - tx0;
        text_anchored(c, tx0, tx1, big ? 23 : 27, ax, v, true, big ? 2 : 1);
        if (s.automated) {  // inverted "A" tag under the value, right side
            c.fill(x1 - 7, 38, 7, 8);
            c.text(x1 - 6, 38, "A", false);
        }
        const int bw = x1 - x0, bx = x0, by = 47, bh = 9;
        c.rect(bx, by, bw, bh);
        const int inner = bw - 4;
        const float n = clamp01(s.norm);
        if (s.bipolar) {
            const int mid = bx + 2 + inner / 2;
            const int to = bx + 2 + static_cast<int>(std::lround(inner * n));
            c.fill(std::min(mid, to), by + 2, std::max(1, std::abs(to - mid)), bh - 4);
            c.fill(mid, by - 1, 1, 2);  // centre mark
        } else {
            c.fill(bx + 2, by + 2, static_cast<int>(std::lround(inner * n)), bh - 4);
        }
    }
    if (hot) c.invert(col.x0, kHeaderH + 1, col.width(), kScreenH - kHeaderH - 1);
}

}  // namespace

void draw_perform(Screen& c, const PerformModel& m) {
    c.clear();
    Header left = m.head[0];
    if (m.shift) left.right.set("SHIFT");
    draw_header(c, 0, left);
    draw_header(c, 1, m.head[1], m.beat_phase);
    for (int k = 0; k < kKnobs; ++k) draw_knob_slot(c, k, m.knobs[k], k == m.touched);
}

// ---------------------------------------------------------------- list / detail
int list_first_row(int count, int selected) {
    if (count <= kListRows) return 0;
    return std::clamp(selected - kListRows / 2, 0, count - kListRows);
}

void draw_list(Screen& c, int side, const ListModel& m) {
    clear_side(c, side);
    draw_header(c, side, m.head);
    const int x0 = side * kScreenW;
    const int count = std::clamp(m.count, 0, ListModel::kMax);
    if (count == 0) {
        text_in(c, x0, x0 + kScreenW, 32, "(EMPTY)", Align::Center);
        return;
    }
    const int sel = std::clamp(m.selected, 0, count - 1);
    const int first = list_first_row(count, sel);
    const bool bar = count > kListRows;
    const int text_x1 = x0 + kScreenW - (bar ? 6 : 2);
    for (int r = 0; r < kListRows && first + r < count; ++r) {
        const int i = first + r, y = kHeaderH + 1 + r * kRowH;
        const std::string_view tag = m.tags[i].view();
        int item_x1 = text_x1;
        if (!tag.empty()) item_x1 = text_in(c, text_x1 - text_width(tag), text_x1, y + 1, tag, Align::Right) - 6;
        text_in(c, x0 + 2, item_x1, y + 1, m.items[i].view());
        if (i == sel) c.invert(x0, y, text_x1 - x0 + 1, kRowH);
    }
    if (bar) {  // scroll bar: track + thumb
        const int tx = x0 + kScreenW - 3, ty = kHeaderH + 1, th = kScreenH - ty;
        for (int y = ty; y < ty + th; y += 2) c.pixel(tx + 1, y);
        const int thumb_h = std::max(4, th * kListRows / count);
        const int thumb_y = ty + (th - thumb_h) * first / (count - kListRows);
        c.fill(tx, thumb_y, 3, thumb_h);
    }
}

void draw_detail(Screen& c, int side, const DetailModel& m) {
    clear_side(c, side);
    draw_header(c, side, m.head);
    const int x0 = side * kScreenW;
    int text_x1 = x0 + kScreenW - 2;
    if (m.thumb && m.thumb_w > 0 && m.thumb_h > 0) {
        const int w = std::min(m.thumb_w, 96), h = std::min(m.thumb_h, kScreenH - kHeaderH - 1);
        const int tx = x0 + kScreenW - w, ty = kHeaderH + 1;
        blit_dithered(c, tx, ty, w, h, m.thumb, m.thumb_w);
        text_x1 = tx - 4;
    }
    for (int i = 0; i < static_cast<int>(m.lines.size()); ++i)
        text_in(c, x0 + 2, text_x1, kHeaderH + 2 + i * kRowH + (i ? 2 : 0), m.lines[i].view());
}

// ---------------------------------------------------------------- audio
void draw_audio(Screen& c, const AudioModel& m) {
    c.clear();
    draw_header(c, 0, m.head[0]);
    draw_header(c, 1, m.head[1], m.beat_phase);
    // Left: L/R meters with dB-ish ticks under them, then BPM.
    for (int ch = 0; ch < 2; ++ch) {
        const int y = 12 + ch * 11;
        c.text(2, y + 1, ch ? "R" : "L");
        draw_meter_h(c, 10, y, 244, 9, m.level[ch], m.peak[ch]);
    }
    for (int i = 0; i <= 10; ++i) c.fill(12 + i * 24, 33, 1, i % 5 ? 1 : 3);
    char bpm[16] = "BPM --";
    if (m.bpm > 0.0f) {
        const int tenths = static_cast<int>(std::lround(m.bpm * 10.0f));
        std::snprintf(bpm, sizeof(bpm), "BPM %d.%d", tenths / 10, tenths % 10);
    }
    c.text(2, 42, bpm, true, 2);
    if (m.bpm > 0.0f) text_in(c, 140, 254, 46, m.beat_lock ? "LOCKED" : "FREE", Align::Right);
    // Right: spectrum bars from the bottom.
    const int n = std::clamp(m.band_count, 1, 32);
    const int bw = kScreenW / n;
    const int top = kHeaderH + 2, h = kScreenH - top;
    for (int i = 0; i < n; ++i) {
        const int bh = static_cast<int>(std::lround(h * clamp01(m.bands[i])));
        c.fill(kScreenW + i * bw + 1, kScreenH - bh, std::max(1, bw - 2), bh);
    }
}

// ---------------------------------------------------------------- message / calibration
void draw_message(Screen& c, int side, std::string_view title, std::string_view big, std::string_view subtitle) {
    clear_side(c, side);
    Header h;
    h.left.set(title);
    draw_header(c, side, h);
    const int x0 = side * kScreenW, x1 = x0 + kScreenW;
    const int scale = text_width(big, 3) <= kScreenW - 8 ? 3 : (text_width(big, 2) <= kScreenW - 8 ? 2 : 1);
    const int y = subtitle.empty() ? 36 - 7 * scale / 2 : 30 - 7 * scale / 2;
    text_in(c, x0 + 4, x1 - 4, y, big, Align::Center, true, scale);
    if (!subtitle.empty()) text_in(c, x0 + 2, x1 - 2, 54, subtitle, Align::Center);
}

void draw_knob_align(Screen& c, int highlight) {
    c.clear();
    Header l, r;
    l.left.set("KNOB ALIGN");
    l.right.set("LINE = KNOB");
    r.left.set("TURN A KNOB");
    r.right.set("EDIT kKnobX");
    draw_header(c, 0, l);
    draw_header(c, 1, r);
    for (int k = 0; k < kKnobs; ++k) {
        const Column col = kKnobColumn(k);
        if (col.x0 % kScreenW) for (int y = kHeaderH + 1; y < kScreenH; y += 3) c.pixel(col.x0, y);  // column bounds, dotted
        c.fill(kKnobX[k], 22, 1, kScreenH - 22);
        char n[4] = {static_cast<char>('1' + k), 0, 0, 0};
        text_anchored(c, col.x0 + 1, col.x1 - 1, 12, kKnobX[k], n);
        if (k == highlight) c.invert(col.x0 + 1, kHeaderH + 1, col.width() - 1, kScreenH - kHeaderH - 1);
    }
}

}  // namespace himmel::mk2::ui
