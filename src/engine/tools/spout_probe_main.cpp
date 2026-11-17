// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// spout_probe.exe - a Spout receiver for checks (M6): receives a sender for a few
// seconds with SpoutDX and prints its size, the frames received, the sender's
// frame rate and the mean brightness of the picture.
//
//   spout_probe [SENDER] [--seconds N]      default: the active sender, 3 s
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SpoutDX.h"

int main(int argc, char** argv) {
    std::string name;
    double seconds = 3.0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else name = argv[i];
    }
    auto rx = std::make_unique<spoutDX>();
    if (!rx->OpenDirectX11()) {
        std::printf("spout_probe: no D3D11 device\n");
        return 2;
    }
    if (!name.empty()) rx->SetReceiverName(name.c_str());
    std::vector<unsigned char> px;
    unsigned w = 0, h = 0;
    long frames = 0, first_frame = -1, last_frame = -1;
    double mean = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (rx->ReceiveImage(px.empty() ? nullptr : px.data(), w, h)) {
            if (rx->IsUpdated()) {
                w = rx->GetSenderWidth(), h = rx->GetSenderHeight();
                px.assign(static_cast<size_t>(w) * h * 4, 0);
                continue;
            }
            if (!px.empty() && rx->IsFrameNew()) {
                ++frames;
                const long f = rx->GetSenderFrame();
                if (first_frame < 0) first_frame = f;
                last_frame = f;
                double sum = 0;
                const size_t step = 997;  // a sparse sample is enough
                size_t n = 0;
                for (size_t i = 0; i + 3 < px.size(); i += step * 4, ++n) sum += px[i] + px[i + 1] + px[i + 2];
                mean = n ? sum / (3.0 * n) : 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (px.empty()) {
        std::printf("spout_probe: no sender%s%s\n", name.empty() ? "" : " named ", name.c_str());
        rx->CloseDirectX11();
        return 1;
    }
    std::printf("spout_probe: \"%s\" %ux%u  %ld frames in %.1f s (sender frames %ld..%ld, %.1f fps)  mean %.1f\n",
                rx->GetSenderName(), w, h, frames, seconds, first_frame, last_frame, rx->GetSenderFps(), mean);
    rx->ReleaseReceiver();
    rx->CloseDirectX11();
    return frames > 0 ? 0 : 1;
}
