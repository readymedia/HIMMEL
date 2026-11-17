// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "himmel/fx/fx_gpu.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "himmel/mk2/output.h"  // the 5x7 font for the ticker strip

namespace himmel::fx {

namespace {

struct FxCB {
    float p[4];
    float info[4];  // mix, time, beats, bpm
    float size[4];  // w, h, 1/w, 1/h
    float ext[4];   // gate, press, aux w, aux h
};

bool compile(const std::string& src, const char* name, const char* entry, const char* target,
             Microsoft::WRL::ComPtr<ID3DBlob>& out, std::string& err) {
    Microsoft::WRL::ComPtr<ID3DBlob> e;
    const HRESULT hr = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &e);
    if (FAILED(hr)) {
        err = e ? static_cast<const char*>(e->GetBufferPointer()) : "D3DCompile failed";
        return false;
    }
    return true;
}

}  // namespace

FxGpu::FxGpu() = default;
FxGpu::~FxGpu() = default;

bool FxGpu::init(ID3D11Device* dev, const Library& lib, const char* ticker_text, std::string* report) {
    const auto t0 = std::chrono::steady_clock::now();
    dev_ = dev;
    lib_ = &lib;
    ticker_id_ = lib.find("gen_ticker");
    const std::string common = Library::common_hlsl();
    const std::string tail = Library::main_hlsl();
    bool ok = true;
    {
        Com<ID3DBlob> b;
        std::string err;
        const std::string vs_src = common + "\nfloat4 fx(float2 uv, float4 s) { return s; }\n" + tail;
        if (!compile(vs_src, "fx_vs", "vs_fx", "vs_5_0", b, err) ||
            FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_))) {
            if (report) *report += "fx vertex shader: " + err + "\n";
            return false;
        }
    }
    // Compile every effect in worker threads (start-up only).
    const int n = lib.count();
    std::vector<Com<ID3DBlob>> blobs(static_cast<size_t>(n));
    std::vector<std::string> errs(static_cast<size_t>(n));
    std::atomic<int> next{0};
    auto work = [&] {
        for (int e; (e = next.fetch_add(1)) < n;) {
            const std::string name = std::string(lib.meta(e).id) + ".hlsl";
            const std::string src = common + "\n#line 1 \"" + name + "\"\n" + lib.hlsl(e) + "\n#line 1 \"_main.hlsl\"\n" + tail;
            compile(src, name.c_str(), "ps_fx", "ps_5_0", blobs[e], errs[e]);
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    for (int e = 0; e < n; ++e) {
        if (!blobs[e] || FAILED(dev->CreatePixelShader(blobs[e]->GetBufferPointer(), blobs[e]->GetBufferSize(), nullptr,
                                                       &ps_[e]))) {
            ok = false;
            if (report) *report += std::string("fx ") + lib.meta(e).id + ": " + errs[e] + "\n";
        }
    }
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(FxCB);
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) return false;
    const D3D11_FILTER filt[4] = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_MIN_MAG_MIP_POINT,
                                  D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_MIN_MAG_MIP_LINEAR};
    const D3D11_TEXTURE_ADDRESS_MODE addr[4] = {D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                                D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR};
    for (int i = 0; i < 4; ++i) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = filt[i];
        sd.AddressU = sd.AddressV = sd.AddressW = addr[i];
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, &samp_[i]))) return false;
    }
    // Ticker strip: one row of 5x7 glyphs, 6 px per character, 1 px border, R8.
    {
        std::string text = ticker_text && *ticker_text ? ticker_text : "HIMMEL";
        if (text.size() > 300) text.resize(300);
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const int w = static_cast<int>(text.size()) * mk2::kGlyphW + 2, h = 9;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h, 0);
        int x = 1;
        for (char c : text) {
            const uint8_t* g = mk2::glyph(c);
            if (!g) g = mk2::glyph('?');
            for (int cx = 0; g && cx < 5; ++cx)
                for (int cy = 0; cy < 7; ++cy)
                    if ((g[cx] >> cy) & 1) px[static_cast<size_t>(cy + 1) * w + x + cx] = 255;
            x += mk2::kGlyphW;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), static_cast<UINT>(w), 0};
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &ticker_tex_)) &&
            SUCCEEDED(dev->CreateShaderResourceView(ticker_tex_.Get(), nullptr, &ticker_srv_))) {
            ticker_w_ = static_cast<float>(w);
            ticker_h_ = static_cast<float>(h);
        }
    }
    compile_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

bool FxGpu::resize(int w, int h) {
    if (!dev_) return false;
    fb_w_ = fb_h_ = 0;
    for (auto& f : fb_) f = Feedback{};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // same as the engine's canvas targets
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& f : fb_) {
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &f.tex)) ||
            FAILED(dev_->CreateRenderTargetView(f.tex.Get(), nullptr, &f.rtv)) ||
            FAILED(dev_->CreateShaderResourceView(f.tex.Get(), nullptr, &f.srv)))
            return false;
    }
    fb_w_ = w, fb_h_ = h;
    return true;
}

void FxGpu::begin_frame(const FrameCtx& ctx) {
    ctx_ = ctx;
    ++frame_;
}

bool FxGpu::active(const protocol::FxSlotState& s) const {
    return lib_ && s.effect >= 0 && s.effect < lib_->count() && !s.bypass && s.mix > 0.001f && ps_[s.effect];
}

