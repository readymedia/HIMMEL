// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Catalog v3 (ENGINE wave 7): a low-priority worker that checks the clip files of the
// engine's clip table, so the frame loop knows without any I/O whether a clip can be
// loaded: Ready, Missing (gone / cannot open) or Busy (locked: still being written).
//
// It sweeps every clip in batches (a full pass over 8192 clips in ~16 s, 300 clips in
// under 1 s) and checks a clip at once when poke(id) asks (a load, a layer that shows it).
// The frame loop reads state(id) (an atomic byte) and changed() (a generation).
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "himmel/protocol/state.h"

namespace himmel::library {
struct ClipTable;
}

namespace himmel::engine {

class ClipWatch {
public:
    explicit ClipWatch(const library::ClipTable* table) : table_(table) {}
    ~ClipWatch() { stop(); }
    ClipWatch(const ClipWatch&) = delete;
    ClipWatch& operator=(const ClipWatch&) = delete;

    void start(int batch = 64, int period_ms = 125);
    void stop();

    // Any thread, no allocation: check this clip soon (within ~10 ms).
    void poke(int id);
    // Ready until the worker says otherwise.
    protocol::ClipState state(int id) const {
        const uint8_t s = state_[id].load(std::memory_order_relaxed);
        return s == 0 ? protocol::ClipState::Ready : static_cast<protocol::ClipState>(s);
    }
    uint32_t changed() const { return gen_.load(std::memory_order_acquire); }
    uint32_t sweeps() const { return sweeps_.load(std::memory_order_relaxed); }

    // Synchronous check of one path (also used by tests and start-up).
    static protocol::ClipState check_path(const char* utf8_path);

private:
    void run(int batch, int period_ms);
    void check(int id);

    const library::ClipTable* table_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    void* wake_ = nullptr;  // HANDLE
    std::atomic<uint8_t> state_[protocol::kMaxCatalogClips] = {};   // 0 = unchecked (Ready)
    std::atomic<uint8_t> poked_[protocol::kMaxCatalogClips] = {};
    std::atomic<uint32_t> any_poke_{0};
    std::atomic<uint32_t> gen_{0};
    std::atomic<uint32_t> sweeps_{0};
};

}  // namespace himmel::engine
