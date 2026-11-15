// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Small Windows helper for the protocol blocks (docs/interfaces/): open an
// existing named mapping, or create-or-open one, and check its header.
// Used by himmel_ui and fake_engine. Never blocks.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::ui {

enum class ShmStatus { Absent, Ok, Version, Starting };

inline const char* shm_status_name(ShmStatus s) {
    switch (s) {
        case ShmStatus::Ok: return "ok";
        case ShmStatus::Version: return "version";
        case ShmStatus::Starting: return "starting";
        default: return "absent";
    }
}

class ShmView {
public:
    ShmView() = default;
    ShmView(const ShmView&) = delete;
    ShmView& operator=(const ShmView&) = delete;
    ~ShmView() { close(); }

    void close() {
        if (ptr_) UnmapViewOfFile(ptr_);
        if (map_) CloseHandle(map_);
        ptr_ = nullptr;
        map_ = nullptr;
        created_ = false;
    }

    // Open an existing block. Checks size (VirtualQuery) and the header.
    ShmStatus open_existing(const wchar_t* name, size_t size, uint32_t magic, uint32_t version, bool writable) {
        close();
        map_ = OpenFileMappingW(writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, FALSE, name);
        if (!map_) return ShmStatus::Absent;
        ptr_ = MapViewOfFile(map_, writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, 0);
        if (!ptr_) {
            close();
            return ShmStatus::Absent;
        }
        return check(size, magic, version);
    }

    // Create the block, or open it if another process was first
    // (docs/interfaces/command-protocol.md §2). A creator writes the header
    // with `write_header()` once it has filled the rest.
    ShmStatus create_or_open(const wchar_t* name, size_t size, uint32_t magic, uint32_t version) {
        close();
        const uint64_t sz = size;
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(sz >> 32),
                                  static_cast<DWORD>(sz & 0xFFFFFFFFu), name);
        if (!map_) return ShmStatus::Absent;
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        ptr_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!ptr_) {
            close();
            return ShmStatus::Absent;
        }
        if (!existed) {
            created_ = true;  // paging-file memory is already zero-filled
            return ShmStatus::Ok;
        }
        return check(size, magic, version);
    }

    // Write the header last (magic stored after a release fence).
    void write_header(size_t size, uint32_t magic, uint32_t version) {
        auto* h = static_cast<protocol::BlockHeader*>(ptr_);
        h->version = version;
        h->size = static_cast<uint32_t>(size);
        std::atomic_thread_fence(std::memory_order_release);
        reinterpret_cast<std::atomic<uint32_t>*>(&h->magic)->store(magic, std::memory_order_release);
    }

    // Re-check a header we hold (the other side may have been replaced).
    ShmStatus check(size_t size, uint32_t magic, uint32_t version) const {
        if (!ptr_) return ShmStatus::Absent;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr_, &mbi, sizeof mbi) == 0 || mbi.RegionSize < size) return ShmStatus::Version;
        const auto* h = static_cast<const protocol::BlockHeader*>(ptr_);
        const uint32_t m = reinterpret_cast<const std::atomic<uint32_t>*>(&h->magic)->load(std::memory_order_acquire);
        if (m == 0) return ShmStatus::Starting;
        if (m != magic || h->version != version || h->size != size) return ShmStatus::Version;
        return ShmStatus::Ok;
    }

    template <class T>
    T* as() const { return static_cast<T*>(ptr_); }
    bool is_open() const { return ptr_ != nullptr; }
    bool created() const { return created_; }

private:
    HANDLE map_ = nullptr;
    void* ptr_ = nullptr;
    bool created_ = false;
};

}  // namespace himmel::ui