bool FxGpu::has_generator(const protocol::FxSlotState (&slots)[protocol::kFxSlots]) const {
    for (const auto& s : slots)
        if (active(s) && lib_->meta(s.effect).kind == Kind::Generator) return true;
    return false;
}

int FxGpu::feedback_for(ID3D11DeviceContext* ctx, int chain, int slot) {
    if (!fb_w_) return -1;
    for (int i = 0; i < kFeedbackPool; ++i)
        if (fb_[i].chain == chain && fb_[i].slot == slot && fb_[i].last_frame + 1 >= frame_) {
            fb_[i].last_frame = frame_;
            return i;
        }
    for (int i = 0; i < kFeedbackPool; ++i)
        if (fb_[i].last_frame + 1 < frame_ || fb_[i].chain < 0) {  // free or not used last frame
            fb_[i].chain = chain, fb_[i].slot = slot, fb_[i].last_frame = frame_;
            const float clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(fb_[i].rtv.Get(), clear);
            return i;
        }
    return -1;  // pool full: this trails slot passes the picture through
}

void FxGpu::draw(ID3D11DeviceContext* ctx, int effect, const Resolved& r, ID3D11ShaderResourceView* src,
                 ID3D11RenderTargetView* dst, int w, int h, ID3D11ShaderResourceView* aux) {
    if (effect < 0 || effect >= kMaxEffects || !ps_[effect]) return;
    FxCB cb{};
    std::memcpy(cb.p, r.p, sizeof(cb.p));
    cb.info[0] = r.mix;
    cb.info[1] = static_cast<float>(ctx_.time_s);
    cb.info[2] = static_cast<float>(std::fmod(ctx_.beats, 4096.0));
    cb.info[3] = ctx_.bpm;
    cb.size[0] = static_cast<float>(w), cb.size[1] = static_cast<float>(h);
    cb.size[2] = 1.0f / static_cast<float>(w), cb.size[3] = 1.0f / static_cast<float>(h);
    cb.ext[0] = r.gate;
    cb.ext[1] = r.press;
    if (effect == ticker_id_) {
        aux = ticker_srv_.Get();
        cb.ext[2] = ticker_w_, cb.ext[3] = ticker_h_;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        ctx->Unmap(cb_.Get(), 0);
    }
    ID3D11ShaderResourceView* none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_[effect].Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(5, 1, cb_.GetAddressOf());
    ID3D11SamplerState* s[4] = {samp_[0].Get(), samp_[1].Get(), samp_[2].Get(), samp_[3].Get()};
    ctx->PSSetSamplers(4, 4, s);
    ID3D11ShaderResourceView* srv[3] = {src, nullptr, aux};
    ctx->PSSetShaderResources(0, 3, srv);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 3, none);
}

int FxGpu::run_chain(ID3D11DeviceContext* ctx, const Target pair[2], int w, int h, int cur,
                     const protocol::FxSlotState (&slots)[protocol::kFxSlots], int chain) {
    if (!lib_ || chain < 0 || chain >= kChains) return cur;
    ID3D11VertexShader* prev_vs = nullptr;  // restored at the end (the engine sets its VS once per frame)
    bool ran = false;
    for (int k = 0; k < protocol::kFxSlots; ++k) {
        const protocol::FxSlotState& s = slots[k];
        if (!active(s)) {
            if (s.effect < 0) rt_[chain][k].effect = -1;  // an emptied slot forgets its envelopes
            continue;
        }
        const EffectMeta& m = lib_->meta(s.effect);
        // D131: this slot's row of the reaction table (layers 0..7, decks 8..9, master 10).
        const protocol::SlotReactions* row = nullptr;
        if (react_) {
            if (chain < chain_deck(0)) row = &react_->layer[chain / protocol::kLayersPerDeck][chain % protocol::kLayersPerDeck][k];
            else if (chain < kChainMaster) row = &react_->deck[chain - chain_deck(0)][k];
            else if (chain == kChainMaster) row = &react_->master[k];
        }
        Reaction reacts[kParams];
        if (row) reactions_from(*row, reacts);
        const Resolved r = resolve(m, s.effect, s, rt_[chain][k], globals_, ctx_, row ? reacts : nullptr);
        rt_[chain][k].frame = frame_;
        if (r.mix <= 0.001f) continue;
        if (!ran) {
            ctx->VSGetShader(&prev_vs, nullptr, nullptr);
            ran = true;
        }
        ID3D11ShaderResourceView* aux = nullptr;
        int fb = -1;
        if (m.state == State::Feedback && fb_w_ == w && fb_h_ == h) {
            fb = feedback_for(ctx, chain, k);
            if (fb >= 0) aux = fb_[fb].srv.Get();
        }
        draw(ctx, s.effect, r, pair[cur].srv, pair[1 - cur].rtv, w, h, aux);
        cur = 1 - cur;
        if (fb >= 0 && pair[cur].tex) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopyResource(fb_[fb].tex.Get(), pair[cur].tex);
        }
    }
    if (ran) {
        ctx->VSSetShader(prev_vs, nullptr, 0);
        if (prev_vs) prev_vs->Release();
    }
    return cur;
}

}  // namespace himmel::fx
