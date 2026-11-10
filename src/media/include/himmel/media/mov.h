// Minimal QuickTime / MP4 reader for HAP clips (decision 2026-11-10: own MOV
// reader + Snappy, FFmpeg is not linked).
//
// Reads only what playback needs from the first video track: codec fourcc,
// size, frame rate and the file offset + size of every frame. Runs at clip load
// time in a worker (it allocates); never in the frame loop.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace himmel::media {

// Random access to the file: read `size` bytes at `offset` into `dst`.
struct ByteSource {
    uint64_t size = 0;
    std::function<bool(uint64_t offset, void* dst, size_t size)> read;
};

struct MovVideoTrack {
    uint32_t fourcc = 0;  // 'Hap1', 'Hap5', 'HapY', 'HapM', 'HapA', ...
    int width = 0, height = 0;
    uint32_t timescale = 0;
    uint64_t duration = 0;   // in timescale units
    double fps = 0.0;        // from the most common frame duration
    std::vector<uint64_t> offsets;  // file offset per frame
    std::vector<uint32_t> sizes;    // bytes per frame
    uint32_t max_frame_size = 0;
    size_t frames() const { return sizes.size(); }
    std::string codec() const;  // fourcc as text
};

std::optional<MovVideoTrack> read_mov(const ByteSource& src, std::string* error);

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

}  // namespace himmel::media
