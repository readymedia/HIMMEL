// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The sidecar meta/<hash>.json ("himmel-clip/1", docs/interfaces/library-db.md §5).
#pragma once

#include <filesystem>
#include <string>

#include "himmel/library/record.h"

namespace himmel::library {

inline constexpr const char* kSidecarFormat = "himmel-clip/1";

std::string sidecar_to_json(const ClipRecord& r);
// False (and `err`) when the text is not a himmel-clip/1 sidecar.
bool sidecar_from_json(const std::string& text, ClipRecord& r, std::string* err = nullptr);

// Write via a temp file + rename, so a reader never sees half a file.
bool write_sidecar(const std::filesystem::path& file, const ClipRecord& r);
bool read_sidecar(const std::filesystem::path& file, ClipRecord& r, std::string* err = nullptr);

std::string rgb_hex(uint32_t rgb);   // "#rrggbb"
uint32_t hex_rgb(const std::string& s);
std::string iso_utc(int64_t unix_s);  // "2026-11-17T14:02:11Z"
int64_t parse_iso_utc(const std::string& s);

}  // namespace himmel::library
