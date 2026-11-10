// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// touch_fx_check.exe [out_dir] - renders every Touch FX effect over a synthetic
// test image with the real shader (render/shaders.h) and writes PNGs: one per
// effect plus a contact sheet (source + 9 effects). No window, no hardware.
// Also times each effect pass on the GPU (1080p, 200 draws).
//
// Default out_dir: tools/_screens/touchfx (gitignored).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../controller/app/png_write.h"
#include "../render/shaders.h"
#include "himmel/engine/touch_fx.h"

using Microsoft::WRL::ComPtr;
namespace tfx = himmel::engine::touchfx;

namespace {

struct TouchFxCB {
    float x, y, mix, time;
    float effect, beat, w, h;
};

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    if (FAILED(D3DCompile(himmel::engine::kShaderSource, sizeof(himmel::engine::kShaderSource) - 1, "himmel", nullptr,
                          nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err))) {
        std::printf("shader %s failed: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return true;
}

// Synthetic source: hue sweep left -> right, darker towards the bottom, a white
// disc (moves with `t` so Trails has something to trail), a checkerboard corner.
std::vector<uint8_t> source_image(int w, int h, float t) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    const float cx = w * (0.30f + 0.4f * t), cy = h * 0.45f, r = h * 0.16f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float hh = static_cast<float>(x) / w, v = 0.25f + 0.75f * (1.0f - static_cast<float>(y) / h);
            float c[3];
            for (int i = 0; i < 3; ++i) {
                const float k = std::fabs(std::fmod(hh + (3 - i) / 3.0f, 1.0f) * 6.0f - 3.0f) - 1.0f;
                c[i] = v * (1.0f + 0.8f * (std::clamp(k, 0.0f, 1.0f) - 1.0f));
            }
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r) c[0] = c[1] = c[2] = 1.0f;
            if (x < w / 5 && y > h * 3 / 4) c[0] = c[1] = c[2] = ((x / 16 + y / 16) & 1) ? 0.9f : 0.1f;
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            for (int i = 0; i < 3; ++i) p[i] = static_cast<uint8_t>(std::clamp(c[i], 0.0f, 1.0f) * 255);
            p[3] = 255;
        }
    }
    return px;
}

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string out_dir = argc > 1 ? argv[1] : "tools/_screens/touchfx";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        std::printf("no D3D11 device\n");
        return 2;
    }
    ComPtr<ID3DBlob> b;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (!compile("vs_full", "vs_5_0", b)) return 2;
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
    if (!compile("ps_touchfx", "ps_5_0", b)) return 2;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 32;
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&bd, nullptr, &cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> lin;
    dev->CreateSamplerState(&sd, &lin);

    auto make = [&](int w, int h, Target& t) {
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
    auto set_cb = [&](const TouchFxCB& c) {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
    };
    auto draw = [&](Target& src, Target& prev, Target& dst, int w, int h) {
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(4, 1, cb.GetAddressOf());
        ctx->PSSetSamplers(0, 1, lin.GetAddressOf());
        ID3D11ShaderResourceView* srv[2] = {src.srv.Get(), prev.srv.Get()};
        ctx->PSSetShaderResources(4, 2, srv);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(4, 2, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    };
    auto read = [&](Target& t, int w, int h) {
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
                        rgb[(static_cast<size_t>(y) * w + x) * 3 + i] =
                            static_cast<const uint8_t*>(ms.pData)[y * ms.RowPitch + x * 4 + i];
            ctx->Unmap(st.Get(), 0);
        }
        return rgb;
    };

    // ---- pictures (480x270, each effect at its starting position, mix 1)
    const int w = 480, h = 270;
    Target src, prev, dst;
    make(w, h, src), make(w, h, prev), make(w, h, dst);
    std::vector<std::vector<uint8_t>> tiles;
    {
        const auto img = source_image(w, h, 0.0f);
        ctx->UpdateSubresource(src.tex.Get(), 0, nullptr, img.data(), w * 4, 0);
        tiles.push_back(read(src, w, h));
        himmel::png::write_rgb(out_dir + "/touchfx_source.png", w, h, tiles.back());
    }
    for (int e = 0; e < tfx::kEffects; ++e) {
        const auto& fi = tfx::info(e);
        TouchFxCB c{fi.x0, fi.y0, 1.0f, 0.3f, static_cast<float>(e), 0.75f, static_cast<float>(w), static_cast<float>(h)};
        if (e == static_cast<int>(tfx::Effect::Strobe)) c.y = 0.5f;  // mid-grey flash, so it is visible
        const int frames = e == static_cast<int>(tfx::Effect::Trails) ? 24 : 1;
        ctx->CopyResource(prev.tex.Get(), src.tex.Get());
        for (int f = 0; f < frames; ++f) {
            const auto img = source_image(w, h, frames > 1 ? static_cast<float>(f) / (frames - 1) : 0.0f);
            ctx->UpdateSubresource(src.tex.Get(), 0, nullptr, img.data(), w * 4, 0);
            set_cb(c);
            draw(src, prev, dst, w, h);
            ctx->CopyResource(prev.tex.Get(), dst.tex.Get());
        }
        tiles.push_back(read(dst, w, h));
        std::string name(fi.name);
        for (auto& ch : name) ch = ch == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        char file[256];
        std::snprintf(file, sizeof(file), "%s/touchfx_%d_%s.png", out_dir.c_str(), e + 1, name.c_str());
        himmel::png::write_rgb(file, w, h, tiles.back());
        const auto img = source_image(w, h, 0.0f);
        ctx->UpdateSubresource(src.tex.Get(), 0, nullptr, img.data(), w * 4, 0);
    }
    // Contact sheet: 5 x 2 (source first), 4 px gaps.
    const int cols = 5, rows = 2, gap = 4, sw = cols * w + (cols + 1) * gap, sh = rows * h + (rows + 1) * gap;
    std::vector<uint8_t> sheet(static_cast<size_t>(sw) * sh * 3, 40);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int ox = gap + static_cast<int>(i % cols) * (w + gap), oy = gap + static_cast<int>(i / cols) * (h + gap);
        for (int y = 0; y < h; ++y)
            std::memcpy(&sheet[(static_cast<size_t>(oy + y) * sw + ox) * 3], &tiles[i][static_cast<size_t>(y) * w * 3],
                        static_cast<size_t>(w) * 3);
    }
    himmel::png::write_rgb(out_dir + "/touchfx_sheet.png", sw, sh, sheet);
    std::printf("wrote %zu pictures + touchfx_sheet.png to %s\n", tiles.size(), out_dir.c_str());

    // ---- GPU time per effect at 1920x1080 (timestamp queries around 200 draws)
    Target bs, bp, bd2;
    make(1920, 1080, bs), make(1920, 1080, bp), make(1920, 1080, bd2);
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, q0, q1;
    dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &q0);
    dev->CreateQuery(&qd, &q1);
    std::printf("GPU time per pass, 1920x1080:\n");
    for (int e = 0; e < tfx::kEffects; ++e) {
        const auto& fi = tfx::info(e);
        set_cb({fi.x0, fi.y0, 1.0f, 0.3f, static_cast<float>(e), 0.75f, 1920.0f, 1080.0f});
        draw(bs, bp, bd2, 1920, 1080);  // warm up
        ctx->Begin(disjoint.Get());
        ctx->End(q0.Get());
        constexpr int kDraws = 200;
        for (int i = 0; i < kDraws; ++i) draw(bs, bp, bd2, 1920, 1080);
        ctx->End(q1.Get());
        ctx->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(1);
        UINT64 a = 0, z = 0;
        while (ctx->GetData(q0.Get(), &a, sizeof(a), 0) == S_FALSE) Sleep(1);
        while (ctx->GetData(q1.Get(), &z, sizeof(z), 0) == S_FALSE) Sleep(1);
        const double ms = dj.Disjoint || !dj.Frequency ? -1.0 : (z - a) * 1000.0 / dj.Frequency / kDraws;
        std::printf("  %d %-9.*s tier %c  %.3f ms\n", e + 1, static_cast<int>(fi.name.size()), fi.name.data(), fi.tier, ms);
    }
    return 0;
}
