// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_render.exe - the slice engine (docs/roadmap.md §0a, task S1).
//
//   himmel_render [--outputs N] [--fullscreen] [--size 640x360] [--mode span|mirror]
//                 [--clip file.mov ...] [--no-overlay] [--preview-fps N] [--stats-csv]
//                 [--flash-cap HZ] [--test-signal] [--ticker "TEXT"]   (effects: src/engine/fx/, M5)
//                 [--project FOLDER|FILE.himmelshow] [--no-show-files] [--show-root DIR]   (show: src/show/, M3)
//                 [--av-offset MS] [--no-audio]   (music: Local\HimmelAudio from himmel_audio.exe, M8)
//
// * Show model: DeckModel (2 decks x 4 layers, FX slots on layer / deck / master,
//   crossfader, TAKE / CUT / black / focus). Every change comes in as a Command:
//   Local\HimmelCommands rings 1..7 (GUI, OSC, tools) and the in-process MK2
//   Perform mapper on slot 0, all through the same validation, applied at the
//   start of the frame (docs/interfaces/command-protocol.md).
// * State out: Local\HimmelState (live + catalog, new epoch per start, restored
//   by clip name from a block that survived us). Previews out: Local\HimmelPreview
//   (DeckA, DeckB, Main, Output1..3) through staging textures mapped with
//   DO_NOT_WAIT - the frame never waits for the GPU.
// * Render: per deck, up to 4 HAP layers (one ClipPlayer worker each) composited
//   with blend mode + opacity, layer FX and deck FX (himmel::fx::FxGpu: one
//   shader per effect from fx/, compiled at start-up); A / B through the crossfader into MAIN, master FX, black, then
//   onto 1..8 outputs (span or mirror). A missing / loading clip shows its
//   placeholder colour.
// * The MK2: ring 0 of Local\HimmelMk2 -> himmel::perform::Mk2Perform; its LEDs
//   and screens go back into the block.
// * The show (M3, src/show/): show commands (types 25..36) go to himmel::show::ShowRuntime,
//   which turns scenes / undo / dice into ordinary commands on slot 4 and keeps every
//   file in its own worker thread (Local\HimmelShow, docs/interfaces/show-files.md).
// * Parked (in himmel_render_legacy.exe): test card, camera + latency test,
//   Touch FX / Entrancer, the generic-weapon knob pages.
//
// Frame loop rules: no allocation, no locks, no disk I/O, nothing compiled.
// Windows, swap chains, textures, clip players and the shared blocks are made
// at start-up (or on a window resize); logging goes through LogRing (a lock-free
// ring printed by its own thread).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "himmel/engine/clip_catalog.h"
#include "himmel/engine/clip_player.h"
#include "himmel/engine/deck_model.h"
#include "himmel/engine/reactions.h"
#include "himmel/engine/engine_shared.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/log_ring.h"
#include "himmel/engine/shm_block.h"
#include "himmel/fx/fx_gpu.h"
#include "himmel/fx/signals.h"
#include "himmel/mk2/output.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"
#include "himmel/perform/mk2_perform.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/preview.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"
#include "himmel/show/files.h"
#include "himmel/show/runtime.h"
#include "slice_shaders.h"

using Microsoft::WRL::ComPtr;
namespace mk2 = himmel::mk2;
namespace proto = himmel::protocol;
using himmel::engine::DeckModel;
using himmel::engine::kHitSlot;
using himmel::engine::kLayerSlots;
using himmel::mk2::now_us;
using proto::kDecks;
using proto::kFxSlots;
using proto::kLayersPerDeck;

namespace {

constexpr int kMaxOutputs = 8;
constexpr int kOverlayW = 512, kOverlayH = 12, kOverlayScale = 2;
constexpr int kEngineRing = 0;          // ring 0 of the MK2 block belongs to the engine
constexpr int kDrainPerProducer = 256;  // command-protocol.md §5.7

himmel::engine::LogRing* g_log = nullptr;
himmel::fx::FxGpu* g_fx = nullptr;  // effects + generators (made at start-up)
#define LOG(...) g_log->logf(__VA_ARGS__)

// ---------------------------------------------------------------- config
struct Config {
    int outputs = 0;  // 0 = one per monitor (windowed: 3)
    bool fullscreen = false;
    int win_w = 640, win_h = 360;
    bool mirror = false;
    bool overlay = true;
    int preview_fps = 20;
    std::vector<std::string> clips;
    float flash_cap = 0;       // D65: optional per-show flash cap (flashes/s), 0 = off (default)
    bool test_signal = false;  // feed the reactions the TestSignal instead of the audio block (checks)
    bool audio = true;         // M8: reactions from Local\HimmelAudio (himmel_audio.exe)
    float av_offset_ms = 0;    // M8: beat / bar signals this much early (music.md §4 AV offset)
    std::string ticker = "HIMMEL";  // GUESS: the text generator's text until shows carry it (M3)
    std::string project;            // M3: a project to open (default: the most recent one)
    bool show_files = true;         // M3: --no-show-files = no project, no autosave (tests)
    std::string show_root;          // M3: --show-root DIR = projects in DIR/Projects, recent + untitled in DIR/App (tests)
};

Config parse_args(int argc, char** argv) {
    Config c;
    bool overlay_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--outputs") c.outputs = std::clamp(std::atoi(next().c_str()), 1, kMaxOutputs);
        else if (a == "--fullscreen") c.fullscreen = true;
        else if (a == "--windowed") c.fullscreen = false;
        else if (a == "--mode") c.mirror = next() == "mirror";
        else if (a == "--clip") c.clips.push_back(next());
        else if (a == "--no-overlay") c.overlay = false, overlay_set = true;
        else if (a == "--overlay") c.overlay = true, overlay_set = true;
        else if (a == "--flash-cap") c.flash_cap = std::clamp(static_cast<float>(std::atof(next().c_str())), 0.0f, 30.0f);
        else if (a == "--test-signal") c.test_signal = true;
        else if (a == "--no-audio") c.audio = false;
        else if (a == "--av-offset") c.av_offset_ms = std::clamp(static_cast<float>(std::atof(next().c_str())), -200.0f, 200.0f);
        else if (a == "--ticker") c.ticker = next();
        else if (a == "--project") c.project = next();
        else if (a == "--no-show-files") c.show_files = false;
        else if (a == "--show-root") c.show_root = next();
        else if (a == "--preview-fps") c.preview_fps = std::clamp(std::atoi(next().c_str()), 15, 30);
        else if (a == "--size") {
            const std::string s = next();
            int w = 0, h = 0;
            if (sscanf_s(s.c_str(), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) c.win_w = w, c.win_h = h;
        }
        // Legacy flags (camera, Entrancer, MIDI) are accepted and ignored: those parts are parked.
    }
    if (!overlay_set) c.overlay = !c.fullscreen;
    return c;
}

std::vector<RECT> monitors() {
    std::vector<RECT> m;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<RECT>*>(p)->push_back(*r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&m));
    std::sort(m.begin(), m.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });
    return m;
}

// Window-proc flags (the message pump runs on the render thread).
struct WinFlags {
    bool quit = false, resized = false, mirror = false, overlay = true;
};
WinFlags g_win;
bool g_fullscreen = false;

// ---------------------------------------------------------------- GPU
struct LayerCB {
    float xform[4];
    float info[4];
    float color[4];
    float blend[4];
};
struct MixCB {
    float info[4];
};
struct BlitCB {
    float src[4];
    float overlay[4];
    float info[4];
};
struct DownCB {
    float src[4];
    float texel[4];
};

