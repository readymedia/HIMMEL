// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// HLSL for the slice engine (himmel_render.exe). Compiled once at start-up
// (D3DCompile), never in the frame loop.
//
// Passes (all full-screen triangles, vs_full):
//   ps_source  - one layer's source (HAP clip / placeholder colour) as RGBA, for a layer with FX
//   ps_layer   - composite a layer source over the deck: blend mode + opacity (reads the deck so far)
//   ps_mix     - deck A / deck B through the crossfader into MAIN
//   ps_blit    - MAIN (its part for this output) onto the swap chain, black, edge blend, IDENTIFY,
//                status overlay (M6)
//   ps_down    - a box-filtered small copy for the preview block (BGRA target)
//   ps_trans   - a layer's FADE transition: the old picture (kept) -> the new source (M2)
#pragma once

namespace himmel::engine {

inline constexpr char kSliceShaderSource[] = R"HLSL(
cbuffer Layer : register(b0) {
    float4 l_xform;    // canvas uv -> source uv: scale.xy, offset.zw
    float4 l_info;     // tex scale x, y, mode, opacity
                       // mode: 0 HAP rgb, 1 HAP rgba, 2 Hap Q YCoCg, 3 HAP alpha only, 4 solid colour, 5 render target,
                       //       6 transparent (an empty layer under a generator)
    float4 l_color;    // solid colour (placeholder)
    float4 l_blend;    // x = BlendMode
};
cbuffer Mix : register(b2) {
    float4 m_info;     // crossfader 0 (A) .. 1 (B)
};
cbuffer Blit : register(b3) {
    float4 b_src;      // canvas uv: offset.xy, scale.zw
    float4 b_overlay;  // overlay rect in output pixels: x, y, w, h (w = 0: none)
    float4 b_info;     // x = gain (0 = black: master or this output), y = IDENTIFY alpha
    float4 b_edge;     // M6 edge blend: x = left ramp, y = right ramp (part of the output width), z = 1 / gamma
    float4 b_ident;    // M6 IDENTIFY picture rect in output pixels: x, y, w, h
};
cbuffer Down : register(b4) {
    float4 d_src;      // source uv: offset.xy, scale.zw
    float4 d_texel;    // 1/w, 1/h of the SOURCE texture, taps step
};

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex3 : register(t3);     // ps_blit: the IDENTIFY picture (R8)
SamplerState lin : register(s0);
SamplerState pnt : register(s1);
SamplerState wrap : register(s2);

static const float kPi = 3.14159265;

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_full(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 hsv(float h, float s, float v) {
    float3 k = saturate(abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1);
    return v * lerp(1.0.xxx, k, s);
}
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---------------------------------------------------------------- layer source
// tex1 = the clip's BC texture or a render target.
float4 layer_src(float2 uv) {
    float mode = l_info.z;
    if (mode > 5.5) return 0;                              // transparent
    if (mode > 4.5) return tex1.Sample(lin, uv);          // render target (layer with FX), identity
    float2 cu = (uv - 0.5) * l_xform.xy + 0.5 + l_xform.zw;  // the layer's place (M6)
    if (any(cu < 0) || any(cu > 1)) return 0;
    if (mode > 3.5) return float4(l_color.rgb, 1);         // placeholder colour
    float4 c = tex1.Sample(lin, cu * l_info.xy);
    float3 rgb = c.rgb;
    float a = 1;
    if (mode > 2.5) { rgb = c.rrr; }
    else if (mode > 1.5) {           // Hap Q: scaled YCoCg in DXT5 (Vidvox reference maths)
        float scale = c.b * (255.0 / 8.0) + 1.0;
        float co = (c.r - 0.5 * 256.0 / 255.0) / scale;
        float cg = (c.g - 0.5 * 256.0 / 255.0) / scale;
        rgb = float3(c.a + co - cg, c.a + cg, c.a - co - cg);
    }
    else if (mode > 0.5) { a = c.a; }
    return float4(saturate(rgb), a);
}

float4 ps_source(VSOut i) : SV_Target { return layer_src(i.uv); }

float3 blend(float3 d, float3 s, int m) {
    if (m == 1) return d + s;                          // add
    if (m == 2) return 1 - (1 - d) * (1 - s);          // screen
    if (m == 3) return d * s;                          // multiply
    if (m == 4) return abs(d - s);                     // difference
    if (m == 5) return max(d, s);                      // lighten
    return s;                                          // normal
}

// tex0 = the deck so far, tex1 = the layer source.
float4 ps_layer(VSOut i) : SV_Target {
    float3 d = tex0.Sample(pnt, i.uv).rgb;
    float4 s = layer_src(i.uv);
    float a = saturate(s.a * l_info.w);
    return float4(saturate(lerp(d, blend(d, s.rgb, (int)(l_blend.x + 0.5)), a)), 1);
}

// Effects live in fx/ (one shader each, himmel::fx::FxGpu, M5); they use registers b5, s4-s7, t0, t2.

// ---------------------------------------------------------------- mixer / outputs
// tex0 = the layer's old picture (kept when the transition started), tex1 = the new
// source (straight alpha), m_info.x = progress 0..1. Alpha crossfades too.
float4 ps_trans(VSOut i) : SV_Target {
    return lerp(tex0.Sample(pnt, i.uv), tex1.Sample(pnt, i.uv), saturate(m_info.x));
}

float4 ps_mix(VSOut i) : SV_Target {
    float3 a = tex0.Sample(pnt, i.uv).rgb;
    float3 b = tex1.Sample(pnt, i.uv).rgb;
    return float4(lerp(a, b, m_info.x), 1);    // linear crossfade (curves later)
}

// Edge-blend ramp: 0 at the outer edge of the overlap, 1 inside; smooth, then gamma-corrected
// so two overlapping projectors add up to one (engine.md §2.0).
float edge_gain(float u) {
    float g = 1;
    if (b_edge.x > 0) { float a = saturate(u / b_edge.x); g *= a * a * (3 - 2 * a); }
    if (b_edge.y > 0) { float a = saturate((1 - u) / b_edge.y); g *= a * a * (3 - 2 * a); }
    return pow(g, b_edge.z);
}

float4 ps_blit(VSOut i) : SV_Target {
    float3 col = tex0.Sample(lin, b_src.xy + i.uv * b_src.zw).rgb * b_info.x;
    col *= edge_gain(i.uv.x);
    float2 ipx = i.pos.xy - b_ident.xy;
    if (b_info.y > 0) {  // IDENTIFY: darken the show, the number on top (over the show if needed)
        col *= 1 - 0.6 * b_info.y;
        if (b_ident.z > 0 && all(ipx >= 0) && all(ipx < b_ident.zw)) {
            float t = tex3.Sample(pnt, ipx / b_ident.zw).r;
            col = lerp(col, float3(1, 1, 1), t * b_info.y);
        }
    }
    float2 opx = i.pos.xy - b_overlay.xy;
    if (b_overlay.z > 0 && all(opx >= 0) && all(opx < b_overlay.zw)) {
        float t = tex1.Sample(pnt, opx / b_overlay.zw).r;
        col = lerp(col * 0.25, 1.0.xxx, t);
    }
    return float4(col, 1);
}

// 4 x 4 bilinear taps spread over the source footprint of one target pixel.
float4 ps_down(VSOut i) : SV_Target {
    float2 uv = d_src.xy + i.uv * d_src.zw;
    float2 step_ = d_texel.xy * d_texel.z;
    float3 acc = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            acc += tex0.SampleLevel(lin, uv + (float2(x, y) - 1.5) * step_, 0).rgb;
    return float4(acc / 16, 1);
}
)HLSL";

}  // namespace himmel::engine
