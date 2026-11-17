// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The engine's append-only clip table (docs/interfaces/library-db.md §8): the
// playlist every clip player loads from, plus each clip's catalog entry.
//
// One writer at a time: the render process's main thread at start-up, then
// only the library feed worker. Entries [0, count) never change once
// published, so readers (the frame loop, the clip players' workers) read them
// without locks: load `count` (acquire), then use any index below it.
// The capacity is fixed, so a reader never sees storage move.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "himmel/protocol/state.h"

namespace himmel::library {

struct ClipTable {
    static constexpr int kCapacity = protocol::kMaxClips;
    std::string path[kCapacity];             // UTF-8 path of the playable HAP file
    protocol::ClipInfo info[kCapacity];      // the catalog entry
    int64_t library_id[kCapacity] = {};      // 0 = not from the library (media/ or --clip)
    std::atomic<uint32_t> count{0};

    uint32_t size() const { return count.load(std::memory_order_acquire); }
    // Writer only. Returns the new index, or -1 when full.
    int append(const std::string& p, const protocol::ClipInfo& ci, int64_t lib_id = 0) {
        const uint32_t n = count.load(std::memory_order_relaxed);
        if (n >= static_cast<uint32_t>(kCapacity)) return -1;
        path[n] = p;
        info[n] = ci;
        library_id[n] = lib_id;
        count.store(n + 1, std::memory_order_release);
        return static_cast<int>(n);
    }
};

}  // namespace himmel::library
