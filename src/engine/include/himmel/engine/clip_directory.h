// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Catalog v3 (ENGINE wave 7; engine-state.md version 4): every clip the engine can play,
// up to protocol::kMaxCatalogClips, by clip id (= index, append-only), with its library id
// and state (Ready / Missing / Busy / Removed).
//
// Pure: no Windows, no locks. The entries are allocated once in the constructor (start-up);
// add() / set_state() / publish() never allocate, so the render loop owns one and keeps it
// in step with the clip table (library feed) and the file watch. publish() writes only the
// pages that changed into the state block (one seqlock per page). DeckModel reads it for
// ids, clip geometry and LayerClipLib.
#pragma once

#include <cstdint>
#include <memory>

#include "himmel/protocol/state.h"

namespace himmel::engine {

class ClipDirectory {
public:
    ClipDirectory();
    ClipDirectory(const ClipDirectory&) = delete;
    ClipDirectory& operator=(const ClipDirectory&) = delete;

    uint32_t count() const { return count_; }
    bool valid(int id) const { return id >= 0 && static_cast<uint32_t>(id) < count_; }
    const protocol::ClipEntry& entry(int id) const { return e_[id]; }
    const protocol::ClipInfo& info(int id) const { return e_[id].info; }
    protocol::ClipState state(int id) const { return static_cast<protocol::ClipState>(e_[id].state); }

    // Appends a clip; returns its id, or -1 when the directory is full (counted in full()).
    int add(const protocol::ClipInfo& info, int64_t library_id = 0,
            protocol::ClipState state = protocol::ClipState::Ready);
    // Changes a clip's state; returns true when it changed.
    bool set_state(int id, protocol::ClipState state);
    void add_full(uint32_t n) { full_ += n; }
    uint32_t full() const { return full_; }

    // The newest id with that library id whose state is Ready, else the newest with that
    // library id in any state, else -1. `state` (optional) gets the state of the id returned.
    int find_library(int64_t library_id, protocol::ClipState* state = nullptr) const;
    // The first id with that name (case-insensitive, ASCII), or -1.
    int find_name(const char* name) const;

    // The `catalog` section's window (ids 0..kMaxClips-1): copies entries [from, to) of the
    // window into `cat` and sets cat.clip_count. Returns true when clip_count or an entry changed.
    bool mirror_window(protocol::Catalog& cat);

    // Writes the pages that changed since the last publish (all of them on the first call,
    // continuing the page generations already in the block) and bumps the header. Returns
    // the number of pages written.
    int publish(protocol::ClipPagesHeader& h, protocol::ClipPage* pages);
    bool dirty() const { return any_dirty_; }

    // Start-up: rebuild from a reader's copy (a previous renderer's block), e.g. to restore by name.
    void load_from(const protocol::ClipEntry* entries, uint32_t n);

private:
    void mark(int id);
    std::unique_ptr<protocol::ClipEntry[]> e_;
    uint32_t count_ = 0;
    uint32_t full_ = 0;
    uint32_t published_full_ = 0;
    bool page_dirty_[protocol::kClipPageCount] = {};
    bool window_dirty_ = true;
    bool any_dirty_ = true;
    bool first_publish_ = true;
};

}  // namespace himmel::engine