struct Rt {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

struct Output {
    HWND hwnd = nullptr;
    int w = 0, h = 0;
    ComPtr<IDXGISwapChain2> swap;
    HANDLE waitable = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> overlay;
    ComPtr<ID3D11ShaderResourceView> overlay_srv;
    std::array<uint8_t, kOverlayW * kOverlayH> overlay_px{};
    float src[4] = {0, 0, 1, 1};
};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_source, ps_layer, ps_mix, ps_blit, ps_down, ps_trans;
    ComPtr<ID3D11Buffer> cb_layer, cb_mix, cb_blit, cb_down;
    ComPtr<ID3D11SamplerState> lin, pnt, wrap;
    // Texture pool (canvas size), made at start-up and on resize only.
    Rt deck[kDecks][2];  // ping-pong per deck
    Rt layer[2];         // a layer with FX: its source, then the FX ping-pong
    Rt main[2];          // MAIN, master FX ping-pong
    Rt hold[kLayerSlots];  // per layer slot: the old picture during a FADE layer transition (M2)
    int canvas_w = 0, canvas_h = 0;
    char adapter[128] = "";
    // GPU timing (timestamp queries, read back DO_NOT_WAIT a few frames later).
    static constexpr int kQ = 4;
    ComPtr<ID3D11Query> q_disjoint[kQ], q_begin[kQ], q_preview[kQ], q_end[kQ];
    bool q_pending[kQ] = {};
};

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(himmel::engine::kSliceShaderSource, sizeof(himmel::engine::kSliceShaderSource) - 1,
                                  "himmel_slice", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  &out, &err);
    if (FAILED(hr)) {
        std::printf("shader %s failed: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return true;
}

bool make_ps(Gpu& g, const char* entry, ComPtr<ID3D11PixelShader>& ps) {
    ComPtr<ID3DBlob> b;
    return compile(entry, "ps_5_0", b) &&
           SUCCEEDED(g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
}

bool make_cb(Gpu& g, UINT bytes, ComPtr<ID3D11Buffer>& cb) {
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = (bytes + 15) & ~15u;
    return SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &cb));
}

bool init_gpu(Gpu& g) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                   &g.dev, nullptr, &g.ctx);
    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed 0x%08lx\n", hr);
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    g.dev.As(&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&g.factory));
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, g.adapter, sizeof(g.adapter), nullptr, nullptr);

    ComPtr<ID3DBlob> b;
    if (!compile("vs_full", "vs_5_0", b)) return false;
    g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.vs);
    if (!make_ps(g, "ps_source", g.ps_source) || !make_ps(g, "ps_layer", g.ps_layer) ||
        !make_ps(g, "ps_mix", g.ps_mix) || !make_ps(g, "ps_blit", g.ps_blit) || !make_ps(g, "ps_down", g.ps_down) ||
        !make_ps(g, "ps_trans", g.ps_trans))
        return false;
    if (!make_cb(g, sizeof(LayerCB), g.cb_layer) ||
        !make_cb(g, sizeof(MixCB), g.cb_mix) || !make_cb(g, sizeof(BlitCB), g.cb_blit) ||
        !make_cb(g, sizeof(DownCB), g.cb_down))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.lin);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    g.dev->CreateSamplerState(&sd, &g.wrap);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g.dev->CreateSamplerState(&sd, &g.pnt);

    D3D11_QUERY_DESC qd{};
    for (int i = 0; i < Gpu::kQ; ++i) {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        g.dev->CreateQuery(&qd, &g.q_disjoint[i]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        g.dev->CreateQuery(&qd, &g.q_begin[i]);
        g.dev->CreateQuery(&qd, &g.q_preview[i]);
        g.dev->CreateQuery(&qd, &g.q_end[i]);
    }
    return true;
}

bool make_rt(Gpu& g, Rt& rt, int w, int h, DXGI_FORMAT fmt) {
    rt = Rt{};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt.tex))) return false;
    g.dev->CreateRenderTargetView(rt.tex.Get(), nullptr, &rt.rtv);
    g.dev->CreateShaderResourceView(rt.tex.Get(), nullptr, &rt.srv);
    rt.w = w, rt.h = h;
    return true;
}

void make_canvas(Gpu& g, int w, int h) {
    if (w == g.canvas_w && h == g.canvas_h && g.main[0].tex) return;
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[2] = {};
    g.ctx->PSSetShaderResources(0, 2, none);
    for (auto& d : g.deck)
        for (auto& r : d) make_rt(g, r, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& r : g.layer) make_rt(g, r, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& r : g.main) make_rt(g, r, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& r : g.hold) make_rt(g, r, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.canvas_w = w;
    g.canvas_h = h;
    if (g_fx && !g_fx->resize(w, h)) LOG("fx: no feedback textures at %dx%d (trails pass through)", w, h);
    constexpr int targets = 8 + kLayerSlots;
    LOG("canvas %dx%d (texture pool: %d targets, %.1f MB)", w, h, targets, targets * 1.0 * w * h * 4 / 1048576.0);
}

template <class T>
void upload_cb(Gpu& g, ID3D11Buffer* cb, const T& data) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &data, sizeof(T));
        g.ctx->Unmap(cb, 0);
    }
}

// One full-screen pass into `rtv` (w x h) reading up to two textures.
void pass(Gpu& g, ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps, ID3D11ShaderResourceView* t0,
          ID3D11ShaderResourceView* t1) {
    ID3D11ShaderResourceView* none[2] = {};
    g.ctx->PSSetShaderResources(0, 2, none);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srv[2] = {t0, t1};
    g.ctx->PSSetShaderResources(0, 2, srv);
    g.ctx->Draw(3, 0);
}

