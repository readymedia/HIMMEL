// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/media/mov.h"

#include <algorithm>
#include <map>

namespace himmel::media {

namespace {

constexpr uint64_t kMaxMoovBytes = 64ull << 20;  // a sane moov is KBs..a few MB

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Atom {
    uint32_t type = 0;
    const uint8_t* body = nullptr;  // payload (after the header)
    size_t size = 0;                // payload size
};

// Iterates the child atoms inside [p, p + n). Returns false on a broken header.
template <class F>
bool for_each_atom(const uint8_t* p, size_t n, F&& f) {
    size_t pos = 0;
    while (pos + 8 <= n) {
        uint64_t size = be32(p + pos);
        const uint32_t type = be32(p + pos + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (pos + 16 > n) return false;
            size = be64(p + pos + 8);
            hdr = 16;
        } else if (size == 0) {
            size = n - pos;
        }
        if (size < hdr || size > n - pos) return false;
        if (!f(Atom{type, p + pos + hdr, static_cast<size_t>(size - hdr)})) return true;  // stop early
        pos += static_cast<size_t>(size);
    }
    return true;
}

const Atom* find(const std::vector<Atom>& atoms, uint32_t type) {
    for (const auto& a : atoms)
        if (a.type == type) return &a;
    return nullptr;
}

std::vector<Atom> children(const Atom& a, bool* ok) {
    std::vector<Atom> out;
    *ok = for_each_atom(a.body, a.size, [&](const Atom& c) {
        out.push_back(c);
        return true;
    }) && *ok;
    return out;
}

struct Fail {
    std::string* err;
    std::optional<MovVideoTrack> operator()(const std::string& m) const {
        if (err) *err = m;
        return std::nullopt;
    }
};

}  // namespace

std::string MovVideoTrack::codec() const {
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((fourcc >> (24 - 8 * i)) & 0xFF);
        s[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

std::optional<MovVideoTrack> read_mov(const ByteSource& src, std::string* error) {
    const Fail fail{error};
    if (!src.read || src.size < 8) return fail("file is empty or unreadable");

    // 1. Find 'moov' among the top-level atoms (it may be at the end of the file).
    uint64_t pos = 0, moov_off = 0, moov_size = 0;
    while (pos + 8 <= src.size) {
        uint8_t h[16];
        if (!src.read(pos, h, 8)) return fail("read error");
        uint64_t size = be32(h);
        const uint32_t type = be32(h + 4);
        uint64_t hdr = 8;
        if (size == 1) {
            if (pos + 16 > src.size || !src.read(pos + 8, h + 8, 8)) return fail("read error");
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = src.size - pos;
        }
        if (size < hdr || size > src.size - pos) return fail("broken top-level atom");
        if (type == fourcc('m', 'o', 'o', 'v')) {
            moov_off = pos + hdr;
            moov_size = size - hdr;
            break;
        }
        pos += size;
    }
    if (!moov_size) return fail("no 'moov' atom (not a QuickTime/MP4 file, or not finished writing)");
    if (moov_size > kMaxMoovBytes) return fail("'moov' atom is too large");
    std::vector<uint8_t> moov(static_cast<size_t>(moov_size));
    if (!src.read(moov_off, moov.data(), moov.size())) return fail("read error in 'moov'");

    // 2. First video track.
    bool ok = true;
    std::vector<Atom> traks;
    ok = for_each_atom(moov.data(), moov.size(), [&](const Atom& a) {
        if (a.type == fourcc('t', 'r', 'a', 'k')) traks.push_back(a);
        return true;
    });
    if (!ok) return fail("broken atom inside 'moov'");

    for (const Atom& trak : traks) {
        const auto tk = children(trak, &ok);
        const Atom* mdia_a = find(tk, fourcc('m', 'd', 'i', 'a'));
        if (!mdia_a) continue;
        const auto mdia = children(*mdia_a, &ok);
        const Atom* hdlr = find(mdia, fourcc('h', 'd', 'l', 'r'));
        if (!hdlr || hdlr->size < 12 || be32(hdlr->body + 8) != fourcc('v', 'i', 'd', 'e')) continue;

        MovVideoTrack t;
        if (const Atom* mdhd = find(mdia, fourcc('m', 'd', 'h', 'd'))) {
            const uint8_t* b = mdhd->body;
            if (mdhd->size >= 32 && b[0] == 1) t.timescale = be32(b + 20), t.duration = be64(b + 24);
            else if (mdhd->size >= 20) t.timescale = be32(b + 12), t.duration = be32(b + 16);
        }
        const Atom* minf_a = find(mdia, fourcc('m', 'i', 'n', 'f'));
        if (!minf_a) return fail("video track without 'minf'");
        const auto minf = children(*minf_a, &ok);
        const Atom* stbl_a = find(minf, fourcc('s', 't', 'b', 'l'));
        if (!stbl_a) return fail("video track without 'stbl'");
        const auto stbl = children(*stbl_a, &ok);
        if (!ok) return fail("broken atom in the sample table");

        // stsd: codec + size.
        const Atom* stsd = find(stbl, fourcc('s', 't', 's', 'd'));
        if (!stsd || stsd->size < 8 + 8 + 28 + 4) return fail("missing or short 'stsd'");
        const uint8_t* e = stsd->body + 8;  // first sample entry
        t.fourcc = be32(e + 4);
        t.width = be16(e + 8 + 24);
        t.height = be16(e + 8 + 26);

        // stsz: frame sizes.
        const Atom* stsz = find(stbl, fourcc('s', 't', 's', 'z'));
        if (!stsz || stsz->size < 12) return fail("missing 'stsz'");
        const uint32_t fixed = be32(stsz->body + 4), count = be32(stsz->body + 8);
        if (!fixed && stsz->size < 12 + 4ull * count) return fail("short 'stsz'");
        t.sizes.resize(count);
        for (uint32_t i = 0; i < count; ++i) t.sizes[i] = fixed ? fixed : be32(stsz->body + 12 + 4 * i);

        // stco / co64: chunk offsets.
        std::vector<uint64_t> chunk_off;
        if (const Atom* stco = find(stbl, fourcc('s', 't', 'c', 'o'))) {
            const uint32_t n = stco->size >= 8 ? be32(stco->body + 4) : 0;
            if (stco->size < 8 + 4ull * n) return fail("short 'stco'");
            for (uint32_t i = 0; i < n; ++i) chunk_off.push_back(be32(stco->body + 8 + 4 * i));
        } else if (const Atom* co64 = find(stbl, fourcc('c', 'o', '6', '4'))) {
            const uint32_t n = co64->size >= 8 ? be32(co64->body + 4) : 0;
            if (co64->size < 8 + 8ull * n) return fail("short 'co64'");
            for (uint32_t i = 0; i < n; ++i) chunk_off.push_back(be64(co64->body + 8 + 8 * i));
        } else {
            return fail("missing 'stco'/'co64'");
        }

        // stsc: samples per chunk (runs) -> one offset per frame.
        const Atom* stsc = find(stbl, fourcc('s', 't', 's', 'c'));
        if (!stsc || stsc->size < 8) return fail("missing 'stsc'");
        const uint32_t runs = be32(stsc->body + 4);
        if (stsc->size < 8 + 12ull * runs || runs == 0) return fail("short 'stsc'");
        t.offsets.reserve(count);
        uint32_t frame = 0;
        for (uint32_t r = 0; r < runs && frame < count; ++r) {
            const uint32_t first = be32(stsc->body + 8 + 12 * r);  // 1-based
            const uint32_t per = be32(stsc->body + 8 + 12 * r + 4);
            const uint32_t last = r + 1 < runs ? be32(stsc->body + 8 + 12 * (r + 1)) - 1
                                               : static_cast<uint32_t>(chunk_off.size());
            if (first == 0 || last > chunk_off.size()) return fail("bad 'stsc' run");
            for (uint32_t c = first; c <= last && frame < count; ++c) {
                uint64_t off = chunk_off[c - 1];
                for (uint32_t s = 0; s < per && frame < count; ++s) {
                    t.offsets.push_back(off);
                    off += t.sizes[frame++];
                }
            }
        }
        if (t.offsets.size() != count) return fail("sample table does not cover every frame");
        for (uint32_t i = 0; i < count; ++i)
            if (t.offsets[i] + t.sizes[i] > src.size) return fail("frame points past the end of the file");

        // stts: frame durations -> fps (most common duration).
        if (const Atom* stts = find(stbl, fourcc('s', 't', 't', 's'))) {
            const uint32_t n = stts->size >= 8 ? be32(stts->body + 4) : 0;
            std::map<uint32_t, uint64_t> by_delta;
            for (uint32_t i = 0; i < n && stts->size >= 8 + 8ull * (i + 1); ++i)
                by_delta[be32(stts->body + 12 + 8 * i)] += be32(stts->body + 8 + 8 * i);
            uint32_t delta = 0;
            uint64_t best = 0;
            for (auto [d, c] : by_delta)
                if (c > best) best = c, delta = d;
            if (delta && t.timescale) t.fps = static_cast<double>(t.timescale) / delta;
        }
        if (t.fps <= 0 && t.duration && t.timescale) t.fps = count * static_cast<double>(t.timescale) / t.duration;
        if (t.fps <= 0) t.fps = 30.0;
        t.max_frame_size = count ? *std::max_element(t.sizes.begin(), t.sizes.end()) : 0;
        if (t.width <= 0 || t.height <= 0 || count == 0) return fail("video track has no size or no frames");
        return t;
    }
    return fail("no video track");
}

}  // namespace himmel::media
