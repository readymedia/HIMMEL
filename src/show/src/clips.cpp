// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/clips.h"

#include <cctype>
#include <cstring>

#include "himmel/engine/clip_directory.h"

namespace himmel::show {

using namespace himmel::protocol;

namespace {

void copy_n(char* dst, int cap, const char* src) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

constexpr auto kReady = static_cast<uint8_t>(ClipState::Ready);

}  // namespace

ClipIndex::ClipIndex() : e_(std::make_unique<ClipIdent[]>(kMaxCatalogClips)) {}

void ClipIndex::fill(ClipIdent& e, const ClipInfo& info, int64_t library_id, int id) {
    e.library_id = library_id;
    copy_n(e.name, kNameLen, info.name);
    e.width = info.width, e.height = info.height;
    e.seconds = info.seconds;
    e.thumb_rgb = info.thumb_rgb;
    e.hash[0] = 0;
    if (hash_fn_ && library_id > 0) {
        char h[kHashLen] = {};
        if (hash_fn_(hash_ctx_, id, h) && is_hash(h)) copy_n(e.hash, kHashLen, h);
    }
}

int ClipIndex::sync(const engine::ClipDirectory& dir, int refresh) {
    const uint32_t have = n_.load(std::memory_order_relaxed);
    uint32_t want = dir.count();
    if (want > static_cast<uint32_t>(kMaxCatalogClips)) want = kMaxCatalogClips;
    for (uint32_t i = have; i < want; ++i) {
        const ClipEntry& d = dir.entry(static_cast<int>(i));
        fill(e_[i], d.info, d.library_id, static_cast<int>(i));
        e_[i].state.store(d.state, std::memory_order_relaxed);
    }
    if (want > have) n_.store(want, std::memory_order_release);
    // States change (a file goes missing, a clip is removed): a slice per call.
    for (int k = 0; k < refresh && want > 0; ++k) {
        if (refresh_at_ >= want) refresh_at_ = 0;
        e_[refresh_at_].state.store(dir.entry(static_cast<int>(refresh_at_)).state, std::memory_order_relaxed);
        ++refresh_at_;
    }
    return want > have ? static_cast<int>(want - have) : 0;
}

int ClipIndex::sync(const Catalog& cat) {
    const uint32_t have = n_.load(std::memory_order_relaxed);
    uint32_t want = cat.clip_count;
    if (want > static_cast<uint32_t>(kMaxClips)) want = kMaxClips;
    for (uint32_t i = have; i < want; ++i) {
        fill(e_[i], cat.clip[i], 0, static_cast<int>(i));
        e_[i].state.store(kReady, std::memory_order_relaxed);
    }
    if (want > have) n_.store(want, std::memory_order_release);
    return want > have ? static_cast<int>(want - have) : 0;
}

int ClipIndex::add(const ClipInfo& info, int64_t library_id, const char* hash, ClipState s) {
    const uint32_t n = n_.load(std::memory_order_relaxed);
    if (n >= static_cast<uint32_t>(kMaxCatalogClips)) return -1;
    fill(e_[n], info, library_id, static_cast<int>(n));
    if (hash && is_hash(hash)) copy_n(e_[n].hash, kHashLen, hash);
    e_[n].state.store(static_cast<uint8_t>(s), std::memory_order_relaxed);
    n_.store(n + 1, std::memory_order_release);
    return static_cast<int>(n);
}

int ClipIndex::find_library(int64_t library_id) const {
    if (library_id <= 0) return -1;
    int any = -1;
    for (uint32_t i = count(); i-- > 0;) {
        if (e_[i].library_id != library_id) continue;
        if (e_[i].state.load(std::memory_order_relaxed) == kReady) return static_cast<int>(i);
        if (any < 0) any = static_cast<int>(i);
    }
    return any;
}

int ClipIndex::find_hash(const char* hash) const {
    if (!hash || !hash[0]) return -1;
    int any = -1;
    for (uint32_t i = count(); i-- > 0;) {
        if (std::strcmp(e_[i].hash, hash) != 0) continue;
        if (e_[i].state.load(std::memory_order_relaxed) == kReady) return static_cast<int>(i);
        if (any < 0) any = static_cast<int>(i);
    }
    return any;
}

int ClipIndex::find_name(const char* name) const {
    if (!name || !name[0]) return -1;
    const uint32_t n = count();
    for (uint32_t i = 0; i < n; ++i)
        if (ieq(e_[i].name, name)) return static_cast<int>(i);
    return -1;
}

bool is_hash(const char* s) {
    if (!s) return false;
    int i = 0;
    for (; s[i]; ++i)
        if (i >= kHashLen - 1 || !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return i == kHashLen - 1;
}

bool hash_from_path(const char* path, char out[kHashLen]) {
    out[0] = 0;
    if (!path) return false;
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    char stem[kHashLen] = {};
    int i = 0;
    for (; base[i] && base[i] != '.' && i < kHashLen - 1; ++i) stem[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(base[i])));
    if (base[i] != '.' && base[i] != 0) return false;  // longer than a hash
    stem[i] = 0;
    if (!is_hash(stem)) return false;
    copy_n(out, kHashLen, stem);
    return true;
}

bool ref_used(const int64_t library_id, const char* hash, const char* name) {
    return library_id > 0 || (hash && hash[0]) || (name && name[0]);
}

void ref_from_id(const ClipIndex& ix, int id, ClipRefView ref) {
    if (!ix.valid(id)) {
        *ref.library_id = 0;
        ref.hash[0] = 0;
        ref.name[0] = 0;
        return;
    }
    const ClipIdent& e = ix.at(id);
    *ref.library_id = e.library_id;
    copy_n(ref.hash, kHashLen, e.hash);
    copy_n(ref.name, kNameLen, e.name);
}

int bind_clip(const ClipIndex& ix, ClipRefView ref, ClipMatch* how) {
    if (how) *how = ClipMatch::None;
    if (!ref_used(*ref.library_id, ref.hash, ref.name)) return -1;
    int id = -1;
    ClipMatch m = ClipMatch::None;
    if (*ref.library_id > 0) {
        id = ix.find_library(*ref.library_id);
        // Another library can reuse the same row ids: a known hash that differs wins.
        if (id >= 0 && ref.hash[0] && ix.at(id).hash[0] && std::strcmp(ix.at(id).hash, ref.hash) != 0) id = -1;
        if (id >= 0) m = ClipMatch::Library;
    }
    // Removed and imported again: the old id is not Ready any more, the same content has a new
    // library id. The hash finds it (relink).
    if (ref.hash[0] && (id < 0 || ix.state(id) != ClipState::Ready)) {
        const int by_hash = ix.find_hash(ref.hash);
        if (by_hash >= 0 && (id < 0 || ix.state(by_hash) == ClipState::Ready)) id = by_hash, m = ClipMatch::Hash;
    }
    if (id < 0 && ref.name[0]) {
        id = ix.find_name(ref.name);
        // A name match is only a guess when the reference knew a library clip: never across hashes.
        if (id >= 0 && ref.hash[0] && ix.at(id).hash[0] && std::strcmp(ix.at(id).hash, ref.hash) != 0) id = -1;
        if (id >= 0) m = ClipMatch::Name;
    }
    if (id < 0) return -1;  // missing: the reference stays as it was (it is saved back unchanged)
    const ClipIdent& e = ix.at(id);
    *ref.library_id = e.library_id;
    if (e.hash[0]) copy_n(ref.hash, kHashLen, e.hash);
    else if (e.library_id == 0) ref.hash[0] = 0;
    copy_n(ref.name, kNameLen, e.name);
    if (how) *how = m;
    return id;
}

}  // namespace himmel::show
