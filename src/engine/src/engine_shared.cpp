// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/engine_shared.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace himmel::engine {

EngineShm::~EngineShm() {
    if (status_) UnmapViewOfFile(status_);
    if (mapping_) CloseHandle(mapping_);
}

bool EngineShm::create() {
    const DWORD size = static_cast<DWORD>(sizeof(EngineStatus));
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, kEngineShmName);
    if (!h) return false;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
    status_ = static_cast<EngineStatus*>(view);
    // A restarted renderer keeps restarts_seen (the supervisor's counter), resets the rest.
    const uint32_t restarts = status_->magic == kEngineMagic ? status_->restarts_seen.load() : 0;
    std::memset(view, 0, size);
    status_->magic = kEngineMagic;
    status_->version = kEngineVersion;
    status_->restarts_seen.store(restarts);
    status_->pid.store(GetCurrentProcessId());
    return true;
}

bool EngineShm::open() {
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kEngineShmName);
    if (!h) return false;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EngineStatus));
    if (!view) {
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
    status_ = static_cast<EngineStatus*>(view);
    return true;
}

}  // namespace himmel::engine
