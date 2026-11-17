// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The outputs block: engine -> GUI / weapons / supervisor (docs/interfaces/outputs-block.md, M6).
//
// Shared block "Local\HimmelOutputs". Written only by the render process. The
// supervisor keeps it open, so it survives a renderer restart. It is read:
//   * by the supervisor, to put its black curtains where the output windows are;
//   * by the GUI and the weapons, for "OUT 2 lost", IDENTIFY and the layer places;
//   * by the next renderer, to bring back runtime output settings (black, places,
//     presets) and the display identities it learned.
// One seqlock section, written on change and every 250 ms. Readers copy it with seq_read.
// The commands that change it (types 70..73) are in commands.h.
#pragma once

#include <atomic>
#include <cstdint>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/shm.h"

namespace himmel::protocol {

inline constexpr const wchar_t* kOutputsShmName = L"Local\\HimmelOutputs";
inline constexpr uint32_t kOutputsMagic = 0x54554F48;  // "HOUT"
inline constexpr uint32_t kOutputsVersion = 1;

inline constexpr int kMaxOutputs = 8;           // output slots (D150: 1..3 as one canvas in 0.1)
inline constexpr int kPlaceSlots = 9;           // 8 deck layers (deck * 4 + layer) + the HIT layer
inline constexpr int kDisplayNameLen = 48;
inline constexpr uint32_t kIdentifyMs = 5000;   // operations.md §2: 5 s, 0.5 s fade in / out

// Where an output is.
enum class OutputState : uint8_t {
    Off = 0,       // slot not used
    Ok = 1,        // full screen on its display
    Lost = 2,      // its display is gone: its part of the canvas is still rendered, nothing is shown
    Window = 3,    // a window (windowed / test mode: no display identity)
    Other = 4,     // its display is gone and a different one is free: "Use the new display as OUT n?" (OutputConfig Adopt)
    Count
};

// Output resolution presets (projects.md §5). Custom = the --size / window size (Advanced).
enum class ResolutionPreset : uint8_t { P720 = 0, P1080 = 1, P1440 = 2, P2160 = 3, Custom = 4, Count };
// Output refresh (operations.md §3). The same for every output of one canvas.
enum class RefreshRate : uint8_t { Hz60 = 0, Hz50 = 1, Hz59_94 = 2, Hz30 = 3, Hz25 = 4, Count };
enum class CanvasMode : uint8_t { Span = 0, Mirror = 1, Count };
// Layer places (engine.md §2.0). Computed when the layer or the canvas changes, never per frame.
enum class Place : uint8_t {
    FillAll = 0,   // cover the whole canvas (crop, keep aspect) - the default
    Out = 1,       // cover one output's part of the canvas (LayerPlace.param = output 0..7)
    Fit = 2,       // the whole clip inside the canvas (letterbox, transparent bars)
    Free = 3,      // f[0..3] = x, y, w, h in canvas 0..1 (the clip covers that rect)
    Count
};
// OutputConfig.param.
enum class OutputParam : uint8_t {
    Resolution = 0,  // slot = output (kNone = all); i = ResolutionPreset. Rebuilds the canvas (a setup action, one hitch)
    Refresh = 1,     // i = RefreshRate (fullscreen: the display mode is switched by a worker; windowed: noted only)
    BlendWidth = 2,  // i = overlap in canvas pixels between neighbours, 0..1024 (0 = screens butt). Rebuilds the canvas
    BlendGamma = 3,  // f[0] 1..3 (2.2 default)
    Mode = 4,        // i = CanvasMode. Rebuilds the canvas
    Adopt = 5,       // slot = output in state Other: take the new display (i = 1), or keep waiting (i = 0)
    SimLost = 6,     // slot = output; i = 1 act as if its display was pulled, 0 = it came back (tests on one screen)
    FrameSmooth = 7, // i = FrameSmooth (state.h): the global frame smoothing for clips (wave 7); shown in `frame_smooth`
    Count
};

// A display, known by identity (EDID maker / product / serial) plus the connector.
struct DisplayId {
    char maker[4];            // EDID PNP id, e.g. "DEL", NUL-terminated ("" = unknown)
    uint16_t product;         // EDID product code
    uint16_t reserved;
    uint32_t serial;          // EDID serial number (0 = none)
    char serial_text[16];     // EDID serial descriptor (0xFF), "" = none
    uint32_t connector;       // connector instance (DISPLAYCONFIG target)
    uint32_t technology;      // DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY
};

struct OutputInfo {
    DisplayId id;             // the display this slot belongs to (learned on the first full-screen start)
    char name[kDisplayNameLen]; // monitor name ("" in windowed mode)
    uint8_t state;            // OutputState
    uint8_t black;            // 1 = this output is black (OutputBlack)
    uint8_t preset;           // ResolutionPreset
    uint8_t identify;         // 1 while IDENTIFY shows on it
    uint32_t width, height;   // its part of the canvas (the preset size)
    int32_t canvas_x;         // left edge of its part, canvas pixels (blend overlap included)
    uint32_t swap_w, swap_h;  // its swap chain (display or window client size)
    float hz;                 // the display's refresh (0 = unknown / windowed)
    int32_t rect[4];          // the output window on the desktop: left, top, right, bottom (screen pixels)
    uint64_t hwnd;            // the output window (the supervisor puts its curtain just below it)
    uint32_t lost_count;      // times its display went away
    uint32_t returned_ms;     // how long the last return took (lost -> on screen), ms
    float blend_left, blend_right;  // edge-blend ramp widths, canvas pixels
};

struct LayerPlaceInfo {
    uint8_t place;            // Place
    uint8_t out;              // Place::Out: the output
    uint8_t reserved[2];
    float rect[4];            // the place on the canvas, 0..1: x, y, w, h (as computed)
};

struct OutputsSection {
    uint64_t time_us;         // engine clock at this write
    uint32_t epoch;           // the renderer's state epoch
    uint32_t count;           // output slots in use (1..kMaxOutputs)
    uint8_t mode;             // CanvasMode
    uint8_t refresh;          // RefreshRate (asked for)
    uint8_t fullscreen;       // 1 = full-screen outputs on displays, 0 = windows
    uint8_t identify;         // 1 while IDENTIFY runs
    uint32_t canvas_w, canvas_h;
    uint32_t blend_px;        // overlap between neighbours, canvas pixels
    float blend_gamma;
    float hz_target;          // the refresh asked for, Hz
    float hz_measured;        // the frame rate measured over the last stats period
    uint32_t identify_ms_left;
    uint32_t displays;        // displays Windows reports now
    uint8_t config_by_command; // 1 = preset / mode / blend / refresh were changed by a command (kept over a restart)
    uint8_t frame_smooth;     // wave 7: the global FrameSmooth (state.h); additive (was reserved[0])
    uint8_t reserved[2];
    OutputInfo out[kMaxOutputs];
    LayerPlaceInfo layer[kPlaceSlots];
};

struct OutputsStatus {
    std::atomic<uint64_t> heartbeat_us;   // renderer clock at the last write
    std::atomic<uint32_t> pid;            // the writing renderer (0 = none)
    std::atomic<uint32_t> reserved;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct OutputsBlock {
    BlockHeader header;                   // kOutputsMagic, kOutputsVersion, sizeof(OutputsBlock)
    OutputsStatus status;
    alignas(64) std::atomic<uint32_t> seq;
    OutputsSection s;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Canvas size of a preset (Custom: 0 x 0, the caller decides).
inline void preset_size(ResolutionPreset p, uint32_t& w, uint32_t& h) {
    switch (p) {
        case ResolutionPreset::P720: w = 1280, h = 720; return;
        case ResolutionPreset::P1080: w = 1920, h = 1080; return;
        case ResolutionPreset::P1440: w = 2560, h = 1440; return;
        case ResolutionPreset::P2160: w = 3840, h = 2160; return;
        default: w = 0, h = 0; return;
    }
}
inline float refresh_hz(RefreshRate r) {
    switch (r) {
        case RefreshRate::Hz50: return 50.0f;
        case RefreshRate::Hz59_94: return 59.94f;
        case RefreshRate::Hz30: return 30.0f;
        case RefreshRate::Hz25: return 25.0f;
        default: return 60.0f;
    }
}

}  // namespace himmel::protocol
