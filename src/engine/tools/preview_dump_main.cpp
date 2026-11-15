// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// preview_dump.exe - reads Local\HimmelPreview like the GUI bridge does and
// writes the newest picture of every active stream as a PNG, plus how fast each
// stream is published.
//
//   preview_dump [out_dir] [--rate]      default out_dir: tools/_screens/preview
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../../controller/app/png_write.h"
#include "himmel/engine/shm_block.h"
#include "himmel/protocol/preview.h"

using namespace himmel::protocol;

int main(int argc, char** argv) {
    std::string out_dir = "tools/_screens/preview";
    bool rate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rate") rate = true;
        else out_dir = a;
    }
    himmel::engine::ShmBlock shm;
    if (!shm.open_existing(kPreviewShmName, sizeof(PreviewBlock))) {
        std::printf("no preview block (%ls): start himmel_render first\n", kPreviewShmName);
        return 2;
    }
    const auto* b = static_cast<const PreviewBlock*>(shm.data());
    if (!himmel::engine::header_ok(b->header, kPreviewMagic, kPreviewVersion, sizeof(PreviewBlock))) {
        std::printf("preview block has another magic / version / size\n");
        return 2;
    }
    static const char* kNames[kPreviewStreams] = {"deck_a", "deck_b", "main", "output1", "output2", "output3"};
    uint64_t pub0[kPreviewStreams];
    for (int i = 0; i < kPreviewStreams; ++i) pub0[i] = b->stream[i].published.load();
    if (rate) std::this_thread::sleep_for(std::chrono::seconds(2));

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::vector<uint8_t> bgra(kPreviewBytes), rgb;
    int written = 0;
    for (int i = 0; i < kPreviewStreams; ++i) {
        const PreviewStreamBlock& s = b->stream[i];
        if (!s.active.load()) {
            std::printf("  %-8s inactive\n", kNames[i]);
            continue;
        }
        const uint32_t w = s.width, h = s.height;
        if (!w || !h || w > kPreviewW || h > kPreviewH) continue;
        const uint32_t k = s.latest.load() % kPreviewBuffers;
        bool ok = false;
        for (int attempt = 0; attempt < 20 && !ok; ++attempt)
            ok = seq_read_bytes(s.buffer[k].seq, s.buffer[k].pixels, bgra.data(), static_cast<size_t>(w) * h * 4);
        if (!ok) {
            std::printf("  %-8s torn copy, skipped\n", kNames[i]);
            continue;
        }
        rgb.resize(static_cast<size_t>(w) * h * 3);
        double sum = 0;
        for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p) {
            rgb[p * 3 + 0] = bgra[p * 4 + 2];
            rgb[p * 3 + 1] = bgra[p * 4 + 1];
            rgb[p * 3 + 2] = bgra[p * 4 + 0];
            sum += bgra[p * 4 + 0] + bgra[p * 4 + 1] + bgra[p * 4 + 2];
        }
        const std::string file = out_dir + "/preview_" + kNames[i] + ".png";
        himmel::png::write_rgb(file, static_cast<int>(w), static_cast<int>(h), rgb);
        ++written;
        std::printf("  %-8s %3ux%-3u frame %-8llu published %-8llu avg level %5.1f", kNames[i], w, h,
                    static_cast<unsigned long long>(s.buffer[k].frame), static_cast<unsigned long long>(s.published.load()),
                    sum / (3.0 * w * h));
        if (rate) std::printf("  %.1f pictures/s (target %u)", (s.published.load() - pub0[i]) / 2.0, s.fps_target);
        std::printf("  -> %s\n", file.c_str());
    }
    return written ? 0 : 3;
}
