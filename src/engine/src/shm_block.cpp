// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/shm_block.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace himmel::engine {

void ShmBlock::close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    view_ = mapping_ = nullptr;
}

ShmBlock::Result ShmBlock::open_or_create(const wchar_t* name, size_t size) {
    close();
    const uint64_t s = size;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(s >> 32),
                                  static_cast<DWORD>(s), name);
    if (!h) return Result::Failed;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    // An existing block that is smaller than ours (another version) fails here.
    void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!v) {
        CloseHandle(h);
        return Result::Failed;
    }
    mapping_ = h;
    view_ = v;
    return existed ? Result::Opened : Result::Created;
}

bool ShmBlock::open_existing(const wchar_t* name, size_t size) {
    close();
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return false;
    void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!v) {
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
    view_ = v;
    return true;
}

}  // namespace himmel::engine
