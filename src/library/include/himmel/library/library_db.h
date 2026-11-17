// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The SQLite index himmel-library.db (docs/interfaces/library-db.md §3).
// Writer: himmel_ingest only. Readers (the engine's feed, the GUI bridge) open
// it read-only; WAL mode means they never block the writer. Not thread-safe:
// one LibraryDb per thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "himmel/library/record.h"

struct sqlite3;

namespace himmel::library {

inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kDbFile = "himmel-library.db";

class LibraryDb {
public:
    LibraryDb() = default;
    ~LibraryDb() { close(); }
    LibraryDb(const LibraryDb&) = delete;
    LibraryDb& operator=(const LibraryDb&) = delete;

    // Writer: creates the file and the schema. Reader: fails when the file is missing.
    bool open(const std::filesystem::path& file, bool read_only, std::string* err = nullptr);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string& last_error() const { return err_; }

    // ---- read ----
    bool get(int64_t id, ClipRecord& out);
    bool find_by_hash(const std::string& hash, ClipRecord& out);
    // Rows in id order; `state` < 0 = every state. after_id: only rows with a larger id.
    std::vector<ClipRecord> list(int state = -1, int64_t after_id = 0, int limit = -1);
    int count(int state = -1);
    // Ready rows that turned ready after `ready_order` (the engine feed's cursor), in that order.
    std::vector<ClipRecord> list_ready_after(int64_t ready_order, int limit = -1);
    uint64_t ready_seq();

    // ---- write (himmel_ingest) ----
    // Inserts when r.id == 0 (sets r.id and makes r.name unique), else updates every field.
    bool upsert(ClipRecord& r);
    bool remove(int64_t id);
    std::string unique_name(const std::string& base, int64_t self_id = 0);
    void bump_ready_seq();
    bool begin();
    bool commit();
    // v3: writes the WAL back into the DB file (before a library move copies it).
    bool checkpoint();

    // v2: the meta table ('watch', 'pending_delete': JSON arrays).
    std::string meta_get(const std::string& key);
    bool meta_set(const std::string& key, const std::string& value);
    // The size / mtime last cached for a path (relink by size).
    bool source_info(const std::string& path, int64_t& size, int64_t& mtime);

    // path -> hash cache, so a rescan does not re-hash unchanged files.
    bool cached_hash(const std::string& path, int64_t size, int64_t mtime, std::string& hash);
    void cache_hash(const std::string& path, int64_t size, int64_t mtime, const std::string& hash);

    // Drops every clip row and rebuilds them from <root>/meta/*.json: a sidecar
    // whose play file exists becomes a ready row. Returns the rows made.
    int rebuild_from_sidecars(const std::filesystem::path& root);

private:
    bool exec(const char* sql);
    sqlite3* db_ = nullptr;
    bool read_only_ = false;
    std::string err_;
};

}  // namespace himmel::library
