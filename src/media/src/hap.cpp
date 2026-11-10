// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/media/hap.h"

#include <cstring>

#include <snappy.h>

namespace himmel::media {

namespace {

struct Section {
    uint8_t type = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t total = 0;  // header + data
};

bool read_section(const uint8_t* p, size_t n, Section& s) {
    if (n < 4) return false;
    size_t size = p[0] | (p[1] << 8) | (p[2] << 16);
    size_t hdr = 4;
    if (size == 0) {
        if (n < 8) return false;
        size = static_cast<size_t>(p[4]) | (static_cast<size_t>(p[5]) << 8) | (static_cast<size_t>(p[6]) << 16) |
               (static_cast<size_t>(p[7]) << 24);
        hdr = 8;
    }
    if (size > n - hdr) return false;
    s = {p[3], p + hdr, size, hdr + size};
    return true;
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

HapTexture texture_of(uint8_t type) {
    switch (type & 0x0F) {
        case 0x0B: return HapTexture::DXT1;
        case 0x0E: return HapTexture::DXT5;
        case 0x0F: return HapTexture::YCoCgDXT5;
        case 0x01: return HapTexture::RGTC1;
        default: return HapTexture::Unknown;
    }
}

HapResult fail(const char* e) {
    HapResult r;
    r.error = e;
    return r;
}

// Undo one compressor into out[0..cap). Returns bytes written or SIZE_MAX.
size_t expand(uint8_t compressor, const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    if (compressor == 0x0A) {
        if (n > cap) return SIZE_MAX;
        std::memcpy(out, in, n);
        return n;
    }
    if (compressor == 0x0B) {
        size_t len = 0;
        const char* src = reinterpret_cast<const char*>(in);
        if (!snappy::GetUncompressedLength(src, n, &len) || len > cap) return SIZE_MAX;
        if (!snappy::RawUncompress(src, n, reinterpret_cast<char*>(out))) return SIZE_MAX;
        return len;
    }
    return SIZE_MAX;
}

}  // namespace

const char* hap_texture_name(HapTexture t) {
    switch (t) {
        case HapTexture::DXT1: return "Hap (DXT1)";
        case HapTexture::DXT5: return "Hap Alpha (DXT5)";
        case HapTexture::YCoCgDXT5: return "Hap Q (YCoCg DXT5)";
        case HapTexture::RGTC1: return "Alpha (RGTC1)";
        default: return "unknown";
    }
}

size_t hap_row_pitch(HapTexture t, int width) {
    const size_t blocks = (static_cast<size_t>(width) + 3) / 4;
    return blocks * (t == HapTexture::DXT1 || t == HapTexture::RGTC1 ? 8 : 16);
}

size_t hap_texture_bytes(HapTexture t, int width, int height) {
    return hap_row_pitch(t, width) * ((static_cast<size_t>(height) + 3) / 4);
}

HapTexture hap_peek_texture(const uint8_t* frame, size_t size) {
    Section s;
    if (!read_section(frame, size, s)) return HapTexture::Unknown;
    const uint8_t hi = s.type & 0xF0;
    if (hi != 0xA0 && hi != 0xB0 && hi != 0xC0) return HapTexture::Unknown;
    return texture_of(s.type);
}

HapResult hap_decode(const uint8_t* frame, size_t size, uint8_t* out, size_t cap) {
    Section top;
    if (!read_section(frame, size, top)) return fail("frame is shorter than its section header says");
    if (top.type == 0x0D) return fail("Hap Q Alpha (multiple images) is not supported yet");
    const HapTexture tex = texture_of(top.type);
    if (tex == HapTexture::Unknown) return fail("unknown HAP texture format");

    HapResult r;
    r.texture = tex;
    const uint8_t compressor = top.type >> 4;
    if (compressor == 0x0A || compressor == 0x0B) {
        const size_t n = expand(compressor, top.data, top.size, out, cap);
        if (n == SIZE_MAX) return fail(compressor == 0x0B ? "Snappy data is broken or too large" : "frame too large");
        r.ok = true;
        r.bytes = n;
        r.chunks = 1;
        return r;
    }
    if (compressor != 0x0C) return fail("unknown HAP compressor");

    // Chunked: [decode instructions container][chunk data...]
    Section dic;
    if (!read_section(top.data, top.size, dic) || dic.type != 0x01) return fail("missing decode instructions");
    const uint8_t* compressors = nullptr;
    const uint8_t* sizes = nullptr;
    const uint8_t* offsets = nullptr;
    size_t n_comp = 0, n_size = 0, n_off = 0;
    for (size_t pos = 0; pos < dic.size;) {
        Section s;
        if (!read_section(dic.data + pos, dic.size - pos, s)) return fail("broken decode instructions");
        if (s.type == 0x02) compressors = s.data, n_comp = s.size;
        else if (s.type == 0x03) sizes = s.data, n_size = s.size / 4;
        else if (s.type == 0x04) offsets = s.data, n_off = s.size / 4;
        pos += s.total;
    }
    if (!compressors || !sizes || n_comp != n_size || n_comp == 0) return fail("chunk tables do not match");
    if (offsets && n_off != n_comp) return fail("chunk offset table does not match");

    const uint8_t* data = top.data + dic.total;
    const size_t data_size = top.size - dic.total;
    size_t in_pos = 0, out_pos = 0;
    for (size_t c = 0; c < n_comp; ++c) {
        const size_t csize = le32(sizes + 4 * c);
        const size_t coff = offsets ? le32(offsets + 4 * c) : in_pos;
        if (coff > data_size || csize > data_size - coff) return fail("chunk points outside the frame");
        const size_t n = expand(compressors[c], data + coff, csize, out + out_pos, cap - out_pos);
        if (n == SIZE_MAX) return fail("a chunk is broken or too large");
        out_pos += n;
        in_pos = coff + csize;
    }
    r.ok = true;
    r.bytes = out_pos;
    r.chunks = static_cast<int>(n_comp);
    return r;
}

}  // namespace himmel::media
