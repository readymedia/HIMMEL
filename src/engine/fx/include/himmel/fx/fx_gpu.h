// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Runs FX chains on the GPU: one pixel shader per effect / generator (from fx/),
// all compiled at start-up in worker threads (never in the frame loop).
//
// The render process owns the ping-pong targets; FxGpu draws one full-screen
// pass per active slot into them. It binds only its own registers (VS, PS,
// b5, s4-s7, t0, t2) and restores the vertex shader it found, so the engine's
// passes are not disturbed.
//
// Frame loop rules: run_chain() resolves (pure maths), maps one constant
// buffer per pass and draws. No allocation, no locks, no I/O. Feedback
// textures (trails) come from a pool made at resize() time.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "himmel/fx/fx.h"

namespace himmel::fx {

// A render target the engine owns (texture + views), canvas sized.
struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};

// Chains (one runtime state per slot): layers 0..7 (deck*4 + layer), deck A 8, deck B 9,
// master 10, outputs 11..15 (later). See chain_layer() etc.
inline constexpr int kChains = 16;
inline constexpr int kFeedbackPool = 4;   // GUESS: 4 trails at once is plenty in 0.1 (more = pass-through)
inline constexpr int chain_layer(int deck, int layer) { return deck * protocol::kLayersPerDeck + layer; }
inline constexpr int chain_deck(int deck) { return 8 + deck; }
inline constexpr int kChainMaster = 10;

class FxGpu {
public:
    FxGpu();
    ~FxGpu();
    FxGpu(const FxGpu&) = delete;
    FxGpu& operator=(const FxGpu&) = delete;

    // Start-up: compiles every effect of `lib` (parallel), makes the sampler states,
    // the constant buffer and the ticker text strip. A shader that fails is logged
    // into `report` and that effect passes the picture through (show-safe).
    bool init(ID3D11Device* dev, const Library& lib, const char* ticker_text, std::string* report = nullptr);
    // Canvas size changed (start-up / window resize, never per frame): feedback pool.
    bool resize(int w, int h);

    // Once per frame, before the first chain.
    void begin_frame(const FrameCtx& ctx);
    const FrameCtx& frame() const { return ctx_; }

    // Runs the active slots of one chain on the ping-pong pair; returns the index of
    // the target holding the result (== cur when nothing ran).
    int run_chain(ID3D11DeviceContext* ctx, const Target pair[2], int w, int h, int cur,
                  const protocol::FxSlotState (&slots)[protocol::kFxSlots], int chain);

    // One pass of one effect with explicit values (fx_check, tests). `aux` may be null.
    void draw(ID3D11DeviceContext* ctx, int effect, const Resolved& r, ID3D11ShaderResourceView* src,
              ID3D11RenderTargetView* dst, int w, int h, ID3D11ShaderResourceView* aux = nullptr);

    bool active(const protocol::FxSlotState& s) const;
    // True if an active slot holds a generator (an empty layer still renders then).
    bool has_generator(const protocol::FxSlotState (&slots)[protocol::kFxSlots]) const;

    Globals& globals() { return globals_; }
    SlotRuntime& runtime(int chain, int slot) { return rt_[chain][slot]; }
    const Library& library() const { return *lib_; }
    bool shader_ok(int effect) const { return effect >= 0 && effect < kMaxEffects && ps_[effect]; }
    double compile_ms() const { return compile_ms_; }
    ID3D11ShaderResourceView* ticker_srv() const { return ticker_srv_.Get(); }
    float ticker_w() const { return ticker_w_; }
    float ticker_h() const { return ticker_h_; }

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    struct Feedback {
        Com<ID3D11Texture2D> tex;
        Com<ID3D11RenderTargetView> rtv;
        Com<ID3D11ShaderResourceView> srv;
        int chain = -1, slot = -1;
        uint64_t last_frame = 0;
    };
    int feedback_for(ID3D11DeviceContext* ctx, int chain, int slot);

    const Library* lib_ = nullptr;
    Com<ID3D11Device> dev_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_[kMaxEffects];
    Com<ID3D11Buffer> cb_;
    Com<ID3D11SamplerState> samp_[4];
    Com<ID3D11Texture2D> ticker_tex_;
    Com<ID3D11ShaderResourceView> ticker_srv_;
    float ticker_w_ = 0, ticker_h_ = 0;
    int ticker_id_ = -1;
    Feedback fb_[kFeedbackPool];
    int fb_w_ = 0, fb_h_ = 0;
    FrameCtx ctx_;
    uint64_t frame_ = 0;
    Globals globals_;
    SlotRuntime rt_[kChains][protocol::kFxSlots];
    double compile_ms_ = 0;
};

}  // namespace himmel::fx
