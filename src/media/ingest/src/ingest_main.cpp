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
//                 [--quality hap|hap_q] [--reserve-gb N] [--rebuild] [--retry] [--status] [--selftest]
//
//   --once     scan, import everything, then exit (tests, measurements)
//   --rebuild  recreate the DB from meta/*.json, then exit
//   --retry    put failed clips back in the queue
//   --status   print the library counts and exit
//   --send ... one command to the running worker (see send_command)
//   --help     usage; an unknown or incomplete argument prints it too and exits 2 (nothing started)
//   --selftest run every ingest command against a temporary library (no ffmpeg) and exit
//
// Commands 60..69 (import, rescan, remove, relink, reanalyse, retry, mode, unwatch, clear queue,
// library) arrive in the library block's channels (docs/interfaces/library-db.md §10-11, ingest_ops.h).
// v3 (wave 7): the disk guard keeps a reserve free (max(10 GB, 10 %) or --reserve-gb), the
// mode is stored in the DB, progress / plan / library root are published in the block (§11).
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
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "himmel/engine/engine_shared.h"
#include "himmel/ingest/ffmpeg.h"
#include "himmel/ingest/guard.h"
#include "himmel/ingest/ingest_ops.h"
#include "himmel/ingest/library_move.h"
#include "himmel/ingest/scan.h"
#include "himmel/ingest/selftest.h"
#include "himmel/ingest/svg.h"
#include "himmel/ingest/throttle.h"
#include "himmel/library/analysis.h"
#include "himmel/library/feed.h"
#include "himmel/library/hash.h"
#include "himmel/library/library_block.h"
#include "himmel/library/library_db.h"
#include "himmel/library/location.h"
#include "himmel/library/sidecar.h"
#include "himmel/media/hap.h"
#include "himmel/media/mov.h"
#include "himmel/protocol/commands.h"

#include <nlohmann/json.hpp>

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
    bool explicit_root = false;   // --library given: a switch / move is not saved as the default (D182)
    uint64_t reserve_bytes = 0;   // --reserve-gb: the disk guard's reserve; 0 = max(10 GB, 10 %)
    bool help = false;
    std::string error;  // a bad command line: print usage, exit 2, start nothing
};

