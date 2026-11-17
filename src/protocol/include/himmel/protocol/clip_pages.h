// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Catalog v3 reader (engine-state.md §1, §2, version 4): a reader's copy of the clip pages
// of Local\HimmelState. Header-only, no Windows, no heap of its own (the caller keeps the
// copy, ~800 KB, on the heap). Used by the GUI bridge, tools and tests.
//
//   auto copy = std::make_unique<ClipPagesCopy>();
//   ...every poll:   if (read_clip_pages(blk->clips, blk->clip_page, *copy) > 0) { rebuild the list }
//   copy->count, copy->entry(id)
#pragma once

#include <cstdint>
#include <cstring>

#include "himmel/protocol/shm.h"
#include "himmel/protocol/state.h"

namespace himmel::protocol {

struct ClipPagesCopy {
    uint32_t count = 0;                    // clip ids in use
    uint32_t gen = 0;                      // header gen the copy is complete for
    bool valid = false;                    // false until the first complete read
    uint32_t full = 0;                     // clips the engine could not take
    uint32_t page_gen[kClipPageCount] = {};
    ClipPageData page[kClipPageCount] = {};

    const ClipEntry& entry(uint32_t id) const { return page[id / kClipPageSize].e[id % kClipPageSize]; }
    // The newest id of a library clip (any state), or -1.
    int find_library(int64_t library_id) const {
        if (library_id <= 0) return -1;
        for (uint32_t i = count; i-- > 0;)
            if (entry(i).library_id == library_id) return static_cast<int>(i);
        return -1;
    }
};

// Copies every page that changed since the last call. Returns the number of pages read
// (0 = nothing changed), or -1 when a page stayed busy (call again next poll; the copy keeps
// the pages it has and `gen` is not advanced, so nothing is missed).
inline int read_clip_pages(const ClipPagesHeader& h, const ClipPage* pages, ClipPagesCopy& c) {
    const uint32_t g = h.gen.load(std::memory_order_acquire);
    if (c.valid && g == c.gen) return 0;
    int read = 0;
    bool busy = false;
    for (int p = 0; p < kClipPageCount; ++p) {
        const uint32_t pg = h.page_gen[p].load(std::memory_order_acquire);
        if (pg == c.page_gen[p]) continue;
        if (!seq_read(pages[p].seq, pages[p].data, c.page[p])) {
            busy = true;
            continue;
        }
        c.page_gen[p] = c.page[p].gen;  // what was actually copied (may be newer than pg)
        ++read;
    }
    c.count = h.count.load(std::memory_order_acquire);
    if (c.count > static_cast<uint32_t>(kMaxCatalogClips)) c.count = kMaxCatalogClips;
    c.full = h.full.load(std::memory_order_relaxed);
    if (busy) return -1;
    c.gen = g;
    c.valid = true;
    return read;
}

}  // namespace himmel::protocol
