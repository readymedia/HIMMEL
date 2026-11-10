// Maschine MK2 controller: LED and screen output reports (spec 3), plus a
// small 1-bit canvas with the 5x7 font (port of tools/mk2_screens.py).
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "himmel/mk2/map.h"
#include "himmel/mk2/types.h"

namespace himmel::mk2 {

inline constexpr int kScreenW = 256;
inline constexpr int kScreenH = 64;
inline constexpr int kScreenBytes = kScreenW * kScreenH / 8;  // 2048
inline constexpr int kStrips = 8;
inline constexpr int kStripReportSize = 265;
inline constexpr int kButtonLeds = 31;

// What the app wants lit. Values 0..127 (higher is clamped).
struct LedState {
    uint8_t pad_rgb[kPads][3];      // by pad number - 1 (Pad 1 = bottom-left)
    uint8_t group_rgb[8][2][3];     // Group A..H, left/right LED, RGB
    uint8_t transport[8];           // Restart, Transport <, Transport >, Grid, Play, Rec, Erase, Shift
    uint8_t buttons[kButtonLeds];   // order: kButtonLedNames
};

inline constexpr std::array<std::string_view, kButtonLeds> kButtonLedNames = {
    "Control", "Step", "Browse", "Sampling", "Page <", "Page >", "All", "Auto Write",
    "Top 1", "Top 2", "Top 3", "Top 4", "Top 5", "Top 6", "Top 7", "Top 8",
    "Scene", "Pattern", "Pad Mode", "Navigate", "Duplicate", "Select", "Solo", "Mute",
    "Volume", "Swing", "Tempo", "Master <", "Master >", "Enter", "Note Repeat",
};

// Where a button's LED lives: kind 0 = none, 1 = button report index, 2 = transport index,
// 3 = group index (both RGB LEDs).
struct LedSlot {
    uint8_t kind;
    uint8_t index;
};
LedSlot led_of(Button b);

void build_pad_led_report(const LedState& s, const Map& map, std::array<uint8_t, 49>& out);
void build_group_led_report(const LedState& s, std::array<uint8_t, 57>& out);
void build_button_led_report(const LedState& s, std::array<uint8_t, 32>& out);

// side 0 = left (0xE0), 1 = right (0xE1). screen = 2048 bytes, MSB-left rows, 1 = lit.
void build_strip_report(int side, int strip, const uint8_t* screen, std::array<uint8_t, kStripReportSize>& out);

// 1-bit canvas (MSB-left rows). Default one screen; 512 wide spans both.
template <int W, int H = kScreenH>
struct Canvas {
    std::array<uint8_t, W * H / 8> buf{};
    void clear(bool on = false) { buf.fill(on ? 0xFF : 0x00); }
    void pixel(int x, int y, bool on = true) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t& b = buf[y * (W / 8) + (x >> 3)];
        const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
        b = on ? static_cast<uint8_t>(b | m) : static_cast<uint8_t>(b & ~m);
    }
    void fill(int x, int y, int w, int h, bool on = true) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) pixel(xx, yy, on);
    }
    void rect(int x, int y, int w, int h, bool on = true) {
        fill(x, y, w, 1, on);
        fill(x, y + h - 1, w, 1, on);
        fill(x, y, 1, h, on);
        fill(x + w - 1, y, 1, h, on);
    }
    int text(int x, int y, std::string_view s, bool on = true, int scale = 1);
    // Split a 512-wide canvas into the two screens.
    void split(uint8_t* left, uint8_t* right) const requires(W == 2 * kScreenW) {
        for (int r = 0; r < H; ++r)
            for (int i = 0; i < 32; ++i) {
                left[r * 32 + i] = buf[r * 64 + i];
                right[r * 32 + i] = buf[r * 64 + 32 + i];
            }
    }
};

// 5 columns per glyph, bit 0 = top row. Returns nullptr for unknown characters.
const uint8_t* glyph(char c);
inline constexpr int kGlyphW = 6;

template <int W, int H>
int Canvas<W, H>::text(int x, int y, std::string_view s, bool on, int scale) {
    for (char c : s) {
        const uint8_t* g = glyph(c);
        if (!g) g = glyph('?');
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 7; ++cy)
                if ((g[cx] >> cy) & 1) fill(x + cx * scale, y + cy * scale, scale, scale, on);
        x += kGlyphW * scale;
    }
    return x;
}

}  // namespace himmel::mk2
