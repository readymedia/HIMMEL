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
#pragma once

#include <cstdint>

#include "himmel/mk2/output.h"
#include "himmel/mk2/types.h"
#include "himmel/protocol/state.h"

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
    // One MK2 event (from the Local\HimmelMk2 ring). `state` is the engine's
    // latest LiveState / Catalog (the engine passes its own copy).
    void on_event(const mk2::Event& e, const protocol::LiveState& state, const protocol::Catalog& catalog,
                  CommandSink& sink);

    // Called once per frame (or at ~30 Hz): time-based behaviour (held buttons,
    // wheel acceleration) and the outputs. Fills LEDs and both 256x64 screens.
    void tick(uint64_t now_us, const protocol::LiveState& state, const protocol::Catalog& catalog, CommandSink& sink,
              mk2::LedState& leds, uint8_t screens[2][mk2::kScreenBytes]);

    // The MK2 was (re)connected: forget held buttons, resend everything.
    void reset();

private:
    // Implementation state (fixed size). CONTROL may change these freely.
    bool shift_ = false;
    uint32_t seq_ = 0;
    int pad_page_ = 0;
    uint8_t reserved_[256] = {};
};

}  // namespace himmel::perform
