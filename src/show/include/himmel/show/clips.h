// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Clips as a show knows them (wave 8, D210; show-files.md section 3).
//
// A look, a kit pad and the live state remember a clip by its library id
// (library-db.md), its content hash (16 hex) and its name. Engine clip ids are
// only a run-time binding: they are found again on load and on recall, by
// library id first, then by hash (a clip removed and imported again gets a new
// library id but keeps its hash), then by name.
//
// ClipIndex is the show's own copy of the engine's clip directory (catalog v3,
// up to kMaxCatalogClips ids), with the hash of each library clip. The frame
// thread owns it and appends to it (preallocated, no heap after the constructor);
// the save worker reads it at the same time: an entry is complete before the
// count that covers it is published (release / acquire), and the only field
// that changes later, `state`, is atomic.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "himmel/protocol/state.h"

namespace himmel::engine {
class ClipDirectory;
}

namespace himmel::show {

namespace proto = himmel::protocol;

inline constexpr int kHashLen = 17;   // 16 lowercase hex + NUL (library-db.md section 4); "" = unknown

struct ClipIdent {
    int64_t library_id = 0;           // 0 = not a library clip (media/ or --clip)
    char hash[kHashLen] = {};
    char name[proto::kNameLen] = {};
    uint32_t width = 0, height = 0;
    float seconds = 0.0f;
    uint32_t thumb_rgb = 0;
    std::atomic<uint8_t> state{static_cast<uint8_t>(proto::ClipState::Ready)};  // proto::ClipState
};

// The hash of engine clip `id` (from the render process's clip table: hap/<hash>.mov), or false.
using ClipHashFn = bool (*)(void* ctx, int id, char out[kHashLen]);

class ClipIndex {
public:
    ClipIndex();
    ClipIndex(const ClipIndex&) = delete;
    ClipIndex& operator=(const ClipIndex&) = delete;

    // ---- readers (any thread) ----
    uint32_t count() const { return n_.load(std::memory_order_acquire); }
    bool valid(int id) const { return id >= 0 && static_cast<uint32_t>(id) < count(); }
    const ClipIdent& at(int id) const { return e_[id]; }
    proto::ClipState state(int id) const {
        return static_cast<proto::ClipState>(e_[id].state.load(std::memory_order_relaxed));
    }
    // The newest Ready id with this library id / hash, else the newest in any state; -1 = none.
    int find_library(int64_t library_id) const;
    int find_hash(const char* hash) const;
    // The first id with this name (case-insensitive, ASCII); -1 = none.
    int find_name(const char* name) const;

    // ---- the writer (one thread: the frame, or a test / tool before readers start) ----
    void set_hash_source(ClipHashFn fn, void* ctx) { hash_fn_ = fn, hash_ctx_ = ctx; }
    // Appends the directory's new ids and refreshes up to `refresh` states (round robin).
    // Never allocates. Returns the number of ids added.
    int sync(const engine::ClipDirectory& dir, int refresh = 256);
    // Without a directory (tests, tools, an engine without catalog v3): the catalog's ids < 256.
    int sync(const proto::Catalog& cat);
    // The state of one id, now (before a recall uses it).
    void set_state(int id, proto::ClipState s) {
        if (valid(id)) e_[id].state.store(static_cast<uint8_t>(s), std::memory_order_relaxed);
    }
    // Tests / tools: append one clip by hand. Returns its id or -1 (full).
    int add(const proto::ClipInfo& info, int64_t library_id, const char* hash,
            proto::ClipState s = proto::ClipState::Ready);

private:
    void fill(ClipIdent& e, const proto::ClipInfo& info, int64_t library_id, int id);
    std::unique_ptr<ClipIdent[]> e_;
    std::atomic<uint32_t> n_{0};
    uint32_t refresh_at_ = 0;
    ClipHashFn hash_fn_ = nullptr;
    void* hash_ctx_ = nullptr;
};

// A clip as a look / kit pad / the live state remembers it. `name` is the
// caller's buffer (kNameLen), `hash` kHashLen.
struct ClipRefView {
    int64_t* library_id;
    char* hash;
    char* name;
};
// How a reference was found (bind_clip).
enum class ClipMatch : uint8_t { None = 0, Library = 1, Hash = 2, Name = 3 };
// Finds the clip a reference names: library id, then hash, then name. On a match
// the reference is brought up to date (the library id, the hash, the name the
// catalog uses now): an old reference (name only) or one whose library id changed
// (removed and imported again) is upgraded. Returns the engine id or -1.
int bind_clip(const ClipIndex& ix, ClipRefView ref, ClipMatch* how = nullptr);
// Fills a reference from engine clip `id` (-1 = empty: everything cleared).
void ref_from_id(const ClipIndex& ix, int id, ClipRefView ref);
// True when the reference names anything at all.
bool ref_used(const int64_t library_id, const char* hash, const char* name);
// Lowercase 16-hex check (library-db.md section 4).
bool is_hash(const char* s);
// The hash from a library play path ".../hap/<hash>.mov" (the file stem), or false.
bool hash_from_path(const char* path, char out[kHashLen]);

}  // namespace himmel::show
