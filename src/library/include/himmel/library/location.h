// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Where the library lives (docs/interfaces/library-db.md §11.5, D182).
// - The saved location: Documents\Himmel\library-location.json, read by
//   default_library_root() (feed.h), so every Himmel process opens the same folder.
// - A moved library leaves himmel-library-moved.json {"to": ...} in its old folder;
//   resolve_library_root() follows it (a process started with --library <old>).
#pragma once

#include <filesystem>

namespace himmel::library {

inline constexpr const char* kLocationFormat = "himmel-library-location/1";
inline constexpr const char* kMovedFile = "himmel-library-moved.json";

// Documents\Himmel\library-location.json.
std::filesystem::path location_file();
// The root saved in `file` ("" = none or unreadable).
std::filesystem::path read_location(const std::filesystem::path& file);
// Writes `file` atomically (temp + rename). False on an I/O error.
bool save_location(const std::filesystem::path& file, const std::filesystem::path& root);

// Follows moved-library redirects (at most 8): a folder with no library DB and a
// himmel-library-moved.json is replaced by its "to".
std::filesystem::path resolve_library_root(std::filesystem::path root);
bool write_moved_redirect(const std::filesystem::path& old_root, const std::filesystem::path& to);

}  // namespace himmel::library
