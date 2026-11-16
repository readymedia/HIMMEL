// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// WASAPI input (shared mode): a capture endpoint (mic, line in) or the
// loopback of a render endpoint. Part of Windows: no new dependency.
// One object per open device; used from one thread (COM MTA on that thread).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "himmel/audio/block.h"

namespace himmel::audio {

struct InputRequest {
    InputMode mode = InputMode::Capture;
    std::string device;          // part of the friendly name, case-insensitive; "" = default
    int channel_first = 0;       // 0-based
    int channel_count = 1;       // 1 or 2 (summed)
};

struct EndpointInfo {
    std::string name;
    bool is_default = false;
    int channels = 0;
    int sample_rate = 0;
};

// Active endpoints for a mode (capture endpoints, or render endpoints for loopback).
// Call with COM initialised on this thread.
std::vector<EndpointInfo> list_endpoints(InputMode mode);

class WasapiInput {
public:
    using Sink = void (*)(const float* mono, int n, uint64_t t_last_us, void* ctx);
    enum class Read { Data, Timeout, Lost };

    WasapiInput() = default;
    ~WasapiInput() { close(); }
    WasapiInput(const WasapiInput&) = delete;
    WasapiInput& operator=(const WasapiInput&) = delete;

    bool open(const InputRequest& req, std::string* error);
    void close();
    bool is_open() const { return client_ != nullptr; }

    // Waits up to timeout_ms for packets and hands each one to `sink` as mono
    // float. Lost = the device went away (unplugged, disabled, format change).
    Read read(int timeout_ms, Sink sink, void* ctx);

    const std::string& name() const { return name_; }
    int sample_rate() const { return rate_; }
    int device_channels() const { return channels_; }
    int channel_first() const { return ch_first_; }
    int channel_count() const { return ch_count_; }
    uint32_t glitches() const { return glitches_; }
    InputMode mode() const { return mode_; }

private:
    void* enumerator_ = nullptr;  // IMMDeviceEnumerator
    void* device_ = nullptr;      // IMMDevice
    void* client_ = nullptr;      // IAudioClient
    void* capture_ = nullptr;     // IAudioCaptureClient
    void* event_ = nullptr;       // HANDLE (capture mode)
    std::string name_;
    InputMode mode_ = InputMode::Capture;
    int rate_ = 0, channels_ = 0, ch_first_ = 0, ch_count_ = 1;
    int format_ = 0;              // 0 float32, 1 pcm16, 2 pcm24, 3 pcm32
    int block_align_ = 0;
    uint32_t glitches_ = 0;
    std::vector<float> mono_;
};

}  // namespace himmel::audio
