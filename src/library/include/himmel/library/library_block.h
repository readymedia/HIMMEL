// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The library block "Local\HimmelLibrary" (docs/interfaces/library-db.md §6).
// Written by himmel_ingest (it creates the block, header last); read by the
// supervisor (heartbeat / stop), the engine's library feed (ready_seq) and the
// GUI bridge (import queue). Fixed-size POD, no heap, lock-free atomics.
#pragma once

#include <atomic>
#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::library {

inline constexpr const wchar_t* kLibraryShmName = L"Local\\HimmelLibrary";
inline constexpr uint32_t kLibraryMagic = 0x424C4D48;  // "HMLB"
inline constexpr uint32_t kLibraryVersion = 1;

enum class IngestMode : uint32_t { Idle = 0, Full = 1, Background = 2, High = 3 };

struct IngestJob {
    char name[64];          // UTF-8 display name of the file being worked on ("" = none)
    char stage[16];         // probing | converting | analysing | thumbs
    float progress;         // 0..1 of the current stage
    uint32_t reserved;
    char last_error[128];   // last failure, one line
};

struct LibraryBlock {
    protocol::BlockHeader header;
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> stop_request;     // supervisor -> ingest: 1 = exit soon
    std::atomic<uint64_t> heartbeat_us;     // steady clock (QueryPerformanceCounter us)
    std::atomic<uint64_t> ready_seq;        // bumps when a clip turns ready (or a ready row changes)
    std::atomic<uint32_t> mode;             // IngestMode
    std::atomic<uint32_t> paused;           // 1 while throttled
    std::atomic<uint32_t> queued;
    std::atomic<uint32_t> working;
    std::atomic<uint32_t> ready_total;
    std::atomic<uint32_t> failed_total;
    std::atomic<uint32_t> pauses;           // how many times the throttle paused (total)
    std::atomic<uint32_t> job_seq;
    IngestJob job;                          // seqlock: job_seq
    // Written by the renderer's library feed (the only field it writes): ready clips that are
    // not in the catalog because it is full (kMaxClips). The GUI shows "catalog full".
    std::atomic<uint32_t> catalog_full;
};

}  // namespace himmel::library
