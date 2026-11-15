// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// A named shared-memory block on the paging file ("Local\..."), for the protocol
// blocks in src/protocol/ (commands, state, preview). Open-or-create: whoever
// comes first creates it (the OS zero-fills it); the others open the same one.
// Start-up / reconnect only, never in the frame loop.
#pragma once

#include <cstddef>
#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::engine {

class ShmBlock {
public:
    enum class Result { Failed, Created, Opened };
    ShmBlock() = default;
    ~ShmBlock() { close(); }
    ShmBlock(const ShmBlock&) = delete;
    ShmBlock& operator=(const ShmBlock&) = delete;

    Result open_or_create(const wchar_t* name, size_t size);
    bool open_existing(const wchar_t* name, size_t size);  // never creates
    void close();
    void* data() const { return view_; }
    bool is_open() const { return view_ != nullptr; }

private:
    void* mapping_ = nullptr;
    void* view_ = nullptr;
};

// The creator writes the header last (after a release fence), so a reader that
// sees the magic sees the rest. Readers call header_ok() before trusting anything.
inline void write_header(protocol::BlockHeader& h, uint32_t magic, uint32_t version, uint32_t size) {
    h.version = version;
    h.size = size;
    h.reserved = 0;
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint32_t>&>(h.magic).store(magic, std::memory_order_release);
}
inline bool header_ok(const protocol::BlockHeader& h, uint32_t magic, uint32_t version, uint32_t size) {
    const uint32_t m = reinterpret_cast<const std::atomic<uint32_t>&>(h.magic).load(std::memory_order_acquire);
    return m == magic && h.version == version && h.size == size;
}

}  // namespace himmel::engine
