// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The renderer's mapping of Local\HimmelAudio. open() is start-up only; read()
// runs per frame and only copies seqlocked sections (no waiting, no I/O).
#include "himmel/engine/reactions.h"

namespace himmel::engine {

bool AudioLink::open() {
    // Open-or-create, and keep the mapping for the renderer's whole life: a
    // restarted himmel_audio then writes into this same block.
    return shm_.open_or_create(audio::kBlockName, sizeof(audio::AudioBlock)) != ShmBlock::Result::Failed;
}

bool AudioLink::read(uint64_t now_us, audio::Signals& sig, audio::ClockSource& midi) {
    alive_ = false;
    auto* b = static_cast<audio::AudioBlock*>(shm_.data());
    if (!b || !header_ok(b->header, audio::kMagic, audio::kVersion, sizeof(audio::AudioBlock))) return false;
    const uint64_t hb = b->status.heartbeat_us.load(std::memory_order_acquire);
    if (hb == 0 || (now_us > hb && now_us - hb > 250000)) return false;  // dead or hung
    alive_ = true;
    audio::ClockSource clocks[audio::kClockSources];
    if (protocol::seq_read(b->clock_seq, b->clock, clocks)) midi = clocks[0];
    if (now_us >= next_input_us_) {
        next_input_us_ = now_us + 500000;
        audio::InputStatus in;
        if (protocol::seq_read(b->input_seq, b->input, in)) input_ = in;
    }
    if (input_.state != static_cast<uint8_t>(audio::InputState::Running)) return false;
    // Writer busy for all retries (rare): the previous copy stands in for this frame.
    // (seq_read copies into its target even when it fails, so read into a scratch copy.)
    if (protocol::seq_read(b->signals_seq, b->signals, scratch_)) {
        last_ = scratch_;
        have_last_ = true;
    }
    if (!have_last_ || last_.hop == 0 || (now_us > last_.time_us && now_us - last_.time_us > 500000))
        return false;  // nothing yet, or the capture stalled
    sig = last_;
    return true;
}

}  // namespace himmel::engine
