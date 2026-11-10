// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// hap_probe.exe - checks a HAP clip without the engine.
//
//   hap_probe clip.mov [--png first_frame.png]
//
// Prints the track (codec, size, fps, frames, sizes), decodes every frame and
// times disk read + Snappy per frame. With --png it also turns the first frame
// into RGB on the CPU (slow, for checking only) and saves it.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "himmel/media/hap.h"
#include "himmel/media/mov.h"
#include "../../controller/app/png_write.h"

using namespace himmel::media;
using Clock = std::chrono::steady_clock;

namespace {

void rgb565(uint16_t c, float o[3]) {
    o[0] = ((c >> 11) & 31) / 31.0f;
    o[1] = ((c >> 5) & 63) / 63.0f;
    o[2] = (c & 31) / 31.0f;
}

// One BC1 colour block -> 16 RGB(A) pixels. alpha_block = BC3 (always 4-colour).
void bc1_block(const uint8_t* b, bool bc3, float px[16][4]) {
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    float p[4][4] = {};
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 1;
    for (int i = 0; i < 3; ++i) {
        if (c0 > c1 || bc3) {
            p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
            p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
        } else {
            p[2][i] = (p[0][i] + p[1][i]) / 2;
            p[3][i] = 0;
        }
    }
    if (!(c0 > c1 || bc3)) p[3][3] = 0;
    const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 4; ++k) px[i][k] = p[(idx >> (2 * i)) & 3][k];
}

void bc4_block(const uint8_t* b, float out[16]) {
    float a[8];
    a[0] = b[0] / 255.0f;
    a[1] = b[1] / 255.0f;
    if (b[0] > b[1])
        for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
    else {
        for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
        a[6] = 0, a[7] = 1;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

bool save_png(const std::string& path, HapTexture tex, const uint8_t* data, int w, int h) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    const size_t block = tex == HapTexture::DXT1 || tex == HapTexture::RGTC1 ? 8 : 16;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (static_cast<size_t>(by) * bw + bx) * block;
            float px[16][4];
            if (tex == HapTexture::RGTC1) {
                float a[16];
                bc4_block(b, a);
                for (int i = 0; i < 16; ++i) px[i][0] = px[i][1] = px[i][2] = a[i];
            } else if (tex == HapTexture::DXT1) {
                bc1_block(b, false, px);
            } else {
                float a[16];
                bc4_block(b, a);  // BC3 alpha block has the same layout as BC4
                bc1_block(b + 8, true, px);
                for (int i = 0; i < 16; ++i) px[i][3] = a[i];
            }
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                float r = px[i][0], g = px[i][1], bl = px[i][2];
                if (tex == HapTexture::YCoCgDXT5) {  // same maths as the engine shader
                    const float scale = px[i][2] * (255.0f / 8.0f) + 1.0f;
                    const float co = (px[i][0] - 0.5f * 256.0f / 255.0f) / scale;
                    const float cg = (px[i][1] - 0.5f * 256.0f / 255.0f) / scale;
                    const float Y = px[i][3];
                    r = Y + co - cg, g = Y + cg, bl = Y - co - cg;
                } else if (tex == HapTexture::DXT5) {  // show alpha over a checkerboard
                    const float bg = ((x / 16 + y / 16) & 1) ? 0.6f : 0.3f, a = px[i][3];
                    r = r * a + bg * (1 - a), g = g * a + bg * (1 - a), bl = bl * a + bg * (1 - a);
                }
                uint8_t* o = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                o[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255 + 0.5f);
                o[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255 + 0.5f);
                o[2] = static_cast<uint8_t>(std::clamp(bl, 0.0f, 1.0f) * 255 + 0.5f);
            }
        }
    return himmel::png::write_rgb(path, w, h, rgb);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: hap_probe clip.mov [--png first_frame.png]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::string png;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--png") png = argv[i + 1];

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::printf("cannot open %s\n", path.c_str());
        return 1;
    }
    ByteSource src;
    src.size = static_cast<uint64_t>(f.tellg());
    src.read = [&](uint64_t off, void* dst, size_t n) {
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f.gcount()) == n;
    };
    std::string err;
    const auto t0 = Clock::now();
    auto track = read_mov(src, &err);
    const double parse_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!track) {
        std::printf("%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    std::vector<uint8_t> in(track->max_frame_size);
    if (!src.read(track->offsets[0], in.data(), track->sizes[0])) return 1;
    const HapTexture tex = hap_peek_texture(in.data(), track->sizes[0]);
    const size_t tex_bytes = hap_texture_bytes(tex == HapTexture::Unknown ? HapTexture::DXT5 : tex, track->width, track->height);
    std::printf("%s\n  codec %s  %s  %dx%d  %.3f fps  %zu frames  %.2f s  parse %.2f ms\n", path.c_str(),
                track->codec().c_str(), hap_texture_name(tex), track->width, track->height, track->fps, track->frames(),
                track->frames() / track->fps, parse_ms);
    std::printf("  frame bytes: max %u, texture %zu (%.0f%% after Snappy)\n", track->max_frame_size, tex_bytes,
                100.0 * track->max_frame_size / tex_bytes);

    std::vector<uint8_t> out(tex_bytes);
    double read_sum = 0, dec_sum = 0, dec_max = 0;
    int chunks = 0;
    size_t failed = 0;
    for (size_t i = 0; i < track->frames(); ++i) {
        const auto a = Clock::now();
        if (!src.read(track->offsets[i], in.data(), track->sizes[i])) {
            ++failed;
            continue;
        }
        const auto b = Clock::now();
        const HapResult r = hap_decode(in.data(), track->sizes[i], out.data(), out.size());
        const auto c = Clock::now();
        if (!r.ok || r.bytes != tex_bytes) {
            if (!failed) std::printf("  frame %zu: %s (%zu of %zu bytes)\n", i, r.error ? r.error : "short", r.bytes, tex_bytes);
            ++failed;
        }
        chunks = r.chunks;
        const double d = std::chrono::duration<double, std::milli>(c - b).count();
        read_sum += std::chrono::duration<double, std::milli>(b - a).count();
        dec_sum += d;
        dec_max = std::max(dec_max, d);
        if (i == 0 && !png.empty() && r.ok) {
            const bool ok = save_png(png, r.texture, out.data(), track->width, track->height);
            std::printf("  first frame -> %s %s\n", png.c_str(), ok ? "" : "(FAILED)");
        }
    }
    const double n = static_cast<double>(track->frames());
    std::printf("  chunks %d  read avg %.2f ms  decode avg %.2f ms  max %.2f ms  failed %zu\n", chunks, read_sum / n,
                dec_sum / n, dec_max, failed);
    return failed ? 1 : 0;
}
