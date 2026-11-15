// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The preview block: small pictures of the show for the GUI (docs/interfaces/preview-block.md).
//
// Shared block "Local\HimmelPreview". Written only by the render process from
// a staging-texture ring it maps with D3D11_MAP_FLAG_DO_NOT_WAIT (so the frame
// loop never stalls on the GPU). Each stream has 3 buffers, each with its own
// seqlock; the writer always fills the oldest buffer and then publishes it as
// `latest`. Readers copy `latest` with seq_read_bytes; a torn copy is retried
// or skipped, never shown.
#pragma once

#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::protocol {

inline constexpr const wchar_t* kPreviewShmName = L"Local\\HimmelPreview";
inline constexpr uint32_t kPreviewMagic = 0x56504D48;  // "HMPV"
inline constexpr uint32_t kPreviewVersion = 1;

inline constexpr uint32_t kPreviewW = 480;
inline constexpr uint32_t kPreviewH = 270;
inline constexpr uint32_t kPreviewBytes = kPreviewW * kPreviewH * 4;  // BGRA8, top-down, stride = width * 4
inline constexpr int kPreviewBuffers = 3;

enum class PreviewStream : uint8_t {
    DeckA = 0,      // deck A after its deck FX (what TAKE would show)
    DeckB = 1,
    Main = 2,       // MAIN after master FX, before output warp
    Output1 = 3,    // what output 1 shows (after its own FX / blend), up to Output3
    Output2 = 4,
    Output3 = 5,
    Count
};
inline constexpr int kPreviewStreams = static_cast<int>(PreviewStream::Count);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas padding is intended
#endif
struct PreviewBuffer {
    std::atomic<uint32_t> seq;   // seqlock over pixels + frame
    uint32_t reserved;
    uint64_t frame;              // engine frame this picture is from
    alignas(64) uint8_t pixels[kPreviewBytes];
};

struct PreviewStreamBlock {
    std::atomic<uint32_t> active;     // 1 = the engine writes this stream
    std::atomic<uint32_t> latest;     // index of the newest complete buffer (0..2)
    std::atomic<uint64_t> published;  // count of published pictures (changes = new picture)
    uint32_t width, height;           // <= kPreviewW / kPreviewH (aspect of the canvas; the rest is unused)
    uint32_t fps_target;              // 15..30
    uint32_t reserved;
    PreviewBuffer buffer[kPreviewBuffers];
};

struct PreviewBlock {
    BlockHeader header;               // kPreviewMagic, kPreviewVersion, sizeof(PreviewBlock)
    PreviewStreamBlock stream[kPreviewStreams];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace himmel::protocol
