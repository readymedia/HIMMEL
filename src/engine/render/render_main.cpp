// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_render.exe - the slice engine (docs/roadmap.md §0a, task S1).
//
//   himmel_render [--outputs N] [--fullscreen] [--size 640x360] [--mode span|mirror]
//                 [--resolution 720p|1080p|1440p|4k|custom] [--refresh 60|50|59.94|30|25]
//                 [--blend PX] [--blend-gamma G] [--spout NAME]   (outputs: M6, docs/interfaces/outputs-block.md)
//                 [--clip file.mov ...] [--no-overlay] [--preview-fps N] [--stats-csv]
//                 [--flash-cap HZ] [--test-signal] [--ticker "TEXT"]   (effects: src/engine/fx/, M5)
//                 [--project FOLDER|FILE.himmelshow] [--no-show-files] [--show-root DIR]   (show: src/show/, M3)
//                 [--av-offset MS] [--no-audio]   (music: Local\HimmelAudio from himmel_audio.exe, M8)
//                 [--library DIR] [--no-library]   (library clips join the catalog live, M7)
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
// * Outputs (M6): 1..8 output slots onto one canvas (span with an optional edge blend, or
//   mirror), resolution presets, black per output, IDENTIFY, layer places. Full-screen
//   outputs belong to a display by identity (DisplayWatch, a worker): a display that goes
//   away hides its window (the others never notice: no swap-chain rebuild, it just leaves
//   the wait set), and it comes back to its slot. Status out: Local\HimmelOutputs (the
//   supervisor puts its black curtains behind our windows from it).
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
#include <dwmapi.h>
#include <dxgi1_5.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "himmel/engine/clip_catalog.h"
#include "himmel/engine/clip_directory.h"
#include "himmel/engine/clip_watch.h"
#include "himmel/protocol/clip_pages.h"
#include "himmel/engine/clip_player.h"
#include "himmel/engine/deck_model.h"
#include "himmel/engine/display_watch.h"
#include "himmel/engine/output_model.h"
#include "himmel/engine/spout_out.h"
#include "himmel/engine/reactions.h"
#include "himmel/engine/engine_shared.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/log_ring.h"
#include "himmel/engine/shm_block.h"
#include "himmel/fx/fx_gpu.h"
#include "himmel/fx/signals.h"
#include "himmel/library/clip_table.h"
#include "himmel/library/feed.h"
#include "himmel/mk2/output.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"
#include "himmel/perform/lightpad_perform.h"
#include "himmel/perform/mk2_perform.h"
#include "himmel/perform/nuvj_perform.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/outputs.h"
#include "himmel/protocol/preview.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"
#include "himmel/show/files.h"
#include "himmel/show/runtime.h"
#include "himmel/weapon/shared.h"
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

constexpr int kMaxOutputs = proto::kMaxOutputs;
constexpr int kOverlayW = 512, kOverlayH = 12, kOverlayScale = 2;
constexpr int kIdentW = 192, kIdentH = 80;  // IDENTIFY picture (1-bit, drawn big with point sampling)
constexpr int kEngineRing = 0;          // ring 0 of the MK2 block belongs to the engine
constexpr int kDrainPerProducer = 256;  // command-protocol.md §5.7

himmel::engine::LogRing* g_log = nullptr;
himmel::fx::FxGpu* g_fx = nullptr;  // effects + generators (made at start-up)
himmel::engine::OutputModel* g_om = nullptr;  // canvas layout + layer places (M6)
const himmel::engine::ClipDirectory* g_clipdir = nullptr;  // catalog v3 (wave 7): every clip id
#define LOG(...) g_log->logf(__VA_ARGS__)

// ==== ENGINE (M6): Spout worker log (LogRing has one writer, the render thread) ====
void spout_log(const char* line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::printf("%02d:%02d:%02d  render  %s\n", st.wHour, st.wMinute, st.wSecond, line);
    std::fflush(stdout);
}
// ==== end ENGINE (M6) ====

// ---------------------------------------------------------------- config
struct Config {
    int outputs = 0;  // 0 = one per monitor (windowed: 3)
    bool fullscreen = false;
    int win_w = 640, win_h = 360;
    bool mirror = false;
    bool overlay = true;
    int resolution = -1;            // M6: ResolutionPreset, -1 = default (fullscreen 1080p, windowed custom = --size)
    proto::RefreshRate refresh = proto::RefreshRate::Hz60;
    int blend_px = 0;               // M6: edge-blend overlap, canvas pixels
    float blend_gamma = 2.2f;
    std::string spout;              // M6: Spout sender name for MAIN ("" = Spout off)
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
    std::string library;            // M7: --library DIR (default Documents\Himmel\Library; DIR/Library with --show-root)
    bool use_library = true;        // M7: --no-library (also off with --no-show-files unless --library is given)
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
        else if (a == "--resolution") {
            const std::string r = next();
            c.resolution = r == "720p" ? 0 : r == "1080p" ? 1 : r == "1440p" ? 2 : (r == "4k" || r == "4K" || r == "2160p") ? 3
                         : r == "custom" ? 4 : -1;
        } else if (a == "--refresh") {
            const double hz = std::atof(next().c_str());
            c.refresh = hz > 59.5 && hz < 59.99 ? proto::RefreshRate::Hz59_94 : hz == 50 ? proto::RefreshRate::Hz50
                      : hz == 30 ? proto::RefreshRate::Hz30 : hz == 25 ? proto::RefreshRate::Hz25 : proto::RefreshRate::Hz60;
        } else if (a == "--blend") c.blend_px = std::clamp(std::atoi(next().c_str()), 0, 1024);
        else if (a == "--spout") c.spout = next();
        else if (a == "--blend-gamma") c.blend_gamma = std::clamp(static_cast<float>(std::atof(next().c_str())), 1.0f, 3.0f);
        else if (a == "--library") c.library = next();
        else if (a == "--no-library") c.use_library = false;
        else if (a == "--preview-fps") c.preview_fps = std::clamp(std::atoi(next().c_str()), 15, 30);
        else if (a == "--size") {
            const std::string s = next();
            int w = 0, h = 0;
            if (sscanf_s(s.c_str(), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) c.win_w = w, c.win_h = h;
        }
        // Legacy flags (camera, Entrancer, MIDI) are accepted and ignored: those parts are parked.
    }
    if (!overlay_set) c.overlay = !c.fullscreen;
    // Test runs never read the owner's library unless they name one (docs/interfaces/library-db.md §8).
    if (c.library.empty() && !c.show_files && c.show_root.empty()) c.use_library = false;
    return c;
}

