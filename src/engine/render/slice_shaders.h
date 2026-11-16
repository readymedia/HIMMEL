// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// HLSL for the slice engine (himmel_render.exe). Compiled once at start-up
// (D3DCompile), never in the frame loop.
//
// Passes (all full-screen triangles, vs_full):
//   ps_source  - one layer's source (HAP clip / placeholder colour) as RGBA, for a layer with FX
//   ps_layer   - composite a layer source over the deck: blend mode + opacity (reads the deck so far)
//   ps_fx      - the uber effect: one of fx::Effect, 4 params, dry/wet mix
//   ps_mix     - deck A / deck B through the crossfader into MAIN
//   ps_blit    - MAIN (its part for this output) onto the swap chain, black, status overlay
//   ps_down    - a box-filtered small copy for the preview block (BGRA target)
//   ps_trans   - a layer's FADE transition: the old picture (kept) -> the new source (M2)
#pragma once

namespace himmel::engine {

inline constexpr char kSliceShaderSource[] = R"HLSL(
cbuffer Layer : register(b0) {
    float4 l_xform;    // canvas uv -> source uv: scale.xy, offset.zw
    float4 l_info;     // tex scale x, y, mode, opacity
                       // mode: 0 HAP rgb, 1 HAP rgba, 2 Hap Q YCoCg, 3 HAP alpha only, 4 solid colour, 5 render target
    float4 l_color;    // solid colour (placeholder)
    float4 l_blend;    // x = BlendMode
};
cbuffer Fx : register(b1) {
    float4 f_param;    // params 0..3, 0..1
    float4 f_info;     // effect id, mix, time (s), beats
    float4 f_size;     // w, h, 1/w, 1/h of the target
};
cbuffer Mix : register(b2) {
    float4 m_info;     // crossfader 0 (A) .. 1 (B)
};
cbuffer Blit : register(b3) {
    float4 b_src;      // canvas uv: offset.xy, scale.zw
    float4 b_overlay;  // overlay rect in output pixels: x, y, w, h (w = 0: none)
    float4 b_info;     // x = master gain (0 = black)
};
cbuffer Down : register(b4) {
    float4 d_src;      // source uv: offset.xy, scale.zw
    float4 d_texel;    // 1/w, 1/h of the SOURCE texture, taps step
};

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
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
    if (mode > 4.5) return tex1.Sample(lin, uv);          // render target (layer with FX), identity
    if (mode > 3.5) return float4(l_color.rgb, 1);         // placeholder colour
    float2 cu = (uv - 0.5) * l_xform.xy + 0.5 + l_xform.zw;
    if (any(cu < 0) || any(cu > 1)) return 0;
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

// ---------------------------------------------------------------- effects
// Ids follow himmel::engine::fx::Effect (src/engine/include/himmel/engine/fx_catalog.h).
float3 hue_turn(float3 c, float a) {
    const float3x3 toYiq = float3x3(0.299, 0.587, 0.114, 0.596, -0.274, -0.322, 0.211, -0.523, 0.312);
    const float3x3 toRgb = float3x3(1.0, 0.956, 0.621, 1.0, -0.272, -0.647, 1.0, -1.106, 1.703);
    float3 yiq = mul(toYiq, c);
    float s, co;
    sincos(a * 2 * kPi, s, co);
    yiq.yz = float2(yiq.y * co - yiq.z * s, yiq.y * s + yiq.z * co);
    return mul(toRgb, yiq);
}

float4 ps_fx(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float4 p = f_param;
    int e = (int)(f_info.x + 0.5);
    float aspect = f_size.x * f_size.w;
    float4 src = tex0.Sample(lin, uv);
    float4 col = src;

    if (e == 0) {                // HUE / SAT: hue turn, saturation 0..2x, brightness -/+0.5, contrast 0.25..4x
        float3 c = hue_turn(src.rgb, p.x);
        c = lerp(luma(c).xxx, c, p.y * 2);
        c = (c - 0.5) * exp2((p.w - 0.5) * 4) + 0.5 + (p.z - 0.5);
        col.rgb = c;
    } else if (e == 1) {         // INVERT: amount, luma only
        float3 inv = lerp(1 - src.rgb, src.rgb + (1 - 2 * luma(src.rgb)), p.y);
        col.rgb = lerp(src.rgb, inv, p.x);
    } else if (e == 2) {         // MIRROR: mode (left / right / top / bottom / quad), centre
        int m = (int)min(4, floor(p.x * 5));
        float c = p.y;
        float2 q = uv;
        if (m == 0 || m == 4) q.x = q.x > c ? 2 * c - q.x : q.x;   // left half to the right
        if (m == 1) q.x = q.x < c ? 2 * c - q.x : q.x;             // right half to the left
        if (m == 2 || m == 4) q.y = q.y > c ? 2 * c - q.y : q.y;   // top half down
        if (m == 3) q.y = q.y < c ? 2 * c - q.y : q.y;             // bottom half up
        col = tex0.Sample(lin, saturate(q));
    } else if (e == 3) {         // KALEIDO: 2..16 segments, turn, zoom 0.25..4x
        float2 d = (uv - 0.5) * float2(aspect, 1);
        float n = 2 + floor(p.x * 14.999);
        float seg = 2 * kPi / n;
        float a = atan2(d.y, d.x) + p.y * 2 * kPi;
        a = abs(fmod(a + 64 * kPi, seg) - seg * 0.5);
        float r = length(d) / exp2((p.z - 0.5) * 4);
        float2 q = float2(cos(a), sin(a)) * r;
        col = tex0.Sample(wrap, q / float2(aspect, 1) + 0.5);
    } else if (e == 4) {         // RGB SPLIT: amount (0..5 % of the width), angle
        float s, c;
        sincos(p.y * 2 * kPi, s, c);
        float2 o = float2(c, s * aspect) * p.x * 0.05;
        col.r = tex0.Sample(lin, uv + o).r;
        col.b = tex0.Sample(lin, uv - o).b;
    } else if (e == 5) {         // ROTOZOOM: zoom 0.25..4x, angle, spin (centre = still, +/- 0.5 turn/s), tile
        float z = exp2((p.x - 0.5) * 4);
        float a = (p.y + (p.z - 0.5) * f_info.z) * 2 * kPi;
        float s, c;
        sincos(a, s, c);
        float2 d = (uv - 0.5) * float2(aspect, 1) / z;
        d = float2(d.x * c - d.y * s, d.x * s + d.y * c);
        float2 q = d / float2(aspect, 1) + 0.5;
        if (p.w > 0.5) col = tex0.Sample(wrap, q);
        else col = (any(q < 0) || any(q > 1)) ? float4(0, 0, 0, 0) : tex0.Sample(lin, q);
    } else if (e == 6) {         // STROBE: rate (4 / 2 / 1 / 1/2 / 1/4 beats), flash colour black..white, duty
        float div = exp2(2 - min(4, floor(p.x * 5)));
        float ph = frac(f_info.w / div);
        col.rgb = ph < lerp(0.05, 0.95, p.z) ? p.y.xxx : src.rgb;
    } else if (e == 7) {         // PIXELATE: cell 1..64 px, posterize (0 = off, 1 = 2 levels)
        float cell = 1 + floor(p.x * p.x * 63);
        float2 px = (floor(uv * f_size.xy / cell) + 0.5) * cell * f_size.zw;
        col = tex0.Sample(pnt, px);
        if (p.y > 0.01) {
            float lv = 2 + floor((1 - p.y) * 14);
            col.rgb = floor(col.rgb * lv) / (lv - 1);
        }
    }
    return float4(lerp(src, saturate(col), f_info.y));
}

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

float4 ps_blit(VSOut i) : SV_Target {
    float3 col = tex0.Sample(lin, b_src.xy + i.uv * b_src.zw).rgb * b_info.x;
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