// ---------------------------------------------------------------- windows
LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: g_win.resized = true; return 0;
        case WM_CLOSE:
            if (!g_fullscreen) g_win.quit = true;  // a show output never closes by click
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && (!g_fullscreen || (GetKeyState(VK_SHIFT) & 0x8000))) g_win.quit = true;
            if (wp == 'M') g_win.mirror = !g_win.mirror, g_win.resized = true;
            if (wp == 'O') g_win.overlay = !g_win.overlay;
            return 0;
        case WM_SETCURSOR:
            if (g_fullscreen && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool create_output(Gpu& g, Output& o, int index, const RECT& r, bool fullscreen) {
    const DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT wr = r;
    if (!fullscreen) AdjustWindowRect(&wr, style, FALSE);
    wchar_t title[64];
    swprintf_s(title, L"Himmel output %d", index + 1);
    o.hwnd = CreateWindowExW(fullscreen ? WS_EX_TOPMOST : 0, L"HimmelOutput", title, style, wr.left, wr.top,
                             wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!o.hwnd) return false;
    ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
    RECT cr;
    GetClientRect(o.hwnd, &cr);
    o.w = std::max<LONG>(1, cr.right);
    o.h = std::max<LONG>(1, cr.bottom);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(o.w);
    sd.Height = static_cast<UINT>(o.h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    const HRESULT hr = g.factory->CreateSwapChainForHwnd(g.dev.Get(), o.hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        std::printf("swap chain %d failed 0x%08lx\n", index + 1, hr);
        return false;
    }
    g.factory->MakeWindowAssociation(o.hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&o.swap);
    o.swap->SetMaximumFrameLatency(1);
    o.waitable = o.swap->GetFrameLatencyWaitableObject();
    ComPtr<ID3D11Texture2D> back;
    o.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g.dev->CreateRenderTargetView(back.Get(), nullptr, &o.rtv);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kOverlayW;
    td.Height = kOverlayH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g.dev->CreateTexture2D(&td, nullptr, &o.overlay);
    g.dev->CreateShaderResourceView(o.overlay.Get(), nullptr, &o.overlay_srv);
    LOG("output %d: %dx%d %s", index + 1, o.w, o.h, fullscreen ? "fullscreen" : "window");
    return true;
}

void resize_output(Gpu& g, Output& o) {
    RECT cr;
    GetClientRect(o.hwnd, &cr);
    const int w = std::max<LONG>(1, cr.right), h = std::max<LONG>(1, cr.bottom);
    if (w == o.w && h == o.h) return;
    o.rtv.Reset();
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx->Flush();
    o.swap->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN,
                          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    ComPtr<ID3D11Texture2D> back;
    o.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g.dev->CreateRenderTargetView(back.Get(), nullptr, &o.rtv);
    o.w = w;
    o.h = h;
}

// Canvas layout: span = side by side, mirror = one output-sized canvas.
void layout(Gpu& g, std::vector<Output>& outs, bool mirror) {
    int total_w = 0, max_w = 0, max_h = 0;
    for (auto& o : outs) total_w += o.w, max_w = std::max(max_w, o.w), max_h = std::max(max_h, o.h);
    make_canvas(g, mirror ? max_w : total_w, max_h);
    int x = 0;
    for (auto& o : outs) {
        if (mirror) {
            o.src[0] = 0, o.src[1] = 0, o.src[2] = 1, o.src[3] = 1;
        } else {
            o.src[0] = static_cast<float>(x) / g.canvas_w;
            o.src[1] = 0;
            o.src[2] = static_cast<float>(o.w) / g.canvas_w;
            o.src[3] = static_cast<float>(o.h) / g.canvas_h;
            x += o.w;
        }
    }
}

// ---------------------------------------------------------------- previews
// Downsample -> CopyResource into one of 3 staging textures -> map an older one
// with DO_NOT_WAIT a few frames later -> seqlock copy into the shared block.
struct PreviewWriter {
    struct Stream {
        Rt down;   // BGRA, w x h
        ComPtr<ID3D11Texture2D> staging[proto::kPreviewBuffers];
        uint64_t staged_frame[proto::kPreviewBuffers] = {};
        bool pending[proto::kPreviewBuffers] = {};
        int next = 0;            // staging slot to fill next
        uint64_t due_us = 0;     // next downsample
        float src[4] = {0, 0, 1, 1};
        int w = 0, h = 0;
        bool enabled = false;
    };
    himmel::engine::ShmBlock shm;
    proto::PreviewBlock* blk = nullptr;
    Stream s[proto::kPreviewStreams];
    int fps = 20;
    uint64_t published = 0, skipped_busy = 0;

    bool open() {
        const auto r = shm.open_or_create(proto::kPreviewShmName, sizeof(proto::PreviewBlock));
        if (r == himmel::engine::ShmBlock::Result::Failed) return false;
        blk = static_cast<proto::PreviewBlock*>(shm.data());
        // We are the only writer: (re)initialise whatever was there.
        reinterpret_cast<std::atomic<uint32_t>&>(blk->header.magic).store(0);
        for (auto& st : blk->stream) {
            st.active.store(0);
            st.latest.store(0);
        }
        himmel::engine::write_header(blk->header, proto::kPreviewMagic, proto::kPreviewVersion,
                                     static_cast<uint32_t>(sizeof(proto::PreviewBlock)));
        return true;
    }

    // Start-up / resize: sizes keep the source aspect inside 480 x 270.
    void configure(Gpu& g, int index, bool enabled, float src_aspect, const float src[4]) {
        Stream& st = s[index];
        st.enabled = enabled && blk;
        std::memcpy(st.src, src, sizeof(st.src));
        if (blk) blk->stream[index].active.store(0);
        if (!st.enabled) return;
        int w = static_cast<int>(proto::kPreviewW), h = static_cast<int>(std::lround(w / src_aspect));
        if (h > static_cast<int>(proto::kPreviewH)) h = proto::kPreviewH, w = static_cast<int>(std::lround(h * src_aspect));
        w = std::clamp(w, 16, static_cast<int>(proto::kPreviewW));
        h = std::clamp(h, 16, static_cast<int>(proto::kPreviewH));
        if (w != st.w || h != st.h || !st.down.tex) {
            make_rt(g, st.down, w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& t : st.staging) t.Reset(), g.dev->CreateTexture2D(&td, nullptr, &t);
            for (bool& p : st.pending) p = false;
            st.w = w, st.h = h;
        }
        auto& b = blk->stream[index];
        b.width = static_cast<uint32_t>(w);
        b.height = static_cast<uint32_t>(h);
        b.fps_target = static_cast<uint32_t>(fps);
        b.active.store(1);
    }

    // Frame loop: read back what the GPU has finished (never waits), then
    // downsample the streams that are due. `budget` = downsamples this frame.
    void frame(Gpu& g, uint64_t t, uint64_t frame_no, ID3D11ShaderResourceView* const srcs[proto::kPreviewStreams],
               int src_w, int src_h) {
        if (!blk) return;
        for (int i = 0; i < proto::kPreviewStreams; ++i) {
            Stream& st = s[i];
            if (!st.enabled) continue;
            // Oldest pending first.
            int oldest = -1;
            for (int k = 0; k < proto::kPreviewBuffers; ++k)
                if (st.pending[k] && (oldest < 0 || st.staged_frame[k] < st.staged_frame[oldest])) oldest = k;
            if (oldest >= 0) {
                D3D11_MAPPED_SUBRESOURCE ms;
                const HRESULT hr = g.ctx->Map(st.staging[oldest].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                    ++skipped_busy;
                } else if (SUCCEEDED(hr)) {
                    auto& b = blk->stream[i];
                    const uint32_t dst = (b.latest.load(std::memory_order_relaxed) + 1) % proto::kPreviewBuffers;
                    auto& buf = b.buffer[dst];
                    const size_t row = static_cast<size_t>(st.w) * 4;
                    buf.seq.fetch_add(1, std::memory_order_acq_rel);
                    std::atomic_thread_fence(std::memory_order_release);
                    for (int y = 0; y < st.h; ++y)
                        std::memcpy(buf.pixels + y * row, static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch, row);
                    buf.frame = st.staged_frame[oldest];
                    std::atomic_thread_fence(std::memory_order_release);
                    buf.seq.fetch_add(1, std::memory_order_acq_rel);
                    g.ctx->Unmap(st.staging[oldest].Get(), 0);
                    b.latest.store(dst, std::memory_order_release);
                    b.published.fetch_add(1, std::memory_order_acq_rel);
                    st.pending[oldest] = false;
                    ++published;
                } else {
                    st.pending[oldest] = false;
                }
            }
            if (t < st.due_us || st.pending[st.next] || !srcs[i]) continue;
            st.due_us = t + 1000000 / static_cast<uint64_t>(fps) - 2000;  // a little early, so 60 Hz / 20 fps = every 3rd frame
            DownCB dc{};
            std::memcpy(dc.src, st.src, sizeof(dc.src));
            dc.texel[0] = 1.0f / src_w, dc.texel[1] = 1.0f / src_h;
            dc.texel[2] = std::max(1.0f, st.src[2] * src_w / st.w / 4.0f);  // tap spacing in source texels
            upload_cb(g, g.cb_down.Get(), dc);
            g.ctx->PSSetConstantBuffers(4, 1, g.cb_down.GetAddressOf());
            pass(g, st.down.rtv.Get(), st.w, st.h, g.ps_down.Get(), srcs[i], nullptr);
            g.ctx->CopyResource(st.staging[st.next].Get(), st.down.tex.Get());
            st.staged_frame[st.next] = frame_no;
            st.pending[st.next] = true;
            st.next = (st.next + 1) % proto::kPreviewBuffers;
        }
    }
};

// ---------------------------------------------------------------- commands in
struct CommandLink {
    himmel::engine::ShmBlock shm;
    proto::CommandBlock* blk = nullptr;
    uint64_t next_try_us = 0;
    bool warned = false;

    void poll(uint64_t t) {
        if (blk || t < next_try_us) return;
        next_try_us = t + 1000000;
        const auto r = shm.open_or_create(proto::kCommandShmName, sizeof(proto::CommandBlock));
        if (r == himmel::engine::ShmBlock::Result::Failed) {
            if (!warned) LOG("commands: cannot map %ls (another version?), retrying every second", proto::kCommandShmName);
            warned = true;
            return;
        }
        auto* b = static_cast<proto::CommandBlock*>(shm.data());
        if (r == himmel::engine::ShmBlock::Result::Created) {
            std::memset(static_cast<void*>(b), 0, sizeof(*b));
            himmel::engine::write_header(b->header, proto::kCommandMagic, proto::kCommandVersion,
                                         static_cast<uint32_t>(sizeof(proto::CommandBlock)));
        } else if (!himmel::engine::header_ok(b->header, proto::kCommandMagic, proto::kCommandVersion,
                                              static_cast<uint32_t>(sizeof(proto::CommandBlock)))) {
            if (!warned) LOG("commands: %ls has another magic / version / size, retrying every second", proto::kCommandShmName);
            warned = true;
            shm.close();  // maybe the creator has not written the header yet
            return;
        }
        for (auto& ring : b->rings) ring.skip_all();  // never replay commands from before we started
        blk = b;
        LOG("commands: %ls %s, listening on producer slots 1..7", proto::kCommandShmName,
            r == himmel::engine::ShmBlock::Result::Created ? "created" : "opened");
    }
};

// The in-process Perform producer (slot 0): the mapper pushes here, the frame
// start drains it through the same validation as the shared rings.
struct LocalSink final : himmel::perform::CommandSink {
    proto::CommandRing* ring = nullptr;
    bool push(const proto::Command& c) override { return ring->push(c); }
};

// ---------------------------------------------------------------- state out
struct StateLink {
    himmel::engine::ShmBlock shm;
    proto::StateBlock* blk = nullptr;
    uint64_t last_write_us = 0;
};

// ---------------------------------------------------------------- MK2 in / out
struct Mk2Link {
    mk2::SharedMemory shm;
    bool open = false;
    uint64_t next_try_us = 0;
    bool was_alive = false;
    mk2::LedState leds{}, leds_last{};
    uint8_t screens[2][mk2::kScreenBytes] = {};
    uint8_t screens_last[2][mk2::kScreenBytes] = {};
    bool written = false;
    uint64_t next_screen_us = 0;
    uint64_t earliest_event_us = 0;

    void poll(uint64_t t) {
        if (open || t < next_try_us) return;
        next_try_us = t + 1000000;
        if (shm.open()) {  // the controller owns the block; we only attach ring 0
            open = true;
            shm.get()->rings[kEngineRing].attach();
            LOG("MK2 controller found, listening on ring %d", kEngineRing);
        }
    }
    bool alive(uint64_t t) const {
        return open && shm.get()->status.connected.load() &&
               t - std::min(t, shm.get()->status.heartbeat_us.load()) < 500000;
    }
};

// ---------------------------------------------------------------- clip layers
struct LayerRuntime {
    std::unique_ptr<himmel::engine::ClipPlayer> player;
    int requested = -1;                 // catalog id asked for
    himmel::engine::LoadedClip* shown = nullptr;
    bool current_is_requested = false;  // the player's current clip is the requested one
    int64_t uploaded = -1;
    uint32_t jump_seen = 0;
    // M2: jump -> screen measurement (a jump is "on time" when its frame is uploaded the same frame).
    bool jump_wait = false;
    int64_t jump_target = -1;
    uint64_t jump_frame_no = 0;
    // M2: warm cue frames sent to the player, and the FADE transition's kept picture.
    int64_t warm[himmel::engine::kWarmSlots] = {};
    himmel::engine::LoadedClip* warm_clip = nullptr;
    uint32_t trans_seen = 0;
    bool hold_valid = false;
    uint32_t fail_seen = 0;
    bool missing = false;
    uint64_t misses = 0;
};


// The layer constant buffer for a clip (or its placeholder colour when not ready).
void layer_cb(Gpu& g, const LayerRuntime& rt, const proto::LayerState& ls, const proto::Catalog& cat, float opacity,
              LayerCB& cb, ID3D11ShaderResourceView*& src);

void overlay_text(Gpu& g, Output& o, const char* line) {
    mk2::Canvas<kOverlayW, kOverlayH> c;
    c.text(2, 3, line);
    for (int y = 0; y < kOverlayH; ++y)
        for (int x = 0; x < kOverlayW; ++x) o.overlay_px[y * kOverlayW + x] = c.get(x, y) ? 255 : 0;
    g.ctx->UpdateSubresource(o.overlay.Get(), 0, nullptr, o.overlay_px.data(), kOverlayW, 0);
}

bool fx_active(const proto::FxSlotState& s) { return g_fx && g_fx->active(s); }

// Runs the active slots of an FX chain on a ping-pong pair (himmel::fx::FxGpu: one shader per
// effect, PRESS / scope / reaction / flash cap resolved per slot); returns the index holding the result.
int run_fx(Gpu& g, Rt pair[2], int cur, const proto::FxSlotState (&slots)[kFxSlots], int chain) {
    const himmel::fx::Target t[2] = {{pair[0].tex.Get(), pair[0].rtv.Get(), pair[0].srv.Get()},
                                     {pair[1].tex.Get(), pair[1].rtv.Get(), pair[1].srv.Get()}};
    return g_fx ? g_fx->run_chain(g.ctx.Get(), t, pair[0].w, pair[0].h, cur, slots, chain) : cur;
}

void rgb_of(uint32_t rgb, float out[4]) {
    out[0] = ((rgb >> 16) & 255) / 255.0f;
    out[1] = ((rgb >> 8) & 255) / 255.0f;
    out[2] = (rgb & 255) / 255.0f;
    out[3] = 1.0f;
}

void layer_cb(Gpu& g, const LayerRuntime& rt, const proto::LayerState& ls, const proto::Catalog& cat, float opacity,
              LayerCB& cb, ID3D11ShaderResourceView*& src) {
    cb = LayerCB{};
    cb.info[3] = opacity;
    cb.blend[0] = ls.blend;
    src = nullptr;
    const bool ready = rt.current_is_requested && rt.shown && rt.uploaded >= 0;
    if (ready) {
        const auto* clip = rt.shown;
        const float cw = static_cast<float>(clip->track.width), ch = static_cast<float>(clip->track.height);
        const float sc = std::max(g.canvas_w / cw, g.canvas_h / ch);  // cover-fit
        cb.xform[0] = g.canvas_w / (cw * sc);
        cb.xform[1] = g.canvas_h / (ch * sc);
        cb.info[0] = cw / clip->tex_w;
        cb.info[1] = ch / clip->tex_h;
        using HT = himmel::media::HapTexture;
        cb.info[2] = clip->texture == HT::DXT5 ? 1.0f : clip->texture == HT::YCoCgDXT5 ? 2.0f
                                             : clip->texture == HT::RGTC1 ? 3.0f : 0.0f;
        src = clip->srv;
    } else if (ls.clip >= 0) {  // loading or missing: the clip's placeholder colour (build-v1 §4.4)
        cb.info[2] = 4.0f;
        rgb_of(cat.clip[ls.clip].thumb_rgb, cb.color);
    } else {  // no clip, a generator in an FX slot: it paints on transparent (effect-metadata.md §6)
        cb.info[2] = 6.0f;
    }
}

}  // namespace

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    himmel::engine::LogRing logring("render");
    g_log = &logring;
    const Config cfg = parse_args(argc, argv);
    g_fullscreen = cfg.fullscreen;
    g_win.mirror = cfg.mirror;
    g_win.overlay = cfg.overlay;

    himmel::engine::EngineShm eshm;
    himmel::engine::EngineStatus* es = eshm.create() ? eshm.get() : nullptr;
    auto set_msg = [&](const char* m) {
        if (es) strncpy_s(es->message, sizeof(es->message), m, _TRUNCATE);
        LOG("%s", m);
    };

    // ---- catalog (start-up: reads every clip's header + first frame)
    auto catalog = std::make_unique<proto::Catalog>();
    std::memset(catalog.get(), 0, sizeof(proto::Catalog));
    const std::vector<std::string> playlist = cfg.clips.empty() ? himmel::engine::find_media_clips() : cfg.clips;
    himmel::engine::fill_clip_catalog(*catalog, playlist);
    himmel::engine::fx::fill_catalog(*catalog);
    LOG("catalog: %u clips%s, %u effects", catalog->clip_count,
        playlist.empty() ? " (put .mov files under media/ or pass --clip)" : "", catalog->effect_count);
    for (uint32_t i = 0; i < catalog->clip_count; ++i) {
        const auto& c = catalog->clip[i];
        LOG("  clip %2u  %-32s %4ux%-4u %5.2f fps %6.1f s  codec %u  placeholder #%06X", i, c.name, c.width, c.height,
            c.fps, c.seconds, c.codec, c.thumb_rgb);
    }
    auto model = std::make_unique<DeckModel>(*catalog);

    // ==== AUDIO (M8): Local\HimmelAudio, mapped once; himmel_audio.exe may come and go ====
    himmel::engine::AudioLink audio_link;
    himmel::engine::ReactionFeed reaction_feed;
    himmel::audio::Signals audio_sig{};
    himmel::audio::ClockSource audio_midi{};
    bool audio_was_ok = false, midi_was_leading = false;
    if (cfg.audio && !cfg.test_signal) {
        if (audio_link.open()) LOG("audio: reactions listen to %ls (AV offset %.0f ms)", himmel::audio::kBlockName, cfg.av_offset_ms);
        else LOG("audio: cannot map %ls - reactions stay still", himmel::audio::kBlockName);
    }
    // ==== END AUDIO (M8) ====

    // ---- state block: restore from a block that survived the last renderer
    StateLink state;
    {
        const auto r = state.shm.open_or_create(proto::kStateShmName, sizeof(proto::StateBlock));
        if (r == himmel::engine::ShmBlock::Result::Failed) {
            LOG("state: cannot map %ls - running without a state block", proto::kStateShmName);
        } else {
            state.blk = static_cast<proto::StateBlock*>(state.shm.data());
            uint32_t epoch = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFF) | 1u;
            if (r == himmel::engine::ShmBlock::Result::Opened &&
                himmel::engine::header_ok(state.blk->header, proto::kStateMagic, proto::kStateVersion,
                                          static_cast<uint32_t>(sizeof(proto::StateBlock)))) {
                auto old_live = std::make_unique<proto::LiveState>();
                auto old_cat = std::make_unique<proto::Catalog>();
                auto old_tr = std::make_unique<proto::TransportSection>();
                if (proto::seq_read(state.blk->live_seq, state.blk->live, *old_live) &&
                    proto::seq_read(state.blk->catalog_seq, state.blk->catalog, *old_cat)) {
                    const bool have_tr = proto::seq_read(state.blk->transport_seq, state.blk->transport, *old_tr) != 0;
                    const int lost = model->restore(*old_live, *old_cat, have_tr ? old_tr.get() : nullptr);
                    epoch = old_live->epoch + 1;
                    LOG("state: restored the show from the previous renderer (epoch %u)%s", old_live->epoch,
                        lost ? " - some clips are gone from the media folder" : "");
                    if (lost) LOG("state: %d layer(s) lost their clip (not found by name)", lost);
                }
            } else {
                std::memset(static_cast<void*>(state.blk), 0, sizeof(proto::StateBlock));
                himmel::engine::write_header(state.blk->header, proto::kStateMagic, proto::kStateVersion,
                                             static_cast<uint32_t>(sizeof(proto::StateBlock)));
                LOG("state: %ls created", proto::kStateShmName);
            }
            model->set_epoch(epoch);
            proto::seq_write(state.blk->catalog_seq, state.blk->catalog, *catalog);
            state.blk->status.pid.store(GetCurrentProcessId());
            LOG("state: epoch %u", epoch);
        }
    }

    // ==== SHOW (M3) hook 1/4: Local\HimmelShow + the runtime (its worker opens the last project) ====
    himmel::engine::ShmBlock show_shm;
    proto::ShowBlock* show_blk = nullptr;
    {
        const auto r = show_shm.open_or_create(proto::kShowShmName, sizeof(proto::ShowBlock));
        if (r == himmel::engine::ShmBlock::Result::Failed) {
            LOG("show: cannot map %ls - running without a show block", proto::kShowShmName);
        } else {
            show_blk = static_cast<proto::ShowBlock*>(show_shm.data());
            if (!himmel::engine::header_ok(show_blk->header, proto::kShowMagic, proto::kShowVersion,
                                           static_cast<uint32_t>(sizeof(proto::ShowBlock)))) {
                std::memset(static_cast<void*>(show_blk), 0, sizeof(proto::ShowBlock));
                himmel::engine::write_header(show_blk->header, proto::kShowMagic, proto::kShowVersion,
                                             static_cast<uint32_t>(sizeof(proto::ShowBlock)));
            }
        }
    }
    auto show = std::make_unique<himmel::show::ShowRuntime>();
    {
        himmel::show::RuntimeConfig sc;
        sc.disk = cfg.show_files;
        if (!cfg.project.empty()) sc.open = himmel::show::files::from_utf8(cfg.project);
        if (!cfg.show_root.empty()) {
            sc.projects_root = himmel::show::files::from_utf8(cfg.show_root) / "Projects";
            sc.app_dir = himmel::show::files::from_utf8(cfg.show_root) / "App";
        }
        show->start(sc, *catalog, show_blk);
        LOG("show: %s", cfg.show_files ? (cfg.project.empty() ? "opening the most recent project" : cfg.project.c_str())
                                       : "no files (--no-show-files)");
    }
    uint32_t show_msg_seq = 0;
    // ==== end SHOW hook 1/4 ====

    // ---- GPU + outputs
    Gpu g;
    if (!init_gpu(g)) return 2;
    LOG("GPU: %s", g.adapter);
    auto fxg = std::make_unique<himmel::fx::FxGpu>();
    {
        std::string report;
        const bool ok = fxg->init(g.dev.Get(), himmel::fx::builtin(), cfg.ticker.c_str(), &report);
        LOG("fx: %d effects, shaders compiled in %.0f ms%s", himmel::fx::builtin().count(), fxg->compile_ms(),
            ok ? "" : " - SOME FAILED (they pass the picture through)");
        if (!report.empty()) LOG("fx: %.200s", report.c_str());
        if (cfg.flash_cap > 0) LOG("fx: flash cap %.1f flashes/s (D65)", cfg.flash_cap);
        if (cfg.test_signal) LOG("fx: reactions get the test signal (not the audio block)");
    }
    g_fx = fxg.get();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HimmelOutput";
    RegisterClassW(&wc);

    const auto mons = monitors();
    const int n = cfg.outputs ? cfg.outputs : (cfg.fullscreen ? static_cast<int>(mons.size()) : 3);
    std::vector<Output> outs(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        RECT r;
        const bool fs = cfg.fullscreen && i < static_cast<int>(mons.size());
        if (fs) r = mons[static_cast<size_t>(i)];
        else {
            const int x0 = 40 + i * (cfg.win_w + 24) % 1800, y0 = 80 + (i * (cfg.win_w + 24) / 1800) * (cfg.win_h + 60);
            r = {x0, y0, x0 + cfg.win_w, y0 + cfg.win_h};
        }
        if (!create_output(g, outs[static_cast<size_t>(i)], i, r, fs)) return 2;
    }
    if (es) es->outputs.store(static_cast<uint32_t>(n));
    std::vector<HANDLE> waits;
    for (auto& o : outs) waits.push_back(o.waitable);

    PreviewWriter preview;
    preview.fps = cfg.preview_fps;
    if (!preview.open()) LOG("preview: cannot map %ls - no previews", proto::kPreviewShmName);
    auto configure_previews = [&]() {
        layout(g, outs, g_win.mirror);
        const float full[4] = {0, 0, 1, 1};
        const float aspect = static_cast<float>(g.canvas_w) / g.canvas_h;
        for (int i = 0; i < 3; ++i) preview.configure(g, i, true, aspect, full);
        for (int i = 0; i < 3; ++i) {
            const bool on = i < n;
            const float* src = on ? outs[static_cast<size_t>(i)].src : full;
            const float a = on ? static_cast<float>(outs[static_cast<size_t>(i)].w) / outs[static_cast<size_t>(i)].h : aspect;
            preview.configure(g, 3 + i, on, a, src);
        }
    };
    configure_previews();

    // ---- clip layers: one player (worker thread) per layer, all made now
    LayerRuntime layers[kLayerSlots];  // 0..7 = deck * 4 + layer, 8 = the HIT layer
    for (auto& l : layers) l.player = std::make_unique<himmel::engine::ClipPlayer>(g.dev.Get(), playlist);
    LOG("clip players: %d (one per layer + the HIT layer)", kLayerSlots);
    uint64_t jumps = 0, jumps_next_frame = 0, jump_worst = 0;

    // ---- commands + Perform
    CommandLink cmd;
    auto perform_ring = std::make_unique<proto::CommandRing>();
    perform_ring->head = 0, perform_ring->tail = 0, perform_ring->dropped = 0;
    LocalSink sink;
    sink.ring = perform_ring.get();
    auto perform = std::make_unique<himmel::perform::Mk2Perform>();
    // CONTROL M4: the mapper reads the show status (LOOKS pads, macros, undo) and the transport
    // (cue points, clock); both objects live as long as the loop.
    perform->set_context(&show->status(), &model->transport());
    Mk2Link mk2link;

    const uint64_t t0 = now_us();
    uint64_t last_frame = t0, stats_start = t0, frames = 0, stat_frames = 0, overlay_next = 0, alive_next = 0;
    double interval_sum = 0, interval_max = 0, cpu_sum = 0, cpu_max = 0, gpu_sum = 0, gpu_max = 0, gpu_prev_sum = 0,
           lat_sum = 0;
    uint32_t late = 0, late_period = 0, gpu_n = 0, lat_n = 0;
    double late_worst_ms = 0;
    uint64_t applied = 0, refused = 0;
    float fps = 0;
    int exit_code = 0;
    int q_index = 0;
    char overlay_line[160] = "";
    set_msg("running");

    while (!g_win.quit) {
        // 1. Wait until the swap chains can take a frame (max latency 1).
        WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), TRUE, 100);
        const uint64_t t = now_us();
        const double dt = (t - last_frame) / 1e6;
        last_frame = t;

        // 2. Window messages.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (es && es->stop_request.load()) g_win.quit = true;
        if (g_win.resized) {  // allowed to allocate: a window changed size (never during a fullscreen show)
            g_win.resized = false;
            for (auto& o : outs) resize_output(g, o);
            configure_previews();
            overlay_next = 0;
        }

        // 3. Input: MK2 events -> Perform -> slot 0; then every producer, at most 256 each.
        cmd.poll(t);
        mk2link.poll(t);
        const bool mk2_alive = mk2link.alive(t);
        if (mk2_alive != mk2link.was_alive) {
            if (mk2_alive) perform->reset(), mk2link.written = false;
            set_msg(mk2_alive ? "MK2 connected" : "MK2 offline");
            mk2link.was_alive = mk2_alive;
            overlay_next = 0;
        }
        mk2link.earliest_event_us = 0;
        if (mk2link.open) {
            auto& ring = mk2link.shm.get()->rings[kEngineRing];
            if (!ring.attached.load()) ring.attach();  // a restarted controller may have reset it
            mk2::Event e;
            for (int k = 0; k < 1024 && ring.pop(e); ++k) {
                if (!mk2link.earliest_event_us) mk2link.earliest_event_us = e.t_us;
                perform->on_event(e, model->live(), *catalog, sink);
            }
        }
        auto drain = [&](int slot, proto::CommandRing& ring) {
            proto::Command c;
            for (int k = 0; k < kDrainPerProducer && ring.pop(c); ++k) {
                // ==== SHOW (M3) hook 2/4: show commands (types 25..39) go to the show runtime ====
                const proto::Reject r = proto::is_show_command(c.type) ? show->submit(slot, c, t, *model)
                                                                        : model->submit(slot, c, t);
                if (r == proto::Reject::None) {
                    ++applied;
                } else {
                    ++refused;
                    if (model->should_log_reject(slot, t))
                        LOG("producer %d: %s refused (%s) deck %u layer %u slot %u param %u i %d f %.3f", slot,
                            himmel::engine::command_name(c.type), himmel::engine::reject_name(r), c.deck, c.layer, c.slot,
                            c.param, c.i, c.f[0]);
                }
            }
        };
        drain(static_cast<int>(proto::Producer::Perform), *perform_ring);
        if (cmd.blk)
            for (int slot = 1; slot < proto::kProducers; ++slot) drain(slot, cmd.blk->rings[slot]);
        // ==== AUDIO (M8): copy the audio block; an external MIDI clock leads the show clock ====
        const bool audio_ok = cfg.audio && !cfg.test_signal && audio_link.read(t, audio_sig, audio_midi);
        {
            const himmel::engine::ClockLead lead =
                cfg.audio && audio_link.alive() ? himmel::engine::midi_lead(audio_midi, t) : himmel::engine::ClockLead{};
            model->follow_clock(lead.active, lead.bpm, lead.beat, lead.phase, t, dt);
            if (lead.active != midi_was_leading) {
                midi_was_leading = lead.active;
                if (lead.active) LOG("clock: MIDI clock leads (%.1f BPM%s)", lead.bpm, lead.phase ? ", phase locked" : "");
                else LOG("clock: MIDI clock gone - internal / tap clock keeps %.1f BPM", model->bpm());
            }
        }
        if (audio_ok != audio_was_ok) {
            audio_was_ok = audio_ok;
            if (audio_ok) LOG("audio: hearing \"%s\" (%u Hz)", audio_link.input().device, audio_link.input().sample_rate);
            else LOG("audio: no audio - reactions glide to rest");
        }
        // ==== END AUDIO (M8) ====
        model->tick(t);
        // ==== SHOW (M3) hook 3/4: once per frame, after the model ====
        show->tick(*model, t);
        if (show->status().message_seq != show_msg_seq) {
            show_msg_seq = show->status().message_seq;
            LOG("show: %s (last recall %u us, %d commands)", show->status().message, show->last_recall_us(),
                show->last_recall_commands());
        }
        // ==== end SHOW hook 3/4 ====
        if (t >= alive_next) {
            alive_next = t + 250000;
            model->set_producer_alive(0, mk2_alive);
            for (int slot = 1; slot < proto::kProducers; ++slot) {
                bool alive = false;
                if (cmd.blk) {
                    const uint64_t hb = cmd.blk->producers[slot].heartbeat_us.load();
                    alive = cmd.blk->producers[slot].pid.load() != 0 && hb && t - std::min(t, hb) < 2000000;
                }
                model->set_producer_alive(slot, alive);
            }
        }
        const proto::LiveState& live = model->live();

        // 4. Clip players: the model's playheads (DeckModel::tick, above) say which frame each
        //    layer shows; follow clip changes, keep cue frames warm, upload ready frames.
        //    Nothing here waits: a frame that is not decoded yet keeps the last one on screen.
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        {
            ID3D11SamplerState* samplers[3] = {g.lin.Get(), g.pnt.Get(), g.wrap.Get()};
            g.ctx->PSSetSamplers(0, 3, samplers);
        }
        for (int s = 0; s < kLayerSlots; ++s) {
            LayerRuntime& rt = layers[s];
            const proto::LayerState& ls = model->layer(s);
            // A FADE transition started: keep the old picture before the new clip replaces it.
            if (model->transition_gen(s) != rt.trans_seen) {
                rt.trans_seen = model->transition_gen(s);
                const float clear[4] = {0, 0, 0, 0};
                g.ctx->ClearRenderTargetView(g.hold[s].rtv.Get(), clear);
                LayerCB cb;
                ID3D11ShaderResourceView* src = nullptr;
                proto::LayerState old = ls;
                old.clip = static_cast<int16_t>(rt.requested);
                if (rt.requested >= 0) {
                    layer_cb(g, rt, old, *catalog, 1.0f, cb, src);
                    upload_cb(g, g.cb_layer.Get(), cb);
                    g.ctx->PSSetConstantBuffers(0, 1, g.cb_layer.GetAddressOf());
                    pass(g, g.hold[s].rtv.Get(), g.canvas_w, g.canvas_h, g.ps_source.Get(), nullptr, src);
                }
                rt.hold_valid = true;
            }
            if (ls.clip != rt.requested) {
                rt.requested = ls.clip;
                rt.current_is_requested = false;
                rt.uploaded = -1;
                rt.missing = false;
                rt.jump_wait = false;
                if (ls.clip >= 0) {
                    rt.fail_seen = rt.player->load_failed.load();
                    rt.player->load(ls.clip);
                }
            }
            himmel::engine::LoadedClip* clip = rt.player->current();
            if (clip != rt.shown) {
                rt.shown = clip;
                rt.uploaded = -1;
                rt.current_is_requested =
                    clip && rt.requested >= 0 && clip->path == playlist[static_cast<size_t>(rt.requested)];
            }
            if (rt.requested >= 0 && !rt.current_is_requested && rt.player->load_failed.load() != rt.fail_seen)
                rt.missing = true;
            model->set_missing_slot(s, rt.requested >= 0 && rt.missing);
            if (rt.requested < 0 || !rt.current_is_requested || !clip) {
                rt.jump_seen = model->jump_gen_slot(s);
                continue;
            }
            const int64_t nframes = static_cast<int64_t>(clip->track.frames());
            const int64_t mf = model->frame(s);
            if (mf < 0) continue;
            const int64_t f = std::min(mf, nframes - 1);
            rt.player->request(f, model->direction(s));
            // Warm frames: the jump targets (cue points / slices), sent only when they change.
            {
                int64_t w[himmel::engine::kWarmSlots];
                model->warm_frames(s, w);
                if (clip != rt.warm_clip || std::memcmp(w, rt.warm, sizeof(w)) != 0) {
                    std::memcpy(rt.warm, w, sizeof(w));
                    rt.warm_clip = clip;
                    rt.player->set_warm(rt.warm);
                }
            }
            if (model->jump_gen_slot(s) != rt.jump_seen) {  // a jump on a clip that is showing
                rt.jump_seen = model->jump_gen_slot(s);
                rt.jump_wait = true;
                rt.jump_target = f;
                rt.jump_frame_no = frames;
            }
            if (f != rt.uploaded) {
                if (const uint8_t* data = rt.player->acquire(f)) {
                    g.ctx->UpdateSubresource(clip->tex, 0, nullptr, data, static_cast<UINT>(clip->row_pitch), 0);
                    rt.player->release();
                    rt.uploaded = f;
                } else if (rt.uploaded >= 0) {
                    ++rt.misses;  // the last frame stays on screen
                }
            }
            if (rt.jump_wait && (rt.uploaded == f || frames - rt.jump_frame_no > 120)) {
                // 0 = the jump's frame was uploaded in the frame the command was applied, so it is in
                // the very next present ("visible on the next frame").
                const uint64_t late_by = frames - rt.jump_frame_no;
                rt.jump_wait = false;
                ++jumps;
                if (late_by == 0) ++jumps_next_frame;
                jump_worst = std::max(jump_worst, late_by);
                if (late_by) LOG("JUMP layer %d -> frame %lld: on screen %llu frame(s) late", s, static_cast<long long>(rt.jump_target),
                                 static_cast<unsigned long long>(late_by));
            }
        }

        // 5. MK2 outputs from the mapper (LEDs on change, screens on change at <= 30 Hz).
        if (mk2_alive) {
            perform->tick(t, live, *catalog, sink, mk2link.leds, mk2link.screens);
            auto* b = mk2link.shm.get();
            if (!mk2link.written || std::memcmp(&mk2link.leds, &mk2link.leds_last, sizeof(mk2::LedState)) != 0) {
                mk2::seq_write(b->outputs.led_seq, b->outputs.leds, mk2link.leds);
                mk2link.leds_last = mk2link.leds;
            }
            if (t >= mk2link.next_screen_us) {
                for (int s = 0; s < 2; ++s)
                    if (!mk2link.written || std::memcmp(mk2link.screens[s], mk2link.screens_last[s], mk2::kScreenBytes) != 0) {
                        mk2::seq_write_bytes(b->outputs.screen_seq[s], b->outputs.screen[s], mk2link.screens[s], mk2::kScreenBytes);
                        std::memcpy(mk2link.screens_last[s], mk2link.screens[s], mk2::kScreenBytes);
                        mk2link.next_screen_us = t + 33000;
                    }
            }
            mk2link.written = true;
        }

        // 6. Render.
        const int qi = q_index;
        g.ctx->Begin(g.q_disjoint[qi].Get());
        g.ctx->End(g.q_begin[qi].Get());
        {
            // The show clock (tap / BPM, M2; MIDI clock leads when it runs, M8).
            himmel::fx::FrameCtx fc;
            fc.time_s = (t - t0) / 1e6;
            fc.beats = std::max(0.0, model->beat(t));
            fc.bpm = static_cast<float>(model->bpm());
            fc.dt = static_cast<float>(dt);
            fc.flash_cap_hz = cfg.flash_cap;
            // ==== AUDIO (M8): the reaction signals ====
            if (cfg.test_signal) fc.sig = himmel::fx::TestSignal::at(fc.time_s, fc.beats);
            else reaction_feed.update(audio_ok ? &audio_sig : nullptr,
                                      model->beat(t + static_cast<int64_t>(cfg.av_offset_ms * 1000.0f)), fc.dt, fc.sig);
            // ==== END AUDIO (M8) ====
            g_fx->begin_frame(fc);
        }
        const float black[4] = {0, 0, 0, 1};
        int deck_out[kDecks] = {0, 0};
        for (int d = 0; d < kDecks; ++d) {
            int cur = 0;
            g.ctx->ClearRenderTargetView(g.deck[d][0].rtv.Get(), black);
            // The 4 layers, then the HIT layer on top of the PROGRAM deck (clips-and-playback.md §3.3).
            for (int l = 0; l <= kLayersPerDeck; ++l) {
                const int s = l < kLayersPerDeck ? himmel::engine::slot_of(d, l) : kHitSlot;
                if (s == kHitSlot && d != live.program_deck) continue;
                const proto::LayerState& ls = model->layer(s);
                const proto::LayerTransport& tr = model->layer_transport(s);
                const LayerRuntime& rt = layers[s];
                const float opacity = ls.opacity * tr.gain;  // fades / black-dip (M2)
                // nobody sees it: skip (an empty layer with a generator in an FX slot is drawn)
                if ((ls.clip < 0 && !g_fx->has_generator(ls.fx)) || ls.mute || opacity <= 0.001f) continue;
                LayerCB cb;
                ID3D11ShaderResourceView* src = nullptr;
                layer_cb(g, rt, ls, *catalog, opacity, cb, src);
                const bool ready = cb.info[2] < 3.5f;
                bool any_fx = false;
                for (const auto& f : ls.fx) any_fx |= fx_active(f);
                const bool fading = tr.transition_progress < 1.0f && rt.hold_valid;
                if (any_fx || fading) {
                    // The layer's source into its own target (straight alpha), the FADE from the kept
                    // picture, FX on it, then composite it.
                    int lk = 0;
                    upload_cb(g, g.cb_layer.Get(), cb);
                    g.ctx->PSSetConstantBuffers(0, 1, g.cb_layer.GetAddressOf());
                    pass(g, g.layer[0].rtv.Get(), g.canvas_w, g.canvas_h, g.ps_source.Get(), nullptr, src);
                    if (fading) {
                        MixCB tc{};
                        tc.info[0] = ready ? tr.transition_progress : 0.0f;  // the new clip still loading: the old picture
                        upload_cb(g, g.cb_mix.Get(), tc);
                        g.ctx->PSSetConstantBuffers(2, 1, g.cb_mix.GetAddressOf());
                        pass(g, g.layer[1].rtv.Get(), g.canvas_w, g.canvas_h, g.ps_trans.Get(), g.hold[s].srv.Get(),
                             g.layer[0].srv.Get());
                        lk = 1;
                    }
                    lk = run_fx(g, g.layer, lk, ls.fx, s == kHitSlot ? himmel::fx::kChainHit : himmel::fx::chain_layer(d, l));
                    cb.info[2] = 5.0f;
                    src = g.layer[lk].srv.Get();
                }
                upload_cb(g, g.cb_layer.Get(), cb);
                g.ctx->PSSetConstantBuffers(0, 1, g.cb_layer.GetAddressOf());
                pass(g, g.deck[d][1 - cur].rtv.Get(), g.canvas_w, g.canvas_h, g.ps_layer.Get(), g.deck[d][cur].srv.Get(), src);
                cur = 1 - cur;
            }
            deck_out[d] = run_fx(g, g.deck[d], cur, live.deck[d].fx, himmel::fx::chain_deck(d));
        }
        MixCB mc{};
        mc.info[0] = live.crossfader;
        upload_cb(g, g.cb_mix.Get(), mc);
        g.ctx->PSSetConstantBuffers(2, 1, g.cb_mix.GetAddressOf());
        pass(g, g.main[0].rtv.Get(), g.canvas_w, g.canvas_h, g.ps_mix.Get(), g.deck[0][deck_out[0]].srv.Get(),
             g.deck[1][deck_out[1]].srv.Get());
        const int main_k = run_fx(g, g.main, 0, live.master_fx, himmel::fx::kChainMaster);
        ID3D11ShaderResourceView* main_srv = g.main[main_k].srv.Get();

        // 7. Outputs.
        const bool overlay_due = g_win.overlay && t >= overlay_next;
        if (overlay_due) {
            overlay_next = t + 500000;
            std::snprintf(overlay_line, sizeof(overlay_line),
                          "HIMMEL %.1f FPS  PGM %c  XF %.2f%s%s  FOCUS %c%d  MK2 %s  GUI %s  TOOL %s  LATE %u", fps,
                          live.program_deck ? 'B' : 'A', live.crossfader, live.take_active ? "  TAKE" : "",
                          live.black ? "  BLACK" : "", live.focus_deck < kDecks ? 'A' + live.focus_deck : '-',
                          live.focus_layer < kLayersPerDeck ? live.focus_layer + 1 : 0, mk2_alive ? "OK" : "-",
                          live.producer_alive[1] ? "OK" : "-", live.producer_alive[3] ? "OK" : "-", late);
        }
        HRESULT present_hr = S_OK;
        for (size_t i = 0; i < outs.size(); ++i) {
            auto& o = outs[i];
            if (overlay_due) overlay_text(g, o, overlay_line);
            BlitCB bcb{};
            std::memcpy(bcb.src, o.src, sizeof(bcb.src));
            if (g_win.overlay) {
                bcb.overlay[0] = 8, bcb.overlay[1] = 8;
                bcb.overlay[2] = static_cast<float>(kOverlayW * kOverlayScale);
                bcb.overlay[3] = static_cast<float>(kOverlayH * kOverlayScale);
            }
            bcb.info[0] = live.black ? 0.0f : 1.0f;
            upload_cb(g, g.cb_blit.Get(), bcb);
            g.ctx->PSSetConstantBuffers(3, 1, g.cb_blit.GetAddressOf());
            pass(g, o.rtv.Get(), o.w, o.h, g.ps_blit.Get(), main_srv, o.overlay_srv.Get());
        }
        // 8. Previews (after the outputs are drawn, before Present).
        g.ctx->End(g.q_preview[qi].Get());
        ID3D11ShaderResourceView* psrc[proto::kPreviewStreams] = {
            g.deck[0][deck_out[0]].srv.Get(), g.deck[1][deck_out[1]].srv.Get(), main_srv, main_srv, main_srv, main_srv};
        preview.frame(g, t, frames, psrc, g.canvas_w, g.canvas_h);
        g.ctx->End(g.q_end[qi].Get());
        g.ctx->End(g.q_disjoint[qi].Get());
        g.q_pending[qi] = true;
        q_index = (q_index + 1) % Gpu::kQ;
        for (auto& o : outs) {
            const HRESULT hr = o.swap->Present(1, 0);
            if (FAILED(hr)) present_hr = hr;
        }
        ID3D11ShaderResourceView* none[2] = {};
        g.ctx->PSSetShaderResources(0, 2, none);
        if (present_hr == DXGI_ERROR_DEVICE_REMOVED || present_hr == DXGI_ERROR_DEVICE_RESET) {
            char m[96];
            std::snprintf(m, sizeof(m), "GPU device lost (0x%08lx): exiting so the supervisor restarts us",
                          static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            set_msg(m);
            exit_code = 3;
            break;
        }

        // GPU time of an older frame (never waits).
        const int qo = q_index;  // the oldest in the ring
        if (g.q_pending[qo]) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 tb = 0, tp = 0, te = 0;
            if (g.ctx->GetData(g.q_disjoint[qo].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                g.ctx->GetData(g.q_begin[qo].Get(), &tb, sizeof(tb), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                g.ctx->GetData(g.q_preview[qo].Get(), &tp, sizeof(tp), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                g.ctx->GetData(g.q_end[qo].Get(), &te, sizeof(te), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                g.q_pending[qo] = false;
                if (!dj.Disjoint && dj.Frequency) {
                    const double ms = (te - tb) * 1000.0 / dj.Frequency, pms = (te - tp) * 1000.0 / dj.Frequency;
                    gpu_sum += ms, gpu_prev_sum += pms, gpu_max = std::max(gpu_max, ms);
                    ++gpu_n;
                }
            }
        }

        // 9. State out (at most once per frame: on change, while clips play, and every 250 ms).
        const uint64_t t_done = now_us();
        ++frames;
        model->set_frame(frames, t_done, fps);
        if (state.blk) {
            bool playing = false;
            for (const auto& dk : live.deck)
                for (const auto& ly : dk.layer) playing |= ly.clip >= 0 && ly.playing;
            playing |= model->layer(kHitSlot).clip >= 0;
            if (model->take_structure_dirty() || playing || t_done - state.last_write_us >= 250000) {
                proto::seq_write(state.blk->live_seq, state.blk->live, live);
                proto::seq_write(state.blk->transport_seq, state.blk->transport, model->transport());
                state.last_write_us = t_done;
            }
            state.blk->status.heartbeat_us.store(t_done);
        }

        // 10. Stats + heartbeat.
        if (mk2link.earliest_event_us && mk2link.earliest_event_us <= t_done) {
            lat_sum += static_cast<double>(t_done - mk2link.earliest_event_us);
            ++lat_n;
        }
        ++stat_frames;
        interval_sum += dt;
        interval_max = std::max(interval_max, dt);
        const double cpu_ms = (t_done - t) / 1000.0;
        cpu_sum += cpu_ms, cpu_max = std::max(cpu_max, cpu_ms);
        if (dt > 1.5 / 60.0 && frames > 2) {
            ++late, ++late_period;
            late_worst_ms = std::max(late_worst_ms, dt * 1000.0);
        }
        if (es) {
            es->heartbeat_us.store(t_done);
            es->frames.store(frames);
        }
        if (t_done - stats_start >= 5000000) {
            const double period = (t_done - stats_start) / 1e6;
            fps = static_cast<float>(stat_frames / period);
            const double avg_ms = interval_sum / stat_frames * 1000.0;
            uint64_t misses = 0, warm = 0;
            for (auto& ly : layers) misses += ly.misses, warm += ly.player->stats().warm_decoded.load();
            // One CSV-friendly line per 5 s (docs/measurements/ is made from these).
            LOG("STATS fps %.2f  interval avg %.2f max %.2f ms  cpu avg %.2f max %.2f ms  gpu avg %.2f max %.2f ms  "
                "preview gpu %.3f ms  late %u (total %u)  cmds %llu ok %llu refused  previews %llu (busy %llu)  "
                "clip misses %llu  input->present %.2f ms  log drops %u  late worst %.1f ms  "
                "jumps %llu next-frame %llu worst %llu  warm decodes %llu  bpm %.1f",
                fps, avg_ms, interval_max * 1000.0, cpu_sum / stat_frames, cpu_max, gpu_n ? gpu_sum / gpu_n : 0.0,
                gpu_max, gpu_n ? gpu_prev_sum / gpu_n : 0.0, late_period, late, static_cast<unsigned long long>(applied),
                static_cast<unsigned long long>(refused), static_cast<unsigned long long>(preview.published),
                static_cast<unsigned long long>(preview.skipped_busy), static_cast<unsigned long long>(misses),
                lat_n ? lat_sum / lat_n / 1000.0 : 0.0, logring.dropped(), late_worst_ms,
                static_cast<unsigned long long>(jumps), static_cast<unsigned long long>(jumps_next_frame),
                static_cast<unsigned long long>(jump_worst), static_cast<unsigned long long>(warm), model->bpm());
            if (es) {
                es->fps_x100.store(static_cast<uint32_t>(fps * 100));
                es->frame_ms_avg_x100.store(static_cast<uint32_t>(avg_ms * 100));
                es->frame_ms_max_x100.store(static_cast<uint32_t>(interval_max * 100000));
                es->late_frames.store(late);
                es->mk2_connected.store(mk2_alive ? 1 : 0);
                es->input_to_present_us.store(static_cast<uint32_t>(lat_n ? lat_sum / lat_n : 0));
            }
            stats_start = t_done;
            stat_frames = 0;
            late_period = 0;
            interval_sum = interval_max = cpu_sum = cpu_max = gpu_sum = gpu_max = gpu_prev_sum = lat_sum = 0;
            late_worst_ms = 0;
            gpu_n = lat_n = 0;
        }
    }

    if (mk2link.open) {
        mk2::LedState off{};
        mk2::seq_write(mk2link.shm.get()->outputs.led_seq, mk2link.shm.get()->outputs.leds, off);
        mk2link.shm.get()->rings[kEngineRing].detach();
    }
    if (preview.blk)
        for (auto& st : preview.blk->stream) st.active.store(0);
    show->stop();  // ==== SHOW (M3) hook 4/4: flushes the autosave journal ====
    set_msg("stopped");
    timeEndPeriod(1);
    return exit_code;
}
