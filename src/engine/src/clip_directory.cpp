// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/clip_directory.h"

#include <algorithm>
#include <cstring>

#include "himmel/protocol/shm.h"

namespace himmel::engine {

using namespace himmel::protocol;

namespace {
bool same_name(const char* a, const char* b) {
    for (int i = 0; i < kNameLen; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
    return true;
}
}  // namespace

ClipDirectory::ClipDirectory() : e_(new ClipEntry[kMaxCatalogClips]) {
    std::memset(static_cast<void*>(e_.get()), 0, sizeof(ClipEntry) * kMaxCatalogClips);
}

void ClipDirectory::mark(int id) {
    page_dirty_[id / kClipPageSize] = true;
    if (id < kMaxClips) window_dirty_ = true;
    any_dirty_ = true;
}

int ClipDirectory::add(const ClipInfo& info, int64_t library_id, ClipState state) {
    if (count_ >= static_cast<uint32_t>(kMaxCatalogClips)) {
        ++full_;
        any_dirty_ = true;
        return -1;
    }
    const int id = static_cast<int>(count_++);
    ClipEntry& e = e_[id];
    e.info = info;
    e.info.name[kNameLen - 1] = 0;
    e.library_id = library_id;
    e.state = static_cast<uint8_t>(state);
    ++e.gen;
    mark(id);
    return id;
}

bool ClipDirectory::set_state(int id, ClipState state) {
    if (!valid(id) || e_[id].state == static_cast<uint8_t>(state)) return false;
    e_[id].state = static_cast<uint8_t>(state);
    ++e_[id].gen;
    mark(id);
    return true;
}

int ClipDirectory::find_library(int64_t library_id, ClipState* state) const {
    if (library_id <= 0) return -1;
    int any = -1;
    for (uint32_t i = count_; i-- > 0;) {
        if (e_[i].library_id != library_id) continue;
        if (e_[i].state == static_cast<uint8_t>(ClipState::Ready)) {
            if (state) *state = ClipState::Ready;
            return static_cast<int>(i);
        }
        if (any < 0) any = static_cast<int>(i);
    }
    if (state && any >= 0) *state = static_cast<ClipState>(e_[any].state);
    return any;
}

int ClipDirectory::find_name(const char* name) const {
    if (!name || !name[0]) return -1;
    for (uint32_t i = 0; i < count_; ++i)
        if (same_name(e_[i].info.name, name)) return static_cast<int>(i);
    return -1;
}

bool ClipDirectory::mirror_window(Catalog& cat) {
    const uint32_t n = std::min<uint32_t>(count_, kMaxClips);
    if (!window_dirty_ && cat.clip_count == n) return false;
    for (uint32_t i = 0; i < n; ++i) cat.clip[i] = e_[i].info;
    cat.clip_count = n;
    window_dirty_ = false;
    return true;
}

int ClipDirectory::publish(ClipPagesHeader& h, ClipPage* pages) {
    if (!any_dirty_) return 0;
    int written = 0;
    for (int p = 0; p < kClipPageCount; ++p) {
        if (!first_publish_ && !page_dirty_[p]) continue;
        const uint32_t base = static_cast<uint32_t>(p) * kClipPageSize;
        const uint32_t used = count_ > base ? std::min<uint32_t>(count_ - base, kClipPageSize) : 0;
        if (first_publish_ && used == 0 && !page_dirty_[p] && h.page_gen[p].load(std::memory_order_relaxed) == 0) continue;
        const uint32_t gen = h.page_gen[p].load(std::memory_order_relaxed) + 1;
        ClipPage& pg = pages[p];
        // Seqlock by hand: the page is big, so copy only the entries in use (the rest is zeroed).
        pg.seq.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        pg.data.gen = gen;
        pg.data.used = used;
        if (used) std::memcpy(static_cast<void*>(pg.data.e), &e_[base], sizeof(ClipEntry) * used);
        if (used < static_cast<uint32_t>(kClipPageSize))
            std::memset(static_cast<void*>(&pg.data.e[used]), 0, sizeof(ClipEntry) * (kClipPageSize - used));
        std::atomic_thread_fence(std::memory_order_release);
        pg.seq.fetch_add(1, std::memory_order_acq_rel);
        h.page_gen[p].store(gen, std::memory_order_release);
        page_dirty_[p] = false;
        ++written;
    }
    h.capacity = kMaxCatalogClips;
    h.page_size = kClipPageSize;
    h.full.store(full_, std::memory_order_relaxed);
    h.count.store(count_, std::memory_order_release);
    h.gen.fetch_add(1, std::memory_order_acq_rel);
    published_full_ = full_;
    first_publish_ = false;
    any_dirty_ = false;
    return written;
}

void ClipDirectory::load_from(const ClipEntry* entries, uint32_t n) {
    count_ = std::min<uint32_t>(n, kMaxCatalogClips);
    for (uint32_t i = 0; i < count_; ++i) e_[i] = entries[i];
    for (auto& d : page_dirty_) d = true;
    window_dirty_ = any_dirty_ = true;
}

}  // namespace himmel::engine
