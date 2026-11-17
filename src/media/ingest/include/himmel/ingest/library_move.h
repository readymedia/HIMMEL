// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Moving the library folder to another disk (docs/interfaces/library-db.md §11.5):
// what to copy, the copy with progress and cancel, and deleting the old copy later.
// The originals/ watch folder is never copied, moved or deleted (originals are the
// user's files); the move adds the old one to the watch list instead.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace himmel::ingest {

namespace fs = std::filesystem;

// The library's own folders (derived files), relative to the root.
inline constexpr const char* kDerivedDirs[] = {"hap", "stills", "meta", "thumbs"};

struct MovePlan {
    std::vector<fs::path> files;  // relative to the root
    uint64_t bytes = 0;
};
// hap/ stills/ meta/ thumbs/ (recursively), the DB file (no -wal / -shm: checkpoint first) and ingest.log.
MovePlan plan_move(const fs::path& root);

// Bytes the library uses: hap/ stills/ meta/ thumbs/ and the DB files.
uint64_t library_bytes(const fs::path& root);

// True when `dir` is missing or has no entries.
bool empty_or_missing(const fs::path& dir);

// Copies every planned file from `from` to `to` (same relative paths), then compares sizes.
// `progress(done_bytes, total_bytes)` returns false to cancel. On any failure the copied
// files are deleted again and `err` says why; `from` is never changed.
bool copy_library(const fs::path& from, const fs::path& to, const MovePlan& plan,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& err);

// Deletes the old copy's derived folders, DB files and work/ (never originals/, never the
// redirect). Returns how many files could not be deleted (in use); 0 = done.
int delete_old_library(const fs::path& old_root);

}  // namespace himmel::ingest
