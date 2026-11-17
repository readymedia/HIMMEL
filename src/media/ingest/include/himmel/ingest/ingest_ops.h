// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The ingest commands 60..69 (docs/interfaces/library-db.md §10): what each one
// does to the library DB and folder, and the transport over the library block's
// channels. No ffmpeg here; the worker (ingest_main.cpp) does the conversions.
// Not thread-safe: one IngestOps per worker thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "himmel/ingest/scan.h"
#include "himmel/library/library_block.h"
#include "himmel/library/library_db.h"

namespace himmel::ingest {

namespace fs = std::filesystem;

// What the commands need from the worker that owns the queue.
class OpsHost {
public:
    virtual ~OpsHost() = default;
    virtual void queue(int64_t id) = 0;      // add to the work queue (no duplicates)
    virtual void unqueue(int64_t id) = 0;    // drop from the work queue
    virtual void rescan_now() = 0;           // forget what was seen; scan the watch folders at once
    virtual void set_mode(library::IngestModeRequest m) = 0;
};

struct EnqueueResult {
    int64_t id = 0;
    bool queued = false;    // new row, revived tombstone, or queued again
    bool known = false;     // the content was already in the library
};

class IngestOps {
public:
    // `fixed_watch`: the default folder and --watch folders (cannot be unwatched).
    IngestOps(library::LibraryDb& db, fs::path root, std::vector<fs::path> fixed_watch, OpsHost& host);

    library::IngestReply apply(const library::IngestCommand& c);

    // Building blocks the worker uses too.
    EnqueueResult enqueue(const Candidate& c, bool revive_removed);
    std::vector<fs::path> watch_folders();          // fixed + stored (meta 'watch')
    std::vector<fs::path> skip_folders() const;     // the library's own folders
    int retry_pending_deletes();                    // files that were busy at Remove time
    bool take_changed() { const bool c = changed_; changed_ = false; return c; }

    // Does this command touch the clip being worked on (so it must wait for the job)?
    // `cancel` = the job should be cancelled for it.
    static bool touches(const library::IngestCommand& c, int64_t current, bool& cancel);

private:
    library::IngestReply import_path(const library::IngestCommand& c, const fs::path& p);
    library::IngestReply rescan(int64_t id);
    library::IngestReply remove(int64_t id);
    library::IngestReply relink_one(int64_t id, const fs::path& p, bool force);
    library::IngestReply relink_folder(const fs::path& dir);
    library::IngestReply reanalyse(int64_t id, bool thumbs);
    library::IngestReply retry(int64_t id);
    library::IngestReply unwatch(const fs::path& p);

    int check_source(library::ClipRecord& r);       // 1 = source_missing changed
    void delete_derived(const library::ClipRecord& r);
    bool candidate_for(const fs::path& p, Candidate& out);
    std::string hash_of(const Candidate& c);
    std::vector<std::string> json_list(const std::string& key);
    void set_json_list(const std::string& key, const std::vector<std::string>& v);

    library::LibraryDb& db_;
    fs::path root_;
    std::vector<fs::path> fixed_watch_;
    OpsHost& host_;
    bool changed_ = false;
};

bool same_path(const fs::path& a, const fs::path& b);  // case-insensitive, normalised

// ---- transport: the worker side of LibraryBlock::channels ------------------------------
// The worker is the only consumer. At most one command per channel is taken and not yet
// replied (applying_seq), so a restart can reply Interrupted for exactly that one.
class CommandPump {
public:
    explicit CommandPump(library::LibraryBlock* b) : b_(b) {}
    // At start-up: a command left half-applied by a dead worker gets an Interrupted reply.
    int recover();
    // Takes the next waiting command (round-robin over the channels). False = none.
    bool take(int& producer, library::IngestCommand& c);
    void reply(int producer, const library::IngestCommand& c, library::IngestReply r);

private:
    library::LibraryBlock* b_;
    int next_ = 0;
};

}  // namespace himmel::ingest
