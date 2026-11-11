// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/mk2/map.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace himmel::mk2 {

namespace {

void fill_pad_inverse(Map& m) {
    m.pad_of_hw_id.fill(-1);
    for (int p = 0; p < kPads; ++p) {
        if (m.pad_hw_id[p] >= 0 && m.pad_hw_id[p] < kPads) m.pad_of_hw_id[m.pad_hw_id[p]] = p;
    }
}

}  // namespace

Map default_map() {
    Map m;
    for (int i = 0; i < kButtons; ++i) m.buttons[i] = {i / 8, i % 8};  // bytes 0..5, bit 0 first
    for (int k = 0; k < kKnobs; ++k) m.knobs[k] = {8 + 2 * k, true, true};
    m.encoder = {7, false, true};
    for (int p = 0; p < kPads; ++p) m.pad_hw_id[p] = (3 - p / 4) * 4 + p % 4;  // Pad 1 = id 12
    fill_pad_inverse(m);
    m.noise_floor.fill(0);
    m.noise_floor[11] = 136;  // Pad 12 is worn
    m.verified = true;
    return m;
}

std::optional<Map> load_map(const std::filesystem::path& path, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Map> {
        if (error) *error = why;
        return std::nullopt;
    };
    std::ifstream in(path);
    if (!in) return fail("cannot open " + path.string());
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        return fail(std::string("bad JSON: ") + e.what());
    }
    try {
        Map m;
        m.verified = j.value("verified", false);
        const auto& buttons = j.at("buttons");
        for (int i = 0; i < kButtons; ++i) {
            auto it = buttons.find(std::string(kButtonNames[i]));
            if (it == buttons.end()) return fail("button missing: " + std::string(kButtonNames[i]));
            m.buttons[i] = {it->at("byte").get<int>(), it->at("bit").get<int>()};
            if (m.buttons[i].byte < 0 || m.buttons[i].byte >= kControlsPayload || m.buttons[i].bit < 0 ||
                m.buttons[i].bit > 7)
                return fail("button out of range: " + std::string(kButtonNames[i]));
        }
        auto knob = [&](const nlohmann::json& k) {
            KnobPos p;
            p.byte = k.at("byte").get<int>();
            const std::string nib = k.value("nibble", "both");
            p.word = nib == "both";
            p.low_nibble = nib != "high";
            return p;
        };
        const auto& knobs = j.at("knobs");
        for (int k = 0; k < kKnobs; ++k) {
            auto name = "Knob " + std::to_string(k + 1);
            if (!knobs.contains(name)) return fail("knob missing: " + name);
            m.knobs[k] = knob(knobs.at(name));
            if (m.knobs[k].byte < 0 || m.knobs[k].byte + (m.knobs[k].word ? 1 : 0) >= kControlsPayload)
                return fail("knob out of range: " + name);
        }
        if (!knobs.contains("Master encoder (turn)")) return fail("encoder missing");
        m.encoder = knob(knobs.at("Master encoder (turn)"));
        const auto& pads = j.at("pads");
        for (int p = 0; p < kPads; ++p) {
            auto name = "Pad " + std::to_string(p + 1);
            if (!pads.contains(name)) return fail("pad missing: " + name);
            m.pad_hw_id[p] = pads.at(name).at("id").get<int>();
            if (m.pad_hw_id[p] < 0 || m.pad_hw_id[p] >= kPads) return fail("pad id out of range: " + name);
        }
        fill_pad_inverse(m);
        for (int id = 0; id < kPads; ++id)
            if (m.pad_of_hw_id[id] < 0) return fail("two pads share a hardware id");
        m.noise_floor.fill(0);
        if (j.contains("pad_noise_floor")) {
            const auto& f = j.at("pad_noise_floor");
            for (int p = 0; p < kPads && p < static_cast<int>(f.size()); ++p) m.noise_floor[p] = f.at(p).get<int>();
        }
        return m;
    } catch (const std::exception& e) {
        return fail(std::string("map format: ") + e.what());
    }
}

std::string_view event_name(EventType t) {
    switch (t) {
        case EventType::ButtonDown: return "ButtonDown";
        case EventType::ButtonUp: return "ButtonUp";
        case EventType::KnobDelta: return "KnobDelta";
        case EventType::EncoderDelta: return "EncoderDelta";
        case EventType::PadHit: return "PadHit";
        case EventType::PadPressure: return "PadPressure";
        case EventType::PadRelease: return "PadRelease";
        case EventType::Connected: return "Connected";
        case EventType::Disconnected: return "Disconnected";
        case EventType::TouchStart: return "TouchStart";
        case EventType::TouchMove: return "TouchMove";
        case EventType::TouchEnd: return "TouchEnd";
        case EventType::Fader: return "Fader";
        case EventType::Switch: return "Switch";
    }
    return "?";
}

}  // namespace himmel::mk2
