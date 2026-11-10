// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// HAP frame decoder (https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md).
//
// A HAP frame is one or more sections: [3-byte LE size][type] (size 0 = a
// 4-byte size follows). The type's high nibble is the second-stage compressor
// (A = none, B = Snappy, C = chunked with decode instructions), the low nibble
// the texture format. Decoding only undoes the second stage: the result is the
// raw BC1/BC3/BC4 texture, uploaded to the GPU as is (no CPU pixel work).
//
// Supported: Hap (DXT1), Hap Alpha (DXT5), Hap Q (scaled YCoCg DXT5), single
// alpha (RGTC1), chunked frames. Not yet: Hap Q Alpha (multiple images), Hap R.
#pragma once

#include <cstddef>
#include <cstdint>

namespace himmel::media {

enum class HapTexture : uint8_t {
    Unknown,
    DXT1,       // RGB            -> DXGI_FORMAT_BC1_UNORM
    DXT5,       // RGBA           -> DXGI_FORMAT_BC3_UNORM
    YCoCgDXT5,  // Hap Q          -> DXGI_FORMAT_BC3_UNORM + YCoCg -> RGB in the shader
    RGTC1,      // alpha only     -> DXGI_FORMAT_BC4_UNORM
};

const char* hap_texture_name(HapTexture t);
// Bytes of one frame's texture (width/height rounded up to 4).
size_t hap_texture_bytes(HapTexture t, int width, int height);
// Bytes per row of 4x4 blocks (for the GPU upload pitch).
size_t hap_row_pitch(HapTexture t, int width);

struct HapResult {
    bool ok = false;
    HapTexture texture = HapTexture::Unknown;
    size_t bytes = 0;    // written to out
    int chunks = 0;      // 1 for a plain frame
    const char* error = nullptr;
};

// Decodes one frame (the bytes of one sample from the MOV) into `out`.
// Never allocates; `out` must be at least hap_texture_bytes() large.
HapResult hap_decode(const uint8_t* frame, size_t size, uint8_t* out, size_t out_capacity);

// Texture format of a frame without decoding it (Unknown if not HAP).
HapTexture hap_peek_texture(const uint8_t* frame, size_t size);

}  // namespace himmel::media
