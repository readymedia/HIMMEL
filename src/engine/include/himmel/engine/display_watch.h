// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Display watcher (M6, design/operations.md §1, §3): a worker thread that knows
// which displays Windows has, by identity (EDID maker / product / serial from the
// registry, plus the connector), where they are on the desktop and their refresh.
//
// The render thread never calls Windows display APIs or the registry: it reads
// the newest snapshot with latest() (a seqlock copy of ~5 KB, only when the
// generation changed). The worker rescans every 500 ms and at once after poke()
// (WM_DISPLAYCHANGE). It also switches the refresh rate of the displays the
// outputs use (ChangeDisplaySettingsEx with CDS_FULLSCREEN: temporary, Windows
// puts the old mode back when the process exits), so the frame loop never blocks
// on a mode change.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "himmel/protocol/outputs.h"

namespace himmel::engine {

inline constexpr int kMaxDisplays = 16;

struct DisplayInfo {
    protocol::DisplayId id;
    char name[protocol::kDisplayNameLen];  // monitor friendly name (UTF-8)
    wchar_t gdi[32];                       // \\.\DISPLAYn (for mode changes)
    int32_t rect[4];                       // desktop rect: left, top, right, bottom (physical pixels)
    float hz;                              // current refresh
    uint8_t primary;
    uint8_t reserved[3];
};

struct DisplayList {
    uint32_t count = 0;
    DisplayInfo d[kMaxDisplays];
    int order[kMaxDisplays];  // indexes sorted left to right (then top to bottom)
};

class DisplayWatch {
public:
    DisplayWatch() = default;
    ~DisplayWatch() { stop(); }
    DisplayWatch(const DisplayWatch&) = delete;
    DisplayWatch& operator=(const DisplayWatch&) = delete;

    // Scans once on the calling thread (so the first list is ready), then starts the worker.
    void start();
    void stop();
    // Rescan as soon as possible (call on WM_DISPLAYCHANGE). Lock-free, any thread.
    void poke();
    // The newest list if its generation differs from `gen` (gen is updated). Never blocks;
    // false = nothing new (or the worker was writing: try next frame).
    bool latest(DisplayList& out, uint32_t& gen) const;
    // Ask the worker to put these displays (by gdi name, from a DisplayList) on `hz`
    // (0 = leave them). The worker keeps the size and only switches when a mode exists.
    void want_refresh(const DisplayList& on, const int* indexes, int n, float hz);
    // The last mode-change result, for the log (0 = none yet; worker-written).
    uint32_t mode_changes() const { return mode_changes_.load(); }
    uint32_t mode_failures() const { return mode_failures_.load(); }

    // One scan (Windows APIs + registry). Worker / start-up only.
    static void scan(DisplayList& out);

private:
    void run();
    void apply_refresh();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    void* wake_ = nullptr;  // auto-reset event
    // Published list (seqlock).
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> gen_{0};
    DisplayList pub_;
    // Refresh request (render thread -> worker): seqlock'd small struct.
    struct RefreshReq {
        float hz = 0;
        int n = 0;
        wchar_t gdi[protocol::kMaxOutputs][32] = {};
        uint32_t serial = 0;
    };
    std::atomic<uint32_t> req_seq_{0};
    RefreshReq req_;
    uint32_t req_done_ = 0;
    std::atomic<uint32_t> mode_changes_{0}, mode_failures_{0};
};

}  // namespace himmel::engine
