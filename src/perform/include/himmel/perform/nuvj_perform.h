// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// NuVJ Perform mapper (M10, docs/design/perform.md §5.2): the NuVJ as a main
// weapon. NuVJ events (Local\HimmelWeapon5 ring 0, weapon_nuvj.exe) + engine
// state -> commands, LED levels per control and the LCD model.
//
// No focus on the NuVJ: the crossfader decides. Deck A of the NuVJ is engine
// deck A, deck B is deck B; the deck the crossfader favours is PROGRAM. Each
// deck plays on its selected layer (the engine's focus layer when the focus is
// on that deck, else layer 1). Wheels: BEND / SCRATCH / FX WHEEL on SCRATCH.
//
// Same rules as Mk2Perform: no heap, no locks, no I/O; commands through the
// sink; unit-tested without hardware (tests/perform/nuvj_tests.cpp). Never asks
// for SysEx 7B: it only fills LED levels and the LCD model; the driver sends.
// Mapping table and GUESSes: docs/agents/handoff/control.md, "NuVJ mapping (M10)".
#pragma once

#include <cstdint>

#include "himmel/mk2/types.h"
#include "himmel/perform/mk2_perform.h"  // CommandSink
#include "himmel/protocol/state.h"

namespace himmel::perform {

// The LCD model the mapper fills (the engine copies it into the weapon block's Display).
struct NuvjLcd {
    char text[16] = {};       // dot-matrix row, 8 characters
    char alpha[16] = {};      // 16-segment row, 11 characters
    char number[2][4] = {};   // [0] BPM "128", [1] PRESET (the bank of the last deck touched)
    int8_t bar[2] = {-1, -1}; // LAST / DATA ramps 0..7 (-1 = off): the crossfader and the auto fade speed
    uint32_t marks = 0;       // nuvj::Mark bits (BPM, PRESET labels, box A / B = PROGRAM deck)
};

class NuvjPerform {
public:
    enum class Wheel : uint8_t { Bend = 0, Scratch = 1, FxWheel = 2, Count };
    static constexpr int kControls = 53;   // weapons/nuvj.weapon.json "controls"
    static constexpr int kPadsPerDeck = 9;

    void set_context(const protocol::TransportSection* transport) { transport_ = transport; }

    void on_event(const mk2::Event& e, const protocol::LiveState& state, const protocol::Catalog& catalog,
                  CommandSink& sink);
    // Once per frame: wheel let-go, LEDs (0..127 by control index) and the LCD.
    void tick(uint64_t now_us, const protocol::LiveState& state, const protocol::Catalog& catalog, CommandSink& sink,
              uint8_t leds[128], NuvjLcd& lcd);
    void reset();

    // Read side for tests.
    Wheel wheel(int deck) const { return wheel_[deck & 1]; }
    int bank(int deck) const { return bank_[deck & 1]; }
    bool picked() const { return picked_; }
    bool latched(int deck) const { return latch_[deck & 1]; }
    uint32_t fade_ms() const { return fade_ms_; }

private:
    struct Jog {
        bool active = false;      // turning (let go after kJogIdleUs without a tick)
        uint64_t last_us = 0;
        float base = 1.0f;        // BEND: the speed before the bend; FX WHEEL: the mix before the sweep
        float bend = 0.0f;        // BEND: the nudge so far
        bool scrub = false;       // BEND on a paused clip = scrub (a scratch hold)
        bool trimmed = false;     // a held pad turned the wheel: trim, not the wheel mode
    };

    void send(protocol::Command c, CommandSink& sink);
    uint8_t layer_of(const protocol::LiveState& s, int deck) const;
    int clip_pages(const protocol::Catalog& cat) const;
    void on_button(int control, bool down, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink,
                   uint64_t now_us);
    void on_jog(int deck, int ticks, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink,
                uint64_t now_us);
    void jog_release(int deck, const protocol::LiveState& s, CommandSink& sink);
    void on_crossfader(float x, const protocol::LiveState& s, CommandSink& sink, uint64_t now_us);

    const protocol::TransportSection* transport_ = nullptr;
    uint32_t seq_ = 0;
    Wheel wheel_[2] = {Wheel::Bend, Wheel::Bend};
    Jog jog_[2];
    int bank_[2] = {0, 0};
    bool latch_[2] = {false, false};
    uint64_t auto_down_us_[2] = {0, 0};
    bool sync_held_[2] = {false, false};
    int sync_loop_[2] = {-1, -1};          // SYNC held + wheel: beat-loop step (-1 = none); 1/16 .. 4 beats
    bool sync_used_[2] = {false, false};
    int pad_held_[2] = {-1, -1};           // the deck's held pad (0..8), for trims
    int last_deck_ = 0;                    // the deck touched last (PRESET digits)
    // Crossfader: a physical fader, so soft takeover against the MK2 wheel / TAKE.
    bool picked_ = false;
    float fader_last_ = -1.0f;
    float sent_x_ = -1.0f;
    uint64_t sent_us_ = 0;
    bool fader_start_ = false;
    uint8_t fader_mode_ = 0;               // A / B / C (curve: not in the engine yet)
    uint32_t fade_ms_ = 1000;              // AUTO FADE SPEED (INSTA A / B); 0 = a cut
    char lcd_msg_[12] = {};                // a short LCD message ("SCRATCH")
    uint64_t lcd_until_us_ = 0;
    uint64_t now_us_ = 0;
};

}  // namespace himmel::perform
