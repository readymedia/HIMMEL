// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/clip_watch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <string>

#include "himmel/library/clip_table.h"

namespace himmel::engine {

using protocol::ClipState;

ClipState ClipWatch::check_path(const char* utf8_path) {
    if (!utf8_path || !utf8_path[0]) return ClipState::Missing;
    wchar_t w[1024];
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, w, static_cast<int>(std::size(w)));
    if (n <= 0) return ClipState::Missing;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return ClipState::Missing;
    // Share read only: a writer that still has the file open makes this fail (a clip being written).
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ? ClipState::Busy : ClipState::Missing;
    }
    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0;
    CloseHandle(h);
    return ok ? ClipState::Ready : ClipState::Busy;  // an empty file is still being made
}

void ClipWatch::start(int batch, int period_ms) {
    if (worker_.joinable() || !table_) return;
    stop_.store(false);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::thread([this, batch, period_ms] { run(batch, period_ms); });
}

void ClipWatch::stop() {
    stop_.store(true);
    if (wake_) SetEvent(static_cast<HANDLE>(wake_));
    if (worker_.joinable()) worker_.join();
    if (wake_) CloseHandle(static_cast<HANDLE>(wake_));
    wake_ = nullptr;
}

void ClipWatch::poke(int id) {
    if (id < 0 || id >= protocol::kMaxCatalogClips) return;
    poked_[id].store(1, std::memory_order_relaxed);
    any_poke_.fetch_add(1, std::memory_order_release);
    if (wake_) SetEvent(static_cast<HANDLE>(wake_));
}

void ClipWatch::check(int id) {
    const ClipState s = check_path(table_->path[static_cast<size_t>(id)].c_str());
    const uint8_t v = static_cast<uint8_t>(s);
    const uint8_t old = state_[id].load(std::memory_order_relaxed);  // 0 = unchecked, reads as Ready
    const uint8_t seen = old == 0 ? static_cast<uint8_t>(ClipState::Ready) : old;
    state_[id].store(v, std::memory_order_relaxed);
    if (seen != v) gen_.fetch_add(1, std::memory_order_release);
}

void ClipWatch::run(int batch, int period_ms) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    uint32_t cursor = 0, seen_poke = 0;
    while (!stop_.load()) {
        const uint32_t n = std::min<uint32_t>(table_->size(), protocol::kMaxCatalogClips);
        const uint32_t pk = any_poke_.load(std::memory_order_acquire);
        if (pk != seen_poke) {
            seen_poke = pk;
            for (uint32_t i = 0; i < n; ++i)
                if (poked_[i].exchange(0, std::memory_order_relaxed)) check(static_cast<int>(i));
        }
        for (int k = 0; k < batch && n > 0; ++k) {
            if (cursor >= n) {
                cursor = 0;
                sweeps_.fetch_add(1, std::memory_order_relaxed);
            }
            check(static_cast<int>(cursor++));
            if (any_poke_.load(std::memory_order_relaxed) != seen_poke) break;  // a load waits: serve it first
        }
        WaitForSingleObject(static_cast<HANDLE>(wake_), static_cast<DWORD>(period_ms));
    }
}

}  // namespace himmel::engine
