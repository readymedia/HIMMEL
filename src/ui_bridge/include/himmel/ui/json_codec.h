// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// JSON <-> protocol conversion for the web GUI (docs/agents/handoff/gui.md, "Wire format").
//
// Pure logic, no Windows: unit-tested by tests/ui_bridge/ui_bridge_tests.cpp.
//
// Browser -> bridge (POST /cmd), one object or an array of them:
//   {"type": "LayerOpacity", "deck": 0, "layer": 2, "value": 0.5, "relative": false}
//   {"type": "FxParam", "deck": "master", "slot": 0, "param": 1, "value": 0.3}
//   {"type": "Take", "value": 2000}   {"type": "Cut"}   {"type": "Black", "value": 2}
// The bridge checks what it can (type, targets, value kind and range, ids
// against the catalog it last saw) so the page gets an answer at once; the
// engine validates everything again (command-protocol.md §5).
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "himmel/protocol/state.h"

namespace himmel::ui {

struct CatalogLimits {
    int clips = -1;    // -1 = unknown (no catalog yet): id checks are left to the engine
    int effects = -1;
};

struct ParseResult {
    bool ok = false;
    protocol::Command cmd{};
    std::string error;   // English, shown in the page
    // Show commands with a path / name (M3 text-box handshake, show-block.md §2): the bridge
    // writes `text` into its box with a fresh id and sets cmd.i to that id before pushing.
    bool needs_text = false;
    std::string text;
};

const char* command_type_name(protocol::CommandType t);
bool command_type_from_name(std::string_view name, protocol::CommandType& out);
const char* reject_name(uint8_t reject);
const char* blend_name(int blend);

// seq is left 0; the bridge fills it when it pushes.
ParseResult command_from_json(const nlohmann::json& j, const CatalogLimits& limits);
nlohmann::json command_to_json(const protocol::Command& c);

// The live view without frame / time_us / fps (those go in the 1 Hz "bridge" event),
// so two equal models give equal text and the bridge only sends changes.
nlohmann::json live_to_json(const protocol::LiveState& s);
nlohmann::json catalog_to_json(const protocol::Catalog& c);

// State v2 transport section (M2): clock settings, per layer the LayerParam settings and
// the playhead status, the HIT layer. Enums as names (the same names LayerSet accepts).
// The running beat is left out (it changes every frame); see clock_to_json.
nlohmann::json transport_to_json(const protocol::TransportSection& t);
// {"beat": the running beat at now_us, extrapolated from the engine's sample}
nlohmann::json clock_to_json(const protocol::ClockState& c, uint64_t now_us);
// Every name the page needs to build the transport controls (LayerParam list with kinds and
// ranges, play actions, grid, cue actions, clock params, phases, blend modes): GET /names.
nlohmann::json transport_names();

// Commands with more than one argument (M2), the extra keys:
//   {"type":"LayerSet","deck":0,"layer":1,"param":"Mode","value":"PingPong"}   (param: name or 0..28)
//   {"type":"LayerTrigger","deck":"hit","value":3,"velocity":0.8,"release":false}
//   {"type":"LayerCue","deck":0,"layer":0,"cue":4,"value":"Set","pos":0.25}   (pos < 0 / absent = playhead)
//   {"type":"LayerBeatLoop","deck":0,"layer":0,"value":0.5}                    (0 = off)
//   {"type":"LayerScratch","deck":0,"layer":0,"value":1,"pos":0.4}
//   {"type":"Clock","param":"Bpm","value":128}   {"type":"Clock","param":"Grid","value":"Bar"}
// "deck":"hit" = the HIT layer for every layer command (layer 0 or absent).

}  // namespace himmel::ui
