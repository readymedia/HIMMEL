// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Watch-folder scan (docs/interfaces/library-db.md §2): what to import, and
// when a file has stopped changing. Filesystem only; no ffmpeg, no DB.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "himmel/library/record.h"

namespace himmel::ingest {

namespace fs = std::filesystem;

struct Candidate {
    library::Kind kind = library::Kind::Video;
    fs::path path;                  // the file, or the sequence folder
    std::string name;               // display name (stem / folder name)
    int64_t size = 0, mtime = 0;    // a sequence: total size, newest time
    // Image sequences only.
    std::vector<fs::path> seq_files;  // sorted by number
    std::string seq_pattern;        // ffmpeg pattern, e.g. "shot_%04d.png" (in `path`)
    int seq_start = 0;
};

// The kind for a file extension (lower case, with the dot); false = not imported.
bool kind_for_extension(const std::string& ext, library::Kind& kind);
bool is_sequence_image(const std::string& ext);

// Finds the numbered image run in one folder's file names (>= 2 files with the
// same prefix, digits and extension; the longest run wins). False when none.
bool find_sequence(const std::vector<fs::path>& files, Candidate& out);

// Every importable thing under `root` (recursive). A sub-folder that holds an
// image sequence becomes one Sequence candidate (its numbered images are not
// imported as stills). Folders in `skip` (and their children) are left out.
std::vector<Candidate> scan_folder(const fs::path& root, const std::vector<fs::path>& skip = {});

// One file, or one folder that holds an image sequence (its own files only), as a
// candidate. False when it is not importable.
bool candidate_from_path(const fs::path& p, Candidate& out);

// A file is "settled" once its size and time stamp did not change for `settle_ms`.
class SettleTracker {
public:
    explicit SettleTracker(int64_t settle_ms = 2000) : settle_ms_(settle_ms) {}
    // Returns true when this candidate is settled at `now_ms`.
    bool settled(const Candidate& c, int64_t now_ms);
    void forget(const fs::path& p) { seen_.erase(p.wstring()); }

private:
    struct Seen { int64_t size = 0, mtime = 0, since_ms = 0; bool known = false; };
    std::map<std::wstring, Seen> seen_;
    int64_t settle_ms_;
};

}  // namespace himmel::ingest
