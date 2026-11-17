// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ingest/guard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace himmel::ingest {

using library::Kind;

DiskSpace disk_space(const fs::path& p) {
    DiskSpace d;
    std::error_code ec;
    fs::path q = fs::absolute(p, ec);
    if (ec) q = p;
    // The nearest folder that exists (the library may not be made yet).
    for (int i = 0; i < 64 && !q.empty() && !fs::exists(q, ec); ++i) {
        const fs::path parent = q.parent_path();
        if (parent == q) break;
        q = parent;
    }
    ULARGE_INTEGER avail{}, total{}, total_free{};
    if (GetDiskFreeSpaceExW(q.wstring().c_str(), &avail, &total, &total_free)) {
        d.free = avail.QuadPart;
        d.total = total.QuadPart;
        d.known = true;
    }
    return d;
}

uint64_t reserve_for(uint64_t total, uint64_t override_bytes) {
    if (override_bytes > 0) return override_bytes;
    return std::clamp<uint64_t>(total / 20, 2 * kGB, 20 * kGB);
}

double conform_fps(double fps, int hz) {
    if (hz <= 0 || fps <= 1.0) return 0;
    const double r = hz >= fps ? hz / fps : fps / hz;
    if (std::fabs(r - std::round(r)) <= 0.02) return 0;  // already a whole number of refreshes per frame
    return static_cast<double>(hz);
}

double bytes_per_pixel(const std::string& codec) { return codec == "hap" ? 0.5 : 1.0; }

OutputShape output_shape(Kind kind, const library::Probe& pr, const library::ImportOptions& o, const std::string& default_codec,
                         int canvas_w, int canvas_h, double seq_fps) {
    OutputShape s;
    double w = std::max(1, pr.width), h = std::max(1, pr.height);
    const bool still = kind == Kind::Still || kind == Kind::Vector;
    if (still) {  // fit inside 2 x the canvas (the PNG step)
        const double k = std::min({1.0, canvas_w * 2.0 / w, canvas_h * 2.0 / h});
        w *= k, h *= k;
    }
    if (o.max_h > 0 && h > o.max_h) {
        w = w * o.max_h / h;
        h = o.max_h;
    }
    s.w = std::max(4, static_cast<int>(w) / 4 * 4);
    s.h = std::max(4, static_cast<int>(h) / 4 * 4);
    const bool alpha = pr.alpha || kind == Kind::Animation || kind == Kind::Vector;
    s.codec = alpha ? "hap_alpha" : (o.codec.empty() ? default_codec : o.codec);
    if (still) {
        s.frames = 1;
        return s;
    }
    if (kind == Kind::Sequence) s.fps = seq_fps;
    else if (kind == Kind::Animation) s.fps = std::clamp(std::round(pr.fps > 0 ? pr.fps : 10.0), 1.0, 60.0);
    else s.fps = pr.fps > 0 ? pr.fps : 25.0;
    s.frames = pr.frames > 0 && kind != Kind::Animation ? pr.frames : std::llround(std::max(0.0, pr.seconds) * s.fps);
    if (s.frames <= 0) s.frames = std::max<int64_t>(1, pr.frames);
    if (const double cf = conform_fps(s.fps, o.conform_hz); cf > 0) {
        s.frames = std::max<int64_t>(1, std::llround(static_cast<double>(s.frames) * cf / s.fps));
        s.fps = cf;
        s.conform = true;
    }
    return s;
}

int64_t estimate_bytes(const OutputShape& s, Kind kind) {
    const double px = static_cast<double>(s.w) * s.h;
    double b = px * static_cast<double>(std::max<int64_t>(1, s.frames)) * bytes_per_pixel(s.codec) + kSideBytes;
    if (kind == Kind::Still || kind == Kind::Vector) b += px * 3;  // the kept PNG (upper bound)
    return static_cast<int64_t>(b);
}

std::string human_bytes(uint64_t b) {
    char t[32];
    if (b >= kGB) std::snprintf(t, sizeof(t), "%.1f GB", b / 1e9);
    else if (b >= 1000ull * 1000ull) std::snprintf(t, sizeof(t), "%.0f MB", b / 1e6);
    else std::snprintf(t, sizeof(t), "%.0f KB", b / 1e3);
    return t;
}

namespace {
bool has(const std::string& s, const char* needle) {
    std::string a = s, n = needle;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(n) != std::string::npos;
}
}  // namespace

bool is_disk_full(const std::string& t) {
    return has(t, "no space left") || has(t, "not enough space") || has(t, "disk full") || has(t, "disk is full") ||
           has(t, "enospc");
}

std::string plain_error(const std::string& stage, const std::string& t) {
    if (is_disk_full(t)) return "The disk is full.";
    if (has(t, "the original is gone") || has(t, "no such file")) return "The original file is gone (moved, renamed or deleted).";
    if (has(t, "permission denied") || has(t, "access is denied")) return "Himmel is not allowed to read this file (permission denied).";
    if (has(t, "cannot start")) return "ffmpeg could not be started. Is ffmpeg installed?";
    if (has(t, "no video stream") || has(t, "does not contain any stream") || has(t, "no picture size"))
        return "This file has no picture in it (audio only, or empty).";
    if (has(t, "decoder") && (has(t, "not found") || has(t, "unknown")))
        return "This file uses a video format ffmpeg cannot decode.";
    if (has(t, "invalid data") || has(t, "moov atom not found") || has(t, "end of file") || has(t, "corrupt") ||
        has(t, "not json") || has(t, "error while decoding"))
        return "This file is damaged or is not a video ffmpeg can read.";
    if (has(t, "no numbered images")) return "The folder has no numbered images any more.";
    if (has(t, "svg")) return "This SVG could not be drawn.";
    if (has(t, "not a hap clip")) return "The converted file could not be played back; it was not added.";
    if (has(t, "sidecar") || has(t, "cannot move") || has(t, "cannot copy"))
        return "Himmel could not write into the library folder.";
    if (has(t, "timed out") || has(t, "timeout")) return "The conversion took too long and was stopped.";
    return "The import failed while " + (stage.empty() ? std::string("importing") : stage) + ".";
}

void BatchProgress::file_done(int64_t work, double seconds, int64_t written) {
    ++done_;
    if (work > 0 && seconds > 0) {
        work_ += static_cast<double>(work);
        seconds_ += seconds;
    }
    if (written > 0) written_ += static_cast<uint64_t>(written);
}

int32_t BatchProgress::eta_s(int64_t remaining_work) const {
    if (work_ <= 0 || seconds_ <= 0) return -1;
    if (remaining_work <= 0) return 0;
    const double s = static_cast<double>(remaining_work) / (work_ / seconds_);
    return static_cast<int32_t>(std::min(s + 0.999, 2.0e9));
}

float file_progress(const char* stage, float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    if (!stage) return 0;
    if (!std::strcmp(stage, "probing")) return 0.02f * p;
    if (!std::strcmp(stage, "converting")) return 0.02f + 0.80f * p;
    if (!std::strcmp(stage, "thumbs")) return 0.82f + 0.03f * p;
    if (!std::strcmp(stage, "analysing")) return 0.85f + 0.15f * p;
    if (!std::strcmp(stage, "ready")) return 1.0f;
    return 0;
}

}  // namespace himmel::ingest
