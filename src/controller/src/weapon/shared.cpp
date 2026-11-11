// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/weapon/shared.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace himmel::weapon {

std::wstring shm_name(int slot) { return L"Local\\HimmelWeapon" + std::to_wstring(slot); }

SharedMemory::~SharedMemory() { close(); }

void SharedMemory::close() {
    if (block_) UnmapViewOfFile(block_);
    if (mapping_) CloseHandle(mapping_);
    block_ = nullptr;
    mapping_ = nullptr;
}

bool SharedMemory::create(int slot, const Header& h) {
    close();
    const DWORD size = static_cast<DWORD>(sizeof(SharedBlock));
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, shm_name(slot).c_str());
    if (!m) return false;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        CloseHandle(m);
        return false;
    }
    mapping_ = m;
    block_ = static_cast<SharedBlock*>(view);
    if (!existed || block_->magic != kShmMagic || block_->version != kShmVersion || block_->size != size) {
        std::memset(view, 0, size);  // atomics in zeroed memory are valid zero values on x64
        block_->magic = kShmMagic;
        block_->version = kShmVersion;
        block_->size = size;
    }
    block_->header = h;
    block_->header.slot = static_cast<uint32_t>(slot);
    auto& st = block_->status;
    st.connected.store(0);
    st.stop_request.store(0);
    st.battery.store(-1);
    st.heartbeat_us.store(now_us());
    st.device[0] = 0;
    return true;
}

bool SharedMemory::open(int slot) {
    close();
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name(slot).c_str());
    if (!m) return false;
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
    if (!view) {
        CloseHandle(m);
        return false;
    }
    auto* b = static_cast<SharedBlock*>(view);
    if (b->magic != kShmMagic || b->version != kShmVersion || b->size != sizeof(SharedBlock)) {
        UnmapViewOfFile(view);
        CloseHandle(m);
        return false;
    }
    mapping_ = m;
    block_ = b;
    return true;
}

void SharedMemory::publish(const Event& e) {
    if (!block_) return;
    for (auto& ring : block_->rings)
        if (ring.attached.load(std::memory_order_acquire)) ring.push(e);
}

void SharedMemory::set_message(const std::string& m) {
    if (block_) set_text(block_->status.message, m);
}

}  // namespace himmel::weapon
