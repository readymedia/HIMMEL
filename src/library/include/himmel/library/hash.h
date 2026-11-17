// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Content hash of a library source (library-db.md §4, D140): FNV-1a 64 over
// the size + three 1 MB samples (start, middle, end). Stable across renames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace himmel::library {

inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime = 0x100000001b3ull;
inline constexpr size_t kHashSample = 1u << 20;

inline uint64_t fnv1a(const void* data, size_t n, uint64_t h = kFnvOffset) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}

std::string hex16(uint64_t h);

// The content hash of one file; empty on a read error.
std::string hash_file(const std::filesystem::path& file);
// The hash of an image sequence: sorted names + sizes, then the first and last
// file's hashes.
std::string hash_sequence(const std::vector<std::filesystem::path>& sorted_files);

}  // namespace himmel::library
