// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Entrancer weapon: turns the MIDI an Entrancer-style X/Y unit sends into
// Touch FX (docs/touch-fx-entrancer.md, docs/entrancer-weapon.md).
//
// The bytes are the real unit's MIDI out (owner's manual, MIDI section):
//   CC 12 = pad X, CC 13 = pad Y, CC 92 = pad touch on/off, program change 0..99.
// The simulator (tools/entrancer_live.py) sends exactly those, plus a few
// Himmel-only controllers for panel controls the real unit keeps to itself
// (FX balance, freeze, the AUDIO/VIDEO lever, tempo, Lightpad pressure). They
// sit outside the unit's assignable ranges (CC 1-31, 64-95), so a real unit
// never sends them by accident.
//
// Pure code: no Windows, no heap. The render thread feeds it bytes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "himmel/engine/touch_fx.h"

namespace himmel::engine::entrancer {

inline constexpr int kPrograms = 100;

// The real unit's controllers.
inline constexpr uint8_t kCcX = 12;
inline constexpr uint8_t kCcY = 13;
inline constexpr uint8_t kCcTouch = 92;
// Himmel extensions (simulator only).
inline constexpr uint8_t kCcFxBalance = 102;  // 0..127 = dry .. wet
inline constexpr uint8_t kCcFreeze = 104;     // >= 64 = freeze the picture
inline constexpr uint8_t kCcVideoOff = 105;   // >= 64 = lever on AUDIO: the pad does not touch the video
inline constexpr uint8_t kCcBpmMsb = 106;     // tempo = msb * 128 + lsb (BPM), sent msb first
inline constexpr uint8_t kCcBpmLsb = 107;
inline constexpr uint8_t kCcPressure = 108;   // finger pressure 0..127 (Lightpad as the pad)

// How one of the unit's 100 programs is played in Himmel: the closest Touch FX
// effect, and whether Y is "how much" (the unit's usual Y = blend) or the
// effect's own Y parameter.
struct Program {
    uint8_t effect;   // touchfx::Effect
    bool y_is_mix;    // Y = depth; the effect's Y parameter stays at fixed_y
    float fixed_y;
};
const Program& program(int n);  // clamped to 0..99

struct State {
    int program = 0;
    bool touch = false;
    float x = 0.5f, y = 0.5f;   // 0..1, y = 0 at the bottom
    float pressure = 1.0f;      // 1 unless the Lightpad sends CC 108
    float fx_balance = 1.0f;
    bool freeze = false;
    bool video_off = false;
    int bpm = 120;
    uint32_t messages = 0;      // complete channel messages seen
};

class Mapper {
public:
    // Raw MIDI bytes, any channel, running status; real-time bytes (clock,
    // active sensing) are skipped. Each complete message is applied to fx.
    void feed(const uint8_t* data, size_t n, touchfx::TouchFx& fx);
    // The link went quiet: lift the finger (a held effect is the unit's job).
    void lost(touchfx::TouchFx& fx);
    const State& state() const { return st_; }
    // Listen to one MIDI channel (1..16, the unit's MIDI Edit setting) or all (0).
    void set_channel(int ch) { channel_ = ch >= 1 && ch <= 16 ? ch : 0; }
    int channel() const { return channel_; }

private:
    void message(uint8_t status, uint8_t d1, uint8_t d2, touchfx::TouchFx& fx);
    void push(touchfx::TouchFx& fx);
    State st_;
    uint8_t status_ = 0;
    uint8_t data_[2] = {};
    int have_ = 0;
    int bpm_msb_ = -1;
    int channel_ = 0;
};

}  // namespace himmel::engine::entrancer
