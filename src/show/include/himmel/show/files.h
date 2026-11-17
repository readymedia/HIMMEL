// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Show files on disk (M3, docs/interfaces/show-files.md): the project folder
// (himmel-show/1 JSON), atomic writes, the autosave journal, recent projects
// and the one-file .himmelshow export (a plain zip, "stored", no compression).
//
// Only the save worker calls these (they allocate and do I/O); never the frame loop.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "himmel/show/model.h"

namespace himmel::show::files {

namespace fs = std::filesystem;

inline constexpr const char* kFormat = "himmel-show/1";
inline constexpr int kFormatVersion = 1;
inline constexpr const char* kProjectExt = ".himmel";
inline constexpr const char* kExportExt = ".himmelshow";

// ---- whole documents --------------------------------------------------------
// The project as files: relative path -> UTF-8 JSON text (project.json last).
struct FileSet {
    std::vector<std::pair<std::string, std::string>> files;
};
FileSet project_files(const ShowData& show, const LiveSnapshot& live, const proto::Catalog& cat, uint64_t saved_ms);
// The project as one document (autosave/show.json, snapshots).
std::string show_document(const ShowData& show, const LiveSnapshot& live, const proto::Catalog& cat, uint64_t saved_ms);

// Load result. `missing` = clips named by the project that the catalog lacks.
struct LoadInfo {
    bool ok = false;
    bool have_live = false;
    bool interrupted = false;     // a file did not match the manifest (a save was cut off)
    int missing = 0;
    std::vector<std::string> missing_names;
    uint64_t saved_ms = 0;        // when it was saved (ms since 1970)
    std::string error;
};
LoadInfo load_project(const fs::path& folder, ShowData& show, LiveSnapshot& live, const proto::Catalog& cat);
LoadInfo load_document(const std::string& json_text, ShowData& show, LiveSnapshot& live, const proto::Catalog& cat);

// ---- writing ----------------------------------------------------------------
// Write to "<file>.tmp", flush, then rename over the old file (MoveFileEx
// REPLACE_EXISTING | WRITE_THROUGH). A crash leaves either the old or the new file.
bool write_atomic(const fs::path& file, const std::string& bytes, std::string* err);
bool read_file(const fs::path& file, std::string& out);
// Every file of the set, project.json last (it carries the CRC manifest).
bool save_project(const fs::path& folder, const ShowData& show, const LiveSnapshot& live, const proto::Catalog& cat,
                  uint64_t saved_ms, std::string* err);

// ---- autosave journal (autosave/journal.bin) ---------------------------------
// Header (names of the effects at write time) + records {unix ms, LiveSnapshot},
// each with its own CRC; a torn last record is ignored on read.
class Journal {
public:
    ~Journal();
    bool open_new(const fs::path& file, const proto::Catalog& cat, std::string* err);  // truncates
    bool append(const LiveSnapshot& s, uint64_t unix_ms);
    void flush();     // FlushFileBuffers (power-cut safety); a killed process loses nothing anyway
    void close();
    bool is_open() const { return h_ != nullptr; }
    uint32_t records() const { return records_; }
    // The newest good record; effect ids remapped by name to `cat`, clip ids bound by name.
    static bool read_last(const fs::path& file, const proto::Catalog& cat, LiveSnapshot& out, uint64_t& unix_ms,
                          uint32_t* count = nullptr);

private:
    void* h_ = nullptr;
    uint32_t records_ = 0;
    uint64_t since_flush_ = 0;
};

// ---- recent projects (<appdata>/recent.json) ----------------------------------
struct Recent {
    std::string name, path;
    uint64_t saved_unix = 0;
    int outputs = 0;
};
std::vector<Recent> load_recents(const fs::path& file);
bool save_recents(const fs::path& file, const std::vector<Recent>& r);
void touch_recent(std::vector<Recent>& r, const Recent& now);   // to the front, at most 10, no duplicates

// ---- .himmelshow (zip, stored) -------------------------------------------------
bool zip_folder(const fs::path& folder, const fs::path& zip_file, std::string* err, bool skip_autosave = true);
bool unzip_to(const fs::path& zip_file, const fs::path& folder, std::string* err);

// ---- misc ------------------------------------------------------------------
uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);
uint64_t unix_now();
uint64_t unix_ms_now();
std::string utf8(const fs::path& p);
fs::path from_utf8(const std::string& s);
// A file-system-safe folder name from a project name ("My show" -> "My show").
std::string safe_name(const std::string& name);
// Default folders: Documents\Himmel\Projects, %LOCALAPPDATA%\Himmel.
fs::path default_projects_root();
fs::path default_app_dir();
// Clips a folder holds that match the names (case-insensitive, recursive): name -> path.
std::vector<std::pair<std::string, std::string>> find_files(const fs::path& folder, const std::vector<std::string>& names);

}  // namespace himmel::show::files
