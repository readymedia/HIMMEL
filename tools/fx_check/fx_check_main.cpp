// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// fx_check.exe - every effect and generator from fx/, through the real engine
// path (himmel::fx::FxGpu + resolve), as PNGs and GPU timings. No hardware.
//
//   fx_check [out_dir] [--size WxH] [--draws N] [--md file.md] [--no-png]
//
// * PNGs (480x270): <out_dir>/fx_<nn>_<id>.png at the effect's defaults with the
//   test signal at t = 1.3 s, <id>_extreme.png at its "extreme" preset, plus
//   fx_sheet.png and fx_sheet_extreme.png (contact sheets, names stamped in).
//   Effects run on a synthetic test picture; generators run on transparent black.
// * Timing: GPU time per pass at --size (default 1920x1080), timestamp queries
//   around N draws (default 200), for the defaults and the extreme preset.
//   --md writes a markdown table (docs/measurements/).
// Exit code: 0 = all good, 1 = a shader failed or an effect is over its cost class, 2 = no D3D11.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/controller/app/png_write.h"
#include "himmel/fx/fx_gpu.h"
#include "himmel/fx/signals.h"
#include "himmel/mk2/output.h"

using Microsoft::WRL::ComPtr;
using namespace himmel::fx;

namespace {

// Synthetic source: hue sweep, darker downwards, a white disc (moves with t, for
// trails), a checker corner and a dark band (for keys / thresholds).
std::vector<uint8_t> source_image(int w, int h, float t) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    const float cx = w * (0.30f + 0.4f * t), cy = h * 0.45f, r = h * 0.16f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float hh = static_cast<float>(x) / w, v = 0.25f + 0.75f * (1.0f - static_cast<float>(y) / h);
            float c[3];
            for (int i = 0; i < 3; ++i) {
                const float k = std::fabs(std::fmod(hh + (3 - i) / 3.0f, 1.0f) * 6.0f - 3.0f) - 1.0f;
                c[i] = v * (1.0f + 0.8f * (std::clamp(k, 0.0f, 1.0f) - 1.0f));
            }
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r) c[0] = c[1] = c[2] = 1.0f;
            if (x < w / 5 && y > h * 3 / 4) c[0] = c[1] = c[2] = ((x * 30 / w + y * 30 / w) & 1) ? 0.9f : 0.1f;
            if (y > h * 12 / 14 && x > w * 3 / 5) c[0] = c[1] = c[2] = 0.03f;
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            for (int i = 0; i < 3; ++i) p[i] = static_cast<uint8_t>(std::clamp(c[i], 0.0f, 1.0f) * 255);
            p[3] = 255;
        }
    return px;
}

struct Rt {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

void stamp(std::vector<uint8_t>& rgb, int w, int h, int x, int y, const char* text, int scale) {
    auto put = [&](int px, int py, uint8_t v) {
        if (px < 0 || py < 0 || px >= w || py >= h) return;
        uint8_t* p = &rgb[(static_cast<size_t>(py) * w + px) * 3];
        p[0] = p[1] = p[2] = v;
    };
    for (const char* c = text; *c; ++c) {
        const uint8_t* g = himmel::mk2::glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(*c))));
        if (!g) g = himmel::mk2::glyph('?');
        for (int cx = -1; cx < 6; ++cx)
            for (int cy = -1; cy < 8; ++cy) {
                const bool on = g && cx >= 0 && cx < 5 && cy >= 0 && cy < 7 && ((g[cx] >> cy) & 1);
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx) put(x + cx * scale + sx, y + cy * scale + sy, on ? 255 : 0);
            }
        x += himmel::mk2::kGlyphW * scale;
    }
}

