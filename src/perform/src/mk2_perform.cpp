// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// STUB written by the LEAD so ENGINE can build against the contract at once.
// CONTROL replaces this file with the slice mapping (docs/roadmap.md, "The
// playable slice"). It only does: wheel = crossfader, PLAY = TAKE, RESTART = CUT.
#include "himmel/perform/mk2_perform.h"

#include <cstring>

namespace himmel::perform {

using namespace himmel::protocol;

void Mk2Perform::on_event(const mk2::Event& e, const LiveState&, const Catalog&, CommandSink& sink) {
    Command c{};
    switch (e.type) {
        case mk2::EventType::EncoderDelta:
            c = make(CommandType::Crossfader, kNone, kNone, 0, e.value / 30.0f, kRelative);
            break;
        case mk2::EventType::ButtonDown:
            if (e.id == static_cast<uint8_t>(mk2::Button::Play)) c = make(CommandType::Take);
            else if (e.id == static_cast<uint8_t>(mk2::Button::Restart)) c = make(CommandType::Cut);
            else return;
            break;
        default:
            return;
    }
    c.seq = ++seq_;
    sink.push(c);
}

void Mk2Perform::tick(uint64_t, const LiveState&, const Catalog&, CommandSink&, mk2::LedState& leds,
                      uint8_t screens[2][mk2::kScreenBytes]) {
    std::memset(&leds, 0, sizeof(leds));
    std::memset(screens, 0, 2 * mk2::kScreenBytes);
}

void Mk2Perform::reset() { shift_ = false; }

}  // namespace himmel::perform
