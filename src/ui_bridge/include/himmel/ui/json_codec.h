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

}  // namespace himmel::ui
