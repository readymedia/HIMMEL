// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Unit tests for the MOV reader and the HAP decoder. Synthetic data only; the
// clips in media/test are checked too when they exist (hap_probe makes them visible).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <snappy.h>

#include "himmel/media/hap.h"
#include "himmel/media/mov.h"

using namespace himmel::media;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

using Bytes = std::vector<uint8_t>;

static void put24(Bytes& b, size_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)}); }
static void put32le(Bytes& b, size_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
static void section(Bytes& out, uint8_t type, const Bytes& data, bool long_header = false) {
    if (long_header || data.size() > 0xFFFFFF) {
        put24(out, 0);
        out.push_back(type);
        put32le(out, data.size());
    } else {
        put24(out, data.size());
        out.push_back(type);
    }
    out.insert(out.end(), data.begin(), data.end());
}
static Bytes snappy_of(const Bytes& raw) {
    std::string c;
    snappy::Compress(reinterpret_cast<const char*>(raw.data()), raw.size(), &c);
    return Bytes(c.begin(), c.end());
}
static Bytes pattern(size_t n, int seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>((i * 7 + seed) ^ (i >> 5));
    return b;
}

static void test_hap() {
    std::printf("HAP frames\n");
    CHECK(hap_texture_bytes(HapTexture::DXT1, 1276, 716) == 456808);  // matches ffmpeg's clip
    CHECK(hap_texture_bytes(HapTexture::DXT5, 1920, 1080) == 2073600);
    CHECK(hap_texture_bytes(HapTexture::DXT1, 6, 6) == 4 * 8);           // rounds up to 2x2 blocks
    CHECK(hap_row_pitch(HapTexture::YCoCgDXT5, 1920) == 480 * 16);

    const Bytes raw = pattern(64 * 8, 1);  // a 32x64 DXT1 texture
    Bytes out(raw.size());
    {  // Snappy, short header
        Bytes f;
        section(f, 0xBB, snappy_of(raw));
        const auto r = hap_decode(f.data(), f.size(), out.data(), out.size());
        CHECK(r.ok && r.texture == HapTexture::DXT1 && r.bytes == raw.size() && out == raw && r.chunks == 1);
        CHECK(hap_peek_texture(f.data(), f.size()) == HapTexture::DXT1);
    }
    {  // uncompressed, long header, Hap Q
        Bytes f;
        section(f, 0xAF, raw, true);
        std::fill(out.begin(), out.end(), uint8_t(0));
        const auto r = hap_decode(f.data(), f.size(), out.data(), out.size());
        CHECK(r.ok && r.texture == HapTexture::YCoCgDXT5 && out == raw);
    }
    {  // chunked: chunk 0 Snappy, chunk 1 none, with an offset table (chunks stored in reverse order)
        const Bytes a(raw.begin(), raw.begin() + 200), b(raw.begin() + 200, raw.end());
        const Bytes ca = snappy_of(a);
        Bytes comp = {0x0B, 0x0A}, sizes, offs;
        put32le(sizes, ca.size());
        put32le(sizes, b.size());
        put32le(offs, b.size());  // chunk 0 after chunk 1
        put32le(offs, 0);
        Bytes inner, dic;
        section(inner, 0x02, comp);
        section(inner, 0x03, sizes);
        section(inner, 0x04, offs);
        section(dic, 0x01, inner);
        dic.insert(dic.end(), b.begin(), b.end());
        dic.insert(dic.end(), ca.begin(), ca.end());
        Bytes f;
        section(f, 0xCE, dic);
        std::fill(out.begin(), out.end(), uint8_t(0));
        const auto r = hap_decode(f.data(), f.size(), out.data(), out.size());
        CHECK(r.ok && r.texture == HapTexture::DXT5 && r.chunks == 2 && r.bytes == raw.size() && out == raw);
    }
    {  // broken input never writes past the buffer and always fails cleanly
        Bytes f;
        section(f, 0xBB, snappy_of(raw));
        Bytes small(raw.size() - 1);
        CHECK(!hap_decode(f.data(), f.size(), small.data(), small.size()).ok);   // too small
        CHECK(!hap_decode(f.data(), 3, out.data(), out.size()).ok);              // short header
        CHECK(!hap_decode(f.data(), f.size() - 5, out.data(), out.size()).ok);   // truncated
        Bytes bad = f;
        for (size_t i = 6; i < bad.size(); i += 3) bad[i] ^= 0x5A;               // garbage Snappy
        const auto r = hap_decode(bad.data(), bad.size(), out.data(), out.size());
        CHECK(!r.ok || r.bytes <= out.size());
        Bytes q;
        section(q, 0x0D, raw);
        CHECK(!hap_decode(q.data(), q.size(), out.data(), out.size()).ok);       // Hap Q Alpha: not yet
        Bytes u;
        section(u, 0xB7, raw);
        CHECK(!hap_decode(u.data(), u.size(), out.data(), out.size()).ok);       // unknown texture
    }
}

// ---- synthetic QuickTime file
static void be32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }
static void be16(Bytes& b, uint16_t v) { b.insert(b.end(), {uint8_t(v >> 8), uint8_t(v)}); }
static void be64(Bytes& b, uint64_t v) { be32(b, uint32_t(v >> 32)), be32(b, uint32_t(v)); }
static Bytes atom(const char* type, const Bytes& body) {
    Bytes a;
    be32(a, static_cast<uint32_t>(body.size() + 8));
    a.insert(a.end(), type, type + 4);
    a.insert(a.end(), body.begin(), body.end());
    return a;
}
static Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes r;
    for (auto& p : parts) r.insert(r.end(), p.begin(), p.end());
    return r;
}

