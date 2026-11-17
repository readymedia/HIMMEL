// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// MK2 Perform mapper: MK2 events + engine state -> commands, LEDs and screens.
// Contract between CONTROL (implements it, owns src/perform/) and ENGINE (calls
// it from the render process). docs/interfaces/command-protocol.md §6.
//
// Rules for the implementation:
//   * no heap, no locks, no I/O, no Windows calls: it runs in the render loop;
//   * commands only through the sink, never by touching engine objects;
//   * pure logic, unit-tested without hardware (tests/perform/).
//
// M4 (2026-11-17): the full Perform v4 mapping (docs/design/perform.md,
// hardware-mapping.md §3). The mapping table is in docs/agents/handoff/control.md.
#pragma once

#include <cstdint>

#include "himmel/mk2/output.h"
#include "himmel/mk2/types.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"

// ==== ENGINE (catalog v3) ====
namespace himmel::engine {
class ClipDirectory;
}
// ==== end ENGINE (catalog v3) ====

namespace himmel::perform {

// Where the mapper puts its commands. The engine implements it (validates and
// queues them on the Perform producer slot). Returns false if refused / full.
struct CommandSink {
    virtual bool push(const protocol::Command& c) = 0;
protected:
    ~CommandSink() = default;
};

class Mk2Perform {
public:
    // Knob pages (PAGE < >), hardware-mapping.md §3.3.
    enum class Page : uint8_t { Play = 0, Clip = 1, Layer = 2, Fx = 3, Mix = 4, Count };
    // What the 16 pads do (hardware-mapping.md §3.4).
    enum class PadMode : uint8_t { Clips = 0, Slice = 1, Looks = 2, FxPads = 3, Moods = 4, Count };

    // M4: the show status (Local\HimmelShow, scenes on the page, undo depth, macros)
    // and the transport section (cue points, clock). Either may be null (then LOOKS
    // pads are dark, SLICE uses even slices, no beat pulse). The pointers must stay
    // valid while the mapper is used; the engine passes its own objects once.
    void set_context(const protocol::ShowStatus* show, const protocol::TransportSection* transport) {
        show_ = show;
        transport_ = transport;
    }

    // BROWSE (MEDIA M7 follow-up): ready library clips left out of the catalog because it is
    // full (Local\HimmelLibrary catalog_full, from the renderer's library feed). Per frame.
    void set_library(uint32_t catalog_full) { catalog_full_ = catalog_full; }
    // ==== ENGINE (catalog v3, wave 7): every clip the engine has (up to 8192), for BROWSE past
    // id 255. Set once by the renderer (render thread only; count() / info(id) / entry(id).library_id
    // / state(id)). Not used yet: CONTROL wave 8. The `catalog` argument stays the ids < 256 window.
    void set_clips(const engine::ClipDirectory* clips) { clips_ = clips; }
    // ==== end ENGINE (catalog v3) ====

    // One MK2 event (from the Local\HimmelMk2 ring). `state` is the engine's
    // latest LiveState / Catalog (the engine passes its own copy).
    void on_event(const mk2::Event& e, const protocol::LiveState& state, const protocol::Catalog& catalog,
                  CommandSink& sink);

    // Called once per frame (or at ~30 Hz): time-based behaviour (held buttons,
    // pressure, flash messages) and the outputs. Fills LEDs and both 256x64 screens.
    void tick(uint64_t now_us, const protocol::LiveState& state, const protocol::Catalog& catalog, CommandSink& sink,
              mk2::LedState& leds, uint8_t screens[2][mk2::kScreenBytes]);

    // The MK2 was (re)connected: forget held buttons, resend everything.
    void reset();

    // Read side for tests and the GUI mirror.
    Page page() const { return page_; }
    PadMode pad_mode() const { return pad_mode_; }
    uint8_t pad_target(int pad) const { return pad >= 1 && pad <= 16 ? pad_target_[pad - 1] : 0; }
    const char* flash() const { return flash_; }
    bool browsing() const { return browse_; }
    int browse_selected() const { return browse_sel_; }
    // ==== ENGINE (quit): the SYSTEM menu (SHIFT + wheel). QUIT HIMMEL = push and hold the wheel
    // for kQuitHoldUs, with a countdown; letting go earlier cancels (command-protocol.md §4e). ====
    static constexpr uint64_t kQuitHoldUs = 2000000;
    bool system_menu() const { return sys_; }
    bool quit_sent() const { return quit_sent_; }
    // ==== end ENGINE (quit) ====

private:
    // Implementation state (fixed size). CONTROL may change these freely.
    // Focus is a ROLE (PREP = the PREVIEW deck, PLAY = the PROGRAM deck) plus a
    // layer, so it stays on "PREVIEW layer n" when a TAKE swaps the decks.
    enum : uint8_t { kPrep = 0, kPlay = 1 };

