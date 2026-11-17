// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// JSON for the M11 part 2 panels (docs/agents/handoff/gui.md, "Wire format"):
// SHOW (Local\HimmelShow), OUTPUTS (Local\HimmelOutputs), AUDIO (Local\HimmelAudio),
// OSC (Local\HimmelOsc), the library block (Local\HimmelLibrary), and the show /
// output commands (types 25..36, 70..73).
//
// Pure logic, no Windows: unit-tested by tests/ui_bridge/ui_bridge_tests.cpp.
// Every block is optional: the bridge sends {"present": false} (or the block's
// state word) when it is missing or has the wrong version, and the page hides
// that panel. Nothing here may fail the bridge.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "himmel/audio/block.h"
#include "himmel/library/library_block.h"
#include "himmel/osc/osc.h"
#include "himmel/protocol/outputs.h"
#include "himmel/protocol/show.h"
#include "himmel/ui/json_codec.h"

namespace himmel::ui {

// ---- blocks -> JSON ---------------------------------------------------------
// `producer` = the bridge's slot (Gui = 1): its acks / rejects and its text box.
// time fields are left out, so equal states give equal text (the hub only sends changes).
nlohmann::json show_to_json(const protocol::ShowStatus& s, const protocol::FilesStatus& f, int producer);
nlohmann::json outputs_to_json(const protocol::OutputsSection& s);
// Signals (meters) change every hop; input / clock / config only now and then.
nlohmann::json audio_to_json(const audio::Signals& sig, const audio::InputStatus& in,
                             const audio::ClockSource (&clock)[audio::kClockSources], const audio::AudioConfig& cfg,
                             uint32_t config_gen_applied, uint64_t now_us);
nlohmann::json osc_to_json(const osc::OscStatus& s);
struct LibraryCounts {
    uint32_t mode = 0, paused = 0, queued = 0, working = 0, ready_total = 0, failed_total = 0, pauses = 0,
             catalog_full = 0;
    uint64_t ready_seq = 0;
};
nlohmann::json library_block_to_json(const LibraryCounts& c, const library::IngestJob& job);
// Library block v3 (MEDIA wave 7, library-db.md §11): the disk guard, the stored mode, the batch
// progress, the last import plan and the library location / move. Merged into the "library" event as
// {stored_mode, disk_low, disk{free, total, reserve, library_bytes, queue_bytes}, progress{done, total,
//  clip, file, file_progress, eta_s, bytes_written, file_bytes, error_clip, error}, plan{seq, mine,
//  dry_run, fits, refused, clips, known, fit_clips, need_bytes, queue_bytes, free_bytes, reserve_bytes,
//  message}, root{path, move_state, move_progress, move_done_bytes, move_total_bytes, move_to, move_message}}.
struct LibraryV3 {
    uint32_t stored_mode = 0, disk_low = 0;
    uint64_t disk_free = 0, disk_total = 0, disk_reserve = 0, library_bytes = 0, queue_bytes = 0;
};
void library_v3_to_json(nlohmann::json& j, const LibraryV3& v, const library::IngestProgress& p,
                        const library::IngestPlan& plan, const library::LibraryRoot& root, int my_producer);

// The page's AUDIO input picker -> AudioConfig (only the keys given change `cfg`).
//   {"device": "Microphone", "mode": "Capture"|"Loopback", "channel_first": 0, "channel_count": 1|2,
//    "auto_gain": true, "gain_db": 0, "gate_db": -60, "onset_sensitivity": 0.5, "midi_clock_port": ""}
// The caller bumps `gen`. Returns false with `error` set when a value is wrong.
bool audio_config_from_json(const nlohmann::json& j, audio::AudioConfig& cfg, std::string& error);

// "Capture endpoints ..." / "Render endpoints ..." text of `himmel_audio --list` ->
// {"capture": [{"name","channels","rate","default"}], "render": [...], "midi": [names]}.
nlohmann::json parse_audio_list(const std::string& text);

// ---- show / output commands (types 25..36, 70..73) --------------------------
// Keys: "type", "deck" ("A"/"B"/0/1; "hit" for LayerPlace), "layer", "slot", "param" (an enum
// name or a number), "value" or "i" (number or an enum name), "f" (number or up to 4 numbers),
// "relative" / "variation" / "clear" (booleans), "text" (Project / SongEdit / MacroSet rename:
// the bridge writes it into its text box and sets i to the box id).
//   {"type":"SceneRecall","deck":"B","value":3}           {"type":"SceneRecall","deck":"A","value":"next"}
//   {"type":"SceneSave","deck":"A","value":-1,"variation":true}
//   {"type":"Undo","value":"redo"}   {"type":"Locks","param":"Toggle","value":["Clip","Fx"]}
//   {"type":"Dice","param":"Roll","deck":"A"}            {"type":"Project","param":"SaveAs","text":"My show"}
//   {"type":"Project","param":"Open","slot":2}           {"type":"ShowSelect","param":"Bank","value":1,"relative":true}
//   {"type":"SongEdit","param":"RenameScene","slot":4,"text":"Drop"}
//   {"type":"OutputBlack","slot":1,"value":"toggle"}     {"type":"Identify","value":"5s"}
//   {"type":"LayerPlace","deck":"A","layer":0,"value":"Out","param":1}
//   {"type":"OutputConfig","param":"Resolution","value":"P1440"}
// Returns false when `j` is not one of these types (the caller then tries command_from_json);
// otherwise fills `r` (r.ok false + error on a bad value).
bool panel_command_from_json(const nlohmann::json& j, ParseResult& r);

// Every enum name the panels use (GET /names, key "panels").
nlohmann::json panels_names();

// ---- M11 part 3 ----
// The clip drawer's motion curve: `n` buckets (max of each), scaled so the highest is 1.
// Empty in -> empty out.
std::vector<float> downsample_curve(const std::vector<float>& curve, int n);

// GET /show/document: the show document (show-files.md §3) without the live part, for the BUILD
// view (kits, macros, songs). `doc` is autosave/show.json or one assembled from the project files.
// Returns {"ok": false, "error"} for anything that is not a himmel-show/1 document.
nlohmann::json show_doc_for_page(const nlohmann::json& doc);

// POST /library/cmd (MEDIA, library-db.md §10): {"type": "Import" | "Rescan" | "Remove" | "Relink" |
// "Reanalyse" | "Retry" | "Mode" | "Unwatch", "clip": library id (0 = all), "path": absolute,
// "watch" / "force" / "thumbs": true, "mode": "Auto" | "Full" | "Background" | "Hold"} -> an IngestCommand.
// v3 (wave 7): "ClearQueue"; "Library" (path, "move", "force"); Import options "height" (0 or 144..4320),
// "hapq", "skip_known", "dry_run", "fit", "list" (path = a list file), "conform_hz" (0 | 50 | 60)
// (seq is stamped when it is pushed). False with `error` for anything the worker would refuse anyway.
bool ingest_command_from_json(const nlohmann::json& j, library::IngestCommand& c, std::string& error);
nlohmann::json ingest_reply_to_json(const library::IngestReply& r);

// GUI wave 7 (the import path dialog, GET /fs/list): what a file name would import as
// (library-db.md §2): "video" | "still" | "vector" | "animation", or "" when it is not importable.
// Case-insensitive on the extension.
std::string import_kind_of(const std::string& file_name);
// The folder above `path` for the dialog's "up" button: "C:\Clips\Club" -> "C:\Clips",
// "C:\Clips" -> "C:\", "C:\" -> "" (= the drive list), "\\srv\share" -> "" too.
std::string parent_dir_of(const std::string& path);
// The import queue's ETA: `left` items at `per_item_s` seconds each, in whole seconds (capped at
// 24 h); 0 when nothing is left, -1 when the rate is unknown.
int eta_seconds(int left, double per_item_s);

// POST /library/upload?name=<file> (GUI wave 7, drag-drop from the desktop; import by path is preferred).
// The file name: no folder parts, no control characters, not "." / "..", at most 200 bytes, and a media
// extension (import_kind_of). Empty string = OK, else the reason.
std::string upload_name_error(const std::string& name);
// The size and disk check, from the library block v3 guard (library-db.md §11.1): refused when the size is
// 0 or above `cap`, when the worker holds the queue (`disk_low`), or when `free - size` would eat into
// `reserve`. `have_guard` = the v3 numbers are there (else refused: no guard, no upload). "" = OK.
std::string upload_space_error(uint64_t size, uint64_t cap, bool have_guard, uint64_t free, uint64_t reserve, bool disk_low);
inline constexpr uint64_t kUploadCapBytes = 8ull << 30;   // 8 GB per file

}  // namespace himmel::ui
