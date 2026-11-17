// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_ingest.exe: the library import worker (M7, design/library-and-analysis.md,
// docs/interfaces/library-db.md). Watches folders, converts with ffmpeg.exe
// (an external tool, never linked) to HAP, makes thumbnails, analyses, and
// writes the sidecar + SQLite row. Never runs in the render process; while a
// show runs it works at idle / background priority and pauses when the
// renderer reports a late frame (§7).
//
//   himmel_ingest [--library DIR] [--watch DIR]... [--ffmpeg DIR] [--once]
//                 [--mode auto|full|background] [--seq-fps N] [--canvas WxH]
//                 [--quality hap|hap_q] [--rebuild] [--retry] [--status] [--selftest]
//
//   --once     scan, import everything, then exit (tests, measurements)
//   --rebuild  recreate the DB from meta/*.json, then exit
//   --retry    put failed clips back in the queue
//   --status   print the library counts and exit
//   --send ... one command to the running worker (see send_command)
//   --selftest run every ingest command against a temporary library (no ffmpeg) and exit
//
// Commands 60..67 (import, rescan, remove, relink, reanalyse, retry, mode, unwatch) arrive
// in the library block's channels (docs/interfaces/library-db.md §10, ingest_ops.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "himmel/engine/engine_shared.h"
#include "himmel/ingest/ffmpeg.h"
#include "himmel/ingest/ingest_ops.h"
#include "himmel/ingest/scan.h"
#include "himmel/ingest/selftest.h"
#include "himmel/ingest/svg.h"
#include "himmel/ingest/throttle.h"
#include "himmel/library/analysis.h"
#include "himmel/library/feed.h"
#include "himmel/library/hash.h"
#include "himmel/library/library_block.h"
#include "himmel/library/library_db.h"
#include "himmel/library/sidecar.h"
#include "himmel/media/hap.h"
#include "himmel/media/mov.h"
#include "himmel/protocol/commands.h"

namespace fs = std::filesystem;
using namespace himmel;
using library::ClipRecord;
using library::Kind;
using library::State;

namespace {

std::atomic<bool> g_stop{false};
FILE* g_log = nullptr;
std::mutex g_log_mu;

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
int64_t unix_now() { return static_cast<int64_t>(std::time(nullptr)); }

void log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard lock(g_log_mu);
    std::printf("%02d:%02d:%02d.%03d ingest: %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    std::fflush(stdout);
    if (g_log) {
        std::fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, msg);
        std::fflush(g_log);
    }
}

BOOL WINAPI on_console(DWORD) {
    g_stop.store(true);
    return TRUE;
}

struct Config {
    fs::path root;
    std::vector<fs::path> watch;    // the default folder + --watch (commands add more, stored in the DB)
    fs::path ffmpeg_dir;
    bool once = false, rebuild = false, retry = false, status = false, selftest = false;
    int force_mode = 0;  // 0 auto, 1 full, 2 background
    double seq_fps = 25.0;
    int canvas_w = 1920, canvas_h = 1080;
    std::string quality = "hap";
};

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--library") c.root = from_utf8(next());
        else if (a == "--watch") c.watch.push_back(from_utf8(next()));
        else if (a == "--ffmpeg") c.ffmpeg_dir = from_utf8(next());
        else if (a == "--once") c.once = true;
        else if (a == "--rebuild") c.rebuild = true;
        else if (a == "--retry") c.retry = true;
        else if (a == "--status") c.status = true;
        else if (a == "--selftest") c.selftest = true;
        else if (a == "--seq-fps") c.seq_fps = std::clamp(std::atof(next().c_str()), 1.0, 120.0);
        else if (a == "--canvas") sscanf_s(next().c_str(), "%dx%d", &c.canvas_w, &c.canvas_h);
        else if (a == "--quality") c.quality = next() == "hap_q" ? "hap_q" : "hap";
        else if (a == "--mode") {
            const std::string m = next();
            c.force_mode = m == "full" ? 1 : m == "background" ? 2 : 0;
        } else {
            std::printf("unknown argument %s\n", a.c_str());
        }
    }
    if (c.root.empty()) c.root = library::default_library_root();
    if (c.watch.empty()) c.watch.push_back(c.root / "originals");
    c.canvas_w = std::clamp(c.canvas_w, 64, 8192);
    c.canvas_h = std::clamp(c.canvas_h, 64, 8192);
    return c;
}

// ---- Local\HimmelLibrary (we create it) and Local\HimmelEngine (we read it) ----
struct Blocks {
    HANDLE lib_map = nullptr;
    library::LibraryBlock* lib = nullptr;
    HANDLE eng_map = nullptr;
    const engine::EngineStatus* eng = nullptr;