    struct PadHold {             // what a held pad started (for pressure and release)
        uint8_t kind = 0;        // 0 none, 1 clip layers, 2 HIT, 3 slice, 4 fx pad, 5 scene, 6 consumed
        uint8_t deck = 0xFF;
        uint8_t layer_mask = 0;  // clip pads: the layers it landed on
        uint8_t loop_step = 0;   // slice: 0 = no beat loop, 1.. = 1, 1/2, 1/4 ... beats
        uint16_t press = 0;      // latest pressure 0..4095
        uint16_t press_sent = 0;
        uint64_t down_us = 0;
    };

    void sync(const protocol::LiveState& s, CommandSink& sink);
    void send(protocol::Command c, CommandSink& sink);
    void send_focus(const protocol::LiveState& s, CommandSink& sink);
    uint8_t focus_deck(const protocol::LiveState& s) const;
    uint8_t side_deck(const protocol::LiveState& s, uint8_t side) const;
    int pad_pages(const protocol::Catalog& cat) const;
    bool held(mk2::Button b) const { return (held_ >> static_cast<int>(b)) & 1u; }
    void say(const char* text, uint64_t now_us);
    void on_button(mk2::Button b, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink,
                   uint64_t now_us);
    void on_button_up(mk2::Button b, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink,
                      uint64_t now_us);
    void on_group(int g, const protocol::LiveState& s, CommandSink& sink, uint64_t now_us);
    void on_knob(int k, int delta, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink);
    int steps(int k, int delta, int units);   // stepped knobs: whole steps out of the raw delta
    void on_pad(int pad, int velocity, const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink,
                uint64_t now_us);
    void on_pad_up(int pad, const protocol::LiveState& s, CommandSink& sink);
    void send_pressure(const protocol::LiveState& s, CommandSink& sink);
    void cycle_fx(uint8_t deck, uint8_t layer, uint8_t slot, const protocol::FxSlotState& fx, int dir,
                  const protocol::Catalog& cat, CommandSink& sink);
    void hold_program_layers(const protocol::LiveState& s, CommandSink& sink, bool on, int what);
    int strobe_effect(const protocol::Catalog& cat) const;

    const protocol::ShowStatus* show_ = nullptr;
    const protocol::TransportSection* transport_ = nullptr;

    uint64_t held_ = 0;              // bit per mk2::Button
    bool shift_ = false;
    uint32_t seq_ = 0;
    Page page_ = Page::Play;
    PadMode pad_mode_ = PadMode::Clips;
    int pad_page_ = 0;
    uint8_t focus_side_ = kPrep;     // D75: start focus PREP
    uint8_t focus_layer_ = 0;
    bool synced_ = false;            // false until the first state is seen (and after reset())
    uint32_t epoch_ = 0;
    uint8_t last_program_ = 0;
    uint8_t seen_focus_deck_ = protocol::kNone, seen_focus_layer_ = protocol::kNone;
    uint16_t pads_held_ = 0;         // bit (pad - 1)
    PadHold pad_[16];
    uint8_t pad_target_[16] = {};    // protocol::PadTarget per pad (session only, GUESS: see handoff)
    int last_group_ = -1;            // double-tap
    uint64_t last_group_us_ = 0;
    bool erase_used_ = false;        // a group / ALL / pad was pressed while ERASE was held
    bool rec_used_ = false;          // a pad was pressed while REC was held
    int touched_knob_ = -1;          // inverted on the screen for a moment
    uint64_t touched_us_ = 0;
    uint64_t last_event_us_ = 0;
    int knob_acc_[8] = {};           // stepped knobs: raw units toward the next step
    uint32_t take_ms_ = 0;           // MIX page: 0 = the show default
    int fx_pad_ = -1;                // FX PADS: the pad whose effect is on (master FX slot 3)
    bool fx_pad_latched_ = false;
    bool freeze_ = false, freeze_latched_ = false;
    bool strobe_ = false, strobe_latched_ = false;
    bool black_latched_ = false;
    bool assist_ = false;
    bool repeat_ = false;            // NOTE REPEAT held
    bool tempo_turned_ = false;      // TEMPO held + wheel: no tap on release
    char flash_[40] = {};            // a short message on the left screen ("3c saved")
    uint64_t flash_until_us_ = 0;
    uint32_t show_msg_seq_ = 0;
    // BROWSE: the catalog as a list on the screens; the wheel / knob 1 scroll, push / ENTER load.
    bool browse_ = false;
    int browse_sel_ = 0;
    uint32_t catalog_full_ = 0;
    const engine::ClipDirectory* clips_ = nullptr;  // ENGINE (catalog v3): see set_clips()
    void browse_load(const protocol::LiveState& s, const protocol::Catalog& cat, CommandSink& sink, uint64_t now_us);
    void draw_browse(uint64_t now_us, const protocol::LiveState& s, const protocol::Catalog& cat,
                     uint8_t screens[2][mk2::kScreenBytes]);
    // ==== ENGINE (quit) ====
    bool sys_ = false;            // the SYSTEM menu is open
    uint64_t quit_hold_us_ = 0;   // the wheel was pushed down in the menu at this time, 0 = not held
    bool quit_sent_ = false;      // SystemQuit went out (once)
    void draw_system(uint64_t now_us, uint8_t screens[2][mk2::kScreenBytes]);
    // ==== end ENGINE (quit) ====
};

}  // namespace himmel::perform
