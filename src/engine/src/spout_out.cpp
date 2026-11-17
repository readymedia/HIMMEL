// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Spout out (M6). See spout_out.h. SpoutDX: BSD-2, (c) Lynn Jarvis (THIRD_PARTY_NOTICES.md).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "himmel/engine/spout_out.h"

#include <cstdio>
#include <memory>

#include "SpoutDX.h"

namespace himmel::engine {

namespace {
template <class T>
void release(T*& p) {
    if (p) p->Release(), p = nullptr;
}
}  // namespace

bool SpoutOut::start(ID3D11Device* render_device, const char* name, LogFn log) {
    if (running_.load()) return true;
    dev_ = render_device;
    log_ = log;
    name_ = name && *name ? name : "Himmel";
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_.store(false);
    failed_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void SpoutOut::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    SetEvent(static_cast<HANDLE>(wake_));
    thread_.join();
    running_.store(false);
    CloseHandle(static_cast<HANDLE>(wake_));
    wake_ = nullptr;
    for (auto& s : shared_) release(s.rtv), release(s.tex), s.handle = nullptr;
}

void SpoutOut::fail(const char* why) {
    if (!failed_.exchange(true) && log_) {
        char line[200];
        std::snprintf(line, sizeof(line), "spout: %s - Spout out stopped, the outputs go on", why);
        log_(line);
    }
}

bool SpoutOut::resize(int w, int h) {
    if (!running_.load() || failed_.load() || !dev_) return false;
    if (w == w_ && h == h_ && shared_[0].tex) return true;
    ready_.store(-1);
    for (auto& s : shared_) release(s.rtv), release(s.tex), s.handle = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // the canvas format
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // legacy share handle: opened by the worker's device
    for (auto& s : shared_) {
        IDXGIResource* r = nullptr;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s.tex)) || FAILED(dev_->CreateRenderTargetView(s.tex, nullptr, &s.rtv)) ||
            FAILED(s.tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&r))) ||
            FAILED(r->GetSharedHandle(&s.handle))) {
            release(r);
            fail("cannot make the shared textures");
            return false;
        }
        release(r);
    }
    w_ = w, h_ = h;
    gen_.fetch_add(1);  // the worker reopens them
    return true;
}

void SpoutOut::frame(ID3D11DeviceContext* ctx, ID3D11Texture2D* main, bool black) {
    if (!running_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed) || !shared_[0].tex) return;
    const int r = ready_.load();
    const int w = r < 0 ? 0 : 1 - r;
    if (busy_[w].load()) return;  // the worker still reads it: skip this frame (Spout runs below the output rate)
    if (black) {
        const float k[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(shared_[w].rtv, k);
    } else {
        ctx->CopyResource(shared_[w].tex, main);
    }
    ready_.store(w);
    written_.fetch_add(1);
    SetEvent(static_cast<HANDLE>(wake_));
}

void SpoutOut::run() {
    SetThreadDescription(GetCurrentThread(), L"himmel spout out");
    // Our own device on the render device's adapter: every Spout wait happens here.
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* ad = nullptr;
    if (dev_ && SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd)))) dd->GetAdapter(&ad);
    release(dd);
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    release(ad);
    if (FAILED(hr)) {
        fail("no D3D11 device for the Spout worker");
        return;
    }
    auto sender = std::make_unique<spoutDX>();
    if (!sender->OpenDirectX11(dev)) {
        fail("SpoutDX could not use the device");
        release(ctx), release(dev);
        return;
    }
    sender->SetSenderName(name_.c_str());
    ID3D11Texture2D* opened[2] = {};
    uint32_t gen_seen = 0;
    bool announced = false;
    while (!stop_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(wake_), 100);
        if (stop_.load() || failed_.load()) break;
        const uint32_t g = gen_.load();
        if (g != gen_seen) {  // new textures (start-up / canvas rebuild)
            release(opened[0]), release(opened[1]);
            bool ok = true;
            for (int i = 0; i < 2 && ok; ++i) {
                void* h = shared_[i].handle;
                ok = h && SUCCEEDED(dev->OpenSharedResource(static_cast<HANDLE>(h), __uuidof(ID3D11Texture2D),
                                                            reinterpret_cast<void**>(&opened[i])));
            }
            if (gen_.load() != g) continue;  // remade again meanwhile: next round
            if (!ok) {
                fail("cannot open the shared textures");
                break;
            }
            gen_seen = g;
        }
        const int r = ready_.load();
        if (r < 0 || !opened[r]) continue;
        busy_[r].store(true);
        if (ready_.load() != r) {  // the render thread moved on: take the newer one next round
            busy_[r].store(false);
            SetEvent(static_cast<HANDLE>(wake_));
            continue;
        }
        const bool ok = sender->SendTexture(opened[r]);
        ctx->Flush();
        busy_[r].store(false);
        if (!ok) {
            fail("SendTexture failed");
            break;
        }
        sent_.fetch_add(1);
        if (!announced && log_) {
            char line[200];
            std::snprintf(line, sizeof(line), "spout: sending MAIN as \"%s\" (%dx%d)", name_.c_str(), w_, h_);
            log_(line);
            announced = true;
        }
    }
    sender->ReleaseSender();
    sender->CloseDirectX11();
    release(opened[0]), release(opened[1]);
    release(ctx), release(dev);
}

}  // namespace himmel::engine
