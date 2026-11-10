// Maschine MK2 controller: the shared-memory interface (spec 5.1, 5.2, 5.5).
//
// One named shared-memory block "Local\HimmelMk2" holds:
//   * Status   - written by the controller process (connected, heartbeat, counters)
//   * Outputs  - written by ONE app writer (engine or GUI) with a sequence lock,
//                read by the controller: LEDs and the two screen buffers
//   * Rings    - one lock-free single-producer/single-consumer event ring per
//                consumer (engine, GUI, monitor, ...). The controller produces.
// Everything is fixed-size and allocation-free; atomics are lock-free 32/64-bit.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "himmel/mk2/output.h"
#include "himmel/mk2/types.h"

namespace himmel::mk2 {

inline constexpr uint32_t kShmMagic = 0x324B4D48;  // "HMK2"
inline constexpr uint32_t kShmVersion = 1;
inline constexpr uint32_t kRingSize = 4096;  // events, power of two
inline constexpr int kMaxConsumers = 4;
inline constexpr const wchar_t* kShmName = L"Local\\HimmelMk2";

static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

struct alignas(64) EventRing {
    std::atomic<uint32_t> head;        // next write index (producer)
    uint8_t pad0[60];
    std::atomic<uint32_t> tail;        // next read index (consumer)
    std::atomic<uint32_t> attached;    // 1 = a consumer reads this ring
    std::atomic<uint32_t> dropped;     // events lost because the ring was full
    uint8_t pad1[52];
    Event items[kRingSize];

    // Producer side (controller). Returns false and counts a drop when full.
    bool push(const Event& e) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t t = tail.load(std::memory_order_acquire);
        if (h - t >= kRingSize) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        items[h & (kRingSize - 1)] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer side.
    bool pop(Event& e) {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        const uint32_t h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        e = items[t & (kRingSize - 1)];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    void attach() {
        tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        attached.store(1, std::memory_order_release);
    }
    void detach() { attached.store(0, std::memory_order_release); }
};

struct Status {
    std::atomic<uint32_t> connected;
    std::atomic<uint32_t> map_verified;     // 1 = map file loaded and verified, 0 = built-in default
    std::atomic<uint64_t> heartbeat_us;     // controller process alive (updated every ~20 ms)
    std::atomic<uint64_t> pad_reports;
    std::atomic<uint64_t> control_reports;
    std::atomic<uint64_t> reconnects;
    std::atomic<uint64_t> led_writes;
    std::atomic<uint64_t> strip_writes;
    char message[128];                      // last notable thing (ASCII), for UIs/logs
};

// Sequence lock: writer makes seq odd, writes, makes it even. Readers retry while
// it is odd or changed during the copy. Exactly one writer per section.
struct Outputs {
    std::atomic<uint32_t> led_seq;
    LedState leds;
    std::atomic<uint32_t> screen_seq[2];
    uint8_t screen[2][kScreenBytes];        // left, right; MSB-left rows, 1 = lit
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas(64) is intended (no false sharing)
#endif
struct SharedBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t reserved;
    Status status;
    Outputs outputs;
    EventRing rings[kMaxConsumers];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ---- seqlock helpers (writer = app, reader = controller) ----
template <class T>
void seq_write(std::atomic<uint32_t>& seq, T& dst, const T& src) {
    seq.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&dst, &src, sizeof(T));
    std::atomic_thread_fence(std::memory_order_release);
    seq.fetch_add(1, std::memory_order_acq_rel);  // even: done
}

// Current version of a section without copying it (odd while being written).
inline uint32_t seq_peek(const std::atomic<uint32_t>& seq) { return seq.load(std::memory_order_acquire); }

// Copies src into dst if a consistent snapshot is available. Returns the
// (even) sequence number + 1, so a valid read is never 0; returns 0 if the
// writer was busy every time (try again later).
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

// Same as seq_write / seq_read for a raw byte buffer (the screens).
inline void seq_write_bytes(std::atomic<uint32_t>& seq, uint8_t* dst, const uint8_t* src, size_t n) {
    seq.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(dst, src, n);
    std::atomic_thread_fence(std::memory_order_release);
    seq.fetch_add(1, std::memory_order_acq_rel);
}

inline uint32_t seq_read_bytes(const std::atomic<uint32_t>& seq, const uint8_t* src, uint8_t* dst, size_t n) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        std::memcpy(dst, src, n);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == s1) return s1 + 1;
    }
    return 0;
}

// Maps the shared block. create=true for the controller process.
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    bool create(const wchar_t* name = kShmName);  // controller: create + initialise
    bool open(const wchar_t* name = kShmName);    // apps: attach to an existing block
    SharedBlock* get() const { return block_; }

private:
    void* mapping_ = nullptr;
    SharedBlock* block_ = nullptr;
};

}  // namespace himmel::mk2
