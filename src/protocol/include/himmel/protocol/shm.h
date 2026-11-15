// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Shared building blocks for the engine protocol blocks (docs/interfaces/).
//
// Header-only, no Windows headers, no heap. The same rules as the MK2 block
// (src/controller/include/himmel/mk2/shared.h): fixed-size PODs, lock-free
// 32/64-bit atomics, one writer per seqlock section, SPSC rings.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace himmel::protocol {

static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

// ---- single-producer / single-consumer ring of fixed-size items ----------
// N must be a power of two. The producer never blocks: a full ring counts a
// drop and returns false. Indices wrap at 2^32 (unsigned arithmetic).
template <class T, uint32_t N>
struct alignas(64) SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    std::atomic<uint32_t> head;      // next write index (producer)
    uint8_t pad0[60];
    std::atomic<uint32_t> tail;      // next read index (consumer)
    std::atomic<uint32_t> dropped;   // items lost because the ring was full
    uint8_t pad1[56];
    T items[N];

    bool push(const T& v) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t t = tail.load(std::memory_order_acquire);
        if (h - t >= N) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        items[h & (N - 1)] = v;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        const uint32_t h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        v = items[t & (N - 1)];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    // Consumer: forget everything queued so far (used when the engine
    // (re)attaches, so stale commands from before a restart are not replayed).
    void skip_all() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }
    uint32_t size() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
};

// ---- sequence lock over one POD section -----------------------------------
// Writer: seq odd -> copy -> seq even. Readers retry while odd or changed.
template <class T>
void seq_write(std::atomic<uint32_t>& seq, T& dst, const T& src) {
    seq.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&dst, &src, sizeof(T));
    std::atomic_thread_fence(std::memory_order_release);
    seq.fetch_add(1, std::memory_order_acq_rel);
}

// Returns (even seq + 1) on a consistent copy, 0 if the writer stayed busy.
template <class T>
uint32_t seq_read(const std::atomic<uint32_t>& seq, const T& src, T& dst) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        std::memcpy(&dst, &src, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = seq.load(std::memory_order_acquire);
        if (s1 == s2) return s1 + 1;
    }
    return 0;
}

inline void seq_write_bytes(std::atomic<uint32_t>& seq, uint8_t* dst, const uint8_t* src, size_t n) {
    seq.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(dst, src, n);
    std::atomic_thread_fence(std::memory_order_release);
    seq.fetch_add(1, std::memory_order_acq_rel);
}

inline bool seq_read_bytes(const std::atomic<uint32_t>& seq, const uint8_t* src, uint8_t* dst, size_t n) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        std::memcpy(dst, src, n);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == s1) return true;
    }
    return false;
}

// Every block starts with this header. A reader checks magic + version +
// size before it trusts anything else in the block.
struct BlockHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t size;      // sizeof the whole block
    uint32_t reserved;
};

}  // namespace himmel::protocol
