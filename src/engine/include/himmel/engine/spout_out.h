// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Spout out (M6, design/engine.md §4): MAIN as a Spout 2 DX11 sender.
//
// SpoutDX (BSD-2, leadedge/Spout2) waits on Spout's named mutex while it copies,
// so it never runs on the render thread. The render thread only copies MAIN into
// one of two shared textures (made on its device with a legacy share handle) and
// signals; a worker with its own D3D11 device on the same adapter opens them and
// calls SpoutDX::SendTexture. The worker always reads the texture written one
// frame earlier, so the two devices never touch the same texture in the same frame.
// Any failure is logged once and Spout stops; the output never notices.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace himmel::engine {

class SpoutOut {
public:
    using LogFn = void (*)(const char* line);
    SpoutOut() = default;
    ~SpoutOut() { stop(); }
    SpoutOut(const SpoutOut&) = delete;
    SpoutOut& operator=(const SpoutOut&) = delete;

    // Start-up: the sender name (shown in receivers) and the render device.
    bool start(ID3D11Device* render_device, const char* name, LogFn log);
    void stop();
    bool running() const { return running_.load() && !failed_.load(); }

    // Start-up / canvas rebuild only (allocates): (re)make the two shared textures.
    bool resize(int w, int h);
    // Frame loop: copy MAIN (or clear to black) into the next shared texture and wake
    // the worker. No waits, no allocation: one CopyResource (or a clear) and SetEvent.
    void frame(ID3D11DeviceContext* ctx, ID3D11Texture2D* main, bool black);

    uint64_t sent() const { return sent_.load(); }

private:
    void run();
    void fail(const char* why);

    ID3D11Device* dev_ = nullptr;
    LogFn log_ = nullptr;
    std::string name_;
    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false}, failed_{false};
    void* wake_ = nullptr;
    struct Shared {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        void* handle = nullptr;  // legacy share handle
    } shared_[2];
    int w_ = 0, h_ = 0;
    int next_ = 0;
    std::atomic<uint32_t> gen_{0};        // bumps when the textures are remade
    std::atomic<int> ready_{-1};          // the texture last written (worker reads it)
    std::atomic<uint64_t> written_{0}, sent_{0};
    std::atomic<bool> busy_[2] = {};      // the worker is reading this one: skip it
};

}  // namespace himmel::engine
