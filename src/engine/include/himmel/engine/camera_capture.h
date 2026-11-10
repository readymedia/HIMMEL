// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Webcam capture (Media Foundation, CLAUDE.md: one capture thread per camera).
//
// The worker opens the camera, picks the best native format (>= 29 fps first,
// then the largest size up to 1920x1080, then NV12 > YUY2 > MJPG), lets the
// source reader convert to NV12 with low-latency mode on, and hands every frame
// to the render thread through a lock-free triple buffer. If the camera fails
// or is unplugged it retries every 2 s; the output never waits for it.
//
// For the latency test it also publishes the mean brightness of the centre of
// each frame with the frame's arrival time (SPSC ring, read by the renderer).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace himmel::engine {

inline constexpr int kCamMaxW = 1920, kCamMaxH = 1080;

struct CameraFrame {
    std::vector<uint8_t> y, uv;  // NV12 planes, packed (pitch = width)
    int width = 0, height = 0;
    uint64_t t_us = 0;           // arrival (now_us) right after ReadSample returned
    uint64_t seq = 0;
};

struct LumaSample {
    uint64_t t_us;
    float luma;  // 0..255, centre third of the frame
};

class CameraCapture {
public:
    // name_hint: pick the first camera whose name contains it (case-insensitive);
    // empty = first camera that is not an IR camera.
    CameraCapture(std::string name_hint, int index);
    ~CameraCapture();
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // Render thread: the newest frame (stays valid until the next call), or the
    // same one again if nothing new arrived. Null before the first frame.
    const CameraFrame* latest();

    bool pop_luma(LumaSample& out);

    std::atomic<bool> running{false};   // camera open and delivering
    std::atomic<uint32_t> fps_x100{0};  // measured delivery rate
    std::atomic<uint32_t> opens{0};
    char name[96] = "";                 // valid once opens > 0
    char format[48] = "";
    char error[160] = "";

private:
    void run();
    bool capture_session();  // returns when the camera fails or stop_ is set

    std::string hint_;
    int index_;
    std::thread worker_;
    std::atomic<bool> stop_{false};

    CameraFrame frames_[3];
    std::atomic<uint32_t> ready_{1 | 0};  // index | kNew; front 0, ready 1, back 2 at start
    uint32_t front_ = 0;                  // render thread
    uint32_t back_ = 2;                   // worker thread

    static constexpr uint32_t kLumaRing = 256;
    LumaSample luma_[kLumaRing]{};
    std::atomic<uint32_t> luma_head_{0}, luma_tail_{0};
};

}  // namespace himmel::engine