    bool create_library() {
        lib_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(library::LibraryBlock),
                                     library::kLibraryShmName);
        if (!lib_map) return false;
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        lib = static_cast<library::LibraryBlock*>(MapViewOfFile(lib_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(library::LibraryBlock)));
        if (!lib) return false;
        const auto& magic = reinterpret_cast<std::atomic<uint32_t>&>(lib->header.magic);
        if (!existed || magic.load() != library::kLibraryMagic || lib->header.version != library::kLibraryVersion ||
            lib->header.size != sizeof(library::LibraryBlock)) {
            std::memset(static_cast<void*>(lib), 0, sizeof(library::LibraryBlock));
            lib->header.version = library::kLibraryVersion;
            lib->header.size = sizeof(library::LibraryBlock);
            std::atomic_thread_fence(std::memory_order_release);
            reinterpret_cast<std::atomic<uint32_t>&>(lib->header.magic).store(library::kLibraryMagic);
        }
        lib->stop_request.store(0);
        return true;
    }
    ingest::EngineSample sample_engine() {
        ingest::EngineSample s;
        if (!eng) {
            eng_map = OpenFileMappingW(FILE_MAP_READ, FALSE, engine::kEngineShmName);
            if (eng_map) {
                eng = static_cast<const engine::EngineStatus*>(MapViewOfFile(eng_map, FILE_MAP_READ, 0, 0, sizeof(engine::EngineStatus)));
                if (!eng) CloseHandle(eng_map), eng_map = nullptr;
            }
        }
        if (!eng || eng->magic != engine::kEngineMagic) return s;
        s.present = true;
        const uint64_t hb = eng->heartbeat_us.load(), t = now_us();
        s.heartbeat_age_ms = t > hb ? (t - hb) / 1000 : 0;
        s.late_frames = eng->late_frames.load();
        s.frames = eng->frames.load();
        s.fps_x100 = eng->fps_x100.load();
        return s;
    }
    void set_job(const std::string& name, const char* stage, float progress, const std::string& err = {}) {
        if (!lib) return;
        library::IngestJob j{};
        std::snprintf(j.name, sizeof(j.name), "%s", name.c_str());
        std::snprintf(j.stage, sizeof(j.stage), "%s", stage);
        j.progress = progress;
        std::snprintf(j.last_error, sizeof(j.last_error), "%s", err.c_str());
        protocol::seq_write(lib->job_seq, lib->job, j);
    }
};

// ---- the worker ----
class Ingest : public ingest::OpsHost {
public:
    explicit Ingest(const Config& c) : cfg_(c) {}

