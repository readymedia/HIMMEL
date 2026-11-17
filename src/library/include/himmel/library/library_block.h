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
inline constexpr uint32_t kLibraryVersion = 2;  // 2: + IngestChannel channels[] (commands 60..69)

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
    // 68, 69: free (stars / tags, Prepare Show priority)
};
inline constexpr uint16_t kIngestFirst = 60, kIngestLast = 69;

// IngestCommand.flags
inline constexpr uint16_t kIngestWatch = 1u << 0;   // Import: add the folder to the watch list (stored in the DB)
inline constexpr uint16_t kIngestForce = 1u << 1;   // Relink: the new file may have other content
inline constexpr uint16_t kIngestThumbs = 1u << 2;  // Reanalyse: make the thumbnails again too

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
    int32_t i;              // integer argument (Mode)
    uint32_t reserved;
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
    BadValue = 9,    // Mode out of range
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
