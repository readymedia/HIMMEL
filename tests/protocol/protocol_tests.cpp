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
#include "himmel/protocol/outputs.h"
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
    static_assert(sizeof(StateBlock) < 1024 * 1024);  // v4: + clip pages (~800 KB)
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
    // Wave 3 ranges (LEAD 2026-11-17): MEDIA 60..69, ENGINE 70..79, CONTROL 80..89.
    static_assert(static_cast<int>(CommandType::OutputBlack) == 70 && static_cast<int>(CommandType::OutputConfig) == 73,
                  "ENGINE M6 owns 70..79");
    static_assert(static_cast<int>(CommandType::SystemQuit) == 74, "ENGINE: quit (command-protocol.md §4d)");
    static_assert(static_cast<int>(CommandType::Count) <= 90, "types 90+ are not given out yet");
    // Outputs block (M6, outputs-block.md).
    static_assert(std::is_trivially_copyable_v<OutputsSection> && sizeof(DisplayId) == 36);
    static_assert(offsetof(OutputsBlock, header) == 0 && offsetof(OutputsBlock, seq) % 64 == 0);
    static_assert(sizeof(OutputsBlock) < 16 * 1024 && kOutputsMagic == 0x54554F48 && kOutputsVersion == 1);
    static_assert(kPlaceSlots == 9 && kMaxOutputs == 8);
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

// FxReact (D131): the packing of i round-trips, out-of-range fields are caught,
// and the v3 reaction table sits after the transport section.
static void test_react_packing() {
    std::printf("react packing\n");
    static_assert(static_cast<int>(CommandType::FxReact) == 40);
    static_assert(sizeof(ReactionEntry) == 48);
    static_assert(sizeof(ReactionSection) == (kDecks * kLayersPerDeck * kFxSlots + kDecks * kFxSlots + kFxSlots) *
                                                 kReactParams * sizeof(ReactionEntry) + 8);
    static_assert(offsetof(StateBlock, react) > offsetof(StateBlock, transport));
    static_assert(kStateVersion == 4);  // v4: clip pages (wave 7)
    ReactArgs a;
    a.op = ReactOp::Set;
    a.signal = ReactSignal::Drop;
    a.mode = ReactMode::Trigger;
    a.curve = ReactCurve::Exp;
    a.quantise = ReactQuantise::Bar;
    a.hold_q = kReactHoldMax;
    const int32_t i = pack_react(a);
    CHECK(i == static_cast<int32_t>(0u | (13u << 4) | (2u << 12) | (4u << 16) | (5u << 20) | (16u << 24)));
    const ReactArgs b = unpack_react(i);
    CHECK(b.op == a.op && b.signal == a.signal && b.mode == a.mode && b.curve == a.curve && b.quantise == a.quantise &&
          b.hold_q == a.hold_q);
    CHECK(react_args_ok(i));
    CHECK(pack_react(ReactArgs{}) == 0 && react_args_ok(0));
    // every op round-trips
    for (int op = 0; op < static_cast<int>(ReactOp::Count); ++op) {
        ReactArgs c;
        c.op = static_cast<ReactOp>(op);
        c.signal = ReactSignal::Kick;
        CHECK(unpack_react(pack_react(c)).op == c.op && unpack_react(pack_react(c)).signal == ReactSignal::Kick);
    }
    // out of range: op, signal, mode, curve, quantise, hold, reserved bits
    CHECK(!react_args_ok(6));
    CHECK(!react_args_ok(static_cast<int32_t>(14u << 4)));
    CHECK(!react_args_ok(static_cast<int32_t>(3u << 12)));
    CHECK(!react_args_ok(static_cast<int32_t>(5u << 16)));
    CHECK(!react_args_ok(static_cast<int32_t>(6u << 20)));
    CHECK(!react_args_ok(static_cast<int32_t>(17u << 24)));
    CHECK(!react_args_ok(static_cast<int32_t>(1u << 29)));
    CHECK(!react_args_ok(static_cast<int32_t>(0x80000000u)));
    // make_react fills the targets and every float
    const Command c = make_react(kMasterDeck, kNone, 2, 1, a, -0.5f, 5.f, 180.f, 0.2f, kRelative);
    CHECK(c.type == CommandType::FxReact && c.deck == kMasterDeck && c.layer == kNone && c.slot == 2 && c.param == 1);
    CHECK(c.i == i && c.f[0] == -0.5f && c.f[1] == 5.f && c.f[2] == 180.f && c.f[3] == 0.2f && c.flags == kRelative);
    CHECK(kReactQuantiseBeats[static_cast<int>(ReactQuantise::Quarter)] == 1.f);
    std::printf("  ReactionSection %zu B\n", sizeof(ReactionSection));
}

int main() {
    test_sizes();
    test_ring();
    test_seqlock();
    test_show_block();
    test_react_packing();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