std::vector<uint8_t> sheet(const std::vector<std::vector<uint8_t>>& tiles, int w, int h, int cols) {
    const int rows = static_cast<int>((tiles.size() + cols - 1) / cols), gap = 4;
    const int sw = cols * w + (cols + 1) * gap, sh = rows * h + (rows + 1) * gap;
    std::vector<uint8_t> out(static_cast<size_t>(sw) * sh * 3, 40);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int ox = gap + static_cast<int>(i % cols) * (w + gap), oy = gap + static_cast<int>(i / cols) * (h + gap);
        for (int y = 0; y < h; ++y)
            std::memcpy(&out[(static_cast<size_t>(oy + y) * sw + ox) * 3], &tiles[i][static_cast<size_t>(y) * w * 3],
                        static_cast<size_t>(w) * 3);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = "tools/_screens/fx", md;
    int bw = 1920, bh = 1080, draws = 200;
    bool png = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) sscanf_s(argv[++i], "%dx%d", &bw, &bh);
        else if (a == "--draws" && i + 1 < argc) draws = std::max(10, std::atoi(argv[++i]));
        else if (a == "--md" && i + 1 < argc) md = argv[++i];
        else if (a == "--no-png") png = false;
        else if (a[0] != '-') out_dir = a;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    bool warp = false;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx))) {
        warp = true;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                     &ctx))) {
            std::printf("no D3D11 device\n");
            return 2;
        }
    }
    char adapter[128] = "?";
    {
        ComPtr<IDXGIDevice> dd;
        ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dev.As(&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter, sizeof(adapter), nullptr, nullptr);
    }
    const Library& lib = builtin();
    FxGpu fx;
    std::string report;
    const bool shaders_ok = fx.init(dev.Get(), lib, "HIMMEL  *  FX CHECK  *  0.1 PLAYBACK", &report);
    std::printf("%s%s: %d effects, shaders compiled in %.0f ms\n", adapter, warp ? " (WARP)" : "", lib.count(), fx.compile_ms());
    if (!report.empty()) std::printf("%s", report.c_str());

    auto make = [&](int w, int h, Rt& t) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &t.tex);
        dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
        dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    };
    auto read = [&](Rt& t, int w, int h) {
        D3D11_TEXTURE2D_DESC td{};
        t.tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        dev->CreateTexture2D(&td, nullptr, &st);
        ctx->CopyResource(st.Get(), t.tex.Get());
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int i = 0; i < 3; ++i)
                        rgb[(static_cast<size_t>(y) * w + x) * 3 + i] = static_cast<const uint8_t*>(ms.pData)[y * ms.RowPitch + x * 4 + i];
            ctx->Unmap(st.Get(), 0);
        }
        return rgb;
    };

    // One frame of one effect through the engine path (run_chain), `frames` frames long.
    // Generators start from transparent black (an empty layer); effects from the test picture.
    auto render = [&](int id, const float* values, int w, int h, Rt pair[2], int frames) -> int {
        const EffectMeta& m = lib.meta(id);
        fx.resize(w, h);
        himmel::protocol::FxSlotState slots[himmel::protocol::kFxSlots] = {};
        for (auto& s : slots) s.effect = -1;
        slots[0].effect = static_cast<int16_t>(id);
        slots[0].mix = 1.0f;
        for (int p = 0; p < m.params; ++p) slots[0].param[p] = values[p];
        const Target t[2] = {{pair[0].tex.Get(), pair[0].rtv.Get(), pair[0].srv.Get()},
                             {pair[1].tex.Get(), pair[1].rtv.Get(), pair[1].srv.Get()}};
        int out = 0;
        for (int f = 0; f < frames; ++f) {
            const float tt = frames > 1 ? static_cast<float>(f) / (frames - 1) : 0.0f;
            if (m.kind == Kind::Generator) {
                const float clear[4] = {0, 0, 0, 0};
                ctx->ClearRenderTargetView(pair[0].rtv.Get(), clear);
            } else {
                const auto img = source_image(w, h, tt);
                ctx->UpdateSubresource(pair[0].tex.Get(), 0, nullptr, img.data(), static_cast<UINT>(w) * 4, 0);
            }
            FrameCtx fc;
            fc.time_s = 1.3 + f / 60.0;
            fc.beats = 0.1 + fc.time_s * 2.0 - 2.6;  // just after a beat: strobe on, kick high, pulses visible
            fc.dt = 1.0f / 60;
            fc.sig = TestSignal::at(fc.time_s, fc.beats);
            fx.begin_frame(fc);
            out = fx.run_chain(ctx.Get(), t, w, h, 0, slots, 0);
        }
        return out;
    };

    bool all_ok = shaders_ok;
    if (png) {
        const int w = 480, h = 270;
        Rt pair[2];
        make(w, h, pair[0]), make(w, h, pair[1]);
        std::vector<std::vector<uint8_t>> tiles, tiles_x;
        {
            const auto img = source_image(w, h, 0.0f);
            ctx->UpdateSubresource(pair[0].tex.Get(), 0, nullptr, img.data(), w * 4, 0);
            tiles.push_back(read(pair[0], w, h));
            stamp(tiles.back(), w, h, 6, 6, "SOURCE", 2);
            tiles_x.push_back(tiles.back());
            himmel::png::write_rgb(out_dir + "/fx_00_source.png", w, h, tiles.back());
        }
        for (int id = 0; id < lib.count(); ++id) {
            const EffectMeta& m = lib.meta(id);
            float def[kParams] = {};
            for (int p = 0; p < m.params; ++p) def[p] = m.param[p].def;
            const int frames = m.state == State::Feedback ? 24 : 1;
            for (int variant = 0; variant < 2; ++variant) {
                const float* v = variant == 0 ? def : m.preset[m.find_preset("extreme")].v;
                const int k = render(id, v, w, h, pair, frames);
                auto rgb = read(pair[k], w, h);
                char label[80];
                std::snprintf(label, sizeof(label), "%d %s%s", id, m.name, variant ? " +" : "");
                stamp(rgb, w, h, 6, 6, label, 2);
                char file[512];
                std::snprintf(file, sizeof(file), "%s/fx_%02d_%s%s.png", out_dir.c_str(), id + 1, m.id, variant ? "_extreme" : "");
                himmel::png::write_rgb(file, w, h, rgb);
                (variant ? tiles_x : tiles).push_back(std::move(rgb));
            }
        }
        himmel::png::write_rgb(out_dir + "/fx_sheet.png", 6 * w + 7 * 4, ((static_cast<int>(tiles.size()) + 5) / 6) * (h + 4) + 4,
                               sheet(tiles, w, h, 6));
        himmel::png::write_rgb(out_dir + "/fx_sheet_extreme.png", 6 * w + 7 * 4,
                               ((static_cast<int>(tiles_x.size()) + 5) / 6) * (h + 4) + 4, sheet(tiles_x, w, h, 6));
        std::printf("wrote %zu pictures + fx_sheet.png + fx_sheet_extreme.png to %s\n", tiles.size() + tiles_x.size() - 1,
                    out_dir.c_str());
    }

    // ---- GPU time per pass
    Rt bp[2];
    make(bw, bh, bp[0]), make(bw, bh, bp[1]);
    const auto img = source_image(bw, bh, 0.3f);
    ctx->UpdateSubresource(bp[0].tex.Get(), 0, nullptr, img.data(), static_cast<UINT>(bw) * 4, 0);
    fx.resize(bw, bh);
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, q0, q1;
    dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &q0);
    dev->CreateQuery(&qd, &q1);
    auto time_pass = [&](int id, const float* values) {
        Resolved r;
        for (int p = 0; p < kParams; ++p) r.p[p] = values[p];
        r.mix = 1.0f, r.gate = 1.0f;
        FrameCtx fc;
        fc.time_s = 1.3, fc.beats = 2.1;
        fx.begin_frame(fc);
        ID3D11ShaderResourceView* aux = lib.meta(id).state == State::Feedback ? bp[0].srv.Get() : nullptr;
        // aux for trails: any canvas-sized texture costs the same as the feedback texture; the
        // destination differs from both sources.
        Rt& dst = bp[1];
        for (int i = 0; i < 5; ++i) fx.draw(ctx.Get(), id, r, bp[0].srv.Get(), dst.rtv.Get(), bw, bh, aux);  // warm up
        ctx->Begin(disjoint.Get());
        ctx->End(q0.Get());
        for (int i = 0; i < draws; ++i) fx.draw(ctx.Get(), id, r, bp[0].srv.Get(), dst.rtv.Get(), bw, bh, aux);
        ctx->End(q1.Get());
        ctx->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(1);
        UINT64 a = 0, z = 0;
        while (ctx->GetData(q0.Get(), &a, sizeof(a), 0) == S_FALSE) Sleep(1);
        while (ctx->GetData(q1.Get(), &z, sizeof(z), 0) == S_FALSE) Sleep(1);
        return dj.Disjoint || !dj.Frequency ? -1.0 : static_cast<double>(z - a) * 1000.0 / static_cast<double>(dj.Frequency) / draws;
    };
    std::string table = "| # | Effect | Kind | Class | Default ms | Extreme ms | Within class |\n|---|---|---|---|---|---|---|\n";
    std::printf("GPU time per pass, %dx%d, %d draws:\n", bw, bh, draws);
    double total = 0;
    for (int id = 0; id < lib.count(); ++id) {
        const EffectMeta& m = lib.meta(id);
        float def[kParams] = {};
        for (int p = 0; p < m.params; ++p) def[p] = m.param[p].def;
        const double ms_d = time_pass(id, def);
        const double ms_x = time_pass(id, m.preset[m.find_preset("extreme")].v);
        const double worst = std::max(ms_d, ms_x);
        // Cost classes at 1080p on a GTX 1070 (effects-and-generators.md §2): S < 0.15 ms, M < 0.6 ms.
        const double limit = m.cost_class == 'S' ? 0.15 : m.cost_class == 'M' ? 0.6 : 1e9;
        const double scaled = worst * (1920.0 * 1080.0) / (static_cast<double>(bw) * bh);
        const bool within = scaled < limit;
        if (!within && !warp) all_ok = false;
        total += ms_d;
        std::printf("  %2d %-18s %-9s %c  %.3f ms  extreme %.3f ms  %s\n", id, m.name,
                    m.kind == Kind::Generator ? "generator" : "effect", m.cost_class, ms_d, ms_x, within ? "ok" : "OVER CLASS");
        char row[256];
        std::snprintf(row, sizeof(row), "| %d | %s | %s | %c | %.3f | %.3f | %s |\n", id, m.name,
                      m.kind == Kind::Generator ? "generator" : "effect", m.cost_class, ms_d, ms_x, within ? "yes" : "**no**");
        table += row;
    }
    std::printf("  sum of all %d at their defaults: %.3f ms\n", lib.count(), total);
    if (!md.empty()) {
        FILE* f = nullptr;
        if (fopen_s(&f, md.c_str(), "wb") == 0 && f) {
            std::fprintf(f, "GPU: %s%s, %dx%d, %d draws per value, fx_check.exe\n\n%s\nSum of all %d at their defaults: %.3f ms.\n",
                         adapter, warp ? " (WARP)" : "", bw, bh, draws, table.c_str(), lib.count(), total);
            std::fclose(f);
        }
    }
    return all_ok ? 0 : 1;
}
