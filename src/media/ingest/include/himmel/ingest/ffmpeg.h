// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// ffmpeg / ffprobe as EXTERNAL tools (never linked, decision 2026-11-10):
// finding them, building their command lines, running them at low priority
// with pause / resume / stop, and reading ffprobe's JSON.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "himmel/library/record.h"

namespace himmel::ingest {

namespace fs = std::filesystem;

struct Tools {
    fs::path ffmpeg, ffprobe;
    // `dir` = --ffmpeg DIR, else PATH. False when either is missing.
    bool find(const fs::path& dir = {});
};

// ---- command lines (UTF-8 arguments, pure, unit-tested) ----
struct ConvertSpec {
    library::Kind kind = library::Kind::Video;
    std::string input;             // file, or folder + "/" + pattern for a sequence
    std::string output;            // work/<hash>.mov
    std::string format = "hap";    // hap | hap_alpha | hap_q
    int chunks = 4;
    int threads = 0;               // 0 = ffmpeg decides
    double fps = 0;                // sequences: input rate; animations: output rate; 0 = keep
    int seq_start = 0;
    int max_w = 3840, max_h = 2160;  // stills: fit inside (2 x the canvas)
    // v3 (library-db.md §11.4)
    int out_max_h = 0;             // every kind: the largest output height, 0 = keep
    double conform_fps = 0;        // > 0: blend to this frame rate (ffmpeg framerate filter)
    bool progress = false;         // -progress pipe:1 (frame= / total_size= lines on stdout)
};
// Reads ffmpeg's -progress lines; feed it stdout chunks.
struct ProgressParser {
    int64_t frame = 0, total_size = 0;
    void feed(const uint8_t* p, size_t n);

private:
    std::string line_;
};
std::vector<std::string> probe_args(const std::string& input, bool sequence = false, double seq_fps = 25, int seq_start = 0);
std::vector<std::string> convert_args(const ConvertSpec& s);
std::vector<std::string> still_png_args(const std::string& input, const std::string& output_png, int max_w, int max_h);
std::vector<std::string> thumb_args(const std::string& hap, const std::string& out_jpg, double at_seconds);
std::vector<std::string> strip_args(const std::string& hap, const std::string& out_jpg, int64_t frames);
std::vector<std::string> proxy_args(const std::string& hap, int w, int h, double fps, int threads);

// ffprobe -print_format json -show_streams -show_format: the first video stream.
bool parse_probe(const std::string& json, library::Probe& out, std::string* err = nullptr);
// "30000/1001" -> 29.97; "0/0" -> 0.
double parse_rate(const std::string& r);
bool pix_fmt_has_alpha(const std::string& pix_fmt);

// ---- running ----
struct RunOptions {
    bool background = false;                 // idle CPU priority + very low I/O priority
    std::function<bool()> should_pause;      // polled every 50 ms: true = suspend the process
    std::function<bool()> should_stop;       // true = kill it
    std::function<void(const uint8_t*, size_t)> on_stdout;  // null = stdout discarded
    int timeout_ms = 0;                      // 0 = none (paused time does not count)
};
struct RunResult {
    bool started = false;
    int exit_code = -1;
    bool stopped = false, timed_out = false;
    std::string err_tail;                    // the last ~1 KB of stderr
    double paused_ms = 0;
    bool ok() const { return started && exit_code == 0 && !stopped && !timed_out; }
};
RunResult run_tool(const fs::path& exe, const std::vector<std::string>& args, const RunOptions& opt);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::string u8path(const fs::path& p);

}  // namespace himmel::ingest
