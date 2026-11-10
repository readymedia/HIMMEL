// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Shared status block of the render process ("Local\HimmelEngine").
//
// The render process creates it and updates it every frame; the supervisor
// reads the heartbeat to detect a hung or dead renderer, and can ask it to stop.
// Plain atomics only, same rules as the MK2 block (docs/mk2-controller-spec.md).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace himmel::engine {

inline constexpr wchar_t kEngineShmName[] = L"Local\\HimmelEngine";
inline constexpr uint32_t kEngineMagic = 0x4C4D4948;  // "HIML"
inline constexpr uint32_t kEngineVersion = 1;

struct EngineStatus {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> stop_request;       // supervisor -> renderer: 1 = quit cleanly
    std::atomic<uint64_t> heartbeat_us;       // now_us() at the last frame (or idle tick)
    std::atomic<uint64_t> frames;
    std::atomic<uint32_t> outputs;
    std::atomic<uint32_t> fps_x100;           // over the last stats period
    std::atomic<uint32_t> frame_ms_avg_x100;
    std::atomic<uint32_t> frame_ms_max_x100;
    std::atomic<uint32_t> late_frames;        // frame interval > 1.5 x refresh, total
    std::atomic<uint32_t> mk2_connected;
    std::atomic<uint32_t> input_to_present_us; // avg: controller read -> Present() returned
    std::atomic<uint32_t> restarts_seen;      // written by the supervisor
    char message[128];
};

// Tiny named shared-memory mapping of one POD-ish struct.
class EngineShm {
public:
    EngineShm() = default;
    ~EngineShm();
    EngineShm(const EngineShm&) = delete;
    EngineShm& operator=(const EngineShm&) = delete;
    bool create();  // renderer
    bool open();    // supervisor / tools
    EngineStatus* get() const { return status_; }

private:
    void* mapping_ = nullptr;
    EngineStatus* status_ = nullptr;
};

}  // namespace himmel::engine