// Three frames: chunk 1 holds frames 0+1, chunk 2 holds frame 2. moov at the end.
static Bytes make_mov(bool use_co64, Bytes* f0, Bytes* f2) {
    *f0 = pattern(100, 3);
    const Bytes f1 = pattern(80, 4);
    *f2 = pattern(120, 5);
    Bytes file = atom("ftyp", {'q', 't', ' ', ' ', 0, 0, 0, 0});
    const Bytes junk(13, 0xEE);
    Bytes mdat_body = cat({*f0, f1, junk, *f2});
    const uint64_t mdat_data = file.size() + 8;
    file = cat({file, atom("mdat", mdat_body)});
    const uint64_t c1 = mdat_data, c2 = mdat_data + 100 + 80 + 13;

    Bytes hdlr = {0, 0, 0, 0, 0, 0, 0, 0};
    hdlr.insert(hdlr.end(), {'v', 'i', 'd', 'e'});
    hdlr.resize(hdlr.size() + 12, 0);
    Bytes mdhd = {0, 0, 0, 0};
    be32(mdhd, 0), be32(mdhd, 0), be32(mdhd, 25), be32(mdhd, 3);  // timescale 25, duration 3
    Bytes stsd = {0, 0, 0, 0};
    be32(stsd, 1);
    Bytes entry;
    entry.resize(6, 0);
    be16(entry, 1);
    be16(entry, 0), be16(entry, 0), be32(entry, 0), be32(entry, 0), be32(entry, 0);
    be16(entry, 1276), be16(entry, 716);
    entry.resize(entry.size() + 50, 0);
    Bytes e2;
    be32(e2, static_cast<uint32_t>(entry.size() + 8));
    e2.insert(e2.end(), {'H', 'a', 'p', '1'});
    e2.insert(e2.end(), entry.begin(), entry.end());
    stsd.insert(stsd.end(), e2.begin(), e2.end());
    Bytes stts = {0, 0, 0, 0};
    be32(stts, 1), be32(stts, 3), be32(stts, 1);
    Bytes stsc = {0, 0, 0, 0};
    be32(stsc, 2), be32(stsc, 1), be32(stsc, 2), be32(stsc, 1), be32(stsc, 2), be32(stsc, 1), be32(stsc, 1);
    Bytes stsz = {0, 0, 0, 0};
    be32(stsz, 0), be32(stsz, 3), be32(stsz, 100), be32(stsz, 80), be32(stsz, 120);
    Bytes stco = {0, 0, 0, 0};
    be32(stco, 2);
    if (use_co64) be64(stco, c1), be64(stco, c2);
    else be32(stco, static_cast<uint32_t>(c1)), be32(stco, static_cast<uint32_t>(c2));
    const Bytes stbl = atom("stbl", cat({atom("stsd", stsd), atom("stts", stts), atom("stsc", stsc), atom("stsz", stsz),
                                         atom(use_co64 ? "co64" : "stco", stco)}));
    const Bytes mdia = atom("mdia", cat({atom("mdhd", mdhd), atom("hdlr", hdlr), atom("minf", stbl)}));
    // a sound track first, to check that the video track is picked
    Bytes shdlr = {0, 0, 0, 0, 0, 0, 0, 0, 's', 'o', 'u', 'n'};
    shdlr.resize(shdlr.size() + 12, 0);
    const Bytes sound = atom("trak", atom("mdia", atom("hdlr", shdlr)));
    return cat({file, atom("moov", cat({sound, atom("trak", mdia)}))});
}

