// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Maschine MK2 controller: shared types.
// Spec: docs/mk2-controller-spec.md
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace himmel::mk2 {

inline constexpr int kButtons = 48;
inline constexpr int kKnobs = 8;
inline constexpr int kPads = 16;
inline constexpr int kControlsPayload = 24;  // report 0x01 without the id
inline constexpr int kPadsPayload = 64;      // report 0x20 without the id

// Canonical button ids, in the verified byte/bit order of report 0x01 (spec 2.1).
// The map file decides where each one really is; this order is only our naming.
enum class Button : uint8_t {
    Top1, Top2, Top3, Top4, Top5, Top6, Top7, Top8,
    Control, Step, Browse, Sampling, PageLeft, PageRight, All, AutoWrite,
    Volume, Swing, Tempo, MasterLeft, MasterRight, Enter, NoteRepeat, EncoderPush,
    GroupA, GroupB, GroupC, GroupD, GroupE, GroupF, GroupG, GroupH,
    Restart, TransportLeft, TransportRight, Grid, Play, Rec, Erase, Shift,
    Scene, Pattern, PadMode, Navigate, Duplicate, Select, Solo, Mute,
};

// Names exactly as in maschine_mk2_map.json.
inline constexpr std::array<std::string_view, kButtons> kButtonNames = {
    "Top 1", "Top 2", "Top 3", "Top 4", "Top 5", "Top 6", "Top 7", "Top 8",
    "Control", "Step", "Browse", "Sampling", "Page <", "Page >", "All", "Auto Write",
    "Volume", "Swing", "Tempo", "Master <", "Master >", "Enter", "Note Repeat", "Master encoder (push)",
    "Group A", "Group B", "Group C", "Group D", "Group E", "Group F", "Group G", "Group H",
    "Restart", "Transport <", "Transport >", "Grid", "Play", "Rec", "Erase", "Shift",
    "Scene", "Pattern", "Pad Mode", "Navigate", "Duplicate", "Select", "Solo", "Mute",
};

enum class EventType : uint8_t {
    ButtonDown,    // id = Button
    ButtonUp,      // id = Button
    KnobDelta,     // id = knob 0..7, value = delta in raw units (one turn ~1008)
    EncoderDelta,  // id = 0, value = detents (+ = clockwise)
    PadHit,        // id = pad 1..16, value = velocity 1..127, raw = pressure used
    PadPressure,   // id = pad 1..16, value = pressure 0..4095 (aftertouch)
    PadRelease,    // id = pad 1..16
    Connected,
    Disconnected,
    // Added for the other weapons (Local\HimmelWeapon<N>, himmel/weapon/shared.h).
    // The MK2 never sends these; the values above never change.
    TouchStart,    // id = finger 0..31, value = x 0..4095, raw = y 0..4095, reserved = z | strike << 8
    TouchMove,     // same fields
    TouchEnd,      // id = finger, value / raw = last x / y
    Fader,         // id = control index (profile), value = 0..16383
    Switch,        // id = control index (profile), value = position 0..n-1
};

// 16 bytes, trivially copyable: lives in shared-memory rings.
struct Event {
    uint64_t t_us;   // steady clock, microseconds (report arrival time)
    EventType type;
    uint8_t id;
    int16_t value;
    uint16_t raw;
    uint16_t reserved;
};
static_assert(sizeof(Event) == 16);

inline uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string_view event_name(EventType t);

}  // namespace himmel::mk2
