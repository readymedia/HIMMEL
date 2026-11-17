// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Builds the clip part of the Catalog (Local\HimmelState) from the media
// playlist: name, size, fps, length, codec and a placeholder colour (the
// average colour of the first frame, from the BC block end points). Start-up
// only: it reads files and allocates.
#pragma once

#include <string>
#include <vector>

#include "himmel/protocol/state.h"

namespace himmel::engine {

// Every .mov under the nearest "media" folder above the exe, sorted (the
// legacy renderer's playlist rule).
std::vector<std::string> find_media_clips();

// Fills catalog.clip[0..n) for the first kMaxClips paths. A file that is not a
// readable HAP clip is still listed (codec 3, grey); playing it shows the
// placeholder and marks the layer missing.
void fill_clip_catalog(protocol::Catalog& catalog, const std::vector<std::string>& paths);

// Catalog v3 (wave 7): the entry of one clip file (name from the file name + probe), for any number of clips.
void clip_info_for(const std::string& path, protocol::ClipInfo& info);
void probe_clip(const std::string& path, protocol::ClipInfo& info);

}  // namespace himmel::engine
