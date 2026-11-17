// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_ui's links to the optional blocks of M11 part 2 (SHOW, OUTPUTS, AUDIO, OSC, LIBRARY).
//
// Every block is optional: missing, wrong version or a stopped writer only changes the
// panel's JSON ({"present": false, "state": "absent" | "version" | "starting" | "stale"}),
// so the page hides that panel. The poller thread (re)opens the blocks once a second;
// request threads write the bridge's own parts (its SHOW text box, the AUDIO config) under
// a shared lock, so a view is never unmapped under them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace himmel::ui {

struct PanelJson {
    std::string show, outputs, audio, osc, library;
    std::string weapons;   // only when `retry` (once a second): the weapon driver blocks found
};

// Watch mode (--watch): read-only, never writes a text box or the audio config.
void panels_init(bool watch, const std::filesystem::path& library_root, const std::filesystem::path& exe_dir);
void panels_close();

// Poller thread, ~30 Hz. `retry` = try to (re)open missing blocks (once a second).
PanelJson panels_poll(uint64_t now_us, bool retry);

// SHOW text-box handshake (show-block.md §2): write `id` + `text` into the GUI producer's box.
// False (with `err`) when the show block is not there or the bridge is read-only.
bool panels_write_show_text(uint32_t id, const std::string& text, std::string& err);

// POST /audio/config: merge the keys into the requested AudioConfig, bump gen, publish.
nlohmann::json panels_audio_config(const nlohmann::json& body);
// GET /audio/devices: `himmel_audio --list`, parsed (cached for 10 s).
nlohmann::json panels_audio_devices();

// GET /library/clips: every row of the library DB (read-only), newest first.
nlohmann::json panels_library_clips();
// GET /library/thumb/<hash>.jpg or <hash>-strip.jpg -> the file's bytes ("" = not there).
std::string panels_library_thumb(const std::string& file);
std::string panels_library_root();

// GET /show/document (M11 part 3): the open project's show document for the BUILD view, read
// from <project>/autosave/show.json, or assembled from project.json + banks/ + songs/ when that
// is older or missing. Read-only; {"ok": false, "error"} when there is no show block or project.
nlohmann::json panels_show_document();

}  // namespace himmel::ui
