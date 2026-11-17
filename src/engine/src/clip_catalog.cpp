// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/clip_catalog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "himmel/media/hap.h"
#include "himmel/media/mov.h"

namespace himmel::engine {

namespace {

std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

bool read_at(HANDLE h, uint64_t offset, void* dst, size_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    return size <= 0xFFFFFFFFu && ReadFile(h, dst, static_cast<DWORD>(size), &got, &ov) && got == size;
}

void rgb565(uint16_t c, float out[3]) {
    out[0] = ((c >> 11) & 31) / 31.0f;
    out[1] = ((c >> 5) & 63) / 63.0f;
    out[2] = (c & 31) / 31.0f;
}

// Average colour of a decoded HAP texture, from the colour end points of every block.
uint32_t average_rgb(media::HapTexture t, const uint8_t* d, size_t bytes) {
    const size_t block = t == media::HapTexture::DXT1 || t == media::HapTexture::RGTC1 ? 8 : 16;
    const size_t n = bytes / block;
    if (!n) return 0x404040;
    double sum[4] = {0, 0, 0, 0};
    for (size_t b = 0; b < n; ++b) {
        const uint8_t* p = d + b * block;
        if (t == media::HapTexture::RGTC1) {
            sum[0] += (p[0] + p[1]) / 510.0;
            continue;
        }
        const uint8_t* c = block == 16 ? p + 8 : p;
        float c0[3], c1[3];
        rgb565(static_cast<uint16_t>(c[0] | (c[1] << 8)), c0);
        rgb565(static_cast<uint16_t>(c[2] | (c[3] << 8)), c1);
        for (int k = 0; k < 3; ++k) sum[k] += (c0[k] + c1[k]) * 0.5;
        if (block == 16) sum[3] += (p[0] + p[1]) / 510.0;
    }
    for (double& s : sum) s /= static_cast<double>(n);
    double r = sum[0], g = sum[1], bl = sum[2];
    if (t == media::HapTexture::RGTC1) {
        g = bl = r;
    } else if (t == media::HapTexture::YCoCgDXT5) {  // same maths as the shader (Vidvox reference)
        const double scale = bl * (255.0 / 8.0) + 1.0;
        const double co = (r - 0.5 * 256.0 / 255.0) / scale, cg = (g - 0.5 * 256.0 / 255.0) / scale;
        const double y = sum[3];
        r = y + co - cg, g = y + cg, bl = y - co - cg;
    }
    auto byte = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return (byte(r) << 16) | (byte(g) << 8) | byte(bl);
}

}  // namespace

void probe_clip(const std::string& path, protocol::ClipInfo& info) {
    info.codec = 3;
    info.thumb_rgb = 0x404040;  // GUESS: grey for a file we cannot read
    const std::wstring w = utf8_path(path).wstring();
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    media::ByteSource src;
    src.size = static_cast<uint64_t>(size.QuadPart);
    src.read = [h](uint64_t off, void* dst, size_t n) { return read_at(h, off, dst, n); };
    std::string err;
    if (auto track = media::read_mov(src, &err); track && track->frames() > 0) {
        info.width = static_cast<uint32_t>(track->width);
        info.height = static_cast<uint32_t>(track->height);
        info.fps = static_cast<float>(track->fps);
        info.seconds = track->fps > 0 ? static_cast<float>(track->frames() / track->fps) : 0.0f;
        std::vector<uint8_t> frame(track->sizes[0]);
        if (read_at(h, track->offsets[0], frame.data(), frame.size())) {
            const auto t = media::hap_peek_texture(frame.data(), frame.size());
            if (t != media::HapTexture::Unknown) {
                info.codec = t == media::HapTexture::DXT1 ? 0 : t == media::HapTexture::DXT5 ? 1
                           : t == media::HapTexture::YCoCgDXT5 ? 2 : 3;
                std::vector<uint8_t> tex(media::hap_texture_bytes(t, track->width, track->height));
                const auto r = media::hap_decode(frame.data(), frame.size(), tex.data(), tex.size());
                if (r.ok) info.thumb_rgb = average_rgb(t, tex.data(), r.bytes);
            }
        }
    }
    CloseHandle(h);
}

namespace {
void probe(const std::string& path, protocol::ClipInfo& info) { probe_clip(path, info); }
}  // namespace

std::vector<std::string> find_media_clips() {
    std::vector<std::string> out;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::error_code ec;
    for (auto dir = std::filesystem::path(self).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const auto media = dir / "media";
        if (std::filesystem::is_directory(media, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(media, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                auto ext = it->path().extension().string();
                for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext == ".mov") {
                    const auto u = it->path().u8string();
                    out.emplace_back(u.begin(), u.end());
                }
            }
            break;
        }
        if (dir == dir.parent_path()) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void clip_info_for(const std::string& path, protocol::ClipInfo& info) {
    std::memset(&info, 0, sizeof(info));
    const auto stem = utf8_path(path).stem().u8string();
    std::snprintf(info.name, sizeof(info.name), "%.*s", static_cast<int>(std::min<size_t>(stem.size(), protocol::kNameLen - 1)),
                  reinterpret_cast<const char*>(stem.data()));
    probe(path, info);
}

void fill_clip_catalog(protocol::Catalog& c, const std::vector<std::string>& paths) {
    c.clip_count = static_cast<uint32_t>(std::min<size_t>(paths.size(), protocol::kMaxClips));
    for (uint32_t i = 0; i < c.clip_count; ++i) {
        protocol::ClipInfo& info = c.clip[i];
        std::memset(&info, 0, sizeof(info));
        const auto stem = utf8_path(paths[i]).stem().u8string();
        std::snprintf(info.name, sizeof(info.name), "%.*s", static_cast<int>(std::min<size_t>(stem.size(), protocol::kNameLen - 1)),
                      reinterpret_cast<const char*>(stem.data()));
        probe(paths[i], info);
    }
}

}  // namespace himmel::engine