static ByteSource memory(const Bytes& b) {
    ByteSource s;
    s.size = b.size();
    s.read = [&b](uint64_t off, void* dst, size_t n) {
        if (off + n > b.size()) return false;
        std::memcpy(dst, b.data() + off, n);
        return true;
    };
    return s;
}

static void test_mov() {
    std::printf("MOV reader\n");
    for (bool co64 : {false, true}) {
        Bytes f0, f2;
        const Bytes file = make_mov(co64, &f0, &f2);
        std::string err;
        auto t = read_mov(memory(file), &err);
        CHECK(t.has_value());
        if (!t) {
            std::printf("  %s\n", err.c_str());
            continue;
        }
        CHECK(t->codec() == "Hap1" && t->width == 1276 && t->height == 716);
        CHECK(t->frames() == 3 && t->max_frame_size == 120);
        CHECK(t->fps > 24.99 && t->fps < 25.01);
        CHECK(t->offsets[1] == t->offsets[0] + 100);
        CHECK(std::memcmp(file.data() + t->offsets[0], f0.data(), f0.size()) == 0);
        CHECK(std::memcmp(file.data() + t->offsets[2], f2.data(), f2.size()) == 0);  // after the junk
    }
    std::string err;
    Bytes f0, f2;
    Bytes file = make_mov(false, &f0, &f2);
    Bytes cut(file.begin(), file.end() - 10);  // moov cut short
    CHECK(!read_mov(memory(cut), &err).has_value() && !err.empty());
    const Bytes no_moov = atom("ftyp", {'q', 't', ' ', ' '});
    CHECK(!read_mov(memory(no_moov), &err).has_value());
    const Bytes empty;
    CHECK(!read_mov(memory(empty), &err).has_value());
}

static void test_real_clips() {
    const std::filesystem::path dir = std::filesystem::path(HIMMEL_SOURCE_DIR) / "media" / "test";
    if (!std::filesystem::exists(dir)) {
        std::printf("real clips: media/test not found, skipped\n");
        return;
    }
    std::printf("real clips in media/test\n");
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".mov") continue;
        std::ifstream f(e.path(), std::ios::binary);
        Bytes file((std::istreambuf_iterator<char>(f)), {});
        std::string err;
        auto t = read_mov(memory(file), &err);
        CHECK(t.has_value());
        if (!t) continue;
        const HapTexture tex = hap_peek_texture(file.data() + t->offsets[0], t->sizes[0]);
        Bytes out(hap_texture_bytes(tex, t->width, t->height));
        size_t bad = 0;
        for (size_t i = 0; i < t->frames(); ++i) {
            const auto r = hap_decode(file.data() + t->offsets[i], t->sizes[i], out.data(), out.size());
            bad += !(r.ok && r.bytes == out.size());
        }
        CHECK(bad == 0);
        std::printf("  %s: %s %dx%d, %zu frames ok\n", e.path().filename().string().c_str(), hap_texture_name(tex),
                    t->width, t->height, t->frames() - bad);
    }
}

int main() {
    test_hap();
    test_mov();
    test_real_clips();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
