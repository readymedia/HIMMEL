// himmel_render.exe - render process skeleton (CLAUDE.md "Engine skeleton").
//
//   himmel_render [--outputs N] [--fullscreen] [--size 640x360] [--mode span|mirror]
//
// * Direct3D 11, one DXGI flip-model swap chain per output, waitable object,
//   maximum frame latency 1.
// * Renders one canvas (a test card for now) and shows it on 1..8 outputs:
//   span = the canvas is split across the outputs, mirror = each shows all.
// * Reads the MK2 through the controller's shared block (ring 0): knobs change
//   the test card, pads flash their part of the canvas, Navigate toggles
//   span/mirror. Works without the controller and picks it up when it appears.
// * Publishes a heartbeat + frame stats in "Local\HimmelEngine" for the
//   supervisor. Exits with code 3 if the GPU device is lost (the supervisor
//   restarts it).
//
// Frame loop rules: no allocation, no locks, no disk I/O. Everything that
// allocates (windows, swap chains, textures) happens at start-up or on resize.
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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "himmel/engine/engine_shared.h"
#include "himmel/mk2/output.h"
#include "himmel/mk2/pages.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"
#include "shaders.h"

using Microsoft::WRL::ComPtr;
namespace mk2 = himmel::mk2;
using himmel::mk2::now_us;

namespace {

constexpr int kMaxOutputs = 8;
constexpr int kOverlayW = 512, kOverlayH = 16, kOverlayScale = 2;
constexpr int kEngineRing = 0;  // ring 0 of the MK2 block belongs to the engine

void log(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::printf("%02d:%02d:%02d  render  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------- config
struct Config {
    int outputs = 0;  // 0 = one per monitor (windowed: 3)
    bool fullscreen = false;
    int win_w = 640, win_h = 360;
    bool mirror = false;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--outputs") c.outputs = std::clamp(std::atoi(next().c_str()), 1, kMaxOutputs);
        else if (a == "--fullscreen") c.fullscreen = true;
        else if (a == "--windowed") c.fullscreen = false;
        else if (a == "--mode") c.mirror = next() == "mirror";
        else if (a == "--size") {
            const std::string s = next();
            int w = 0, h = 0;
            if (sscanf_s(s.c_str(), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) c.win_w = w, c.win_h = h;
        }
    }
    return c;
}

std::vector<RECT> monitors() {
    std::vector<RECT> m;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<RECT>*>(p)->push_back(*r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&m));
    std::sort(m.begin(), m.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });  // left to right
    return m;
}

// ---------------------------------------------------------------- shared state
struct Params {
    std::array<float, mk2::kKnobs> knob = {0.60f, 0.70f, 0.40f, 0.50f, 0.50f, 0.30f, 0.50f, 1.00f};
    // hue, saturation, speed, zoom, grid, bar width, pad decay, master
    std::array<float, mk2::kPads> pad{};
    int touched = -1;           // last turned knob (inverted on the MK2 screen)
    uint64_t touched_us = 0;
    bool mirror = false;
    bool quit = false;
    bool resized = false;
};
Params g_params;
bool g_fullscreen = false;

// ---------------------------------------------------------------- GPU
struct PatternCB {
    float time, canvas_w, canvas_h, hue;
    float sat, speed, zoom, grid;
    float bar, master, bar_pos, beat;
    float pads[16];
    float seams[8];
};
struct BlitCB {
    float src[4];
    float overlay[4];
    float out_master, pad_[3];
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
    ComPtr<ID3D11PixelShader> ps_pattern, ps_blit;
    ComPtr<ID3D11Buffer> cb_pattern, cb_blit;
    ComPtr<ID3D11SamplerState> lin, pnt;
    ComPtr<ID3D11Texture2D> canvas;
    ComPtr<ID3D11RenderTargetView> canvas_rtv;
    ComPtr<ID3D11ShaderResourceView> canvas_srv;
    int canvas_w = 0, canvas_h = 0;
    char adapter[128] = "";
};

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(himmel::engine::kShaderSource, sizeof(himmel::engine::kShaderSource) - 1, "himmel",
                                  nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) {
        log("shader %s failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return true;
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
        log("D3D11CreateDevice failed 0x%08lx", hr);
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
    if (!compile("ps_pattern", "ps_5_0", b)) return false;
    g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.ps_pattern);
    if (!compile("ps_blit", "ps_5_0", b)) return false;
    g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.ps_blit);

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = (sizeof(PatternCB) + 15) & ~15u;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb_pattern);
    bd.ByteWidth = (sizeof(BlitCB) + 15) & ~15u;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb_blit);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g.dev->CreateSamplerState(&sd, &g.pnt);
    return true;
}

