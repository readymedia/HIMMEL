// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Checks the protocol contracts: sizes, ring and seqlock behaviour (no GPU, no Windows).
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/preview.h"
#include "himmel/protocol/show.h"
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

// The show block (M3, docs/interfaces/show-block.md): layout and the text handshake.
static void test_show_block() {
    std::printf("show block\n");
    // Layout: fixed-size PODs, the seqlocked parts on their own cache lines, sizes pinned
    // (a change here is a layout change: bump kShowVersion and update show-block.md).
    static_assert(std::is_trivially_copyable_v<ShowStatus> && std::is_trivially_copyable_v<FilesStatus>);
    static_assert(sizeof(ScenePadInfo) == 32);
    static_assert(sizeof(DeckSceneInfo) == 16);
    static_assert(sizeof(RecentProject) == kNameLen + kPathLen + 16);
    static_assert(sizeof(TextBox) == 8 + kPathLen);
    static_assert(offsetof(TextBox, id) == 4 && offsetof(TextBox, text) == 8);
    static_assert(offsetof(ShowBlock, header) == 0);
    static_assert(offsetof(ShowBlock, status_seq) % 64 == 0 && offsetof(ShowBlock, files_seq) % 64 == 0 &&
                  offsetof(ShowBlock, text) % 64 == 0);
    static_assert(sizeof(ShowBlock) < 64 * 1024);
    static_assert(kShowMagic == 0x57534D48 && kShowVersion == 1);
    // Command numbers: SHOW owns 25..39 (LEAD, wave 2); never renumber.
    static_assert(static_cast<int>(CommandType::SceneRecall) == 25 && static_cast<int>(CommandType::SongEdit) == 36);
    static_assert(static_cast<int>(CommandType::Count) <= 40, "types 40+ belong to AUDIO / GUI");
    static_assert(static_cast<int>(Producer::Show) == 4);
    static_assert(kLookCommandsMax == 352 && kShowCommandsPerFrame >= kLookCommandsMax);
    CHECK(is_show_command(CommandType::SceneRecall) && is_show_command(static_cast<CommandType>(39)));
    CHECK(!is_show_command(CommandType::FxPress) && !is_show_command(static_cast<CommandType>(40)));
    std::printf("  ShowBlock %zu B (status %zu, files %zu)\n", sizeof(ShowBlock), sizeof(ShowStatus), sizeof(FilesStatus));

    // Text handshake: id + text travel together; a busy writer is never read.
    auto blk = std::make_unique<ShowBlock>();
    TextBox& box = blk->text[static_cast<int>(Producer::Gui)];
    write_text(box, 41, "C:\\Shows\\Night.himmel");
    uint32_t id = 0;
    char out[kPathLen];
    CHECK(read_text(box, id, out) && id == 41 && std::strcmp(out, "C:\\Shows\\Night.himmel") == 0);
    write_text(box, 42, "Second");
    CHECK(read_text(box, id, out) && id == 42 && std::strcmp(out, "Second") == 0);
    std::string long_text(2000, 'x');
    write_text(box, 43, long_text.c_str());
    CHECK(read_text(box, id, out) && id == 43 && std::strlen(out) == kPathLen - 1);
    box.seq.store(7);  // writer busy
    CHECK(!read_text(box, id, out));
}

int main() {
    test_sizes();
    test_ring();
    test_seqlock();
    test_show_block();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
