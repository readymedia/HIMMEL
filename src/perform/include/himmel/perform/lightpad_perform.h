// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Lightpad Block Perform mapper (M9, docs/design/hardware-mapping.md §5 "Block"):
// Lightpad events (Local\HimmelWeapon<N> ring 0) + engine state -> commands and
// the 15 x 15 LED picture. Modes: TOUCH (Touch FX on PROGRAM), MIX (a real
// crossfader with soft takeover against the MK2 wheel; top row = TAKE, bottom
// row = CUT) and SCRUB (the focused layer's playhead under the finger).
//
// Same rules as Mk2Perform: no heap, no locks, no I/O, no Windows calls (it runs
// in the render loop); commands only through the sink; unit-tested without
// hardware (tests/perform/lightpad_tests.cpp). Mapping table and GUESSes:
// docs/agents/handoff/control.md, "Lightpad mapping (M9)".
#pragma once

#include <cstdint>

#include "himmel/mk2/types.h"
#include "himmel/perform/mk2_perform.h"  // CommandSink
#include "himmel/protocol/state.h"

namespace himmel::perform {

class LightpadPerform {
public:
    enum class Mode : uint8_t { Touch = 0, Mix = 1, Scrub = 2, Count };
    static constexpr int kGrid = 15;                 // 15 x 15 LEDs, row-major from the top-left
    static constexpr int kPixels = kGrid * kGrid;
    static constexpr uint8_t kModeControl = 1;       // weapons/lightpad.weapon.json: 0 Surface, 1 Mode

    // The transport section (clip in / out for SCRUB, the clock for the beat). May be null.
    void set_context(const protocol::TransportSection* transport) { transport_ = transport; }

    // One Lightpad event: TouchStart / TouchMove / TouchEnd (id = finger, value = x 0..4095,
    // raw = y 0..4095 from the top, reserved & 0xFF = z 0..255), ButtonDown / ButtonUp (id = control).
    // PadHit / PadPressure / PadRelease (the 4 x 4 zones) are ignored: the touches carry it all.
    void on_event(const mk2::Event& e, const protocol::LiveState& state, const protocol::Catalog& catalog,
                  CommandSink& sink);

    // Once per frame: soft-takeover bookkeeping and the LED picture (RGB 0..255).
    void tick(uint64_t now_us, const protocol::LiveState& state, const protocol::Catalog& catalog, CommandSink& sink,
              uint8_t pixels[kPixels][3]);

    // The block (re)connected: forget fingers and the pickup; the mode stays.
    void reset();

    // Read side for tests.
    Mode mode() const { return mode_; }
    bool picked() const { return picked_; }
    bool hold() const { return hold_; }

private:
    struct Finger {
        bool down = false;
        uint16_t x = 0, y = 0;
        uint8_t z = 0;
    };
    static constexpr int kFingers = 8;

    void send(protocol::Command c, CommandSink& sink);
    void set_mode(Mode m, const protocol::LiveState& s, CommandSink& sink);
    void end_all(const protocol::LiveState& s, CommandSink& sink);
    int fingers_down() const;
    int first_finger() const;
    // TOUCH
    void touch_fx(const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink, bool start);
    int touch_effect(const protocol::Catalog& cat, int zone) const;
    int touch_zones(const protocol::Catalog& cat) const;
    // MIX
    void mix_move(float x, const protocol::LiveState& s, CommandSink& sink, uint64_t now_us);
    // SCRUB
    bool scrub_target(const protocol::LiveState& s, uint8_t& deck, uint8_t& layer) const;
    void scrub_move(const Finger& f, const protocol::LiveState& s, CommandSink& sink);

    const protocol::TransportSection* transport_ = nullptr;
    Mode mode_ = Mode::Touch;
    uint32_t seq_ = 0;
    Finger finger_[kFingers];
    uint64_t mode_tap_us_ = 0;       // the last Mode press (double-tap = next mode)
    bool hold_before_tap_ = false;   // TOUCH: the hold state before the first tap of a double-tap
    // TOUCH
    bool hold_ = false;              // Mode in TOUCH = keep the effect after the finger lifts
    int touch_fx_ = -1;              // effect id now in the touch slot (-1 = none)
    float touch_mix_sent_ = -1.0f, touch_press_sent_ = -1.0f, touch_p2_sent_ = -1.0f;
    // MIX
    int mix_finger_ = -1;            // the finger on the fader (-1 = none)
    bool picked_ = false;            // soft takeover: the finger has caught the crossfader
    float mix_last_x_ = -1.0f;       // the finger's previous position (to see it cross the fader)
    float mix_sent_ = -1.0f;         // the last crossfader value we sent
    uint64_t mix_sent_us_ = 0;
    bool took_ = false;              // the wheel moved the fader under a picked finger
    // SCRUB
    int scrub_finger_ = -1;
    uint8_t scrub_deck_ = 0xFF, scrub_layer_ = 0xFF;
    float scrub_sent_ = -1.0f, scrub_press_sent_ = -1.0f;
    uint64_t last_us_ = 0;
};

}  // namespace himmel::perform