    // ---- ingest::OpsHost (called on the worker thread, from IngestOps) ----
    void queue(int64_t id) override {
        if (std::find(queue_.begin(), queue_.end(), id) == queue_.end()) queue_.push_back(id);
    }
    void unqueue(int64_t id) override { queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end()); }
    void rescan_now() override {
        known_.clear();
        next_scan_ = 0;
    }
    void set_mode(library::IngestModeRequest m) override { mode_req_.store(static_cast<int>(m)); }

    bool start() {
        std::error_code ec;
        for (const char* d : {"originals", "hap", "stills", "meta", "thumbs", "work"}) fs::create_directories(cfg_.root / d, ec);
        for (const auto& w : cfg_.watch) fs::create_directories(w, ec);
        std::string err;
        if (!db_.open(cfg_.root / library::kDbFile, false, &err)) {
            log("cannot open the library DB in %s: %s", ingest::u8path(cfg_.root).c_str(), err.c_str());
            return false;
        }
        if (!tools_.find(cfg_.ffmpeg_dir)) {
            log("ffmpeg.exe / ffprobe.exe not found (%s) - install ffmpeg or pass --ffmpeg DIR",
                cfg_.ffmpeg_dir.empty() ? "PATH" : ingest::u8path(cfg_.ffmpeg_dir).c_str());
            return false;
        }
        chunks_ = static_cast<int>(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u, 8u));
        // Old temp files: never "ready", always safe to delete.
        for (auto it = fs::directory_iterator(cfg_.root / "work", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            fs::remove(it->path(), ec);
        if (!blocks_.create_library()) log("cannot create %ls - no status for the engine / GUI", library::kLibraryShmName);
        if (blocks_.lib) {
            blocks_.lib->pid.store(GetCurrentProcessId());
            blocks_.lib->ready_seq.store(db_.ready_seq());
        }
        ops_ = std::make_unique<ingest::IngestOps>(db_, cfg_.root, cfg_.watch, *this);
        pump_ = std::make_unique<ingest::CommandPump>(blocks_.lib);
        if (const int n = pump_->recover()) log("%d command(s) were cut short by a restart: replied Interrupted", n);
        if (const int n = ops_->retry_pending_deletes()) log("deleted %d file(s) of removed clips", n);
        return true;
    }

    void run() {
        monitor_ = std::thread([this] { monitor(); });
        SettleTracker settle(2000);
        bool idle_logged = false;
        while (!stopping()) {
            poll_commands(0);
            if (now_us() >= next_scan_) {
                scan(settle);
                next_scan_ = now_us() + 2000000;
            }
            if (!queue_.empty() && !hold()) {
                idle_logged = false;
                const int64_t id = queue_.front();
                queue_.pop_front();
                update_counts();
                process(id);
                update_counts();
                continue;
            }
            if (cfg_.once && first_scan_done_ && settle_pending_ == 0 && queue_.empty()) break;
            if (!idle_logged) {
                log("idle: %d ready, %d failed", db_.count(static_cast<int>(State::Ready)), db_.count(static_cast<int>(State::Failed)));
                idle_logged = true;
            }
            Sleep(200);
        }
        g_stop.store(true);
        monitor_.join();
        if (blocks_.lib) blocks_.set_job("", "", 0);
    }

private:
    using SettleTracker = ingest::SettleTracker;

    bool stopping() const { return g_stop.load() || (blocks_.lib && blocks_.lib->stop_request.load() != 0); }
    bool hold() const { return mode_req_.load() == static_cast<int>(library::IngestModeRequest::Hold); }
    // The running job must end now: the worker stops, or a command cancelled this clip.
    bool aborted() const { return stopping() || (current_ != 0 && cancel_id_ == current_); }

    // ---- commands (library-db.md §10), always on the worker thread ----
    // Between files current = 0: every waiting command is applied. During a job (called from
    // run_tool's poll): commands for other clips are applied; one for the running clip is held
    // until the job ends (Remove / forced Relink cancel the job first).
    void poll_commands(int64_t current) {
        if (!pump_) return;
        if (held_) {
            if (current != 0) return;
            apply_command(held_prod_, *held_);
            held_.reset();
        }
        int prod = 0;
        library::IngestCommand c;
        while (pump_->take(prod, c)) {
            bool cancel = false;
            if (ingest::IngestOps::touches(c, current, cancel)) {
                held_ = std::make_unique<library::IngestCommand>(c);
                held_prod_ = prod;
                if (cancel) {
                    cancel_id_ = current;
                    log("command %u for the clip being imported: cancelling its job first", c.type);
                }
                return;
            }
            apply_command(prod, c);
        }
    }

    void apply_command(int prod, const library::IngestCommand& c) {
        const auto r = ops_->apply(c);
        pump_->reply(prod, c, r);
        log("command %u (producer %d, seq %u, clip %lld%s%s): status %u, %d - %s", c.type, prod, c.seq,
            static_cast<long long>(c.clip), c.path[0] ? ", " : "", c.path[0] ? c.path : "", r.status, r.count, r.message);
        if (ops_->take_changed()) {
            db_.bump_ready_seq();
            if (auto* b = blocks_.lib) b->ready_seq.fetch_add(1, std::memory_order_acq_rel);
        }
        update_counts();
    }
    bool background() const { return background_.load(); }

    // Every 100 ms: heartbeat, engine sample, throttle, mode.
    void monitor() {
        bool was_bg = false, was_paused = false;
        while (!g_stop.load()) {
            const auto s = blocks_.sample_engine();
            throttle_.update(s, static_cast<int64_t>(now_us() / 1000));
            // A Mode command wins over --mode; Auto falls back to --mode.
            const int req = mode_req_.load();
            const int mode = req == 1 ? 1 : req == 2 ? 2 : cfg_.force_mode;
            const bool bg = mode == 2 || (mode == 0 && throttle_.show_running());
            background_.store(bg);
            paused_.store((bg && throttle_.paused()) || hold());
            if (bg != was_bg) log(bg ? "a show is running: background import (idle priority, 1 job, pauses on late frames)"
                                     : "no show running: full-speed import");
            if (paused_.load() != was_paused) log(paused_.load() ? (hold() ? "on hold (Mode command)" : "paused: the renderer had a late frame") : "resumed");
            was_bg = bg, was_paused = paused_.load();
            if (auto* b = blocks_.lib) {
                b->heartbeat_us.store(now_us());
                b->mode.store(static_cast<uint32_t>(hold() ? library::IngestMode::Hold
                                                    : bg       ? library::IngestMode::Background
                                                    : working_.load() ? library::IngestMode::Full
                                                                      : library::IngestMode::Idle));
                b->paused.store(paused_.load() ? 1 : 0);
                b->pauses.store(throttle_.pauses());
                if (b->stop_request.load()) g_stop.store(true);
            }
            Sleep(100);
        }
    }

    void wait_while_paused() {
        while (paused_.load() && !aborted()) {
            poll_commands(current_);
            Sleep(50);
        }
    }

    void update_counts() {
        if (auto* b = blocks_.lib) {
            b->queued.store(static_cast<uint32_t>(queue_.size()));
            b->working.store(working_.load() ? 1 : 0);
            b->ready_total.store(static_cast<uint32_t>(db_.count(static_cast<int>(State::Ready))));
            b->failed_total.store(static_cast<uint32_t>(db_.count(static_cast<int>(State::Failed))));
        }
    }

    void set_thread_background(bool on) {
        if (on == thread_bg_) return;
        SetThreadPriority(GetCurrentThread(), on ? THREAD_MODE_BACKGROUND_BEGIN : THREAD_MODE_BACKGROUND_END);
        thread_bg_ = on;
    }

    // ---- scan: new files -> queued rows ----
    void scan(SettleTracker& settle) {
        set_thread_background(background());
        std::vector<fs::path> skip;
        skip = ops_->skip_folders();
        const int64_t now_ms = static_cast<int64_t>(now_us() / 1000);
        int pending = 0;
        if (!first_scan_done_) resume_unfinished();
        ops_->retry_pending_deletes();
        for (const auto& w : ops_->watch_folders()) {
            for (auto& c : ingest::scan_folder(w, skip)) {
                const std::string key = ingest::u8path(c.path);
                if (known_.count(key) && known_[key] == std::make_pair(c.size, c.mtime)) continue;
                if (!settle.settled(c, now_ms)) {
                    ++pending;
                    continue;
                }
                known_[key] = {c.size, c.mtime};
                enqueue(c);
                if (stopping()) return;
            }
        }
        settle_pending_ = pending;
        first_scan_done_ = true;
    }

    void resume_unfinished() {
        for (auto& r : db_.list()) {
            if (r.state == State::Ready || r.state == State::Failed || r.state == State::Removed) continue;
            queue_.push_back(r.id);
        }
        if (!queue_.empty()) log("resuming %zu unfinished import(s)", queue_.size());
    }

    void enqueue(const ingest::Candidate& c) {
        const auto res = ops_->enqueue(c, false);
        if (res.id == 0) log("cannot read or queue %s - skipped", ingest::u8path(c.path).c_str());
        else if (res.queued) log("queued %s (%s)", ingest::u8path(c.path).c_str(), library::kind_name(c.kind));
        if (ops_->take_changed()) update_counts();
    }

    // ---- one clip, all stages ----
    bool fail(ClipRecord& r, const std::string& why) {
        r.state = State::Failed;
        r.error = why.substr(0, 300);
        std::replace(r.error.begin(), r.error.end(), '\n', ' ');
        db_.upsert(r);
        blocks_.set_job(r.name, "failed", 0, r.error);
        log("FAILED %s: %s", r.name.c_str(), r.error.c_str());
        return false;
    }

    void set_state(ClipRecord& r, State s, const char* stage) {
        r.state = s;
        db_.upsert(r);
        blocks_.set_job(r.name, stage, 0);
    }

    ingest::RunOptions run_opts(std::function<void(const uint8_t*, size_t)> out = nullptr) {
        ingest::RunOptions o;
        o.background = background();
        o.should_pause = [this] {
            poll_commands(current_);  // run_tool polls on this (the worker) thread
            return paused_.load();
        };
        o.should_stop = [this] { return aborted(); };
        o.on_stdout = std::move(out);
        o.timeout_ms = 30 * 60 * 1000;
        return o;
    }

    fs::path abs(const std::string& rel) const { return cfg_.root / from_utf8(rel); }

    bool process(int64_t id) {
        ClipRecord r;
        if (!db_.get(id, r) || r.state == State::Removed || (r.state == State::Ready && r.stage_done == 15)) return false;
        working_.store(true);
        current_ = id;
        cancel_id_ = 0;
        struct Done {
            Ingest& in;
            ~Done() {
                in.working_.store(false);
                in.current_ = 0;
                in.cancel_id_ = 0;
            }
        } done{*this};
        set_thread_background(background());
        const uint64_t t0 = now_us();
        const fs::path src = from_utf8(r.source);
        std::error_code ec;
        if (!fs::exists(src, ec)) return fail(r, "the original is gone");

        // Sequence details come from the folder again (they are not stored).
        ingest::Candidate seq;
        if (r.kind == Kind::Sequence) {
            std::vector<fs::path> files;
            for (auto it = fs::directory_iterator(src, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            if (!ingest::find_sequence(files, seq)) return fail(r, "no numbered images in the folder");
        }
        const std::string seq_input = r.kind == Kind::Sequence ? ingest::u8path(src / from_utf8(seq.seq_pattern)) : "";
        const fs::path work = cfg_.root / "work";
        const std::string h = r.hash;

        // 1. Still image for stills / vectors (also the HAP input).
        fs::path hap_input = src;
        if (r.kind == Kind::Vector || r.kind == Kind::Still) {
            set_state(r, State::Converting, "converting");
            const fs::path png_tmp = work / (h + ".png"), png = cfg_.root / "stills" / (h + ".png");
            if (!(r.stage_done & library::kStageConvert) || !fs::exists(png, ec)) {
                std::string err;
                if (r.kind == Kind::Vector) {
                    wait_while_paused();
                    if (!ingest::rasterize_svg(src, png_tmp, cfg_.canvas_h * 2, cfg_.canvas_w * 2, &err)) return fail(r, "SVG: " + err);
                } else {
                    wait_while_paused();
                    const auto res = ingest::run_tool(tools_.ffmpeg, ingest::still_png_args(r.source, ingest::u8path(png_tmp),
                                                                                             cfg_.canvas_w * 2, cfg_.canvas_h * 2),
                                                      run_opts());
                    if (!res.ok()) return aborted() ? false : fail(r, "still: " + res.err_tail);
                }
                fs::rename(png_tmp, png, ec);
                if (ec) return fail(r, "cannot move the PNG into stills/");
            }
            r.still_path = "stills/" + h + ".png";
            hap_input = png;
        }

        // 2. Probe.
        if (!(r.stage_done & library::kStageProbe)) {
            set_state(r, State::Probing, "probing");
            std::string out;
            const auto args = r.kind == Kind::Sequence ? ingest::probe_args(seq_input, true, cfg_.seq_fps, seq.seq_start)
                                                        : ingest::probe_args(ingest::u8path(hap_input));
            auto o = run_opts([&](const uint8_t* p, size_t n) { out.append(reinterpret_cast<const char*>(p), n); });
            o.timeout_ms = 60000;
            const auto res = ingest::run_tool(tools_.ffprobe, args, o);
            if (aborted()) return false;
            std::string err;
            if (!res.ok() || !ingest::parse_probe(out, r.probe, &err))
                return fail(r, "probe: " + (res.err_tail.empty() ? err : res.err_tail));
            if (r.kind == Kind::Vector) r.probe.alpha = true;
            if (r.kind == Kind::Sequence) r.probe.fps = cfg_.seq_fps;
            r.stage_done |= library::kStageProbe;
            db_.upsert(r);
        }

        // 3. Convert to HAP (work/ -> hap/).
        const fs::path hap = cfg_.root / "hap" / (h + ".mov");
        if (!(r.stage_done & library::kStageConvert) || !fs::exists(hap, ec)) {
            set_state(r, State::Converting, "converting");
            const fs::path tmp = work / (h + ".mov");
            const bool alpha = r.probe.alpha || r.kind == Kind::Animation;
            r.codec = alpha ? "hap_alpha" : cfg_.quality;
            const bool already_hap = r.kind == Kind::Video && r.probe.codec == "hap" && r.probe.width % 4 == 0 && r.probe.height % 4 == 0;
            if (already_hap) {
                fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
                if (ec) return fail(r, "cannot copy the HAP file");
            } else {
                ingest::ConvertSpec spec;
                spec.kind = r.kind;
                spec.input = r.kind == Kind::Sequence ? seq_input : ingest::u8path(hap_input);
                spec.output = ingest::u8path(tmp);
                spec.format = r.codec;
                spec.chunks = chunks_;
                spec.threads = background() ? 2 : 0;
                spec.seq_start = seq.seq_start;
                spec.max_w = cfg_.canvas_w * 2, spec.max_h = cfg_.canvas_h * 2;
                if (r.kind == Kind::Sequence) spec.fps = cfg_.seq_fps;
                if (r.kind == Kind::Animation) spec.fps = std::clamp(std::round(r.probe.fps > 0 ? r.probe.fps : 10.0), 1.0, 60.0);
                const auto res = ingest::run_tool(tools_.ffmpeg, ingest::convert_args(spec), run_opts());
                if (res.paused_ms > 0) log("  %s: paused %.1f s for the show", r.name.c_str(), res.paused_ms / 1000.0);
                if (!res.ok()) {
                    fs::remove(tmp, ec);
                    return aborted() ? false : fail(r, "ffmpeg: " + res.err_tail);
                }
            }
            fs::rename(tmp, hap, ec);
            if (ec) return fail(r, "cannot move the HAP file into hap/");
            r.stage_done |= library::kStageConvert;
        }
        r.play_path = "hap/" + h + ".mov";
        if (!read_hap_header(hap, r)) return fail(r, "the converted file is not a HAP clip Himmel can play");
        r.judder = library::judder_for(r.frames > 1 ? r.fps : 0);
        db_.upsert(r);

        // 4. Thumbnails.
        if (!(r.stage_done & library::kStageThumbs) || !fs::exists(abs(r.thumb_path), ec)) {
            set_state(r, State::Analysing, "thumbs");
            wait_while_paused();
            const fs::path t_tmp = work / (h + ".jpg"), t = cfg_.root / "thumbs" / (h + ".jpg");
            auto res = ingest::run_tool(tools_.ffmpeg, ingest::thumb_args(ingest::u8path(hap), ingest::u8path(t_tmp), r.seconds * 0.1), run_opts());
            if (!res.ok()) return aborted() ? false : fail(r, "thumbnail: " + res.err_tail);
            fs::rename(t_tmp, t, ec);
            if (r.frames > 1) {
                const fs::path s_tmp = work / (h + "-strip.jpg"), s = cfg_.root / "thumbs" / (h + "-strip.jpg");
                res = ingest::run_tool(tools_.ffmpeg, ingest::strip_args(ingest::u8path(hap), ingest::u8path(s_tmp), r.frames), run_opts());
                if (res.ok()) fs::rename(s_tmp, s, ec);  // the strip is optional
            }
            r.thumb_path = "thumbs/" + h + ".jpg";
            r.stage_done |= library::kStageThumbs;
            db_.upsert(r);
        }

        // 5. Analysis on a small proxy.
        if (!(r.stage_done & library::kStageAnalyse) || r.analysis.version < library::kAnalysisVersion) {
            set_state(r, State::Analysing, "analysing");
            wait_while_paused();
            library::ProxyClip px;
            const bool bg = background();
            px.w = bg ? 64 : 96, px.h = bg ? 36 : 54;
            double pf = std::min(bg ? 12.5 : 25.0, r.fps > 0 ? r.fps : 25.0);
            if (r.seconds > 0) pf = std::min(pf, 3000.0 / r.seconds);  // at most ~3000 proxy frames
            pf = std::max(pf, 0.5);
            px.fps = static_cast<float>(pf);
            px.hap_fps = r.fps;
            px.hap_frames = r.frames;
            px.rgb.reserve(static_cast<size_t>(px.w) * px.h * 3 * static_cast<size_t>(std::max(1.0, r.seconds * pf + 2)));
            auto o = run_opts([&](const uint8_t* p, size_t n) { px.rgb.insert(px.rgb.end(), p, p + n); });
            const auto res = ingest::run_tool(tools_.ffmpeg, ingest::proxy_args(ingest::u8path(hap), px.w, px.h, pf, bg ? 1 : 0), o);
            if (!res.ok() || px.frames() == 0) return aborted() ? false : fail(r, "proxy: " + res.err_tail);
            px.rgb.resize(static_cast<size_t>(px.frames()) * px.w * px.h * 3);
            wait_while_paused();
            r.analysis = library::analyse(px);
            r.stage_done |= library::kStageAnalyse;
        }

        // 6. Sidecar, then READY (only now does the engine see it).
        r.state = State::Ready;
        r.error.clear();
        if (r.ready_unix == 0) r.ready_unix = unix_now();  // a re-analysis is not NEW again
        if (aborted()) return false;
        if (!library::write_sidecar(cfg_.root / "meta" / (h + ".json"), r)) return fail(r, "cannot write the sidecar");
        db_.begin();
        db_.upsert(r);
        db_.bump_ready_seq();
        db_.commit();
        if (auto* b = blocks_.lib) b->ready_seq.fetch_add(1, std::memory_order_acq_rel);
        blocks_.set_job(r.name, "ready", 1.0f);
        log("READY %s: %s %dx%d %.2f fps %lld frames, %s, energy %d, %s, %s%s in %.1f s", r.name.c_str(), r.codec.c_str(),
            r.width, r.height, r.fps, static_cast<long long>(r.frames), library::rgb_hex(r.analysis.avg_rgb).c_str(),
            r.analysis.energy, r.analysis.loop_style.c_str(), r.analysis.flicker.c_str(),
            r.judder.empty() ? "" : " (judders)", (now_us() - t0) / 1e6);
        return true;
    }

    // Our own MOV reader decides: if the engine cannot read it, it is not ready.
    static bool read_hap_header(const fs::path& file, ClipRecord& r) {
        HANDLE fh = CreateFileW(file.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (fh == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(fh, &size);
        media::ByteSource bs;
        bs.size = static_cast<uint64_t>(size.QuadPart);
        bs.read = [fh](uint64_t off, void* dst, size_t n) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            return ReadFile(fh, dst, static_cast<DWORD>(n), &got, &ov) && got == n;
        };
        std::string err;
        bool ok = false;
        if (auto t = media::read_mov(bs, &err); t && t->frames() > 0) {
            r.width = t->width, r.height = t->height;
            r.fps = t->fps;
            r.frames = static_cast<int64_t>(t->frames());
            r.seconds = t->fps > 0 ? r.frames / t->fps : 0;
            std::vector<uint8_t> f0(t->sizes[0]);
            if (bs.read(t->offsets[0], f0.data(), f0.size())) {
                const auto tex = media::hap_peek_texture(f0.data(), f0.size());
                ok = tex != media::HapTexture::Unknown;
                if (tex == media::HapTexture::YCoCgDXT5) r.codec = "hap_q";
                else if (tex == media::HapTexture::DXT5) r.codec = "hap_alpha";
                else if (tex == media::HapTexture::DXT1) r.codec = "hap";
            }
        }
        CloseHandle(fh);
        return ok;
    }

    Config cfg_;
    library::LibraryDb db_;
    ingest::Tools tools_;
    Blocks blocks_;
    ingest::Throttle throttle_;
    std::thread monitor_;
    std::atomic<bool> background_{false}, paused_{false}, working_{false};
    std::deque<int64_t> queue_;
    std::map<std::string, std::pair<int64_t, int64_t>> known_;
    std::unique_ptr<ingest::IngestOps> ops_;
    std::unique_ptr<ingest::CommandPump> pump_;
    std::unique_ptr<library::IngestCommand> held_;  // a command waiting for the running job
    int held_prod_ = 0;
    int64_t current_ = 0, cancel_id_ = 0;           // worker thread only
    std::atomic<int> mode_req_{0};                  // IngestModeRequest (Mode command)
    uint64_t next_scan_ = 0;
    int chunks_ = 4;
    int settle_pending_ = 0;
    bool first_scan_done_ = false;
    bool thread_bg_ = false;
};

// ---- himmel_ingest --send VERB ...: one command to the running worker (producer slot Tool) ----
// import PATH [watch] | rescan [ID] | remove ID | relink ID|0 PATH [force] | reanalyse [ID] [thumbs]
// | retry [ID] | mode auto|full|background|hold | unwatch PATH. Prints the reply; exit 0 = Ok/Accepted.
// One --send at a time (the Tool slot has one writer).
int send_command(int argc, char** argv) {
    auto arg = [&](int k) -> std::string { return k < argc ? argv[k] : ""; };
    const std::string verb = arg(2);
    library::IngestCommand c;
    auto id_at = [&](int k) { return static_cast<int64_t>(std::atoll(arg(k).c_str())); };
    auto abs_path = [&](int k) {
        std::error_code ec;
        return ingest::u8path(fs::absolute(from_utf8(arg(k)), ec));
    };
    auto has = [&](const char* w) {
        for (int k = 3; k < argc; ++k)
            if (arg(k) == w) return true;
        return false;
    };
    bool ok = true;
    using T = library::IngestCommandType;
    if (verb == "import") ok = library::make_ingest(c, T::Import, 0, abs_path(3).c_str(), has("watch") ? library::kIngestWatch : 0);
    else if (verb == "rescan") ok = library::make_ingest(c, T::Rescan, id_at(3));
    else if (verb == "remove") ok = library::make_ingest(c, T::Remove, id_at(3));
    else if (verb == "relink") ok = library::make_ingest(c, T::Relink, id_at(3), abs_path(4).c_str(), has("force") ? library::kIngestForce : 0);
    else if (verb == "reanalyse") ok = library::make_ingest(c, T::Reanalyse, id_at(3), nullptr, has("thumbs") ? library::kIngestThumbs : 0);
    else if (verb == "retry") ok = library::make_ingest(c, T::Retry, id_at(3));
    else if (verb == "unwatch") ok = library::make_ingest(c, T::Unwatch, 0, abs_path(3).c_str());
    else if (verb == "mode") {
        const std::string m = arg(3);
        const int i = m == "full" ? 1 : m == "background" ? 2 : m == "hold" ? 3 : m == "auto" ? 0 : -1;
        ok = library::make_ingest(c, T::Mode, 0, nullptr, 0, i);
    } else {
        std::printf("usage: himmel_ingest --send import PATH [watch] | rescan [ID] | remove ID | relink ID|0 PATH [force]\n"
                    "                     | reanalyse [ID] [thumbs] | retry [ID] | mode auto|full|background|hold | unwatch PATH\n");
        return 2;
    }
    if (!ok) return std::printf("the path is too long\n"), 2;
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, library::kLibraryShmName);
    auto* b = map ? static_cast<library::LibraryBlock*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(library::LibraryBlock))) : nullptr;
    if (!b || b->header.magic != library::kLibraryMagic || b->header.version != library::kLibraryVersion ||
        b->header.size != sizeof(library::LibraryBlock)) {
        std::printf("no himmel_ingest (version %u) is running\n", library::kLibraryVersion);
        return 3;
    }
    auto& ch = b->channels[static_cast<int>(protocol::Producer::Tool)];
    library::IngestReply r{};
    while (ch.replies.pop(r)) {}  // old replies
    const uint32_t seq = std::max(ch.done_seq.load(), ch.applying_seq.load()) + 1;
    if (!library::push_ingest(ch, c, seq, GetCurrentProcessId(), now_us())) return std::printf("the command ring is full\n"), 4;
    for (int t = 0; t < 600; ++t) {  // 60 s: a command for the clip being converted waits for its job
        while (ch.replies.pop(r))
            if (r.seq == seq) {
                static const char* kNames[] = {"Ok", "Accepted", "UnknownType", "BadId", "BadPath", "Mismatch",
                                               "Duplicate", "Failed", "Interrupted", "BadValue"};
                std::printf("%s count=%d clip=%lld: %s\n", r.status < 10 ? kNames[r.status] : "?", r.count,
                            static_cast<long long>(r.clip), r.message);
                return r.status <= 1 ? 0 : 1;
            }
        Sleep(100);
    }
    std::printf("no reply within 60 s\n");
    return 5;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_console, TRUE);
    if (argc > 1 && std::string(argv[1]) == "--send") return send_command(argc, argv);
    // The supervisor starts its children at high priority; the import never competes with the show.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const Config cfg = parse_args(argc, argv);
    if (cfg.selftest) {
        // Never under Documents: a temp folder, deleted afterwards; an in-memory block.
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const fs::path dir = fs::path(tmp) / ("himmel_ingest_selftest_" + std::to_string(GetCurrentProcessId()));
        const int fails = ingest::run_command_selftest(dir, [](const std::string& line) { std::printf("%s\n", line.c_str()); });
        return fails == 0 ? 0 : 1;
    }
    std::error_code ec;
    fs::create_directories(cfg.root, ec);
    g_log = _wfsopen((cfg.root / "ingest.log").c_str(), L"a", _SH_DENYWR);
    log("library %s, watching %zu folder(s)%s", ingest::u8path(cfg.root).c_str(), cfg.watch.size(), cfg.once ? " (once)" : "");

    if (cfg.rebuild || cfg.status || cfg.retry) {
        library::LibraryDb db;
        std::string err;
        if (!db.open(cfg.root / library::kDbFile, false, &err)) {
            log("cannot open the DB: %s", err.c_str());
            return 1;
        }
        if (cfg.rebuild) log("rebuilt from the sidecars: %d ready clips", db.rebuild_from_sidecars(cfg.root));
        if (cfg.retry) {
            int n = 0;
            for (auto& r : db.list(static_cast<int>(State::Failed))) {
                r.state = State::Queued;
                r.error.clear();
                r.stage_done = 0;
                n += db.upsert(r) ? 1 : 0;
            }
            log("%d failed clip(s) queued again", n);
        }
        log("status: %d clips, %d ready, %d failed, %d in progress", db.count(), db.count(static_cast<int>(State::Ready)),
            db.count(static_cast<int>(State::Failed)),
            db.count() - db.count(static_cast<int>(State::Ready)) - db.count(static_cast<int>(State::Failed)));
        if (!cfg.retry) return 0;
    }

    Ingest ing(cfg);
    if (!ing.start()) return 1;
    ing.run();
    log("stopped");
    if (g_log) std::fclose(g_log);
    return 0;
}