void make_canvas(Gpu& g, int w, int h) {
    if (w == g.canvas_w && h == g.canvas_h && g.canvas) return;
    g.canvas.Reset();
    g.canvas_rtv.Reset();
    g.canvas_srv.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g.dev->CreateTexture2D(&td, nullptr, &g.canvas);
    g.dev->CreateRenderTargetView(g.canvas.Get(), nullptr, &g.canvas_rtv);
    g.dev->CreateShaderResourceView(g.canvas.Get(), nullptr, &g.canvas_srv);
    g.canvas_w = w;
    g.canvas_h = h;
    log("canvas %dx%d", w, h);
}

// ---------------------------------------------------------------- windows
LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: g_params.resized = true; return 0;
        case WM_CLOSE:
            if (!g_fullscreen) g_params.quit = true;  // a show output never closes by click
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && (!g_fullscreen || (GetKeyState(VK_SHIFT) & 0x8000))) g_params.quit = true;
            if (wp == 'M') g_params.mirror = !g_params.mirror, g_params.resized = true;
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
    HRESULT hr = g.factory->CreateSwapChainForHwnd(g.dev.Get(), o.hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        log("swap chain %d failed 0x%08lx", index + 1, hr);
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
    log("output %d: %dx%d %s", index + 1, o.w, o.h, fullscreen ? "fullscreen" : "window");
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
void layout(Gpu& g, std::vector<Output>& outs, bool mirror, PatternCB& cb) {
    int total_w = 0, max_w = 0, max_h = 0;
    for (auto& o : outs) total_w += o.w, max_w = std::max(max_w, o.w), max_h = std::max(max_h, o.h);
    make_canvas(g, mirror ? max_w : total_w, max_h);
    std::fill(std::begin(cb.seams), std::end(cb.seams), 0.0f);
    int x = 0;
    for (size_t i = 0; i < outs.size(); ++i) {
        auto& o = outs[i];
        if (mirror) {
            o.src[0] = 0, o.src[1] = 0, o.src[2] = 1, o.src[3] = 1;
        } else {
            o.src[0] = static_cast<float>(x) / g.canvas_w;
            o.src[1] = 0;
            o.src[2] = static_cast<float>(o.w) / g.canvas_w;
            o.src[3] = static_cast<float>(o.h) / g.canvas_h;
            x += o.w;
            if (i + 1 < outs.size() && i < 7) cb.seams[i] = static_cast<float>(x);
        }
    }
}

void update_overlay(Gpu& g, Output& o, int index, int count, bool mirror, float fps, bool mk2) {
    mk2::Canvas<kOverlayW, kOverlayH> c;
    char line[96];
    std::snprintf(line, sizeof(line), "HIMMEL  OUT %d/%d  %dx%d  %s  %.1f FPS  MK2 %s", index + 1, count, o.w, o.h,
                  mirror ? "MIRROR" : "SPAN", fps, mk2 ? "OK" : "OFF");
    c.text(2, 4, line);
    for (int y = 0; y < kOverlayH; ++y)
        for (int x = 0; x < kOverlayW; ++x) o.overlay_px[y * kOverlayW + x] = c.get(x, y) ? 255 : 0;
    g.ctx->UpdateSubresource(o.overlay.Get(), 0, nullptr, o.overlay_px.data(), kOverlayW, 0);
}

// Knob value -> unit, shared by the frame loop and the MK2 screen readout.
float pad_fade_s(float k) { return 2.0f * std::pow(0.015f, k); }        // 2 s .. 0.03 s
double bar_rate(float k) { return 0.02 + 0.8 * k * k; }               // canvas widths per second

constexpr std::array<std::string_view, mk2::kKnobs> kKnobLabels = {"HUE", "SATURATE", "SPEED", "ZOOM",
                                                                   "GRID", "BAR", "PAD FADE", "MASTER"};

// ---------------------------------------------------------------- MK2 input
struct Mk2Link {
    mk2::SharedMemory shm;
    bool open = false;
    uint64_t next_try_us = 0;
    uint64_t earliest_event_us = 0;  // oldest event consumed this frame (for latency)

    void poll(uint64_t t) {
        if (open || t < next_try_us) return;
        next_try_us = t + 1000000;
        if (shm.open()) {
            open = true;
            shm.get()->rings[kEngineRing].attach();
            log("MK2 controller found, listening on ring %d", kEngineRing);
        }
    }
    bool alive(uint64_t t) const {
        return open && shm.get()->status.connected.load() &&
               t - std::min(t, shm.get()->status.heartbeat_us.load()) < 500000;
    }
    void drain(Params& p) {
        earliest_event_us = 0;
        if (!open) return;
        auto& ring = shm.get()->rings[kEngineRing];
        if (!ring.attached.load()) ring.attach();  // a restarted controller may have reset it
        mk2::Event e;
        while (ring.pop(e)) {
            if (!earliest_event_us) earliest_event_us = e.t_us;
            switch (e.type) {
                case mk2::EventType::KnobDelta:
                    p.knob[e.id] = std::clamp(p.knob[e.id] + e.value / 1000.0f, 0.0f, 1.0f);
                    p.touched = e.id;
                    p.touched_us = e.t_us;
                    break;
                case mk2::EventType::PadHit: p.pad[e.id - 1] = e.value / 127.0f; break;
                case mk2::EventType::PadPressure:
                    p.pad[e.id - 1] = std::max(p.pad[e.id - 1], std::min(1.0f, e.value / 3300.0f));
                    break;
                case mk2::EventType::ButtonDown:
                    if (e.id == static_cast<int>(mk2::Button::Navigate)) p.mirror = !p.mirror, p.resized = true;
                    break;
                default: break;
            }
        }
    }
    // The knob page on the MK2 screens (~30 Hz, only the strips that change are sent).
    uint64_t next_screen_us = 0;
    himmel::mk2::ui::Screen canvas;
    std::array<uint8_t, mk2::kScreenBytes> left{}, right{};
    void screens(const Params& p, float fps, uint64_t t) {
        if (!open || t < next_screen_us) return;
        next_screen_us = t + 33000;
        namespace ui = himmel::mk2::ui;
        ui::PerformModel m;
        m.head[0].left.set("HIMMEL ENGINE");
        m.head[0].right.set("TEST CARD");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s  %.0f FPS", p.mirror ? "MIRROR" : "SPAN", fps);
        m.head[1].left.set(buf);
        m.head[1].right.set("NAVIGATE=MODE");
        for (int k = 0; k < mk2::kKnobs; ++k) {
            auto& slot = m.knobs[static_cast<size_t>(k)];
            slot.label.set(kKnobLabels[static_cast<size_t>(k)]);
            slot.norm = p.knob[static_cast<size_t>(k)];
            const float v = p.knob[static_cast<size_t>(k)];
            if (k == 2) std::snprintf(buf, sizeof(buf), "%.2f/S", bar_rate(v));
            else if (k == 6) std::snprintf(buf, sizeof(buf), "%.2fS", pad_fade_s(v));
            else std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(v * 100)));
            slot.value.set(buf);
        }
        m.touched = p.touched >= 0 && t - std::min(t, p.touched_us) < 1500000 ? p.touched : -1;
        ui::draw_perform(canvas, m);
        canvas.split(left.data(), right.data());
        mk2::seq_write_bytes(shm.get()->outputs.screen_seq[0], shm.get()->outputs.screen[0], left.data(), mk2::kScreenBytes);
        mk2::seq_write_bytes(shm.get()->outputs.screen_seq[1], shm.get()->outputs.screen[1], right.data(), mk2::kScreenBytes);
    }
    // Pad LEDs follow the pad flashes; Navigate shows the output mode.
    void leds(const Params& p) {
        if (!open) return;
        mk2::LedState s{};
        for (int i = 0; i < mk2::kPads; ++i) {
            const auto v = static_cast<uint8_t>(std::clamp(p.pad[i], 0.0f, 1.0f) * 127);
            s.pad_rgb[i][0] = v;
            s.pad_rgb[i][1] = v;
            s.pad_rgb[i][2] = static_cast<uint8_t>(std::max<int>(v, 12));
        }
        s.buttons[static_cast<int>(mk2::led_of(mk2::Button::Navigate).index)] = p.mirror ? 127 : 20;
        mk2::seq_write(shm.get()->outputs.led_seq, shm.get()->outputs.leds, s);
    }
};

}  // namespace

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Config cfg = parse_args(argc, argv);
    g_fullscreen = cfg.fullscreen;
    g_params.mirror = cfg.mirror;

    himmel::engine::EngineShm eshm;
    himmel::engine::EngineStatus* es = eshm.create() ? eshm.get() : nullptr;
    auto set_msg = [&](const char* m) {
        if (es) strncpy_s(es->message, sizeof(es->message), m, _TRUNCATE);
        log("%s", m);
    };

    Gpu g;
    if (!init_gpu(g)) return 2;
    log("GPU: %s", g.adapter);

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

    PatternCB pcb{};
    layout(g, outs, g_params.mirror, pcb);
    std::vector<HANDLE> waits;
    for (auto& o : outs) waits.push_back(o.waitable);

    Mk2Link mk2link;
    const uint64_t t0 = now_us();
    uint64_t last_frame = t0, stats_start = t0, frames = 0, stat_frames = 0, overlay_next = 0;
    double interval_sum = 0, interval_max = 0, lat_sum = 0;
    uint32_t late = 0, lat_n = 0;
    float fps = 0;
    bool mk2_was_alive = false;
    double bar_pos = 0.0;
    int exit_code = 0;
    std::array<float, mk2::kPads> led_last{};
    led_last.fill(-1);
    bool mirror_led_last = !g_params.mirror;
    set_msg("running");

    while (!g_params.quit) {
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
        if (es && es->stop_request.load()) g_params.quit = true;
        if (g_params.resized) {
            g_params.resized = false;
            for (auto& o : outs) resize_output(g, o);
            layout(g, outs, g_params.mirror, pcb);
            overlay_next = 0;
        }

        // 3. Input.
        mk2link.poll(t);
        mk2link.drain(g_params);
        const bool mk2_alive = mk2link.alive(t);
        if (mk2_alive != mk2_was_alive) {
            set_msg(mk2_alive ? "MK2 connected" : "MK2 offline");
            mk2_was_alive = mk2_alive;
            overlay_next = 0;
        }
        // Knob 7: pad fade time, 2 s (left) .. 0.03 s (right), exponential.
        const float fade = std::exp(-static_cast<float>(dt) / pad_fade_s(g_params.knob[6]));
        for (auto& p : g_params.pad) p = p * fade < 0.002f ? 0.0f : p * fade;
        // Knob 3: bar speed, integrated so turning the knob never makes the bar jump.
        bar_pos = std::fmod(bar_pos + bar_rate(g_params.knob[2]) * dt, 1.0);
        if (mk2_alive) mk2link.screens(g_params, fps, t);
        bool leds_changed = g_params.mirror != mirror_led_last;
        for (int i = 0; i < mk2::kPads; ++i) leds_changed |= std::fabs(g_params.pad[i] - led_last[i]) > 0.02f;
        if (leds_changed && mk2_alive) {
            mk2link.leds(g_params);
            led_last = g_params.pad;
            mirror_led_last = g_params.mirror;
        }

        // 4. Render the canvas.
        const auto& k = g_params.knob;
        const double secs = (t - t0) / 1e6;
        pcb.time = static_cast<float>(secs);
        pcb.canvas_w = static_cast<float>(g.canvas_w);
        pcb.canvas_h = static_cast<float>(g.canvas_h);
        pcb.hue = k[0], pcb.sat = k[1], pcb.speed = k[2], pcb.zoom = k[3], pcb.grid = k[4], pcb.bar = k[5];
        pcb.master = 1.0f;
        pcb.bar_pos = static_cast<float>(bar_pos);
        pcb.beat = static_cast<float>(std::pow(1.0 - std::fmod(secs * 2.0, 1.0), 3.0));  // 120 BPM pulse
        for (int i = 0; i < 16; ++i) pcb.pads[i] = g_params.pad[i];
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g.ctx->Map(g.cb_pattern.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &pcb, sizeof(pcb));
            g.ctx->Unmap(g.cb_pattern.Get(), 0);
        }
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(g.canvas_w), static_cast<float>(g.canvas_h), 0, 1};
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->OMSetRenderTargets(1, g.canvas_rtv.GetAddressOf(), nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.ps_pattern.Get(), nullptr, 0);
        g.ctx->PSSetConstantBuffers(0, 1, g.cb_pattern.GetAddressOf());
        g.ctx->Draw(3, 0);

        // 5. Each output shows its part of the canvas (+ status overlay).
        const bool overlay_due = t >= overlay_next;
        if (overlay_due) overlay_next = t + 1000000;
        ID3D11SamplerState* samplers[2] = {g.lin.Get(), g.pnt.Get()};
        g.ctx->PSSetSamplers(0, 2, samplers);
        g.ctx->PSSetShader(g.ps_blit.Get(), nullptr, 0);
        HRESULT present_hr = S_OK;
        for (size_t i = 0; i < outs.size(); ++i) {
            auto& o = outs[i];
            if (overlay_due) update_overlay(g, o, static_cast<int>(i), n, g_params.mirror, fps, mk2_alive);
            BlitCB bcb{};
            std::memcpy(bcb.src, o.src, sizeof(bcb.src));
            bcb.overlay[0] = 8, bcb.overlay[1] = 8;
            bcb.overlay[2] = static_cast<float>(kOverlayW * kOverlayScale);
            bcb.overlay[3] = static_cast<float>(kOverlayH * kOverlayScale);
            bcb.out_master = k[7];
            if (SUCCEEDED(g.ctx->Map(g.cb_blit.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &bcb, sizeof(bcb));
                g.ctx->Unmap(g.cb_blit.Get(), 0);
            }
            ID3D11ShaderResourceView* null_srv[2] = {};
            g.ctx->PSSetShaderResources(0, 2, null_srv);
            D3D11_VIEWPORT ov{0, 0, static_cast<float>(o.w), static_cast<float>(o.h), 0, 1};
            g.ctx->RSSetViewports(1, &ov);
            g.ctx->OMSetRenderTargets(1, o.rtv.GetAddressOf(), nullptr);
            ID3D11ShaderResourceView* srv[2] = {g.canvas_srv.Get(), o.overlay_srv.Get()};
            g.ctx->PSSetShaderResources(0, 2, srv);
            g.ctx->PSSetConstantBuffers(1, 1, g.cb_blit.GetAddressOf());
            g.ctx->Draw(3, 0);
            const HRESULT hr = o.swap->Present(1, 0);
            if (FAILED(hr)) present_hr = hr;
        }
        ID3D11ShaderResourceView* null_srv[2] = {};
        g.ctx->PSSetShaderResources(0, 2, null_srv);
        if (present_hr == DXGI_ERROR_DEVICE_REMOVED || present_hr == DXGI_ERROR_DEVICE_RESET) {
            char m[96];
            std::snprintf(m, sizeof(m), "GPU device lost (0x%08lx): exiting so the supervisor restarts us",
                          static_cast<unsigned long>(g.dev->GetDeviceRemovedReason()));
            set_msg(m);
            exit_code = 3;
            break;
        }

        // 6. Stats + heartbeat.
        const uint64_t t_done = now_us();
        if (mk2link.earliest_event_us && mk2link.earliest_event_us <= t_done) {
            lat_sum += static_cast<double>(t_done - mk2link.earliest_event_us);
            ++lat_n;
        }
        ++frames;
        ++stat_frames;
        interval_sum += dt;
        interval_max = std::max(interval_max, dt);
        if (dt > 1.5 / 60.0 && frames > 2) ++late;
        if (es) {
            es->heartbeat_us.store(t_done);
            es->frames.store(frames);
        }
        if (t_done - stats_start >= 5000000) {
            const double period = (t_done - stats_start) / 1e6;
            fps = static_cast<float>(stat_frames / period);
            const double avg_ms = interval_sum / stat_frames * 1000.0;
            const double lat_ms = lat_n ? lat_sum / lat_n / 1000.0 : 0.0;
            log("%.1f fps  frame avg %.2f ms  max %.2f ms  late %u  input->present %s%.2f ms (%u)", fps, avg_ms,
                interval_max * 1000.0, late, lat_n ? "" : "-", lat_ms, lat_n);
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
            interval_sum = interval_max = lat_sum = 0;
            lat_n = 0;
        }
    }

    if (mk2link.open) {
        mk2::LedState off{};
        mk2::seq_write(mk2link.shm.get()->outputs.led_seq, mk2link.shm.get()->outputs.leds, off);
        mk2link.shm.get()->rings[kEngineRing].detach();
    }
    set_msg("stopped");
    timeEndPeriod(1);
    return exit_code;
}