// Window-proc flags (the message pump runs on the render thread).
struct WinFlags {
    bool quit = false, resized = false, mirror_toggle = false, overlay = true, display_change = false;
};
WinFlags g_win;
bool g_fullscreen = false;
// Full-screen output windows only move when we move them (M6): when a display goes
// away, Windows would move its window onto another display - over another output.
// WM_WINDOWPOSCHANGING refuses every move we did not make.
HWND g_out_hwnd[proto::kMaxOutputs] = {};
bool g_self_move = false;

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
    float info[4];   // x = gain (0 = black), y = IDENTIFY alpha
    float edge[4];   // M6 edge blend: left / right ramp width (part of the output), 1 / gamma
    float ident[4];  // M6 IDENTIFY picture rect in output pixels: x, y, w, h
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
    int w = 0, h = 0;                  // swap chain size (display or window client)
    ComPtr<IDXGISwapChain2> swap;
    HANDLE waitable = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> overlay;
    ComPtr<ID3D11ShaderResourceView> overlay_srv;
    std::array<uint8_t, kOverlayW * kOverlayH> overlay_px{};
    ComPtr<ID3D11Texture2D> ident;     // IDENTIFY picture (R8, kIdentW x kIdentH)
    ComPtr<ID3D11ShaderResourceView> ident_srv;
    std::array<uint8_t, kIdentW * kIdentH> ident_px{};
    float src[4] = {0, 0, 1, 1};
    // M6: where it is.
    proto::OutputState state = proto::OutputState::Window;
    int display = -1;                  // index into the current DisplayList (full screen)
    proto::DisplayId id{};             // the display this slot belongs to
    char name[proto::kDisplayNameLen] = "";
    float hz = 0;
    RECT rect{};                       // window rect on the desktop
    uint64_t lost_us = 0;
    uint32_t lost_count = 0, returned_ms = 0;
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
// The visible window rect (without the invisible resize borders), for the curtains.
void window_rect(HWND h, RECT& r) {
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: g_win.resized = true; return 0;
        case WM_MOVE: g_win.resized = true; return 0;  // the curtain follows a moved window
        case WM_CLOSE:
            if (!g_fullscreen) g_win.quit = true;  // a show output never closes by click
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && (!g_fullscreen || (GetKeyState(VK_SHIFT) & 0x8000))) g_win.quit = true;
            if (wp == 'M') g_win.mirror_toggle = true;
            if (wp == 'O') g_win.overlay = !g_win.overlay;
            return 0;
        case WM_DISPLAYCHANGE: g_win.display_change = true; break;
        case WM_WINDOWPOSCHANGING:
            if (g_fullscreen && !g_self_move) {
                for (HWND h : g_out_hwnd)
                    if (h == hwnd) {
                        reinterpret_cast<WINDOWPOS*>(lp)->flags |= SWP_NOMOVE | SWP_NOSIZE;
                        break;
                    }
            }
            break;
        case WM_SETCURSOR:
            if (g_fullscreen && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Start-up only. A full-screen output with no display yet is made hidden at `r`
// (its swap chain at that size) and shown when its display comes.
bool create_output(Gpu& g, Output& o, int index, const RECT& r, bool fullscreen, bool visible) {
    const DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT wr = r;
    if (!fullscreen) AdjustWindowRect(&wr, style, FALSE);
    wchar_t title[64];
    swprintf_s(title, L"Himmel output %d", index + 1);
    g_self_move = true;
    o.hwnd = CreateWindowExW(fullscreen ? (WS_EX_TOPMOST | WS_EX_NOACTIVATE) : 0, L"HimmelOutput", title, style, wr.left,
                             wr.top, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (o.hwnd) {
        // No DWM show / hide / close animations: a lost output vanishes at once, and a dying
        // renderer's windows do not fade over the curtains.
        const BOOL off = TRUE;
        DwmSetWindowAttribute(o.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    }
    if (o.hwnd && visible) ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
    g_self_move = false;
    if (!o.hwnd) return false;
    g_out_hwnd[index] = o.hwnd;
    RECT cr;
    GetClientRect(o.hwnd, &cr);
    o.w = std::max<LONG>(1, cr.right);
    o.h = std::max<LONG>(1, cr.bottom);
    window_rect(o.hwnd, o.rect);

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
    td.Width = kIdentW;
    td.Height = kIdentH;
    g.dev->CreateTexture2D(&td, nullptr, &o.ident);
    g.dev->CreateShaderResourceView(o.ident.Get(), nullptr, &o.ident_srv);
    LOG("output %d: %dx%d %s%s", index + 1, o.w, o.h, fullscreen ? "fullscreen" : "window",
        visible ? "" : " (hidden: no display yet)");
    return true;
}

// The swap chain follows its window's client size. Only this output's swap chain is touched.
void resize_output(Gpu& g, Output& o) {
    RECT cr;
    GetClientRect(o.hwnd, &cr);
    window_rect(o.hwnd, o.rect);
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

// IDENTIFY picture: the big number, then "OUT n  WxH  HZ" and the display's name.
// Made when the layout or the display changes (never per frame).
void draw_ident(Gpu& g, Output& o, int index, const himmel::engine::OutputRegion& r) {
    mk2::Canvas<kIdentW, kIdentH> c;
    char num[8], line[64];
    std::snprintf(num, sizeof(num), "%d", index + 1);
    const int nw = static_cast<int>(std::strlen(num)) * 6 * 8;
    c.text((kIdentW - nw) / 2 + 4, 2, num, true, 8);
    if (o.hz > 0) std::snprintf(line, sizeof(line), "OUT %d  %dx%d  %.0fHZ", index + 1, r.w, r.h, o.hz);
    else std::snprintf(line, sizeof(line), "OUT %d  %dx%d", index + 1, r.w, r.h);
    c.text(std::max(0, (kIdentW - static_cast<int>(std::strlen(line)) * 6) / 2), 61, line);
    char nm[proto::kDisplayNameLen];
    std::snprintf(nm, sizeof(nm), "%s", o.name[0] ? o.name : "WINDOW");
    for (char* q = nm; *q; ++q) *q = static_cast<char>(std::toupper(static_cast<unsigned char>(*q)));
    c.text(std::max(0, (kIdentW - static_cast<int>(std::strlen(nm)) * 6) / 2), 71, nm);
    for (int y = 0; y < kIdentH; ++y)
        for (int x = 0; x < kIdentW; ++x) o.ident_px[y * kIdentW + x] = c.get(x, y) ? 255 : 0;
    g.ctx->UpdateSubresource(o.ident.Get(), 0, nullptr, o.ident_px.data(), kIdentW, 0);
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
// Several mappers share slot 0 (MK2, Lightpad), so the sink numbers the commands itself:
// one running seq per producer slot (command-protocol.md §4).
struct LocalSink final : himmel::perform::CommandSink {
    proto::CommandRing* ring = nullptr;
    uint32_t seq = 0;
    bool push(const proto::Command& c) override {
        proto::Command n = c;
        n.seq = ++seq;
        return ring->push(n);
    }
};

// ---------------------------------------------------------------- state out
struct StateLink {
    himmel::engine::ShmBlock shm;
    proto::StateBlock* blk = nullptr;
    uint64_t last_write_us = 0;
};

// ==== AUDIO (D131): the reaction table for the state block, with each knob's live
// envelope and value from the FX resolver (0 for a slot not drawn this frame). ====
bool react_live_wanted(const proto::LiveState& live) {
    for (const auto& d : live.deck) {
        for (const auto& l : d.layer)
            for (const auto& f : l.fx)
                if (f.effect >= 0) return true;
        for (const auto& f : d.fx)
            if (f.effect >= 0) return true;
    }
    for (const auto& f : live.master_fx)
        if (f.effect >= 0) return true;
    return false;
}
void fill_react_row(proto::SlotReactions& out, const proto::SlotReactions& in, const himmel::fx::FxGpu& fx, int chain,
                    int slot) {
    out = in;
    const himmel::fx::SlotRuntime& rt = fx.runtime(chain, slot);
    const bool now = rt.frame != 0 && rt.frame == fx.frame_count();
    for (int p = 0; p < proto::kReactParams; ++p) {
        out.p[p].env = now ? rt.react_env[p] : 0.0f;
        out.p[p].value = now ? rt.out[p] : 0.0f;
    }
}
void fill_react_out(proto::ReactionSection& out, const proto::ReactionSection& in, const himmel::fx::FxGpu& fx) {
    for (int d = 0; d < proto::kDecks; ++d) {
        for (int l = 0; l < proto::kLayersPerDeck; ++l)
            for (int k = 0; k < proto::kFxSlots; ++k)
                fill_react_row(out.layer[d][l][k], in.layer[d][l][k], fx, himmel::fx::chain_layer(d, l), k);
        for (int k = 0; k < proto::kFxSlots; ++k)
            fill_react_row(out.deck[d][k], in.deck[d][k], fx, himmel::fx::chain_deck(d), k);
    }
    for (int k = 0; k < proto::kFxSlots; ++k) fill_react_row(out.master[k], in.master[k], fx, himmel::fx::kChainMaster, k);
    out.gen = in.gen;
}
// ==== END AUDIO (D131) ====

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

// ---------------------------------------------------------------- Lightpad in / out (CONTROL M9)
// The first weapon slot (Local\HimmelWeapon<N>, weapon_blocks.exe) whose driver says "lightpad":
// ring 0 -> himmel::perform::LightpadPerform; its 15 x 15 picture goes back as the slot's pixels.
struct LightpadLink {
    const char* weapon = "lightpad";  // CONTROL M10: the same link finds the NuVJ ("nuvj")
    himmel::weapon::SharedMemory shm;
    int slot = 0;                // 1..16 once found
    uint64_t next_try_us = 0;
    bool was_alive = false;
    uint8_t px[himmel::weapon::kMaxPixels][3] = {};
    uint8_t px_last[himmel::weapon::kMaxPixels][3] = {};
    bool written = false;
    uint64_t next_out_us = 0;

    void poll(uint64_t t) {
        if (slot || t < next_try_us) return;
        next_try_us = t + 1000000;
        for (int i = 1; i <= himmel::weapon::kMaxSlots; ++i) {
            if (!shm.open(i)) continue;
            if (std::strncmp(shm.get()->header.weapon, weapon, sizeof(shm.get()->header.weapon)) == 0) {
                slot = i;
                shm.get()->rings[himmel::weapon::kEngineRing].attach();
                LOG("%s found in weapon slot %d (%s%s), listening on ring %d", weapon, i, shm.get()->header.driver,
                    shm.get()->header.simulated ? ", simulated" : "", himmel::weapon::kEngineRing);
                return;
            }
            shm.close();
        }
    }
    bool alive(uint64_t t) const {
        return slot && shm.get()->status.connected.load() &&
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
    // Wave 7: the clip's two textures (frame blend) - which frame each holds, and which one is on screen.
    int64_t up[2] = {-1, -1};
    int main_tex = -1;
    int64_t blend_next = -1;   // the frame in the other texture that is mixed in (-1 = none)
    float blend_w = 0.0f;
    // Wave 7 cadence measurement: refreshes each clip frame stayed on screen.
    int64_t cad_frame = -1;
    uint32_t cad_hold = 0;
    double cad_sum = 0.0;      // sum of holds since the run started
    uint32_t cad_frames = 0;   // frames in the run
    double cad_ideal = 0.0;    // refreshes per frame the run is measured against
    double cad_err_sum = 0.0;  // stats period: sum of |cumulative error| at each frame change
    uint32_t cad_err_n = 0;
    uint32_t cad_irregular = 0;
    uint32_t cad_changes = 0;
    float judder = 0.0f;       // last stats period
    uint32_t irregular = 0;
};


// The layer constant buffer for a clip (or its placeholder colour when not ready).
void layer_cb(Gpu& g, int slot, const LayerRuntime& rt, const proto::LayerState& ls, const proto::Catalog& cat,
              float opacity, LayerCB& cb, ID3D11ShaderResourceView*& src);

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

void layer_cb(Gpu& g, int slot, const LayerRuntime& rt, const proto::LayerState& ls, const proto::Catalog& cat,
              float opacity, LayerCB& cb, ID3D11ShaderResourceView*& src) {
    cb = LayerCB{};
    cb.info[3] = opacity;
    cb.blend[0] = ls.blend;
    src = nullptr;
    // Wave 7 pre-roll: while a new clip loads, the old clip's last frame stays (no placeholder flash).
    (void)cat;
    const bool ready = rt.shown && rt.uploaded >= 0 && rt.main_tex >= 0 &&
                       (rt.current_is_requested || (ls.clip >= 0 && !rt.missing));
    if (ready) {
        const auto* clip = rt.shown;
        const float cw = static_cast<float>(clip->track.width), ch = static_cast<float>(clip->track.height);
        g_om->layer_xform(slot, cw, ch, cb.xform);  // the layer's place (M6): FILL ALL / OUT n / FIT / FREE
        cb.info[0] = cw / clip->tex_w;
        cb.info[1] = ch / clip->tex_h;
        using HT = himmel::media::HapTexture;
        cb.info[2] = clip->texture == HT::DXT5 ? 1.0f : clip->texture == HT::YCoCgDXT5 ? 2.0f
                                             : clip->texture == HT::RGTC1 ? 3.0f : 0.0f;
        src = clip->view(rt.main_tex);
        if (rt.current_is_requested && rt.blend_w > 0.0f && rt.blend_next >= 0) {
            const int other = 1 - rt.main_tex;
            if (rt.up[other] == rt.blend_next) {
                cb.blend[1] = rt.blend_w;
                ID3D11ShaderResourceView* next = clip->view(other);
                g.ctx->PSSetShaderResources(4, 1, &next);
            }
        }
    } else if (ls.clip >= 0) {  // loading or missing: the clip's placeholder colour (build-v1 §4.4), in its place
        cb.info[2] = 4.0f;
        const proto::ClipInfo& ci = g_clipdir->info(ls.clip);  // catalog v3: any id
        g_om->layer_xform(slot, static_cast<float>(ci.width), static_cast<float>(ci.height), cb.xform);
        rgb_of(ci.thumb_rgb, cb.color);
    } else {  // no clip, a generator in an FX slot: it paints on transparent (effect-metadata.md §6)
        cb.info[2] = 6.0f;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t t_launch = now_us();  // M6: start-up time (a restart must be back in < 2 s)
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    himmel::engine::LogRing logring("render");
    g_log = &logring;
    const Config cfg = parse_args(argc, argv);
    g_fullscreen = cfg.fullscreen;
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
    // ==== MEDIA (M7) hook 1/3: one append-only clip table (playlist + catalog entries) for every
    // player; the library feed adds READY library clips to it, now and while the show runs ====
    auto clip_table = std::make_unique<himmel::library::ClipTable>();
    for (size_t i = 0; i < playlist.size() && i < static_cast<size_t>(himmel::library::ClipTable::kCapacity); ++i) {
        if (i < catalog->clip_count) {
            clip_table->append(playlist[i], catalog->clip[i]);
        } else {  // wave 7: media/ folders beyond the 256-clip window
            proto::ClipInfo ci;
            himmel::engine::clip_info_for(playlist[i], ci);
            clip_table->append(playlist[i], ci);
        }
    }
    himmel::library::LibraryFeed library_feed;
    if (cfg.use_library) {
        const auto root = !cfg.library.empty() ? himmel::show::files::from_utf8(cfg.library)
                        : !cfg.show_root.empty() ? himmel::show::files::from_utf8(cfg.show_root) / "Library"
                                                 : himmel::library::default_library_root();
        const int lib = library_feed.load_initial(root, *clip_table);
        library_feed.start();
        const auto root_u8 = root.u8string();
        LOG("library: %s, %d ready clip(s) added; new ones join while running", reinterpret_cast<const char*>(root_u8.c_str()), lib);
    } else {
        LOG("library: off");
    }
    // ==== end MEDIA hook 1/3 ====
    // ==== ENGINE wave 7: catalog v3 - the clip directory (every id, library id, state) + the file watch.
    //      Clip id = clip table index; the directory follows the table, the catalog section is its window.
    auto clipdir = std::make_unique<himmel::engine::ClipDirectory>();
    g_clipdir = clipdir.get();
    for (uint32_t i = 0; i < clip_table->size(); ++i) clipdir->add(clip_table->info[i], clip_table->library_id[i]);
    auto clip_watch = std::make_unique<himmel::engine::ClipWatch>(clip_table.get());
    clip_watch->start();
    uint32_t clip_watch_seen = 0;
    clipdir->mirror_window(*catalog);
    // ==== end ENGINE wave 7 ====
    himmel::engine::fx::fill_catalog(*catalog);
    LOG("catalog: %u clips (%u in the catalog window)%s, %u effects", clipdir->count(), catalog->clip_count,
        playlist.empty() ? " (put .mov files under media/ or pass --clip)" : "", catalog->effect_count);
    for (uint32_t i = 0; i < clipdir->count() && i < 32; ++i) {
        const auto& c = clipdir->info(static_cast<int>(i));
        LOG("  clip %2u  %-32s %4ux%-4u %5.2f fps %6.1f s  codec %u  placeholder #%06X", i, c.name, c.width, c.height,
            c.fps, c.seconds, c.codec, c.thumb_rgb);
    }
    if (clipdir->count() > 32) LOG("  ... and %u more", clipdir->count() - 32);
    auto model = std::make_unique<DeckModel>(*catalog);
    model->set_clips(clipdir.get());

    // ==== ENGINE (M6) outputs: displays by identity, the output model, Local\HimmelOutputs ====
    himmel::engine::DisplayWatch watch;
    watch.start();  // the first scan runs here; then a worker keeps the list current
    auto displays = std::make_unique<himmel::engine::DisplayList>();
    uint32_t displays_gen = 0;
    watch.latest(*displays, displays_gen);
    for (uint32_t k = 0; k < displays->count; ++k) {
        const auto& d = displays->d[displays->order[k]];
        LOG("display %u: \"%s\" %s %04X serial %u \"%s\" connector %u  %dx%d at %d,%d  %.2f Hz", k + 1, d.name,
            d.id.maker, d.id.product, d.id.serial, d.id.serial_text, d.id.connector, d.rect[2] - d.rect[0],
            d.rect[3] - d.rect[1], d.rect[0], d.rect[1], d.hz);
    }
    himmel::engine::OutputSetup osetup;
    // Default: one output per display in full screen, 3 windows otherwise (the default rig).
    osetup.count = cfg.outputs ? cfg.outputs : (cfg.fullscreen ? std::max(1, static_cast<int>(displays->count)) : 3);
    osetup.fullscreen = cfg.fullscreen;
    osetup.preset = cfg.resolution >= 0 ? static_cast<proto::ResolutionPreset>(cfg.resolution)
                                       : (cfg.fullscreen ? proto::ResolutionPreset::P1080 : proto::ResolutionPreset::Custom);
    osetup.custom_w = static_cast<uint32_t>(cfg.win_w), osetup.custom_h = static_cast<uint32_t>(cfg.win_h);
    osetup.mode = cfg.mirror ? proto::CanvasMode::Mirror : proto::CanvasMode::Span;
    osetup.blend_px = static_cast<uint32_t>(cfg.blend_px);
    osetup.blend_gamma = cfg.blend_gamma;
    osetup.refresh = cfg.refresh;
    auto om = std::make_unique<himmel::engine::OutputModel>(osetup);
    g_om = om.get();
    model->set_outputs(om.get());
    const int n = om->count();
    himmel::engine::ShmBlock outputs_shm;
    proto::OutputsBlock* outputs_blk = nullptr;
    auto out_sec = std::make_unique<proto::OutputsSection>();  // written into the block on change
    proto::DisplayId slot_ids[kMaxOutputs] = {};
    {
        const auto r = outputs_shm.open_or_create(proto::kOutputsShmName, sizeof(proto::OutputsBlock));
        if (r == himmel::engine::ShmBlock::Result::Failed) {
            LOG("outputs: cannot map %ls - no output status (curtains cannot follow)", proto::kOutputsShmName);
        } else {
            outputs_blk = static_cast<proto::OutputsBlock*>(outputs_shm.data());
            if (r == himmel::engine::ShmBlock::Result::Opened &&
                himmel::engine::header_ok(outputs_blk->header, proto::kOutputsMagic, proto::kOutputsVersion,
                                          static_cast<uint32_t>(sizeof(proto::OutputsBlock)))) {
                auto old = std::make_unique<proto::OutputsSection>();
                if (proto::seq_read(outputs_blk->seq, outputs_blk->s, *old)) {
                    om->restore(*old);
                    if (old->fullscreen && cfg.fullscreen)
                        for (int o = 0; o < kMaxOutputs; ++o) slot_ids[o] = old->out[o].id;
                    LOG("outputs: restored black / places%s from the previous renderer",
                        old->config_by_command ? " / canvas settings" : "");
                }
            } else {
                std::memset(static_cast<void*>(outputs_blk), 0, sizeof(proto::OutputsBlock));
                himmel::engine::write_header(outputs_blk->header, proto::kOutputsMagic, proto::kOutputsVersion,
                                             static_cast<uint32_t>(sizeof(proto::OutputsBlock)));
            }
            outputs_blk->status.pid.store(GetCurrentProcessId());
        }
    }
    // ==== end ENGINE (M6) ====

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
    // D131: every effect's default reaction row; the deck model keeps the per-knob table (FxReact).
    auto react_defaults = std::make_unique<proto::SlotReactions[]>(proto::kMaxEffects);
    himmel::engine::react_defaults(himmel::fx::builtin(), react_defaults.get(), proto::kMaxEffects);
    model->set_react_defaults(react_defaults.get(), proto::kMaxEffects);
    auto react_out = std::make_unique<proto::ReactionSection>();  // the state block copy (+ live env / value)
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
                    // ==== AUDIO (D131): the reaction table comes back too ====
                    auto old_rx = std::make_unique<proto::ReactionSection>();
                    const bool have_rx = proto::seq_read(state.blk->react_seq, state.blk->react, *old_rx) != 0;
                    // ==== END AUDIO (D131) ====
                    // ==== ENGINE wave 7: the previous renderer's clip pages (ids >= 256 come back too) ====
                    auto old_pages = std::make_unique<proto::ClipPagesCopy>();
                    auto old_dir = std::make_unique<himmel::engine::ClipDirectory>();
                    const bool have_pages = proto::read_clip_pages(state.blk->clips, state.blk->clip_page, *old_pages) >= 0;
                    if (have_pages) {
                        auto flat = std::make_unique<proto::ClipEntry[]>(proto::kMaxCatalogClips);
                        for (uint32_t i = 0; i < old_pages->count; ++i) flat[i] = old_pages->entry(i);
                        old_dir->load_from(flat.get(), old_pages->count);
                    }
                    const int lost = model->restore(*old_live, *old_cat, have_tr ? old_tr.get() : nullptr,
                                                    have_rx ? old_rx.get() : nullptr, have_pages ? old_dir.get() : nullptr);
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
            clipdir->publish(state.blk->clips, state.blk->clip_page);  // wave 7: catalog v3 pages
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
    g_fx->set_reactions(&model->reactions());  // ==== AUDIO (D131): per-knob reactions ====
    // ==== ENGINE (M6): Spout out (off without --spout; a failure never touches the outputs) ====
    himmel::engine::SpoutOut spout;
    if (!cfg.spout.empty()) {
        spout.start(g.dev.Get(), cfg.spout.c_str(), spout_log);
        LOG("spout: MAIN goes out as \"%s\" (SpoutDX on its own thread and device)", cfg.spout.c_str());
    }
    // ==== end ENGINE (M6) ====

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HimmelOutput";
    RegisterClassW(&wc);

    // Full screen: slot -> display by identity (unknown slots take the displays left to right).
    std::vector<Output> outs(static_cast<size_t>(n));
    int slot_display[kMaxOutputs];
    bool slot_other[kMaxOutputs];
    proto::DisplayId disp_ids[himmel::engine::kMaxDisplays];
    auto match_now = [&]() {
        for (uint32_t k = 0; k < displays->count; ++k) disp_ids[k] = displays->d[k].id;
        himmel::engine::match_displays(slot_ids, n, disp_ids, displays->order, static_cast<int>(displays->count),
                                       slot_display, slot_other);
        for (int o = 0; o < n; ++o)
            if (om->sim_lost(o)) slot_display[o] = -1, slot_other[o] = false;
    };
    match_now();
    for (int i = 0; i < n; ++i) {
        Output& o = outs[static_cast<size_t>(i)];
        RECT r;
        bool visible = true;
        if (cfg.fullscreen) {
            const int d = slot_display[i];
            if (d >= 0) {
                const auto& di = displays->d[d];
                r = {di.rect[0], di.rect[1], di.rect[2], di.rect[3]};
                o.display = d, o.id = di.id, o.hz = di.hz;
                std::memcpy(o.name, di.name, sizeof(o.name));
                slot_ids[i] = di.id;
                o.state = proto::OutputState::Ok;
            } else {  // no display yet: hidden, far away, preset-sized; shown when its display comes
                const auto& rg = om->region(i);
                r = {-32000, -32000, -32000 + std::max(64, rg.w), -32000 + std::max(64, rg.h)};
                visible = false;
                o.state = slot_other[i] ? proto::OutputState::Other : proto::OutputState::Lost;
                o.lost_us = now_us();
            }
        } else {
            const int x0 = 40 + i * (cfg.win_w + 24) % 1800, y0 = 80 + (i * (cfg.win_w + 24) / 1800) * (cfg.win_h + 60);
            r = {x0, y0, x0 + cfg.win_w, y0 + cfg.win_h};
            o.state = proto::OutputState::Window;
            if (displays->count) o.hz = displays->d[displays->order[0]].hz;  // GUESS: the windows sit on the first display
        }
        if (!create_output(g, o, i, r, cfg.fullscreen, visible)) return 2;
    }
    if (es) es->outputs.store(static_cast<uint32_t>(n));
    // The refresh (operations.md §3): full-screen displays are switched by the watcher's worker.
    auto ask_refresh = [&]() {
        if (!cfg.fullscreen) return;
        watch.want_refresh(*displays, slot_display, n, proto::refresh_hz(om->setup().refresh));
    };
    ask_refresh();
    uint32_t refresh_gen_seen = om->refresh_gen();
    uint32_t adopt_seen[kMaxOutputs];
    for (int o = 0; o < kMaxOutputs; ++o) adopt_seen[o] = om->adopt_gen(o);
    LOG("outputs: %d %s, canvas %ux%u (%s%s), refresh %.2f Hz%s", n, cfg.fullscreen ? "full screen" : "window(s)",
        om->canvas_w(), om->canvas_h(), om->setup().mode == proto::CanvasMode::Mirror ? "mirror" : "span",
        om->setup().blend_px ? ", edge blend" : "", proto::refresh_hz(om->setup().refresh),
        cfg.fullscreen ? "" : " (windowed: the desktop's refresh)");

    PreviewWriter preview;
    preview.fps = cfg.preview_fps;
    if (!preview.open()) LOG("preview: cannot map %ls - no previews", proto::kPreviewShmName);
    // The canvas follows the output model (presets, span / mirror, blend): made now and
    // rebuilt only when a setup command changes it (one hitch, like a window resize).
    // DeckA / DeckB / Main previews have the canvas aspect (3 x 16:9 span = a 48:9 strip);
    // Output1..3 have their output's aspect.
    auto configure_previews = [&]() {
        make_canvas(g, static_cast<int>(om->canvas_w()), static_cast<int>(om->canvas_h()));
        spout.resize(g.canvas_w, g.canvas_h);  // ENGINE (M6): Spout's shared textures follow the canvas
        for (int i = 0; i < n; ++i) {
            const auto& rg = om->region(i);
            std::memcpy(outs[static_cast<size_t>(i)].src, rg.uv, sizeof(rg.uv));
            draw_ident(g, outs[static_cast<size_t>(i)], i, rg);
        }
        const float full[4] = {0, 0, 1, 1};
        const float aspect = static_cast<float>(g.canvas_w) / g.canvas_h;
        for (int i = 0; i < 3; ++i) preview.configure(g, i, true, aspect, full);
        for (int i = 0; i < 3; ++i) {
            const bool on = i < n;
            const auto& rg = om->region(on ? i : 0);
            const float* src = on ? rg.uv : full;
            const float a = on && rg.h > 0 ? static_cast<float>(rg.w) / rg.h : aspect;
            preview.configure(g, 3 + i, on, a, src);
        }
        om->clear_layout_dirty();
    };
    configure_previews();

    // ---- clip layers: one player (worker thread) per layer, all made now
    LayerRuntime layers[kLayerSlots];  // 0..7 = deck * 4 + layer, 8 = the HIT layer
    for (auto& l : layers) l.player = std::make_unique<himmel::engine::ClipPlayer>(g.dev.Get(), clip_table.get());  // MEDIA (M7) hook 2/3
    LOG("clip players: %d (one per layer + the HIT layer)", kLayerSlots);
    uint64_t jumps = 0, jumps_next_frame = 0, jump_worst = 0;

    // ---- commands + Perform
    CommandLink cmd;
    auto perform_ring = std::make_unique<proto::CommandRing>();
    perform_ring->head = 0, perform_ring->tail = 0, perform_ring->dropped = 0;
    LocalSink sink;
    sink.ring = perform_ring.get();
    auto perform = std::make_unique<himmel::perform::Mk2Perform>();
    perform->set_clips(clipdir.get());  // ENGINE (catalog v3): the full clip list for BROWSE (CONTROL wave 8)
    // CONTROL M4: the mapper reads the show status (LOOKS pads, macros, undo) and the transport
    // (cue points, clock); both objects live as long as the loop.
    perform->set_context(&show->status(), &model->transport());
    Mk2Link mk2link;
    auto lightpad = std::make_unique<himmel::perform::LightpadPerform>();  // CONTROL M9
    lightpad->set_context(&model->transport());
    LightpadLink lplink;
    auto nuvj = std::make_unique<himmel::perform::NuvjPerform>();  // CONTROL M10
    nuvj->set_clips(clipdir.get());  // CONTROL wave 8: the pad banks reach every clip (catalog v3)
    nuvj->set_context(&model->transport());
    LightpadLink nvlink;
    nvlink.weapon = "nuvj";
    himmel::perform::NuvjLcd nv_lcd;
    uint8_t nv_leds[himmel::weapon::kMaxControls] = {}, nv_leds_last[himmel::weapon::kMaxControls] = {};
    himmel::weapon::Display nv_disp_last{};

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
    LOG("start-up took %.0f ms (process start -> first frame)", (now_us() - t_launch) / 1000.0);

    // ==== ENGINE (M6) outputs: a slot's display comes and goes; the other outputs never notice ====
    auto shown = [](const Output& o) {
        return o.state == proto::OutputState::Ok || o.state == proto::OutputState::Window;
    };
    bool outputs_dirty = true;
    uint64_t outputs_next_us = 0;
    bool sim_seen[kMaxOutputs] = {};
    auto set_display = [&](int i, int d, uint64_t t) {  // rare events only (a cable, a mode, a test)
        Output& o = outs[static_cast<size_t>(i)];
        const bool was_shown = shown(o);
        if (!cfg.fullscreen) {
            if (d < 0 && was_shown) {
                ShowWindow(o.hwnd, SW_HIDE);
                o.state = proto::OutputState::Lost;
            } else if (d >= 0 && !was_shown) {
                ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
                o.state = proto::OutputState::Window;
            }
        } else if (d < 0) {
            const auto st = slot_other[i] ? proto::OutputState::Other : proto::OutputState::Lost;
            if (was_shown) ShowWindow(o.hwnd, SW_HIDE);
            if (st != o.state) LOG("OUT %d: %s", i + 1, st == proto::OutputState::Other
                                                          ? "lost - a different display is free (Adopt to use it)"
                                                          : "lost - its part of the canvas keeps rendering");
            o.state = st;
            o.display = -1;
        } else {
            const auto& di = displays->d[d];
            const RECT r{di.rect[0], di.rect[1], di.rect[2], di.rect[3]};
            const bool moved = !EqualRect(&r, &o.rect);
            if (was_shown && !moved && o.display == d && o.hz == di.hz) return;  // nothing changed
            if (!was_shown || moved) {
                g_self_move = true;
                SetWindowPos(o.hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                g_self_move = false;
                resize_output(g, o);  // only this output's swap chain, and only if the size changed
            }
            o.display = d, o.id = di.id, o.hz = di.hz;
            std::memcpy(o.name, di.name, sizeof(o.name));
            slot_ids[i] = di.id;
            o.state = proto::OutputState::Ok;
            draw_ident(g, o, i, om->region(i));
            if (moved && was_shown) LOG("OUT %d: its display moved on the desktop - followed it", i + 1);
        }
        const bool now_shown = shown(o);
        if (was_shown && !now_shown) {
            o.lost_us = t;
            ++o.lost_count;
            if (!cfg.fullscreen) LOG("OUT %d: lost (simulated) - its part of the canvas keeps rendering", i + 1);
        } else if (!was_shown && now_shown) {
            o.returned_ms = o.lost_us ? static_cast<uint32_t>((t - o.lost_us) / 1000) : 0;
            LOG("OUT %d: back on \"%s\" after %u ms", i + 1, o.name[0] ? o.name : "its window", o.returned_ms);
            if (cfg.fullscreen) ask_refresh();
        }
        outputs_dirty = true;
        overlay_next = 0;
    };
    auto update_outputs = [&](uint64_t t) {
        bool changed = watch.latest(*displays, displays_gen);
        if (changed) LOG("displays: %u now", displays->count);
        for (int i = 0; i < n; ++i) {
            if (om->adopt_gen(i) != adopt_seen[i]) {
                adopt_seen[i] = om->adopt_gen(i);
                if (outs[static_cast<size_t>(i)].state == proto::OutputState::Other) {
                    slot_ids[i] = proto::DisplayId{};  // forget the old display: take the free one
                    changed = true;
                    LOG("OUT %d: adopting the new display", i + 1);
                }
            }
            const bool sim_shown = !om->sim_lost(i);
            if (!cfg.fullscreen && sim_shown != shown(outs[static_cast<size_t>(i)])) set_display(i, sim_shown ? 0 : -1, t);
            if (cfg.fullscreen && om->sim_lost(i) != sim_seen[i]) changed = true;
            sim_seen[i] = om->sim_lost(i);
        }
        if (cfg.fullscreen && changed) {
            match_now();
            for (int i = 0; i < n; ++i) set_display(i, slot_display[i], t);
        }
        if (om->refresh_gen() != refresh_gen_seen) {
            refresh_gen_seen = om->refresh_gen();
            ask_refresh();
            LOG("outputs: refresh %.2f Hz asked%s", proto::refresh_hz(om->setup().refresh),
                cfg.fullscreen ? "" : " (windowed: noted only)");
            outputs_dirty = true;
        }
    };
    // ==== end ENGINE (M6) ====

    // The frame period for "late": the asked refresh in full screen, the desktop's in a window.
    double frame_period = 1.0 / proto::refresh_hz(om->setup().refresh);
    if (!cfg.fullscreen && displays->count && displays->d[displays->order[0]].hz > 1.0f)
        frame_period = 1.0 / displays->d[displays->order[0]].hz;

    model->set_refresh_hz(1.0 / frame_period);  // wave 7: the playheads' refresh grid (Even / Blend)
    LOG("playback: refresh grid %.2f Hz, frame smoothing %s by default", 1.0 / frame_period, "Auto");
    uint64_t quit_at_us = 0;  // ENGINE (quit): when quit_request was raised
    while (!g_win.quit) {
        // ==== ENGINE (M6): wait only for the shown outputs ====
        // 1. Wait until the shown swap chains can take a frame (max latency 1). A hidden
        //    output (its display is gone) leaves the wait set, so it never holds up the others.
        HANDLE waits[kMaxOutputs];
        DWORD nwaits = 0;
        for (const auto& o : outs)
            if (shown(o)) waits[nwaits++] = o.waitable;
        if (nwaits) WaitForMultipleObjects(nwaits, waits, TRUE, 100);
        else Sleep(static_cast<DWORD>(1000.0f / proto::refresh_hz(om->setup().refresh)));
        // ==== end ENGINE (M6) ====
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
        // ==== ENGINE (M6): display change, M key, window resize ====
        if (g_win.display_change) {  // a display came, went or changed mode: rescan now
            g_win.display_change = false;
            watch.poke();
        }
        if (g_win.mirror_toggle) {  // the M key: same as OutputConfig Mode
            g_win.mirror_toggle = false;
            proto::Command c = proto::make(proto::CommandType::OutputConfig);
            c.param = static_cast<uint8_t>(proto::OutputParam::Mode);
            c.i = om->setup().mode == proto::CanvasMode::Mirror ? 0 : 1;
            om->apply(c, -1, t);
        }
        if (g_win.resized) {  // a window changed size: only its swap chain follows (the canvas stays)
            g_win.resized = false;
            for (auto& o : outs)
                if (shown(o)) resize_output(g, o);
            outputs_dirty = true;
            overlay_next = 0;
        }
        // ==== end ENGINE (M6) ====

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
        // ==== CONTROL M9: the Lightpad (TOUCH / MIX / SCRUB) feeds slot 0 like the MK2 ====
        lplink.poll(t);
        const bool lp_alive = lplink.alive(t);
        if (lp_alive != lplink.was_alive) {
            if (lp_alive) lightpad->reset(), lplink.written = false;
            LOG("Lightpad %s", lp_alive ? "connected" : "offline");
            lplink.was_alive = lp_alive;
        }
        if (lplink.slot) {
            auto& ring = lplink.shm.get()->rings[himmel::weapon::kEngineRing];
            if (!ring.attached.load()) ring.attach();  // a restarted driver may have reset it
            mk2::Event e;
            for (int k = 0; k < 1024 && ring.pop(e); ++k) lightpad->on_event(e, model->live(), *catalog, sink);
        }
        // ==== CONTROL M10: the NuVJ (crossfader decides PROGRAM) feeds slot 0 too ====
        nvlink.poll(t);
        const bool nv_alive = nvlink.alive(t);
        if (nv_alive != nvlink.was_alive) {
            if (nv_alive) nuvj->reset(), nvlink.written = false;
            LOG("NuVJ %s", nv_alive ? "connected" : "offline");
            nvlink.was_alive = nv_alive;
        }
        if (nvlink.slot) {
            auto& ring = nvlink.shm.get()->rings[himmel::weapon::kEngineRing];
            if (!ring.attached.load()) ring.attach();
            mk2::Event e;
            for (int k = 0; k < 1024 && ring.pop(e); ++k) nuvj->on_event(e, model->live(), *catalog, sink);
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
        // ==== ENGINE (quit): a confirmed SystemQuit (74) -> quit_request; the supervisor stops us
        // and everything else (command-protocol.md §4e). On our own (no supervisor): quit after 3 s.
        if (model->quit_producer() >= 0) {
            if (quit_at_us == 0) {
                quit_at_us = t;
                if (es) es->quit_request.store(static_cast<uint32_t>(model->quit_producer()) + 1);
                LOG("QUIT: confirmed by producer %d - asking the supervisor to stop Himmel", model->quit_producer());
            } else if (t - quit_at_us > 3000000) {
                LOG("QUIT: no supervisor stopped us in 3 s - quitting on our own");
                g_win.quit = true;
            }
        }
        // ==== end ENGINE (quit) ====
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
            model->set_producer_alive(0, mk2_alive, t);
            for (int slot = 1; slot < proto::kProducers; ++slot) {
                bool alive = false;
                if (cmd.blk) {
                    const uint64_t hb = cmd.blk->producers[slot].heartbeat_us.load();
                    alive = cmd.blk->producers[slot].pid.load() != 0 && hb && t - std::min(t, hb) < 2000000;
                }
                model->set_producer_alive(slot, alive, t);
            }
        }
        const proto::LiveState& live = model->live();

        // ==== ENGINE (M6) 3b. Outputs: displays that came / went, adopt, refresh; a setup command
        //     (preset, mode, blend) rebuilds the canvas here - a setup action, one hitch.
        update_outputs(t);
        if (om->layout_dirty()) {
            configure_previews();
            for (auto& l : layers) l.hold_valid = false;
            LOG("outputs: canvas %ux%u (%s)", om->canvas_w(), om->canvas_h(),
                om->setup().mode == proto::CanvasMode::Mirror ? "mirror" : "span");
            outputs_dirty = true;
            overlay_next = 0;
        }
        // ==== end ENGINE (M6) 3b ====

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
                    layer_cb(g, s, rt, old, *catalog, 1.0f, cb, src);
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
                rt.cad_frame = -1;
                if (ls.clip >= 0) {
                    rt.fail_seen = rt.player->load_failed.load();
                    // Wave 7 pre-roll: the worker decodes the start frame before it hands the clip over.
                    rt.player->load(ls.clip, std::max<int64_t>(0, model->frame(s)), model->direction(s));
                    clip_watch->poke(ls.clip);
                }
            }
            himmel::engine::LoadedClip* clip = rt.player->current();
            if (clip != rt.shown) {
                const bool is_req =
                    clip && rt.requested >= 0 && clip->path == clip_table->path[static_cast<size_t>(rt.requested)];
                // A new clip takes over only with its first frame: if the pre-rolled frame is not
                // ready (it always is), the old picture stays one more frame.
                rt.shown = clip;
                rt.uploaded = -1;
                rt.main_tex = -1;
                rt.up[0] = rt.up[1] = -1;
                rt.current_is_requested = is_req;
            }
            if (rt.requested >= 0 && !rt.current_is_requested && rt.player->load_failed.load() != rt.fail_seen)
                rt.missing = true;
            model->set_missing_slot(s, rt.requested >= 0 && rt.missing);
            model->set_loading(s, rt.requested >= 0 && !rt.current_is_requested && !rt.missing);
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
            // Wave 7: two textures per clip. The frame on screen and (frame blend) the next one;
            // each clip frame is uploaded once - the next frame is already there when it is due.
            if (f != rt.uploaded) {
                const int have = rt.up[0] == f ? 0 : rt.up[1] == f ? 1 : -1;
                if (have >= 0) {
                    rt.main_tex = have;
                    rt.uploaded = f;
                } else if (const uint8_t* data = rt.player->acquire(f)) {
                    const int k = rt.main_tex < 0 ? 0 : 1 - rt.main_tex;  // never the one on screen
                    g.ctx->UpdateSubresource(clip->gpu_tex(k), 0, nullptr, data, static_cast<UINT>(clip->row_pitch), 0);
                    rt.player->release();
                    rt.up[k] = f;
                    rt.main_tex = k;
                    rt.uploaded = f;
                } else if (rt.uploaded >= 0) {
                    ++rt.misses;  // the last frame stays on screen
                }
            }
            rt.blend_next = -1;
            rt.blend_w = 0.0f;
            if (rt.main_tex >= 0 && rt.uploaded == f) {
                int64_t nf = -1;
                float w = 0.0f;
                model->blend_frames(s, nf, w);
                if (nf >= 0 && w > 0.0f) {
                    const int other = 1 - rt.main_tex;
                    if (rt.up[other] != nf) {
                        if (const uint8_t* data = rt.player->acquire(nf)) {
                            g.ctx->UpdateSubresource(clip->gpu_tex(other), 0, nullptr, data,
                                                     static_cast<UINT>(clip->row_pitch), 0);
                            rt.player->release();
                            rt.up[other] = nf;
                        }
                    }
                    if (rt.up[other] == nf) rt.blend_next = nf, rt.blend_w = w;
                }
            }
            // Wave 7 cadence: how many refreshes each clip frame stays (a missed refresh counts double).
            if (rt.uploaded >= 0) {
                const uint32_t refreshes = static_cast<uint32_t>(std::max(1.0, std::floor(dt / frame_period + 0.5)));
                const double ideal = model->refreshes_per_frame(s);
                const bool playing_on = ls.playing && ideal > 0.0 && model->jump_gen_slot(s) == rt.jump_seen;
                if (!playing_on || std::abs(ideal - rt.cad_ideal) > 1e-3 || rt.cad_frame < 0) {
                    rt.cad_ideal = ideal;  // a new run: speed / clip / pause / jump
                    rt.cad_sum = 0.0, rt.cad_frames = 0, rt.cad_hold = 0;
                    rt.cad_frame = rt.uploaded;
                } else if (rt.uploaded != rt.cad_frame) {
                    const int64_t step = std::llabs(rt.uploaded - rt.cad_frame);
                    if (step == 1 && rt.cad_hold > 0) {
                        rt.cad_sum += rt.cad_hold;
                        ++rt.cad_frames;
                        const double lo = std::floor(ideal + 1e-6), hi = std::ceil(ideal - 1e-6);
                        if (rt.cad_hold < lo || rt.cad_hold > hi) ++rt.cad_irregular;
                        if (rt.cad_frames > 1) {  // the first hold of a run starts mid-frame
                            rt.cad_err_sum += std::abs(rt.cad_sum - rt.cad_frames * ideal);
                            ++rt.cad_err_n;
                        } else {
                            rt.cad_sum = rt.cad_frames * ideal;  // align the run to the first full frame
                        }
                        ++rt.cad_changes;
                    } else {
                        rt.cad_sum = 0.0, rt.cad_frames = 0;  // a loop or a skip: start again
                    }
                    rt.cad_frame = rt.uploaded;
                    rt.cad_hold = 0;
                }
                rt.cad_hold += refreshes;
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
            perform->set_library(library_feed.catalog_full.load(std::memory_order_relaxed));  // CONTROL: MK2 BROWSE
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

        // 5b. Lightpad pixels (on change, at most ~30 Hz; the driver sends only what changed).
        if (lp_alive) {
            lightpad->tick(t, live, *catalog, sink,
                           reinterpret_cast<uint8_t(*)[3]>(lplink.px));
            if (t >= lplink.next_out_us && (!lplink.written || std::memcmp(lplink.px, lplink.px_last, sizeof(lplink.px)) != 0)) {
                mk2::seq_write(lplink.shm.get()->outputs.pixel_seq, lplink.shm.get()->outputs.pixel, lplink.px);
                std::memcpy(lplink.px_last, lplink.px, sizeof(lplink.px));
                lplink.written = true;
                lplink.next_out_us = t + 33000;
            }
        }

        // 5c. NuVJ LEDs and LCD (on change; the driver resends all of it every 2 s itself).
        if (nv_alive) {
            nuvj->tick(t, live, *catalog, sink, nv_leds, nv_lcd);
            auto* b = nvlink.shm.get();
            if (t >= nvlink.next_out_us) {
                if (!nvlink.written || std::memcmp(nv_leds, nv_leds_last, sizeof(nv_leds)) != 0) {
                    mk2::seq_write(b->outputs.led_seq, b->outputs.led, nv_leds);
                    std::memcpy(nv_leds_last, nv_leds, sizeof(nv_leds));
                }
                himmel::weapon::Display d{};
                std::memcpy(d.text, nv_lcd.text, sizeof(d.text));
                std::memcpy(d.alpha, nv_lcd.alpha, sizeof(d.alpha));
                std::memcpy(d.number, nv_lcd.number, sizeof(d.number));
                d.bar[0] = nv_lcd.bar[0], d.bar[1] = nv_lcd.bar[1];
                d.marks = nv_lcd.marks;
                if (!nvlink.written || std::memcmp(&d, &nv_disp_last, sizeof(d)) != 0) {
                    mk2::seq_write(b->outputs.display_seq, b->outputs.display, d);
                    nv_disp_last = d;
                }
                nvlink.written = true;
                nvlink.next_out_us = t + 33000;
            }
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
                layer_cb(g, s, rt, ls, *catalog, opacity, cb, src);
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
        // ==== ENGINE (M6): Spout out - one copy of MAIN (black when master black), the worker sends it ====
        spout.frame(g.ctx.Get(), g.main[main_k].tex.Get(), live.black != 0);
        // ==== end ENGINE (M6) ====

        // ==== ENGINE (M6) 7. Outputs: shown outputs only, black per output, edge blend, IDENTIFY ====
        const bool overlay_due = g_win.overlay && t >= overlay_next;
        if (overlay_due) {
            overlay_next = t + 500000;
            std::snprintf(overlay_line, sizeof(overlay_line),
                          "HIMMEL %.1f FPS  PGM %c  XF %.2f%s%s  FOCUS %c%d  MK2 %s  GUI %s  TOOL %s  LATE %u", fps,
                          live.program_deck ? 'B' : 'A', live.crossfader, live.take_active ? "  TAKE" : "",
                          live.black ? "  BLACK" : "", live.focus_deck < kDecks ? 'A' + live.focus_deck : '-',
                          live.focus_layer < kLayersPerDeck ? live.focus_layer + 1 : 0, mk2_alive ? "OK" : "-",
                          live.producer_alive[1] ? "OK" : "-", live.producer_alive[3] ? "OK" : "-", late);
            for (int i = 0; i < n; ++i) {  // "OUT 2 LOST" (operations.md §1)
                const auto st = outs[static_cast<size_t>(i)].state;
                if (st != proto::OutputState::Lost && st != proto::OutputState::Other) continue;
                const size_t len = std::strlen(overlay_line);
                std::snprintf(overlay_line + len, sizeof(overlay_line) - len, "  OUT%d %s", i + 1,
                              st == proto::OutputState::Other ? "NEW?" : "LOST");
            }
        }
        HRESULT present_hr = S_OK;
        const float ident_a = om->identify_alpha(t);
        for (int i = 0; i < n; ++i) {
            auto& o = outs[static_cast<size_t>(i)];
            if (!shown(o)) continue;  // its display is gone: its part of the canvas is still in MAIN / the previews
            if (overlay_due) overlay_text(g, o, overlay_line);
            const auto& rg = om->region(i);
            BlitCB bcb{};
            std::memcpy(bcb.src, o.src, sizeof(bcb.src));
            if (g_win.overlay) {
                bcb.overlay[0] = 8, bcb.overlay[1] = 8;
                bcb.overlay[2] = static_cast<float>(kOverlayW * kOverlayScale);
                bcb.overlay[3] = static_cast<float>(kOverlayH * kOverlayScale);
            }
            bcb.info[0] = live.black || om->black(i) ? 0.0f : 1.0f;  // master black, black per output (M6)
            // Edge blend: ramp widths as parts of this output, and 1 / gamma (engine.md §2.0).
            if (rg.w > 0) {
                bcb.edge[0] = rg.blend_left / rg.w;
                bcb.edge[1] = rg.blend_right / rg.w;
            }
            bcb.edge[2] = 1.0f / om->setup().blend_gamma;
            // IDENTIFY: the picture centred, 60 % of the output's height (operations.md §2).
            if (ident_a > 0.0f) {
                const float ih = o.h * 0.6f, iw = ih * kIdentW / kIdentH;
                const float sc = std::min(1.0f, o.w * 0.9f / iw);
                bcb.ident[2] = iw * sc, bcb.ident[3] = ih * sc;
                bcb.ident[0] = (o.w - bcb.ident[2]) * 0.5f, bcb.ident[1] = (o.h - bcb.ident[3]) * 0.5f;
                bcb.info[1] = ident_a;
            }
            upload_cb(g, g.cb_blit.Get(), bcb);
            g.ctx->PSSetConstantBuffers(3, 1, g.cb_blit.GetAddressOf());
            ID3D11ShaderResourceView* ident_srv = o.ident_srv.Get();
            g.ctx->PSSetShaderResources(3, 1, &ident_srv);
            pass(g, o.rtv.Get(), o.w, o.h, g.ps_blit.Get(), main_srv, o.overlay_srv.Get());
        }
        {
            ID3D11ShaderResourceView* none1 = nullptr;
            g.ctx->PSSetShaderResources(3, 1, &none1);
        }
        // ==== end ENGINE (M6) 7 ====
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
            if (!shown(o)) continue;
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
            bool react_due = false;  // AUDIO (D131)
            if (model->take_structure_dirty() || playing || t_done - state.last_write_us >= 250000) {
                proto::seq_write(state.blk->live_seq, state.blk->live, live);
                proto::seq_write(state.blk->transport_seq, state.blk->transport, model->transport());
                // Wave 7: frame smoothing + cadence per layer (a ~150 B section, no allocation).
                proto::PlaybackSection pb{};
                pb.smooth = static_cast<uint8_t>(model->frame_smooth());
                pb.out_hz = static_cast<float>(model->refresh_hz());
                for (int s = 0; s < kLayerSlots; ++s) {
                    auto& lp = pb.layer[s];
                    lp.smooth = model->smooth_setting(s);
                    lp.in_use = static_cast<uint8_t>(model->smooth_in_use(s));
                    lp.loading = model->loading(s) ? 1 : 0;
                    const int c = model->layer(s).clip;
                    lp.clip_fps = clipdir->valid(c) ? clipdir->info(c).fps : 0.0f;
                    lp.refreshes_per_frame = static_cast<float>(model->refreshes_per_frame(s));
                    lp.judder = layers[s].judder;
                    lp.irregular = layers[s].irregular;
                }
                proto::seq_write(state.blk->playback_seq, state.blk->playback, pb);
                state.last_write_us = t_done;
                react_due = true;
            }
            // ==== AUDIO (D131): the reaction table + live env / value, every frame while an FX slot is used ====
            if (react_due || react_live_wanted(live)) {
                fill_react_out(*react_out, model->reactions(), *g_fx);
                proto::seq_write(state.blk->react_seq, state.blk->react, *react_out);
            }
            // ==== END AUDIO (D131) ====
            state.blk->status.heartbeat_us.store(t_done);
        }
        // ==== ENGINE (M6): outputs block - on change, while IDENTIFY runs, and every 100 ms (the supervisor's
        // curtains follow the window rects from here). ~3 KB memcpy, no allocation.
        if (outputs_blk) {
            if (outputs_dirty || t_done >= outputs_next_us || ident_a > 0.0f) {
                om->fill(*out_sec, t_done);
                out_sec->time_us = t_done;
                out_sec->epoch = live.epoch;
                out_sec->hz_measured = fps;
                out_sec->frame_smooth = static_cast<uint8_t>(model->frame_smooth());  // wave 7
                out_sec->displays = displays->count;
                for (int i = 0; i < kMaxOutputs; ++i) {
                    auto& oi = out_sec->out[i];
                    if (i >= n) {
                        oi.state = static_cast<uint8_t>(proto::OutputState::Off);
                        oi.hwnd = 0;
                        continue;
                    }
                    const Output& o = outs[static_cast<size_t>(i)];
                    oi.state = static_cast<uint8_t>(o.state);
                    oi.id = cfg.fullscreen ? slot_ids[i] : proto::DisplayId{};
                    std::memcpy(oi.name, o.name, sizeof(oi.name));
                    oi.swap_w = static_cast<uint32_t>(o.w), oi.swap_h = static_cast<uint32_t>(o.h);
                    oi.hz = o.hz;
                    oi.rect[0] = o.rect.left, oi.rect[1] = o.rect.top, oi.rect[2] = o.rect.right, oi.rect[3] = o.rect.bottom;
                    oi.hwnd = reinterpret_cast<uint64_t>(o.hwnd);
                    oi.lost_count = o.lost_count;
                    oi.returned_ms = o.returned_ms;
                }
                proto::seq_write(outputs_blk->seq, outputs_blk->s, *out_sec);
                outputs_dirty = false;
                outputs_next_us = t_done + 100000;
            }
            outputs_blk->status.heartbeat_us.store(t_done);
        }
        // ==== end ENGINE (M6) ====

        // ==== MEDIA (M7) hook 3/3: library clips that turned ready join the catalog. Ids are
        // append-only, so nothing on air moves; no I/O, no allocation (fixed table) ====
        // ENGINE wave 7 (catalog v3): the directory follows the table (any number of clips up to
        // kMaxCatalogClips), the file watch sets each clip's state, changed pages are published.
        if (const uint32_t tn = clip_table->size(); tn > clipdir->count()) {
            const uint32_t before = clipdir->count();
            for (uint32_t i = before; i < tn; ++i)
                clipdir->add(clip_table->info[i], clip_table->library_id[i], clip_watch->state(static_cast<int>(i)));
            LOG("library: %u new clip(s) in the catalog, %u in all", clipdir->count() - before, clipdir->count());
        }
        if (const uint32_t wg = clip_watch->changed(); wg != clip_watch_seen) {
            clip_watch_seen = wg;
            for (uint32_t i = 0; i < clipdir->count(); ++i) {
                const auto st = clip_watch->state(static_cast<int>(i));
                if (clipdir->state(static_cast<int>(i)) != st && clipdir->set_state(static_cast<int>(i), st))
                    LOG("clip %u \"%s\": %s", i, clipdir->info(static_cast<int>(i)).name,
                        st == proto::ClipState::Ready ? "ready" : st == proto::ClipState::Busy ? "busy (being written)" : "missing");
            }
        }
        if (clipdir->dirty()) {
            if (clipdir->mirror_window(*catalog) && state.blk)
                proto::seq_write(state.blk->catalog_seq, state.blk->catalog, *catalog);
            if (state.blk) clipdir->publish(state.blk->clips, state.blk->clip_page);
        }
        if (static bool full_logged = false; !full_logged && library_feed.catalog_full.load(std::memory_order_relaxed)) {
            full_logged = true;  // once: the clips stay in the library
            LOG("library: the catalog is full (%d clips) - new library clips stay in the library", proto::kMaxCatalogClips);
        }
        // ==== end MEDIA hook 3/3 ====

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
        if (dt > 1.5 * frame_period && frames > 2) {
            ++late, ++late_period;
            late_worst_ms = std::max(late_worst_ms, dt * 1000.0);
        }
        if (es) {
            es->heartbeat_us.store(t_done);
            es->frames.store(frames);
            es->late_frames.store(late);  // every frame: himmel_ingest pauses on a late frame (M7)
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
            // Wave 7: the cadence of every layer that plays a clip (judder = mean |cumulative error| in refreshes).
            for (int s = 0; s < kLayerSlots; ++s) {
                auto& ly = layers[s];
                ly.judder = ly.cad_err_n ? static_cast<float>(ly.cad_err_sum / ly.cad_err_n) : 0.0f;
                ly.irregular = ly.cad_irregular;
                if (ly.cad_changes && model->layer(s).clip >= 0) {
                    static const char* const kSm[] = {"auto", "off", "blend", "even"};
                    const int c = model->layer(s).clip;
                    LOG("CADENCE layer %d  clip %d %.3f fps  %.3f refreshes/frame  smooth %s  frames %u  judder %.3f  irregular %u",
                        s, c, clipdir->valid(c) ? clipdir->info(c).fps : 0.0f, model->refreshes_per_frame(s),
                        kSm[static_cast<int>(model->smooth_in_use(s)) & 3], ly.cad_changes, ly.judder, ly.cad_irregular);
                }
                ly.cad_err_sum = 0.0, ly.cad_err_n = 0, ly.cad_irregular = 0, ly.cad_changes = 0;
            }
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
    watch.stop();
    spout.stop();  // ENGINE (M6)
    if (outputs_blk) outputs_blk->status.pid.store(0);  // the curtains stay where the windows were
    set_msg("stopped");
    timeEndPeriod(1);
    return exit_code;
}
