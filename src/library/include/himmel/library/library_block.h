// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The library block "Local\HimmelLibrary" (docs/interfaces/library-db.md §6).
// Written by himmel_ingest (it creates the block, header last); read by the
// supervisor (heartbeat / stop), the engine's library feed (ready_seq) and the
// GUI bridge (import queue). Fixed-size POD, no heap, lock-free atomics.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "himmel/protocol/shm.h"

namespace himmel::library {

inline constexpr const wchar_t* kLibraryShmName = L"Local\\HimmelLibrary";
inline constexpr uint32_t kLibraryMagic = 0x424C4D48;  // "HMLB"
// 2: + IngestChannel channels[] (commands 60..69)
// 3: + disk guard, stored mode, progress, import plan, library root and move (wave 7, appended)
inline constexpr uint32_t kLibraryVersion = 3;

enum class IngestMode : uint32_t { Idle = 0, Full = 1, Background = 2, High = 3, Hold = 4 };

// ---- commands to the ingest worker (docs/interfaces/library-db.md §10) -----------------
// The numbers are MEDIA's CommandType range 60..69 (command-protocol.md). They travel in
// this block, not in Local\HimmelCommands: the ingest worker is their only consumer, and
// the engine refuses them in its own rings (UnknownType). Append-only; never renumber.
enum class IngestCommandType : uint16_t {
    Import = 60,     // path = a file or a folder (recursive). flags kIngestWatch = keep watching the folder
    Rescan = 61,     // clip = library id: check its original; clip = 0: every watch folder now + every original
    Remove = 62,     // clip = library id: out of the library (derived files deleted; the original is never touched)
    Relink = 63,     // clip = id + path = the file; clip = 0 + path = a folder to search for every missing original.
                     //   flags kIngestForce = accept other content (the clip is imported again under the same id)
    Reanalyse = 64,  // clip = id, 0 = every ready clip. flags kIngestThumbs = thumbnails too
    Retry = 65,      // clip = id, 0 = every failed clip
    Mode = 66,       // i = IngestModeRequest
    Unwatch = 67,    // path = a folder added with Import + kIngestWatch (the default folder cannot be removed)
    // v3 (wave 7):
    ClearQueue = 68, // every queued / unfinished import leaves the queue at once (ready clips stay)
    Library = 69,    // path = a folder: the library moves there (kIngestMove) or the worker switches to it
};
inline constexpr uint16_t kIngestFirst = 60, kIngestLast = 69;

// IngestCommand.flags
inline constexpr uint16_t kIngestWatch = 1u << 0;   // Import: add the folder to the watch list (stored in the DB)
inline constexpr uint16_t kIngestForce = 1u << 1;   // Relink: the new file may have other content.
                                                    // Library (switch): use the library already in that folder
inline constexpr uint16_t kIngestThumbs = 1u << 2;  // Reanalyse: make the thumbnails again too
// v3 (wave 7), Import options. With them `i` = the largest output height (0 = keep the source size,
// 720 = "1080p -> 720p"), `conform_hz` = conform the frame rate (0 = off, 50 or 60).
inline constexpr uint16_t kIngestHapQ = 1u << 3;      // Import: HAP Q (better colour, about 2x the disk) instead of HAP
inline constexpr uint16_t kIngestSkipKnown = 1u << 4; // Import: content already in the library is skipped, removed clips too
inline constexpr uint16_t kIngestList = 1u << 5;      // Import: `path` is a UTF-8 text file, one absolute path per line
inline constexpr uint16_t kIngestDryRun = 1u << 6;    // Import: only estimate ("needs ~X GB for N clips"); queue nothing
inline constexpr uint16_t kIngestFit = 1u << 7;       // Import: queue the clips that fit above the reserve instead of refusing
inline constexpr uint16_t kIngestMove = 1u << 8;      // Library: move the library's files there (with progress)

enum class IngestModeRequest : int32_t {
    Auto = 0,        // background while a show runs, else full speed (the default)
    Full = 1,        // full speed even during a show (not show-safe; the GUI should warn)
    Background = 2,  // background even without a show
    Hold = 3,        // start no new work (the current ffmpeg job is suspended); Auto / Full / Background resume
};

inline constexpr int kIngestPathBytes = 1000;
struct IngestCommand {
    uint16_t type;          // IngestCommandType
    uint16_t flags;         // kIngest*
    uint32_t seq;           // producer's running number (1, 2, 3 ...); echoed in the reply
    int64_t clip;           // library id (the DB `clips.id`, not the engine catalog id); 0 = all / none
    int32_t i;              // integer argument (Mode; v3 Import: the largest output height, 0 = keep)
    uint32_t conform_hz;    // v3 Import: conform the frame rate to 50 / 60 Hz, 0 = off (was `reserved`, always 0)
    char path[kIngestPathBytes];  // UTF-8, NUL-terminated, absolute; "" when unused
};
static_assert(sizeof(IngestCommand) == 1024);

enum class IngestStatus : uint8_t {
    Ok = 0,          // done; `count` = rows changed
    Accepted = 1,    // valid, the work is queued (follow it in the DB / `job`); `count` = rows queued
    UnknownType = 2,
    BadId = 3,       // no such clip, or the clip is in a state the command does not fit
    BadPath = 4,     // empty, not absolute, not NUL-terminated, missing, or not importable
    Mismatch = 5,    // Relink: the file has other content (resend with kIngestForce)
    Duplicate = 6,   // Relink kIngestForce / Import: that content already is clip `clip`
    Failed = 7,      // I/O or DB error; see `message`
    Interrupted = 8, // the worker restarted while applying this command; it may be half done
    BadValue = 9,    // Mode out of range; v3: an Import option out of range
    // v3 (wave 7):
    NoSpace = 10,    // Import refused by the disk guard (the plan section has the numbers); Library: target too small
    Busy = 11,       // a library move is running; try again when LibraryBlock::move is done
};

struct IngestReply {
    uint32_t seq;           // the command's seq
    uint16_t type;          // the command's type
    uint8_t status;         // IngestStatus
    uint8_t reserved;
    int32_t count;          // rows changed / queued / relinked
    uint32_t reserved2;
    int64_t clip;           // the clip it is about (Duplicate: the existing clip)
    char message[104];      // one line, UTF-8, for the log / a toast
};
static_assert(sizeof(IngestReply) == 128);

inline constexpr int kIngestProducers = 8;     // = protocol::kProducers
inline constexpr uint32_t kIngestCommandRing = 16;
inline constexpr uint32_t kIngestReplyRing = 32;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas padding is intended
#endif
// One per producer slot: the producer writes `commands`, `pid`, `heartbeat_us`; the ingest
// worker reads `commands` and writes everything else.
struct IngestChannel {
    std::atomic<uint32_t> pid;            // producer's process id (0 = nobody)
    std::atomic<uint32_t> applying_seq;   // ingest: seq being applied (0 = none); left set = a crash mid-command
    std::atomic<uint32_t> done_seq;       // ingest: last seq replied to
    std::atomic<uint32_t> rejected;       // ingest: replies with status >= UnknownType
    std::atomic<uint64_t> heartbeat_us;   // producer's steady clock at its last push
    protocol::SpscRing<IngestCommand, kIngestCommandRing> commands;  // producer -> ingest
    protocol::SpscRing<IngestReply, kIngestReplyRing> replies;       // ingest -> producer (full = dropped, see done_seq)
};

struct IngestJob {
    char name[64];          // UTF-8 display name of the file being worked on ("" = none)
    char stage[16];         // probing | converting | analysing | thumbs
    float progress;         // 0..1 of the current stage
    uint32_t reserved;
    char last_error[128];   // last failure, one line
};

// ---- version 3 (wave 7): progress, the import plan, the library move ---------------------

// What the worker is doing now and for the whole batch (seqlock progress_seq). A batch starts
// when something is queued while the queue was empty, and ends when the queue is empty again.
struct IngestProgress {
    uint32_t done;            // n: files of this batch that are finished (ready, failed or cleared)
    uint32_t total;           // m: files in this batch (grows when more are queued)
    int64_t clip;             // library id of the file being worked on, 0 = none
    float file_progress;      // 0..1 over every stage of the current file
    int32_t eta_s;            // seconds until the queue is empty at the current speed, -1 = not known yet
    uint64_t bytes_written;   // bytes written to the library in this batch (finished files + the current one)
    uint64_t file_bytes;      // the current file's estimated final size
    char file[192];           // UTF-8 name (no folder) of the current original, "" = none
    int64_t error_clip;       // the last clip that failed, 0 = none since the worker started
    char error[192];          // its error in plain words ("The disk is full.", "This file is damaged ...")
};
static_assert(sizeof(IngestProgress) == 432);

// The disk estimate of the last Import (seqlock plan_seq): written for a dry run, a refusal and
// an accepted import alike, so the GUI can show "needs ~X GB for N clips".
struct IngestPlan {
    uint32_t seq;             // the Import command's seq
    uint8_t producer;         // its producer slot
    uint8_t dry_run;          // 1 = kIngestDryRun: nothing was queued
    uint8_t fits;             // 1 = everything fits above the reserve
    uint8_t refused;          // 1 = refused (NoSpace): nothing was queued
    uint32_t clips;           // new clips the import would queue (or queued)
    uint32_t known;           // already in the library (skipped)
    uint32_t fit_clips;       // how many of `clips` fit above the reserve (kIngestFit queues these)
    uint32_t reserved;
    uint64_t need_bytes;      // estimated bytes for `clips` (HAP ~0.5, HAP Q / Alpha ~1 byte per pixel per frame)
    uint64_t queue_bytes;     // estimated bytes the queue already needs before this import
    uint64_t free_bytes;      // free on the library's disk when estimated
    uint64_t reserve_bytes;   // the reserve the guard keeps free
    char message[128];        // one line, e.g. "needs ~17.2 GB for 86 clips; 7.3 GB free, 10.0 GB kept free"
};
static_assert(sizeof(IngestPlan) == 184);

enum class MoveState : uint32_t { None = 0, Copying = 1, Verifying = 2, Switching = 3, Done = 4, Failed = 5 };

// The library folder in use and a running move (seqlock root_seq).
struct LibraryRoot {
    char path[kIngestPathBytes];  // UTF-8, absolute: the library folder in use now
    uint32_t move_state;          // MoveState
    float move_progress;          // 0..1 of the copy
    uint64_t move_done_bytes, move_total_bytes;
    char move_to[kIngestPathBytes];  // the move's target ("" = none)
    char move_message[128];       // one line: "copying 3.1 of 17.2 GB", "moved; the old copy goes at the next start"
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
    // ---- version 2: commands to the ingest worker (docs/interfaces/library-db.md §10) ----
    IngestChannel channels[kIngestProducers];  // index = protocol::Producer (Gui = 1, Tool = 3, ...)
    // ---- version 3 (wave 7) ----
    std::atomic<uint32_t> stored_mode;      // IngestModeRequest in force; stored in the DB, so it survives a restart
    std::atomic<uint32_t> disk_low;         // 1 = the disk guard holds the queue: the next file would eat into the reserve
    std::atomic<uint64_t> disk_free;        // bytes free on the library's disk (updated every 2 s)
    std::atomic<uint64_t> disk_total;       // size of that disk
    std::atomic<uint64_t> disk_reserve;     // bytes the guard keeps free: max(10 GB, 10 % of the disk) by default
    std::atomic<uint64_t> library_bytes;    // bytes the library uses (hap/ stills/ thumbs/ meta/ + the DB)
    std::atomic<uint64_t> queue_bytes;      // estimated bytes the queue still has to write
    std::atomic<uint32_t> progress_seq;
    std::atomic<uint32_t> plan_seq;
    IngestProgress progress;                // seqlock progress_seq
    IngestPlan plan;                        // seqlock plan_seq
    std::atomic<uint32_t> root_seq;         // bumps (by 2) on every change of `root` (a switch or a move step)
    std::atomic<uint32_t> reserved3;
    LibraryRoot root;                       // seqlock root_seq
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Producer side (no allocation): builds a command. `path` may be null. False when the
// path does not fit (kIngestPathBytes - 1 bytes of UTF-8).
inline bool make_ingest(IngestCommand& c, IngestCommandType t, int64_t clip = 0, const char* path = nullptr,
                        uint16_t flags = 0, int32_t i = 0) {
    std::memset(&c, 0, sizeof(c));
    c.type = static_cast<uint16_t>(t);
    c.flags = flags;
    c.clip = clip;
    c.i = i;
    if (path) {
        const size_t n = std::strlen(path);
        if (n >= sizeof(c.path)) return false;
        std::memcpy(c.path, path, n + 1);
    }
    return true;
}

// Producer side: stamps seq / pid / heartbeat and pushes. False when the ring is full.
inline bool push_ingest(IngestChannel& ch, IngestCommand& c, uint32_t seq, uint32_t pid, uint64_t now_us) {
    c.seq = seq;
    ch.pid.store(pid, std::memory_order_relaxed);
    ch.heartbeat_us.store(now_us, std::memory_order_relaxed);
    return ch.commands.push(c);
}

}  // namespace himmel::library
