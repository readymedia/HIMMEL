// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ingest/library_move.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "himmel/library/library_db.h"

namespace himmel::ingest {

namespace {

const char* const kDbFiles[] = {"himmel-library.db", "himmel-library.db-wal", "himmel-library.db-shm"};

void add_tree(const fs::path& root, const fs::path& rel_dir, MovePlan& p) {
    std::error_code ec;
    const fs::path dir = root / rel_dir;
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        p.files.push_back(fs::relative(it->path(), root, ec));
        p.bytes += it->file_size(ec);
    }
}

struct CopyCtx {
    const std::function<bool(uint64_t, uint64_t)>* progress;
    uint64_t base, total;
};

DWORD CALLBACK on_copy(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                       LPVOID data) {
    auto* c = static_cast<CopyCtx*>(data);
    if (c->progress && *c->progress && !(*c->progress)(c->base + static_cast<uint64_t>(done.QuadPart), c->total))
        return PROGRESS_CANCEL;
    return PROGRESS_CONTINUE;
}

}  // namespace

MovePlan plan_move(const fs::path& root) {
    MovePlan p;
    for (const char* d : kDerivedDirs) add_tree(root, d, p);
    std::error_code ec;
    for (const char* f : {library::kDbFile, "ingest.log"}) {
        const fs::path a = root / f;
        if (fs::is_regular_file(a, ec)) {
            p.files.push_back(f);
            p.bytes += fs::file_size(a, ec);
        }
    }
    return p;
}

uint64_t library_bytes(const fs::path& root) {
    MovePlan p;
    for (const char* d : kDerivedDirs) add_tree(root, d, p);
    std::error_code ec;
    for (const char* f : kDbFiles)
        if (fs::is_regular_file(root / f, ec)) p.bytes += fs::file_size(root / f, ec);
    return p.bytes;
}

bool empty_or_missing(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    if (!fs::is_directory(dir, ec)) return false;
    return fs::directory_iterator(dir, ec) == fs::directory_iterator();
}

bool copy_library(const fs::path& from, const fs::path& to, const MovePlan& plan,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& err) {
    std::error_code ec;
    std::vector<fs::path> made;
    auto undo = [&] {
        for (const auto& f : made) fs::remove(to / f, ec);
        for (const char* d : kDerivedDirs) fs::remove(to / d, ec);  // only when empty: never the target's own files
    };
    uint64_t done = 0;
    for (const auto& rel : plan.files) {
        const fs::path src = from / rel, dst = to / rel;
        fs::create_directories(dst.parent_path(), ec);
        CopyCtx ctx{&progress, done, plan.bytes};
        BOOL cancel = FALSE;
        if (!CopyFileExW(src.wstring().c_str(), dst.wstring().c_str(), on_copy, &ctx, &cancel, COPY_FILE_FAIL_IF_EXISTS)) {
            const DWORD e = GetLastError();
            err = e == ERROR_REQUEST_ABORTED ? "the move was cancelled"
                  : (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) ? "the target disk is full"
                  : e == ERROR_FILE_EXISTS ? "the target already has a file " + rel.string()
                                           : "cannot copy " + rel.string() + " (Windows error " + std::to_string(e) + ")";
            if (e != ERROR_FILE_EXISTS) made.push_back(rel);
            undo();
            return false;
        }
        made.push_back(rel);
        if (fs::file_size(src, ec) != fs::file_size(dst, ec) || ec) {
            err = "the copy of " + rel.string() + " has the wrong size";
            undo();
            return false;
        }
        done += fs::file_size(src, ec);
        if (progress && !progress(done, plan.bytes)) {
            err = "the move was cancelled";
            undo();
            return false;
        }
    }
    return true;
}

int delete_old_library(const fs::path& old_root) {
    std::error_code ec;
    int left = 0;
    auto rm_tree = [&](const fs::path& dir) {
        if (!fs::is_directory(dir, ec)) return;
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
        for (const auto& f : files) {
            std::error_code e2;
            fs::remove(f, e2);
            if (e2) ++left;
        }
        std::error_code e3;
        if (left == 0) fs::remove_all(dir, e3);
    };
    for (const char* d : kDerivedDirs) rm_tree(old_root / d);
    rm_tree(old_root / "work");
    for (const char* f : kDbFiles) {
        std::error_code e2;
        if (fs::exists(old_root / f, e2)) {
            fs::remove(old_root / f, e2);
            if (e2) ++left;
        }
    }
    return left;
}

}  // namespace himmel::ingest
