// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// SVG -> PNG with Direct2D (Windows), operations.md §4: rasterised at
// 2 x canvas height, as a still.
#pragma once

#include <filesystem>
#include <string>

namespace himmel::ingest {

namespace fs = std::filesystem;

// Draws `svg` at `target_h` pixels high (width from its aspect, at most
// `max_w`; both rounded down to a multiple of 4) into `png` with alpha.
bool rasterize_svg(const fs::path& svg, const fs::path& png, int target_h, int max_w, std::string* err = nullptr,
                   int* out_w = nullptr, int* out_h = nullptr);

}  // namespace himmel::ingest
