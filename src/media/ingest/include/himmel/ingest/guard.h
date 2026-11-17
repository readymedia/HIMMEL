// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The import disk guard, the output-size / frame-rate options, the progress and
// ETA maths, and plain-words errors (docs/interfaces/library-db.md §11). Pure
// functions plus one Windows call (disk_space), so the tests can fake the disk.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "himmel/library/record.h"

namespace himmel::ingest {

namespace fs = std::filesystem;

inline constexpr uint64_t kGB = 1000ull * 1000ull * 1000ull;
inline constexpr int64_t kSideBytes = 64 * 1024;  // thumbnails + strip + sidecar per clip

struct DiskSpace {
    uint64_t free = 0, total = 0;
    bool known = false;
};
// Free / total bytes of the disk that holds `p` (the nearest existing parent folder).
DiskSpace disk_space(const fs::path& p);

// The reserve the guard keeps free: max(10 GB, 10 % of the disk), or `override_bytes` when > 0.
uint64_t reserve_for(uint64_t total, uint64_t override_bytes = 0);

// What a clip becomes after the options (sizes rounded down to a multiple of 4, never up).
struct OutputShape {
    int w = 0, h = 0;
    double fps = 0;
    int64_t frames = 0;
    bool conform = false;  // the frame rate is changed (blend) for conform_hz
    std::string codec;     // hap | hap_q | hap_alpha
};
OutputShape output_shape(library::Kind kind, const library::Probe& probe, const library::ImportOptions& o,
                         const std::string& default_codec, int canvas_w, int canvas_h, double seq_fps = 25.0);

// The frame rate a conform to `hz` makes from `fps`: 0 = no conform needed (already a whole
// number of refreshes per frame, a still, or conform off).
double conform_fps(double fps, int hz);

// Estimated library bytes of one clip (HAP 0.5, HAP Q / HAP Alpha 1 byte per pixel per frame).
int64_t estimate_bytes(const OutputShape& s, library::Kind kind);
double bytes_per_pixel(const std::string& codec);

// "17.2 GB", "640 MB".
std::string human_bytes(uint64_t b);

// Does the import fit: free - queue - need >= reserve?
inline bool fits(uint64_t free, uint64_t queue, uint64_t need, uint64_t reserve) {
    return free >= reserve && free - reserve >= queue && free - reserve - queue >= need;
}

// ffmpeg / tool error tails -> one sentence for the operator. `stage` = probe | still | convert | ...
std::string plain_error(const std::string& stage, const std::string& tech);
bool is_disk_full(const std::string& tech);

// Batch progress and ETA. Work is counted in estimated bytes (pixels x frames x codec).
class BatchProgress {
public:
    void add(uint32_t files) { total_ += files; }
    // A file finished (ready, failed or cleared); `work` its estimated bytes, `seconds` the time it took.
    void file_done(int64_t work, double seconds, int64_t written);
    void reset() { *this = BatchProgress{}; }
    // -1 until a file with work > 0 has finished; else (remaining work) / (speed so far).
    int32_t eta_s(int64_t remaining_work) const;
    uint32_t done() const { return done_; }
    uint32_t total() const { return total_ < done_ ? done_ : total_; }
    uint64_t written() const { return written_; }

private:
    uint32_t done_ = 0, total_ = 0;
    double work_ = 0, seconds_ = 0;
    uint64_t written_ = 0;
};

// The share of a file's whole import that is finished when `stage` is at `p` (0..1):
// probe 2 %, convert 80 %, thumbnails 3 %, analysis 15 %.
float file_progress(const char* stage, float p);

}  // namespace himmel::ingest
