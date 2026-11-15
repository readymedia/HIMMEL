// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Checks the protocol contracts: sizes, ring and seqlock behaviour (no GPU, no Windows).
#include <cmath>
#include <cstdio>
#include <memory>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/preview.h"
#include "himmel/protocol/state.h"

using namespace himmel::protocol;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void test_sizes() {
    std::printf("sizes\n");
    static_assert(sizeof(Command) == 32);
    static_assert(sizeof(CommandBlock) < 512 * 1024);
    static_assert(sizeof(StateBlock) < 256 * 1024);
    static_assert(sizeof(PreviewBlock) < 12 * 1024 * 1024);
    static_assert(kPreviewBytes == 480u * 270u * 4u);
    std::printf("  CommandBlock %zu B, StateBlock %zu B, PreviewBlock %zu B\n", sizeof(CommandBlock),
                sizeof(StateBlock), sizeof(PreviewBlock));
}

static void test_ring() {
    std::printf("ring\n");
    auto blk = std::make_unique<CommandBlock>();
    auto& r = blk->rings[static_cast<int>(Producer::Gui)];
    r.head = 0, r.tail = 0, r.dropped = 0;
    Command c = make(CommandType::LayerOpacity, 0, 2, 0, 0.5f);
    for (uint32_t i = 0; i < kCommandRingSize; ++i) {
        c.seq = i + 1;
        CHECK(r.push(c) || i >= kCommandRingSize);
    }
    CHECK(!r.push(c));
    CHECK(r.dropped == 1);
    Command out{};
    CHECK(r.pop(out) && out.seq == 1 && out.layer == 2 && out.f[0] == 0.5f);
    r.skip_all();
    CHECK(!r.pop(out));
    // wrap-around of the 32-bit indices
    r.head = 0xFFFFFFF0u, r.tail = 0xFFFFFFF0u;
    for (int i = 0; i < 40; ++i) {
        c.seq = 100 + i;
        CHECK(r.push(c));
        CHECK(r.pop(out) && out.seq == static_cast<uint32_t>(100 + i));
    }
}

static void test_seqlock() {
    std::printf("seqlock\n");
    auto blk = std::make_unique<StateBlock>();
    blk->live_seq = 0;
    LiveState a{};
    a.crossfader = 0.25f;
    a.deck[1].layer[3].opacity = 0.75f;
    seq_write(blk->live_seq, blk->live, a);
    LiveState b{};
    CHECK(seq_read(blk->live_seq, blk->live, b) == 3);
    CHECK(b.crossfader == 0.25f && b.deck[1].layer[3].opacity == 0.75f);
    blk->live_seq = 5;  // writer busy
    CHECK(seq_read(blk->live_seq, blk->live, b) == 0);
}

int main() {
    test_sizes();
    test_ring();
    test_seqlock();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
