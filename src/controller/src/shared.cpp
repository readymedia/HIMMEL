#include "himmel/mk2/shared.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <new>

namespace himmel::mk2 {

SharedMemory::~SharedMemory() {
    if (block_) UnmapViewOfFile(block_);
    if (mapping_) CloseHandle(mapping_);
}

bool SharedMemory::create(const wchar_t* name) {
    const DWORD size = static_cast<DWORD>(sizeof(SharedBlock));
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name);
    if (!h) return false;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
    block_ = static_cast<SharedBlock*>(view);
    // A restarted controller keeps the apps' outputs and attached rings, but resets status.
    if (!existed || block_->magic != kShmMagic || block_->version != kShmVersion || block_->size != size) {
        std::memset(view, 0, size);  // atomics in zeroed memory are valid zero values on x64
        block_->magic = kShmMagic;
        block_->version = kShmVersion;
        block_->size = size;
    }
    auto& st = block_->status;
    st.connected.store(0);
    st.heartbeat_us.store(now_us());
    return true;
}

bool SharedMemory::open(const wchar_t* name) {
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return false;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
    if (!view) {
        CloseHandle(h);
        return false;
    }
    auto* b = static_cast<SharedBlock*>(view);
    if (b->magic != kShmMagic || b->version != kShmVersion || b->size != sizeof(SharedBlock)) {
        UnmapViewOfFile(view);
        CloseHandle(h);
        return false;
    }
    mapping_ = h;
    block_ = b;
    return true;
}

}  // namespace himmel::mk2
