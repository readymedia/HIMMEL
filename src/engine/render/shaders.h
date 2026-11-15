// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// HLSL for the render skeleton. Compiled once at start-up (D3DCompile), never in
// the frame loop. Later: precompiled .cso at build time.
#pragma once

namespace himmel::engine {

inline constexpr char kShaderSource[] = R"HLSL(
cbuffer Pattern : register(b0) {
    float time; float canvas_w; float canvas_h; float hue;
    float sat; float speed; float zoom; float grid;
    float bar; float master; float bar_pos; float beat;   // bar_pos 0..1, integrated on the CPU
    float4 pads[4];      // pad 1..16 flash, pad 1 = bottom-left
    float4 seams[2];     // x (pixels) where an output ends, up to 8
};
cbuffer Blit : register(b1) {
    float4 src;          // canvas uv: offset.xy, scale.zw
    float4 overlay;      // overlay rect in output pixels: x, y, w, h
    float out_master; float3 pad_;
};
cbuffer Clip : register(b2) {
    float4 xform;        // canvas uv -> clip uv: scale.xy, offset.zw
    float4 info;         // tex scale x, y, mode (0 rgb, 1 rgba, 2 Hap Q YCoCg, 3 alpha), opacity
};
cbuffer Camera : register(b3) {
    float4 cam_xform;    // canvas uv -> camera uv: scale.xy (x < 0 = mirror), offset.zw
    float4 cam_info;     // brightness, contrast, -, opacity
};
Texture2D canvas_tex : register(t0);
Texture2D cam_y : register(t2);
Texture2D cam_uv : register(t3);
Texture2D overlay_tex : register(t1);
SamplerState lin : register(s0);
SamplerState pnt : register(s1);

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

// Test card generator: shows seams, judder (moving bar), grid scale, pads.
float4 ps_pattern(VSOut i) : SV_Target {
    float2 px = i.uv * float2(canvas_w, canvas_h);
    float3 col = hsv(hue + i.uv.x * 0.12, sat, 0.16 + 0.10 * (1 - i.uv.y));

    float cell = lerp(32.0, 256.0, zoom);
    float2 g = abs(frac(px / cell + 0.5) - 0.5) * cell;          // distance to a grid line
    col += step(min(g.x, g.y), 1.0) * grid * 0.6;

    // Colour bars across the top 5 %.
    if (i.uv.y < 0.05) col = hsv(floor(i.uv.x * 16) / 16, 0.9, 0.9);

    // Moving bar across the whole canvas (judder / tearing / span check).
    float bx = bar_pos * canvas_w;
    float bw = lerp(2.0, canvas_h, bar * bar);   // 2 px .. one canvas height
    if (abs(px.x - bx) < bw * 0.5) col = 1.0.xxx;

    // Beat pulse ring in the middle of the canvas.
    float r = length((px - float2(canvas_w, canvas_h) * 0.5) / canvas_h);
    col += smoothstep(0.012, 0.0, abs(r - 0.25 - 0.08 * beat)) * 0.8;

    // Pads: a 4x4 grid over the canvas, Pad 1 bottom-left.
    int cx = min(3, int(i.uv.x * 4));
    int cy = 3 - min(3, int(i.uv.y * 4));
    int p = cy * 4 + cx;
    float f = pads[p / 4][p % 4];
    col += hsv(hue + p / 16.0, 0.6, 1.0) * f;

    // Output seams: a thin red line where one output ends and the next begins.
    for (int s = 0; s < 7; ++s) {
        float sx = seams[s / 4][s % 4];
        if (sx > 0 && abs(px.x - sx) < 1.5) col = float3(1, 0, 0);
    }
    return float4(col * master, 1);
}

// A HAP clip layer (BC texture straight from the decoder), blended over the canvas.
float4 ps_clip(VSOut i) : SV_Target {
    float2 cu = (i.uv - 0.5) * xform.xy + 0.5 + xform.zw;
    if (any(cu < 0) || any(cu > 1)) return 0;
    float4 c = canvas_tex.Sample(lin, cu * info.xy);
    float3 rgb = c.rgb;
    float a = 1;
    if (info.z > 2.5) { rgb = c.rrr; }
    else if (info.z > 1.5) {          // Hap Q: scaled YCoCg in DXT5 (Vidvox reference maths)
        float scale = c.b * (255.0 / 8.0) + 1.0;
        float co = (c.r - 0.5 * 256.0 / 255.0) / scale;
        float cg = (c.g - 0.5 * 256.0 / 255.0) / scale;
        rgb = float3(c.a + co - cg, c.a + cg, c.a - co - cg);
    }
    else if (info.z > 0.5) { a = c.a; }
    return float4(saturate(rgb), a * info.w);
}

// Camera layer: NV12 (Y + interleaved UV), BT.709 limited range -> RGB.
float4 ps_camera(VSOut i) : SV_Target {
    float2 cu = (i.uv - 0.5) * cam_xform.xy + 0.5 + cam_xform.zw;
    if (any(cu < 0) || any(cu > 1)) return 0;
    float y = (cam_y.Sample(lin, cu).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = (cam_uv.Sample(lin, cu).rg - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
    rgb = (rgb - 0.5) * cam_info.y + 0.5 + cam_info.x;
    return float4(saturate(rgb), cam_info.w);
}

// ---------------------------------------------------------------- Touch FX
// One full-screen pass over a copy of the canvas (docs/touch-fx-entrancer.md).
// Effect ids follow himmel::engine::touchfx::Effect. x, y, mix are 0..1.
cbuffer TouchFx : register(b4) {
    float fx_x; float fx_y; float fx_mix; float fx_time;
    float fx_effect; float fx_beat; float fx_w; float fx_h;   // fx_beat = beats since start (fake 120 BPM for now)
};
Texture2D fx_src : register(t4);   // the canvas before the effect
Texture2D fx_prev : register(t5);  // last output (Trails)

static const float kPi = 3.14159265;

float fx_luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 fx_sample(float2 uv) { return fx_src.Sample(lin, uv).rgb; }

// Rotates the hue by a turns (YIQ rotation: cheap, no branches).
float3 fx_hue(float3 c, float a) {
    const float3x3 toYiq = float3x3(0.299, 0.587, 0.114, 0.596, -0.274, -0.322, 0.211, -0.523, 0.312);
    const float3x3 toRgb = float3x3(1.0, 0.956, 0.621, 1.0, -0.272, -0.647, 1.0, -1.106, 1.703);
    float3 yiq = mul(toYiq, c);
    float s, co;
    sincos(a * 2 * kPi, s, co);
    yiq.yz = float2(yiq.y * co - yiq.z * s, yiq.y * s + yiq.z * co);
    return mul(toRgb, yiq);
}

float4 ps_touchfx(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float aspect = fx_w / fx_h;
    float3 src = fx_sample(uv);
    float3 col = src;
    float mix_ = fx_mix;
    int e = (int)(fx_effect + 0.5);

    if (e == 0) {            // HUE TURN: X = hue rotation (one full turn), Y = saturation 0..2x
        float3 c = fx_hue(src, fx_x);
        col = lerp(fx_luma(c).xxx, c, fx_y * 2);
    } else if (e == 1) {     // DUOTONE: X = dark colour, Y = light colour
        float l = smoothstep(0.05, 0.95, fx_luma(src));
        col = lerp(hsv(fx_x, 0.85, 0.18), hsv(fx_y, 0.55, 1.0), l);
    } else if (e == 2) {     // GRID: X = 1..16 tiles across, Y < 0.5 repeat, > 0.5 mirror every other tile
        float n = 1 + floor(fx_x * 15.999);
        float2 t = uv * n;                      // n x n copies of the whole picture: shape kept
        float2 f = frac(t);
        float2 odd = fmod(floor(t), 2);
        float m = saturate((fx_y - 0.5) * 4);   // 0 = repeat, 1 = mirror
        f = lerp(f, lerp(f, 1 - f, odd), m);
        col = fx_sample(f);
    } else if (e == 3) {     // KALEIDO: X = 2..16 segments, Y = turn
        float2 p = (uv - 0.5) * float2(aspect, 1);
        float n = 2 + floor(fx_x * 14.999);
        float seg = 2 * kPi / n;
        float a = atan2(p.y, p.x) + fx_y * 2 * kPi;
        a = abs(fmod(a + 64 * kPi, seg) - seg * 0.5);    // fold into one mirrored wedge
        float r = length(p);
        float2 q = float2(cos(a), sin(a)) * r;
        col = fx_sample(q / float2(aspect, 1) + 0.5);
    } else if (e == 4) {     // BULGE: X, Y = centre, mix = strength (no crossfade)
        float2 c = float2(fx_x, 1 - fx_y);
        float2 d = (uv - c) * float2(aspect, 1);
        float r = length(d) / 0.35;
        float k = r < 1 ? lerp(1.0, pow(r, 0.5), fx_mix) : 1.0;
        col = fx_sample(c + d * k / float2(aspect, 1));
        mix_ = 1;
    } else if (e == 5) {     // SLIDE: X, Y = shift (centre = none), wraps around
        col = fx_sample(frac(uv - float2(fx_x - 0.5, 0.5 - fx_y)));
    } else if (e == 6) {     // WAVES: X = frequency, Y = height, rolling sideways
        float f = lerp(1, 40, fx_x * fx_x);
        float h = fx_y * 0.08;
        float2 o = float2(sin((uv.y * f + fx_time * 0.8) * 2 * kPi), sin((uv.x * aspect * f * 0.5 + fx_time * 0.6) * 2 * kPi) * 0.5) * h;
        col = fx_sample(uv + o);
    } else if (e == 7) {     // STROBE: X = rate (4 / 2 / 1 / 1/2 / 1/4 beats), Y = flash black .. white
        float div = exp2(2 - floor(fx_x * 4.999));
        float ph = frac(fx_beat / div);
        col = ph < 0.5 ? src : fx_y.xxx;
    } else {                 // TRAILS: X = decay, Y = drift (zoom out .. in per frame)
        float decay = lerp(0.70, 0.985, fx_x);
        float z = 1 - (fx_y - 0.5) * 0.04;
        float3 prev = fx_prev.Sample(lin, (uv - 0.5) * z + 0.5).rgb * decay;
        col = max(src, prev);
    }
    return float4(lerp(src, saturate(col), mix_), 1);
}

float4 ps_blit(VSOut i) : SV_Target {
    float3 col = canvas_tex.Sample(lin, src.xy + i.uv * src.zw).rgb;
    float2 opx = i.pos.xy - overlay.xy;
    if (all(opx >= 0) && all(opx < overlay.zw)) {
        float t = overlay_tex.Sample(pnt, opx / overlay.zw).r;
        col = lerp(col * 0.25, 1.0.xxx, t);
    }
    return float4(col * out_master, 1);
}
)HLSL";

}  // namespace himmel::engine