void print_usage() {
    std::printf(
        "usage: himmel_ingest [--library DIR] [--watch DIR]... [--ffmpeg DIR] [--once]\n"
        "                     [--mode auto|full|background] [--seq-fps N] [--canvas WxH]\n"
        "                     [--quality hap|hap_q] [--reserve-gb N] [--rebuild] [--retry] [--status]\n"
        "       himmel_ingest --selftest      every ingest command against a temp library, then exit\n"
        "       himmel_ingest --send VERB ... one command to the running worker (--send alone: the verbs)\n"
        "       himmel_ingest --help\n"
        "  --library DIR  the library (default Documents\\Himmel\\Library; tests: always a temp folder)\n"
        "  --watch DIR    another watch folder (default: <library>\\originals)\n"
        "  --once         scan, import everything, then exit\n"
        "  --rebuild      recreate the DB from meta\\*.json, then exit\n"
        "  --retry        put failed clips back in the queue\n"
        "  --status       print the library counts and the disk space, then exit\n"
        "  --reserve-gb N the disk guard keeps N GB free (default: 10 GB or 10 %% of the disk, the larger)\n"
        "An unknown argument prints this and exits with code 2 (nothing is started or written).\n");
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc && c.error.empty(); ++i) {
        const std::string a = argv[i];
        // An option that needs a value never takes a missing one (that would fall back to Documents).
        auto next = [&]() -> std::string {
            if (i + 1 >= argc || std::string(argv[i + 1]).empty() || std::string(argv[i + 1]).rfind("--", 0) == 0) {
                c.error = a + " needs a value";
                return "";
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h" || a == "/?") c.help = true;
        else if (a == "--library") c.root = from_utf8(next()), c.explicit_root = true;
        else if (a == "--watch") c.watch.push_back(from_utf8(next()));
        else if (a == "--ffmpeg") c.ffmpeg_dir = from_utf8(next());
        else if (a == "--once") c.once = true;
        else if (a == "--rebuild") c.rebuild = true;
        else if (a == "--retry") c.retry = true;
        else if (a == "--status") c.status = true;
        else if (a == "--selftest") c.selftest = true;
        else if (a == "--seq-fps") {
            const std::string v = next();
            const double f = std::atof(v.c_str());
            if (c.error.empty() && f <= 0) c.error = "--seq-fps needs a number";
            c.seq_fps = std::clamp(f, 1.0, 120.0);
        } else if (a == "--canvas") {
            const std::string v = next();
            if (c.error.empty() && sscanf_s(v.c_str(), "%dx%d", &c.canvas_w, &c.canvas_h) != 2) c.error = "--canvas needs WxH";
        } else if (a == "--quality") {
            const std::string q = next();
            if (c.error.empty() && q != "hap" && q != "hap_q") c.error = "--quality is hap or hap_q";
            c.quality = q == "hap_q" ? "hap_q" : "hap";
        } else if (a == "--reserve-gb") {
            const std::string v = next();
            const double g = std::atof(v.c_str());
            if (c.error.empty() && !(g > 0 && g < 100000)) c.error = "--reserve-gb needs a number of GB";
            c.reserve_bytes = static_cast<uint64_t>(g * 1e9);
        } else if (a == "--mode") {
            const std::string m = next();
            if (c.error.empty() && m != "auto" && m != "full" && m != "background") c.error = "--mode is auto, full or background";
            c.force_mode = m == "full" ? 1 : m == "background" ? 2 : 0;
        } else {
            c.error = "unknown argument " + a;
        }
    }
    if (!c.error.empty() || c.help) return c;  // never resolve the default library for a bad command line
    // v3: the saved location (default_library_root) and moved-library redirects (D182).
    c.root = c.root.empty() ? library::default_library_root() : library::resolve_library_root(c.root);
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
        s.pid = eng->pid.load();
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
    // v3: the library folder in use and a running move.
    void set_root(const fs::path& root, library::MoveState st, float progress, uint64_t done, uint64_t total,
                  const fs::path& to, const std::string& msg) {
        if (!lib) return;
        auto r = std::make_unique<library::LibraryRoot>();
        copy_utf8(r->path, sizeof(r->path), ingest::u8path(root));
        r->move_state = static_cast<uint32_t>(st);
        r->move_progress = progress;
        r->move_done_bytes = done, r->move_total_bytes = total;
        copy_utf8(r->move_to, sizeof(r->move_to), to.empty() ? std::string() : ingest::u8path(to));
        copy_utf8(r->move_message, sizeof(r->move_message), msg);
        protocol::seq_write(lib->root_seq, lib->root, *r);
    }
    // Cuts on a UTF-8 boundary.
    static void copy_utf8(char* dst, size_t n, const std::string& s) {
        size_t len = std::min(s.size(), n - 1);
        while (len > 0 && len < s.size() && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
        std::memcpy(dst, s.data(), len);
        dst[len] = 0;
    }
};

// ---- the worker ----
class Ingest : public ingest::OpsHost {
public:
    explicit Ingest(const Config& c) : cfg_(c) {}

    // ---- ingest::OpsHost (called on the worker thread, from IngestOps) ----
    void queue(int64_t id) override {
        if (std::find(queue_.begin(), queue_.end(), id) != queue_.end()) return;
        if (queue_.empty() && !working_.load()) batch_.reset();  // a new batch (library-db.md §11.7)
        queue_.push_back(id);
        batch_.add(1);
    }
    void unqueue(int64_t id) override {
        const auto it = std::find(queue_.begin(), queue_.end(), id);
        if (it == queue_.end()) return;
        queue_.erase(it);
        batch_.file_done(0, 0, 0);  // cleared / removed: finished for the batch count
    }
    void rescan_now() override {
        known_.clear();
        next_scan_ = 0;
    }
    void set_mode(library::IngestModeRequest m) override { mode_req_.store(static_cast<int>(m)); }

    // ---- v3 OpsHost (library-db.md §11) ----
    bool estimate(const ingest::Candidate& c, const library::ImportOptions& o, int64_t& bytes) override {
        library::Probe pr;
        bool ok = false;
        if (c.kind == Kind::Vector) {  // drawn at 2 x the canvas (no ffprobe for SVG)
            pr.width = cfg_.canvas_w * 2, pr.height = cfg_.canvas_h * 2, pr.alpha = true;
            ok = true;
        } else {
            std::string out;
            const auto args = c.kind == Kind::Sequence
                                  ? ingest::probe_args(ingest::u8path(c.path / from_utf8(c.seq_pattern)), true, cfg_.seq_fps, c.seq_start)
                                  : ingest::probe_args(ingest::u8path(c.path));
            ingest::RunOptions ro;
            ro.background = background();
            ro.timeout_ms = 15000;
            ro.should_stop = [this] { return stopping(); };
            ro.on_stdout = [&](const uint8_t* q, size_t n) { out.append(reinterpret_cast<const char*>(q), n); };
            const auto res = ingest::run_tool(tools_.ffprobe, args, ro);
            ok = res.ok() && ingest::parse_probe(out, pr);
            if (ok && c.kind == Kind::Sequence) pr.fps = cfg_.seq_fps, pr.frames = static_cast<int64_t>(c.seq_files.size());
        }
        if (!ok) {
            bytes = ingest::kSideBytes;
            return false;
        }
        bytes = ingest::estimate_bytes(ingest::output_shape(c.kind, pr, o, cfg_.quality, cfg_.canvas_w, cfg_.canvas_h, cfg_.seq_fps), c.kind);
        return true;
    }
    ingest::DiskSpace disk() override {
        refresh_disk();
        return disk_;
    }
    uint64_t reserve(uint64_t total) override { return ingest::reserve_for(total, cfg_.reserve_bytes); }
    bool moving() override { return moving_; }
    void on_plan(const library::IngestPlan& plan) override {
        if (!blocks_.lib) return;
        library::IngestPlan q = plan;
        q.producer = static_cast<uint8_t>(cur_prod_);
        protocol::seq_write(blocks_.lib->plan_seq, blocks_.lib->plan, q);
        log("import plan: %s%s", q.message, q.dry_run ? " (dry run)" : q.refused ? " - REFUSED" : "");
    }
    library::IngestReply library_command(const library::IngestCommand& c) override {
        library::IngestReply r{};
        auto say = [&](library::IngestStatus st, const std::string& msg) {
            r.status = static_cast<uint8_t>(st);
            r.count = st == library::IngestStatus::Accepted ? 1 : 0;
            Blocks::copy_utf8(r.message, sizeof(r.message), msg);
            return r;
        };
        std::error_code ec;
        const fs::path to = from_utf8(c.path).lexically_normal();
        if (ingest::same_path(to, cfg_.root)) return say(library::IngestStatus::BadPath, "that already is the library folder");
        for (fs::path q = to.parent_path(); !q.empty() && q != q.root_path(); q = q.parent_path())
            if (ingest::same_path(q, cfg_.root)) return say(library::IngestStatus::BadPath, "the new place cannot be inside the library");
        if (fs::exists(to, ec) && !fs::is_directory(to, ec)) return say(library::IngestStatus::BadPath, "that is a file, not a folder");
        if (c.flags & library::kIngestMove) {
            if (!ingest::empty_or_missing(to)) return say(library::IngestStatus::BadPath, "the target folder must be empty (or not exist yet)");
            const uint64_t need = ingest::library_bytes(cfg_.root) + (1u << 20);
            const auto d = ingest::disk_space(to);
            const uint64_t res = ingest::reserve_for(d.total, cfg_.reserve_bytes);
            if (d.known && !ingest::fits(d.free, 0, need, res))
                return say(library::IngestStatus::NoSpace, "the library needs " + ingest::human_bytes(need) + " there; " +
                                                               ingest::human_bytes(d.free) + " free, " + ingest::human_bytes(res) + " kept free");
            pending_lib_ = LibraryJob{to, true};
            moving_ = true;
            blocks_.set_root(cfg_.root, library::MoveState::Copying, 0, 0, need, to, "starting the move");
            return say(library::IngestStatus::Accepted, "moving the library (" + ingest::human_bytes(need) + ") to " + ingest::u8path(to));
        }
        if (fs::exists(to / library::kDbFile, ec) && !(c.flags & library::kIngestForce))
            return say(library::IngestStatus::BadPath, "there is already a library there (send it with force to use it)");
        pending_lib_ = LibraryJob{to, false};
        moving_ = true;
        blocks_.set_root(cfg_.root, library::MoveState::Switching, 0, 0, 0, to, "switching");
        return say(library::IngestStatus::Accepted, "switching to the library in " + ingest::u8path(to));
    }

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
        // v3: the stored mode (D183): a Hold survives a restart.
        mode_req_.store(static_cast<int>(ingest::IngestOps::stored_mode(db_)));
        if (hold()) log("on hold (stored mode): nothing is imported until the mode is changed");
        library_bytes_ = ingest::library_bytes(cfg_.root);
        blocks_.set_root(cfg_.root, library::MoveState::None, 0, 0, 0, {}, "");
        refresh_disk();
        log("disk: %s free of %s, %s kept free; the library uses %s", ingest::human_bytes(disk_.free).c_str(),
            ingest::human_bytes(disk_.total).c_str(), ingest::human_bytes(reserve_bytes_).c_str(),
            ingest::human_bytes(library_bytes_).c_str());
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
            if (pending_lib_) {  // v3: a Library command (switch / move) runs between files
                run_library_job(settle);
                continue;
            }
            if (now_us() >= next_scan_) {
                scan(settle);
                next_scan_ = now_us() + 2000000;
                refresh_disk();
                publish_progress();
            }
            if (!queue_.empty() && !hold()) {
                idle_logged = false;
                // v3: the disk guard (library-db.md §11.1): the next file must fit above the reserve.
                if (disk_low_ && now_us() < next_disk_check_) {
                    Sleep(200);
                    if (cfg_.once) break;
                    continue;
                }
                const int64_t id = queue_.front();
                if (!disk_ok_for(id)) {
                    set_disk_low(true, id);
                    continue;
                }
                set_disk_low(false, 0);
                queue_.pop_front();
                update_counts();
                const uint64_t t0 = now_us();
                outcome_ = Outcome::Cancelled;
                process(id);
                if (outcome_ != Outcome::Deferred)
                    batch_.file_done(std::max<int64_t>(cur_est_, cur_written_), (now_us() - t0) / 1e6, outcome_ == Outcome::Ready ? cur_written_ : 0);
                cur_written_ = 0, cur_est_ = 0, cur_fp_ = 0;
                cur_file_.clear();
                update_counts();
                publish_progress();
                continue;
            }
            if (cfg_.once && first_scan_done_ && settle_pending_ == 0 && (queue_.empty() || hold())) {
                if (hold() && !queue_.empty()) log("--once: on hold with %zu queued, stopping", queue_.size());
                break;
            }
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
        cur_prod_ = prod;
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
                b->stored_mode.store(static_cast<uint32_t>(mode_req_.load()));
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
        queue_bytes_ = ops_ ? ops_->queue_need() : 0;
        if (auto* b = blocks_.lib) {
            b->queue_bytes.store(queue_bytes_);
            b->library_bytes.store(library_bytes_);
        }
    }

    // ---- v3: disk guard, progress, library switch / move (library-db.md §11) ----
    void refresh_disk() {
        disk_ = ingest::disk_space(cfg_.root);
        reserve_bytes_ = disk_.known ? ingest::reserve_for(disk_.total, cfg_.reserve_bytes) : 0;
        last_disk_us_ = now_us();
        if (auto* b = blocks_.lib) {
            b->disk_free.store(disk_.free);
            b->disk_total.store(disk_.total);
            b->disk_reserve.store(reserve_bytes_);
            b->library_bytes.store(library_bytes_);
        }
    }

    int64_t estimate_row(const ClipRecord& r) {
        if (r.stage_done & library::kStageConvert) return ingest::kSideBytes;
        if (r.stage_done & library::kStageProbe) {
            library::Probe pr = r.probe;
            if (r.kind == Kind::Vector) pr.width = cfg_.canvas_w * 2, pr.height = cfg_.canvas_h * 2;
            return ingest::estimate_bytes(ingest::output_shape(r.kind, pr, r.import, cfg_.quality, cfg_.canvas_w, cfg_.canvas_h, cfg_.seq_fps), r.kind);
        }
        return std::max<int64_t>(r.import.est_bytes, ingest::kSideBytes);
    }

    bool disk_ok_for(int64_t id) {
        ClipRecord r;
        if (!db_.get(id, r)) return true;
        refresh_disk();
        return !disk_.known || ingest::fits(disk_.free, 0, static_cast<uint64_t>(estimate_row(r)), reserve_bytes_);
    }

    void set_disk_low(bool low, int64_t id) {
        if (low) {
            next_disk_check_ = now_us() + 2000000;
            if (!disk_low_) {
                ClipRecord r;
                db_.get(id, r);
                const std::string msg = "disk low: " + ingest::human_bytes(disk_.free) + " free, " + ingest::human_bytes(reserve_bytes_) +
                                        " kept free; \"" + r.name + "\" needs ~" + ingest::human_bytes(static_cast<uint64_t>(estimate_row(r)));
                log("%s - the import waits (free space, Clear queue or a library move lets it go on)", msg.c_str());
                blocks_.set_job(r.name, "disk low", 0, msg);
            }
        } else if (disk_low_) {
            log("disk space is fine again: the import goes on");
        }
        disk_low_ = low;
        if (auto* b = blocks_.lib) b->disk_low.store(low ? 1 : 0);
    }

    // During a conversion (from run_tool's poll, every 50 ms): stop when the free space falls
    // below the reserve, so a file is never written half (library-db.md §11.1).
    bool disk_abort_check() {
        if (disk_abort_) return true;
        if (now_us() - last_disk_us_ < 500000) return false;
        refresh_disk();
        if (disk_.known && disk_.free < reserve_bytes_) {
            disk_abort_ = true;
            log("disk low during the conversion (%s free, %s kept free): stopping it", ingest::human_bytes(disk_.free).c_str(),
                ingest::human_bytes(reserve_bytes_).c_str());
        }
        return disk_abort_;
    }

    // The clip goes back to the front of the queue untouched (never failed, never half-written).
    bool defer_for_disk(ClipRecord& r, const std::string& why) {
        std::error_code ec;
        for (auto it = fs::directory_iterator(cfg_.root / "work", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (ingest::u8path(it->path().filename()).rfind(r.hash, 0) == 0) fs::remove(it->path(), ec);
        r.state = State::Queued;
        r.error.clear();
        db_.upsert(r);
        queue_.push_front(r.id);
        outcome_ = Outcome::Deferred;
        log("  %s: back in the queue (%s)", r.name.c_str(), why.c_str());
        refresh_disk();
        set_disk_low(true, r.id);
        return false;
    }

    void publish_progress() {
        if (!blocks_.lib) return;
        library::IngestProgress p{};
        p.done = batch_.done();
        p.total = batch_.total();
        p.clip = current_;
        p.file_progress = cur_fp_;
        const int64_t remaining = static_cast<int64_t>(queue_bytes_) + static_cast<int64_t>((1.0f - cur_fp_) * static_cast<float>(cur_est_));
        p.eta_s = (queue_.empty() && current_ == 0) ? 0 : batch_.eta_s(remaining);
        p.bytes_written = batch_.written() + static_cast<uint64_t>(std::max<int64_t>(0, cur_written_));
        p.file_bytes = static_cast<uint64_t>(std::max<int64_t>(0, cur_est_));
        Blocks::copy_utf8(p.file, sizeof(p.file), cur_file_);
        p.error_clip = err_clip_;
        Blocks::copy_utf8(p.error, sizeof(p.error), err_msg_);
        protocol::seq_write(blocks_.lib->progress_seq, blocks_.lib->progress, p);
        last_progress_us_ = now_us();
    }

    void stage_progress(const char* stage, float p) {
        cur_fp_ = ingest::file_progress(stage, p);
        if (now_us() - last_progress_us_ >= 250000 || p <= 0.0f || p >= 1.0f) publish_progress();
    }

    // Opens (or creates) the library in `to` and makes it the one in use.
    bool open_library(const fs::path& to, std::string& err) {
        std::error_code ec;
        for (const char* d : {"originals", "hap", "stills", "meta", "thumbs", "work"}) fs::create_directories(to / d, ec);
        db_.close();
        if (!db_.open(to / library::kDbFile, false, &err)) return false;
        for (auto& w : cfg_.watch)
            if (ingest::same_path(w, cfg_.root / "originals")) w = to / "originals";
        cfg_.root = to;
        ops_ = std::make_unique<ingest::IngestOps>(db_, cfg_.root, cfg_.watch, *this);
        queue_.clear();
        known_.clear();
        first_scan_done_ = false;
        next_scan_ = 0;
        batch_.reset();
        {
            std::lock_guard lock(g_log_mu);
            if (g_log) std::fclose(g_log);
            g_log = _wfsopen((cfg_.root / "ingest.log").c_str(), L"a", _SH_DENYWR);
        }
        library_bytes_ = ingest::library_bytes(cfg_.root);
        if (!cfg_.explicit_root) {
            if (library::save_location(library::location_file(), cfg_.root)) log("saved the library location: %s", ingest::u8path(cfg_.root).c_str());
            else log("cannot save the library location in %s", ingest::u8path(library::location_file()).c_str());
        }
        db_.bump_ready_seq();
        if (auto* b = blocks_.lib) b->ready_seq.fetch_add(1, std::memory_order_acq_rel);
        refresh_disk();
        return true;
    }

    // A Library command (69), between files on the worker thread (library-db.md §11.5).
    void run_library_job(SettleTracker& settle) {
        const LibraryJob job = *pending_lib_;
        pending_lib_.reset();
        const fs::path old = cfg_.root;
        std::string err;
        settle = SettleTracker(2000);
        if (!job.move) {
            if (open_library(job.to, err)) {
                mode_req_.store(static_cast<int>(ingest::IngestOps::stored_mode(db_)));
                log("switched to the library in %s", ingest::u8path(cfg_.root).c_str());
                blocks_.set_root(cfg_.root, library::MoveState::Done, 1, 0, 0, {}, "switched to this library");
            } else {
                log("cannot open the library in %s: %s - staying in %s", ingest::u8path(job.to).c_str(), err.c_str(), ingest::u8path(old).c_str());
                std::string e2;
                open_library(old, e2);
                blocks_.set_root(cfg_.root, library::MoveState::Failed, 0, 0, 0, job.to, "cannot open that library: " + err);
            }
            moving_ = false;
            update_counts();
            return;
        }
        // The move: checkpoint and close the DB, copy everything, compare, then switch.
        log("moving the library from %s to %s", ingest::u8path(old).c_str(), ingest::u8path(job.to).c_str());
        db_.checkpoint();
        db_.close();
        const auto plan = ingest::plan_move(old);
        uint64_t last_pub = 0;
        const bool ok = ingest::copy_library(old, job.to, plan, [&](uint64_t done, uint64_t total) {
            if (stopping()) return false;
            if (now_us() - last_pub >= 250000) {
                last_pub = now_us();
                const float f = total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f;
                blocks_.set_root(old, library::MoveState::Copying, f, done, total, job.to,
                                 "copying " + ingest::human_bytes(done) + " of " + ingest::human_bytes(total));
                poll_commands(0);  // Mode is applied; everything else replies Busy
            }
            return true;
        }, err);
        if (!ok) {
            log("the move failed: %s - the library stays in %s", err.c_str(), ingest::u8path(old).c_str());
            std::string e2;
            if (!db_.open(old / library::kDbFile, false, &e2)) log("cannot reopen the library DB: %s", e2.c_str());
            blocks_.set_root(old, library::MoveState::Failed, 0, 0, plan.bytes, job.to, "the move failed: " + err);
            moving_ = false;
            resume_unfinished();
            update_counts();
            return;
        }
        blocks_.set_root(old, library::MoveState::Switching, 1, plan.bytes, plan.bytes, job.to, "switching to the new folder");
        const int mode = mode_req_.load();
        if (!open_library(job.to, err)) {
            log("cannot open the moved library: %s - staying in %s", err.c_str(), ingest::u8path(old).c_str());
            std::string e2;
            open_library(old, e2);
            blocks_.set_root(old, library::MoveState::Failed, 0, 0, plan.bytes, job.to, "cannot open the moved library: " + err);
            moving_ = false;
            return;
        }
        static const char* kNames[] = {"auto", "full", "background", "hold"};
        db_.meta_set("mode", kNames[std::clamp(mode, 0, 3)]);  // a Mode sent during the copy
        // The old copy goes once the renderer that loaded it has restarted or stopped.
        const auto eng = blocks_.sample_engine();
        db_.meta_set("old_root", nlohmann::json{{"path", ingest::u8path(old)},
                                                {"engine_pid", eng.present && eng.heartbeat_age_ms < 2000 ? eng.pid : 0}}.dump());
        library::write_moved_redirect(old, cfg_.root);
        // Originals are never moved: the old drop folder stays watched where it is.
        if (!ingest::empty_or_missing(old / "originals")) {
            auto j = nlohmann::json::parse(db_.meta_get("watch"), nullptr, false);
            if (!j.is_array()) j = nlohmann::json::array();
            j.push_back(ingest::u8path(old / "originals"));
            db_.meta_set("watch", j.dump());
        }
        log("moved the library to %s (%s); the old copy is deleted when the renderer has restarted", ingest::u8path(cfg_.root).c_str(),
            ingest::human_bytes(plan.bytes).c_str());
        blocks_.set_root(cfg_.root, library::MoveState::Done, 1, plan.bytes, plan.bytes, {},
                         "moved; the old copy is deleted after the renderer restarts");
        moving_ = false;
        update_counts();
    }

    // At every scan: delete a moved library's old copy once no renderer uses it.
    void cleanup_old_root() {
        const auto j = nlohmann::json::parse(db_.meta_get("old_root"), nullptr, false);
        if (!j.is_object()) return;
        const std::string path = j.value("path", std::string());
        const uint32_t pid = j.value("engine_pid", 0u);
        const auto eng = blocks_.sample_engine();
        if (eng.present && eng.heartbeat_age_ms < 2000 && eng.pid == pid && pid != 0) return;  // still the old renderer
        const int left = ingest::delete_old_library(from_utf8(path));
        if (left == 0) {
            db_.meta_set("old_root", "");
            log("deleted the old library copy in %s", path.c_str());
        } else if (!old_root_logged_) {
            old_root_logged_ = true;
            log("the old library copy in %s: %d file(s) still in use, retrying", path.c_str(), left);
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
        cleanup_old_root();
        if (now_us() >= next_bytes_us_) {  // keep library_bytes current (Remove, Clear queue, other tools)
            library_bytes_ = ingest::library_bytes(cfg_.root);
            next_bytes_us_ = now_us() + 10000000;
        }
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
            queue(r.id);
        }
        if (!queue_.empty()) log("resuming %zu unfinished import(s)", queue_.size());
    }

    void enqueue(const ingest::Candidate& c) {
        const auto res = ops_->enqueue_scanned(c);  // v3: with the disk guard
        if (res.no_space)
            log("not enough disk space for %s (needs ~%s): left out; Rescan tries again", ingest::u8path(c.path).c_str(),
                ingest::human_bytes(static_cast<uint64_t>(res.est_bytes)).c_str());
        else if (res.id == 0) log("cannot read or queue %s - skipped", ingest::u8path(c.path).c_str());
        else if (res.queued) log("queued %s (%s)", ingest::u8path(c.path).c_str(), library::kind_name(c.kind));
        if (ops_->take_changed()) update_counts();
    }

    // ---- one clip, all stages ----
    bool fail(ClipRecord& r, const std::string& why) {
        // v3: no space is never a failure: the clip waits in the queue (library-db.md §11.1).
        if (disk_abort_ || ingest::is_disk_full(why)) return defer_for_disk(r, disk_abort_ ? "disk low" : "the disk is full");
        std::string tech = why.substr(0, 300);
        std::replace(tech.begin(), tech.end(), '\n', ' ');
        r.state = State::Failed;
        r.error = ingest::plain_error(cur_stage_, tech);  // plain words for the GUI; the detail goes in the log
        db_.upsert(r);
        blocks_.set_job(r.name, "failed", 0, r.error);
        err_clip_ = r.id;
        err_msg_ = r.error;
        outcome_ = Outcome::Failed;
        log("FAILED %s: %s (%s)", r.name.c_str(), r.error.c_str(), tech.c_str());
        return false;
    }

    void set_state(ClipRecord& r, State s, const char* stage) {
        r.state = s;
        db_.upsert(r);
        blocks_.set_job(r.name, stage, 0);
        cur_stage_ = stage;
        stage_progress(stage, 0);
    }

    ingest::RunOptions run_opts(std::function<void(const uint8_t*, size_t)> out = nullptr) {
        ingest::RunOptions o;
        o.background = background();
        o.should_pause = [this] {
            poll_commands(current_);  // run_tool polls on this (the worker) thread
            return paused_.load();
        };
        o.should_stop = [this] { return aborted() || disk_abort_check(); };
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
        outcome_ = Outcome::Cancelled;
        disk_abort_ = false;
        cur_stage_ = "probing";
        cur_file_ = ingest::u8path(from_utf8(r.source).filename());
        cur_est_ = estimate_row(r);
        cur_written_ = 0;
        cur_fp_ = 0;
        publish_progress();
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
                    const int mh = r.import.max_h > 0 ? std::min(cfg_.canvas_h * 2, r.import.max_h) : cfg_.canvas_h * 2;
                    const auto res = ingest::run_tool(tools_.ffmpeg, ingest::still_png_args(r.source, ingest::u8path(png_tmp),
                                                                                             cfg_.canvas_w * 2, mh),
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
        cur_est_ = estimate_row(r);  // v3: the real numbers now

        // v3: the disk guard before the big write (library-db.md §11.1).
        if (!(r.stage_done & library::kStageConvert)) {
            refresh_disk();
            if (disk_.known && !ingest::fits(disk_.free, 0, static_cast<uint64_t>(cur_est_), reserve_bytes_))
                return defer_for_disk(r, "needs ~" + ingest::human_bytes(static_cast<uint64_t>(cur_est_)));
        }

        // 3. Convert to HAP (work/ -> hap/).
        const fs::path hap = cfg_.root / "hap" / (h + ".mov");
        if (!(r.stage_done & library::kStageConvert) || !fs::exists(hap, ec)) {
            set_state(r, State::Converting, "converting");
            const fs::path tmp = work / (h + ".mov");
            const bool alpha = r.probe.alpha || r.kind == Kind::Animation;
            r.codec = alpha ? "hap_alpha" : (r.import.codec.empty() ? cfg_.quality : r.import.codec);
            const bool already_hap = r.kind == Kind::Video && r.probe.codec == "hap" && r.probe.width % 4 == 0 && r.probe.height % 4 == 0 &&
                                     (r.import.max_h == 0 || r.probe.height <= r.import.max_h) &&
                                     ingest::conform_fps(r.probe.fps, r.import.conform_hz) == 0;
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
                // v3 options (library-db.md §11.4)
                spec.out_max_h = r.import.max_h;
                const double in_fps = r.kind == Kind::Sequence ? cfg_.seq_fps : r.kind == Kind::Animation ? spec.fps : r.probe.fps;
                spec.conform_fps = ingest::conform_fps(in_fps, r.import.conform_hz);
                spec.progress = true;
                const auto shape = ingest::output_shape(r.kind, r.probe, r.import, cfg_.quality, cfg_.canvas_w, cfg_.canvas_h, cfg_.seq_fps);
                const double out_frames = static_cast<double>(std::max<int64_t>(1, shape.frames));
                ingest::ProgressParser pp;
                const auto res = ingest::run_tool(tools_.ffmpeg, ingest::convert_args(spec), run_opts([&](const uint8_t* q, size_t n) {
                    pp.feed(q, n);
                    cur_written_ = pp.total_size;
                    stage_progress("converting", static_cast<float>(pp.frame / out_frames));
                }));
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
        cur_written_ = static_cast<int64_t>(fs::file_size(hap, ec));
        stage_progress("converting", 1.0f);

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
        outcome_ = Outcome::Ready;
        {
            int64_t sz = static_cast<int64_t>(fs::file_size(hap, ec));
            for (const auto& rel : {r.thumb_path, r.still_path, "thumbs/" + h + "-strip.jpg", "meta/" + h + ".json"})
                if (!rel.empty() && fs::exists(abs(rel), ec)) sz += static_cast<int64_t>(fs::file_size(abs(rel), ec));
            cur_written_ = sz;
            library_bytes_ += static_cast<uint64_t>(sz);
        }
        cur_fp_ = 1.0f;
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
    std::atomic<int> mode_req_{0};                  // IngestModeRequest (Mode command; v3: stored in the DB)
    // ---- v3 (wave 7), worker thread only ----
    enum class Outcome { Ready, Failed, Cancelled, Deferred };
    struct LibraryJob {
        fs::path to;
        bool move = false;
    };
    std::optional<LibraryJob> pending_lib_;
    bool moving_ = false;                           // a Library job is pending / running: commands reply Busy
    Outcome outcome_ = Outcome::Cancelled;
    ingest::BatchProgress batch_;
    ingest::DiskSpace disk_;
    uint64_t reserve_bytes_ = 0, library_bytes_ = 0, queue_bytes_ = 0;
    uint64_t last_disk_us_ = 0, next_disk_check_ = 0, last_progress_us_ = 0, next_bytes_us_ = 0;
    bool disk_low_ = false, disk_abort_ = false, old_root_logged_ = false;
    int64_t cur_est_ = 0, cur_written_ = 0, err_clip_ = 0;
    float cur_fp_ = 0;
    std::string cur_file_, err_msg_;
    const char* cur_stage_ = "probing";
    int cur_prod_ = 0;
    uint64_t next_scan_ = 0;
    int chunks_ = 4;
    int settle_pending_ = 0;
    bool first_scan_done_ = false;
    bool thread_bg_ = false;
};

// ---- himmel_ingest --send VERB ...: one command to the running worker (producer slot Tool) ----
// import PATH [watch] [OPTIONS] | import-list FILE [OPTIONS] | rescan [ID] | remove ID | relink ID|0 PATH [force]
// | reanalyse [ID] [thumbs] | retry [ID] | mode auto|full|background|hold | unwatch PATH | clear
// | library DIR [move] [force]. OPTIONS (v3): dry fit skip hapq max=H (or 720) conform50 conform60.
// Prints the reply; exit 0 = Ok/Accepted. `library DIR move` waits for the move and prints its end.
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
    // v3 import options (library-db.md §11.4)
    auto import_flags = [&]() {
        uint16_t f = 0;
        if (has("watch")) f |= library::kIngestWatch;
        if (has("dry")) f |= library::kIngestDryRun;
        if (has("fit")) f |= library::kIngestFit;
        if (has("skip")) f |= library::kIngestSkipKnown;
        if (has("hapq")) f |= library::kIngestHapQ;
        return f;
    };
    auto import_opts = [&](library::IngestCommand& cmd) {
        for (int k = 4; k < argc; ++k) {
            const std::string a = arg(k);
            if (a == "720" || a == "1080" || a == "540") cmd.i = std::atoi(a.c_str());
            else if (a.rfind("max=", 0) == 0) cmd.i = std::atoi(a.c_str() + 4);
            else if (a == "conform50") cmd.conform_hz = 50;
            else if (a == "conform60") cmd.conform_hz = 60;
        }
    };
    if (verb == "import" || verb == "import-list") {
        ok = library::make_ingest(c, T::Import, 0, abs_path(3).c_str(),
                                  static_cast<uint16_t>(import_flags() | (verb == "import-list" ? library::kIngestList : 0)));
        import_opts(c);
    } else if (verb == "clear") ok = library::make_ingest(c, T::ClearQueue);
    else if (verb == "library")
        ok = library::make_ingest(c, T::Library, 0, abs_path(3).c_str(),
                                  static_cast<uint16_t>((has("move") ? library::kIngestMove : 0) | (has("force") ? library::kIngestForce : 0)));
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
        std::printf("usage: himmel_ingest --send import PATH [watch] [OPTIONS] | import-list FILE [OPTIONS] | rescan [ID]\n"
                    "                     | remove ID | relink ID|0 PATH [force] | reanalyse [ID] [thumbs] | retry [ID]\n"
                    "                     | mode auto|full|background|hold | unwatch PATH | clear | library DIR [move] [force]\n"
                    "  OPTIONS: dry (estimate only)  fit (queue what fits)  skip (skip known, removed too)\n"
                    "           hapq  720 | max=H (largest height)  conform50 | conform60\n");
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
                                               "Duplicate", "Failed", "Interrupted", "BadValue", "NoSpace", "Busy"};
                std::printf("%s count=%d clip=%lld: %s\n", r.status < 12 ? kNames[r.status] : "?", r.count,
                            static_cast<long long>(r.clip), r.message);
                if (verb == "import" || verb == "import-list") {
                    library::IngestPlan plan{};
                    if (protocol::seq_read(b->plan_seq, b->plan, plan) && plan.seq == seq)
                        std::printf("plan: %u new, %u known, %u fit, need %.2f GB, queue %.2f GB, free %.2f GB, reserve %.2f GB\n",
                                    plan.clips, plan.known, plan.fit_clips, plan.need_bytes / 1e9, plan.queue_bytes / 1e9,
                                    plan.free_bytes / 1e9, plan.reserve_bytes / 1e9);
                }
                if (verb == "library" && r.status == 1) {  // follow the switch / move to its end
                    for (int w = 0; w < 36000; ++w) {
                        auto lr = std::make_unique<library::LibraryRoot>();
                        if (protocol::seq_read(b->root_seq, b->root, *lr) && b->root_seq.load() > 0) {
                            const auto st = static_cast<library::MoveState>(lr->move_state);
                            if (w % 10 == 0 || st == library::MoveState::Done || st == library::MoveState::Failed)
                                std::printf("library: state %u, %.0f %%, %s (in %s)\n", lr->move_state, lr->move_progress * 100.0,
                                            lr->move_message, lr->path);
                            if (st == library::MoveState::Done) return 0;
                            if (st == library::MoveState::Failed) return 1;
                        }
                        Sleep(100);
                    }
                }
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
    if (cfg.help || !cfg.error.empty()) {  // before anything is opened, created or started
        if (!cfg.error.empty()) std::printf("himmel_ingest: %s\n", cfg.error.c_str());
        print_usage();
        return 2;
    }
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
        {
            const auto d = ingest::disk_space(cfg.root);
            log("disk: %s free of %s, %s kept free; the library uses %s; stored mode %s", ingest::human_bytes(d.free).c_str(),
                ingest::human_bytes(d.total).c_str(), ingest::human_bytes(ingest::reserve_for(d.total, cfg.reserve_bytes)).c_str(),
                ingest::human_bytes(ingest::library_bytes(cfg.root)).c_str(),
                db.meta_get("mode").empty() ? "auto" : db.meta_get("mode").c_str());
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
