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

}  // namespace himmel::ui
