// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Analysis v1 (design/library-and-analysis.md §3) on a small RGB proxy of the
// clip (decoded by ffmpeg in the ingest worker, e.g. 64 x 36 at 12.5 fps).
// Pure CPU code, no I/O, no Windows: unit-tested in tests/library.
//
// Every threshold is in AnalysisRules, so a wrong tag can be traced to the
// rule that wrote it. Most defaults are GUESSES (see docs/agents/handoff/media.md)
// until they are tuned on the starter library.
#pragma once

#include <cstdint>
#include <vector>

#include "himmel/library/record.h"

namespace himmel::library {

struct ProxyClip {
    int w = 0, h = 0;
    float fps = 12.5f;             // proxy frame rate
    std::vector<uint8_t> rgb;      // frames * h * w * 3, row-major
    int frames() const { return w > 0 && h > 0 ? static_cast<int>(rgb.size() / (static_cast<size_t>(w) * h * 3)) : 0; }
    const uint8_t* frame(int i) const { return rgb.data() + static_cast<size_t>(i) * w * h * 3; }
    // The HAP file the proxy was made from (frame numbers in the result are its frames).
    double hap_fps = 25.0;
    int64_t hap_frames = 0;
};

struct AnalysisRules {
    // Cuts: histogram distance (0..1) above both of these.
    float cut_min = 0.35f;          // GUESS
    float cut_over_median = 3.0f;   // GUESS: x the local median distance
    // Motion amount: mean |dY| per 1/12.5 s, times this, clamped to 0..1.
    float motion_gain = 10.0f;      // GUESS
    int motion_search = 3;          // proxy pixels, each way
    // Loop: best in / out within this part of each end.
    float loop_search = 0.10f;
    float loopable_score = 0.80f;   // GUESS: best SSIM for "loopable"
    float crossfade_below = 0.90f;  // GUESS: best SSIM under this = "needs crossfade"
    int min_cuts_for_slices = 4;    // fewer cuts: equal 16ths
    // Flicker (photosensitivity, 3 flashes / s): a transition = |dY| >= this with the
    // darker frame under flash_dark_max.
    float flash_delta = 0.10f;
    float flash_dark_max = 0.80f;
    float flashes_bad = 3.0f;       // per second, any 1 s window
    float flashes_warn = 2.0f;      // GUESS
    float black_luma = 0.03f;
};

Analysis analyse(const ProxyClip& clip, const AnalysisRules& rules = {});

// ---- pieces, public for the tests ----
void luma_of(const uint8_t* rgb, int w, int h, std::vector<float>& y);  // 0..1, Rec.709
float ssim(const std::vector<float>& a, const std::vector<float>& b, int w, int h);  // 8x8 windows, mean
std::vector<PaletteEntry> palette_kmeans(const ProxyClip& clip, int k = kPaletteSize);
float hist_distance(const uint8_t* a, const uint8_t* b, int pixels);  // 0..1 (4x4x4 RGB bins)
int pow2_round(double v);  // nearest power of two (>= 1)

}  // namespace himmel::library
