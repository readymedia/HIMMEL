// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/feed.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "himmel/library/library_block.h"
#include "himmel/library/library_db.h"
#include "himmel/library/location.h"

namespace himmel::library {

std::filesystem::path default_library_root() {
    // v3 (D182): the location saved by the Library command wins, then moved-library redirects.
    if (auto saved = read_location(location_file()); !saved.empty()) return resolve_library_root(saved);
    PWSTR docs = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) p = docs;
    CoTaskMemFree(docs);
    if (p.empty()) p = std::filesystem::temp_directory_path();
    return resolve_library_root(p / "Himmel" / "Library");
}

namespace {

bool same_name(const char* a, const char* b) { return _stricmp(a, b) == 0; }

bool name_taken(const ClipTable& t, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; ++i)
        if (same_name(t.info[i].name, name)) return true;
    return false;
}

void set_name(char (&dst)[protocol::kNameLen], const std::string& s) {
    // Truncate on a UTF-8 boundary.
    size_t len = std::min<size_t>(s.size(), protocol::kNameLen - 1);
    while (len > 0 && len < s.size() && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
    std::memcpy(dst, s.data(), len);
    dst[len] = 0;
}

}  // namespace

protocol::ClipInfo clip_info_from(const ClipRecord& r, const ClipTable& table, uint32_t n) {
    protocol::ClipInfo ci{};
    set_name(ci.name, r.name);
    if (name_taken(table, n, ci.name)) {
        for (int k = 1; k < 100; ++k) {
            const std::string cand = r.name.substr(0, protocol::kNameLen - 12) + (k == 1 ? " (lib)" : " (lib " + std::to_string(k) + ")");
            set_name(ci.name, cand);
            if (!name_taken(table, n, ci.name)) break;
        }
    }
    ci.width = static_cast<uint32_t>(std::max(0, r.width));
    ci.height = static_cast<uint32_t>(std::max(0, r.height));
    ci.fps = static_cast<float>(r.fps);
    ci.seconds = static_cast<float>(r.seconds);
    ci.codec = r.codec == "hap" ? 0 : r.codec == "hap_alpha" ? 1 : r.codec == "hap_q" ? 2 : 3;
    ci.thumb_rgb = r.analysis.version > 0 ? (r.analysis.avg_rgb & 0xFFFFFF) : 0x404040;
    return ci;
}

int LibraryFeed::poll_once() {
    auto* db = static_cast<LibraryDb*>(db_);
    if (!db) db_ = db = new LibraryDb();
    if (!db->is_open()) {
        std::error_code ec;
        const auto file = root_ / kDbFile;
        if (!std::filesystem::exists(file, ec) || !db->open(file, true)) return 0;
        // v3: a library with another uid (a switch, not a move) starts the cursor again.
        const std::string uid = db->meta_get("uid");
        if (!uid_.empty() && uid != uid_) last_order_ = 0;
        uid_ = uid;
    }
    polls.fetch_add(1, std::memory_order_relaxed);
    int made = 0;
    for (const ClipRecord& r : db->list_ready_after(last_order_, 64)) {
        last_order_ = std::max(last_order_, r.ready_order);
        const auto play = root_ / std::filesystem::path(std::u8string(r.play_path.begin(), r.play_path.end()));
        std::error_code ec;
        if (r.play_path.empty() || !std::filesystem::exists(play, ec)) {
            skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint32_t n = table_->size();
        const auto u8 = play.u8string();
        if (table_->append(std::string(u8.begin(), u8.end()), clip_info_from(r, *table_, n), r.id) < 0) {
            skipped.fetch_add(1, std::memory_order_relaxed);
            catalog_full.fetch_add(1, std::memory_order_relaxed);  // the clip stays in the library
            continue;
        }
        ++made;
    }
    return made;
}

int LibraryFeed::load_initial(const std::filesystem::path& root, ClipTable& table) {
    root_ = resolve_library_root(root);  // v3: a moved library left a redirect (D182)
    table_ = &table;
    int total = 0;
    for (int made; (made = poll_once()) > 0;) total += made;
    return total;
}

void LibraryFeed::start(int poll_ms) {
    if (!table_ || worker_.joinable()) return;
    stop_.store(false);
    worker_ = std::thread([this, poll_ms] { run(poll_ms); });
}

void LibraryFeed::run(int poll_ms) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HANDLE map = nullptr;
    LibraryBlock* blk = nullptr;
    uint64_t seen_seq = 0;
    uint32_t seen_root = 0;
    ULONGLONG next_poll = 0;
    while (!stop_.load()) {
        if (!blk) {
            map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kLibraryShmName);
            if (map) {
                blk = static_cast<LibraryBlock*>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LibraryBlock)));
                if (!blk) CloseHandle(map), map = nullptr;
            }
        }
        bool due = GetTickCount64() >= next_poll;
        if (blk && reinterpret_cast<const std::atomic<uint32_t>&>(blk->header.magic).load() == kLibraryMagic) {
            const uint64_t s = blk->ready_seq.load(std::memory_order_acquire);
            if (s != seen_seq) seen_seq = s, due = true;
            blk->catalog_full.store(catalog_full.load(std::memory_order_relaxed), std::memory_order_relaxed);
            // v3 (library-db.md §11.5): follow the library folder when the worker switches or moves it.
            if (blk->header.version >= 3) {
                const uint32_t rs = blk->root_seq.load(std::memory_order_acquire);
                if (rs != seen_root && !(rs & 1)) {
                    LibraryRoot lr;
                    if (protocol::seq_read(blk->root_seq, blk->root, lr)) {
                        seen_root = rs;
                        lr.path[sizeof(lr.path) - 1] = 0;
                        const std::string u8 = lr.path;
                        const std::filesystem::path np(std::u8string(u8.begin(), u8.end()));
                        if (!u8.empty() && np != root_) {
                            if (auto* db = static_cast<LibraryDb*>(db_)) db->close();
                            root_ = np;
                            root_changes.fetch_add(1, std::memory_order_relaxed);
                            due = true;
                        }
                    }
                }
            }
        }
        if (due) {
            int made;
            while ((made = poll_once()) > 0) added.fetch_add(static_cast<uint32_t>(made), std::memory_order_relaxed);
            next_poll = GetTickCount64() + static_cast<ULONGLONG>(poll_ms);
        }
        Sleep(100);
    }
    if (blk) UnmapViewOfFile(blk);
    if (map) CloseHandle(map);
}

void LibraryFeed::stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    delete static_cast<LibraryDb*>(db_);
    db_ = nullptr;
}

}  // namespace himmel::library
