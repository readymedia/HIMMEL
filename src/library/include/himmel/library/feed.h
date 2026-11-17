// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The engine's library feed (docs/interfaces/library-db.md §8). Lives in the
// render process; its worker reads the library DB read-only and appends
// clips that turned READY to the ClipTable. The frame loop only watches
// ClipTable::size() rise and copies the new catalog entries (no I/O, no locks).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "himmel/library/clip_table.h"
#include "himmel/library/record.h"

namespace himmel::library {

// Documents\Himmel\Library (the default library folder).
std::filesystem::path default_library_root();

// The catalog entry of a ready library clip (name made unique against the
// first `n` entries of `table`).
protocol::ClipInfo clip_info_from(const ClipRecord& r, const ClipTable& table, uint32_t n);

class LibraryFeed {
public:
    LibraryFeed() = default;
    ~LibraryFeed() { stop(); }
    LibraryFeed(const LibraryFeed&) = delete;
    LibraryFeed& operator=(const LibraryFeed&) = delete;

    // Start-up (blocking, may do I/O): appends every ready clip now, so a
    // restarted renderer finds its library clips before it restores the show.
    int load_initial(const std::filesystem::path& root, ClipTable& table);
    // The worker: polls the DB every `poll_ms` and soon after the library
    // block's ready_seq changes. Appends new ready clips to the table.
    void start(int poll_ms = 1000);
    void stop();

    std::atomic<uint32_t> added{0};       // clips appended by the worker
    std::atomic<uint32_t> skipped{0};     // ready rows that could not be added (table full, file gone)
    std::atomic<uint32_t> polls{0};
    std::atomic<uint32_t> catalog_full{0};  // ready clips left out: the table (catalog) is full

private:
    int poll_once();
    void run(int poll_ms);

    std::filesystem::path root_;
    ClipTable* table_ = nullptr;
    int64_t last_order_ = 0;  // ready_order cursor
    std::thread worker_;
    std::atomic<bool> stop_{false};
    void* db_ = nullptr;  // LibraryDb*, worker-owned after start
};

}  // namespace himmel::library
