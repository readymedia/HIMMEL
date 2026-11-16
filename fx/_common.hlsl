// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Shared prelude for every Himmel effect / generator shader (fx/<id>/<id>.hlsl).
// The engine compiles  _common.hlsl + <id>.hlsl + the ps_fx entry below  once per
// effect at start-up (never in the frame loop). Each effect file defines
//     float4 fx(float2 uv, float4 s)
// uv = canvas uv (0..1, y down), s = the source pixel at uv (straight alpha).
// It returns the wet picture; ps_fx mixes it with s by the slot's MIX.
// All parameters arrive resolved (knob + scope + PRESS + reaction + flash cap),
// normalised 0..1, in P (docs/interfaces/effect-metadata.md §4).

cbuffer FxCB : register(b5) {
    float4 P;   // params 1..4, resolved, 0..1
    float4 I;   // x mix 0..1, y time s, z beats (show clock), w BPM
    float4 S;   // target w, h, 1/w, 1/h
    float4 X;   // x flash gate 0/1 (flash effects), y PRESS 0..1, z aux width px, w aux height px
};
Texture2D src : register(t0);
Texture2D aux : register(t2);          // feedback (trails) or the text strip (ticker)
SamplerState sLin : register(s4);      // linear, clamp
SamplerState sPnt : register(s5);      // point, clamp
SamplerState sRep : register(s6);      // linear, wrap
SamplerState sMir : register(s7);      // linear, mirror

static const float kPi = 3.14159265;
static const float kTau = 6.2831853;

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_fx(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float aspect() { return S.x * S.w; }
float4 tex(float2 uv) { return src.SampleLevel(sLin, uv, 0); }
float4 tex_mirror(float2 uv) { return src.SampleLevel(sMir, uv, 0); }
float4 tex_clip(float2 uv) { return (any(uv < 0) || any(uv > 1)) ? float4(0, 0, 0, 0) : src.SampleLevel(sLin, uv, 0); }
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 hsv(float h, float s, float v) {
    float3 k = saturate(abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1);
    return v * lerp(1.0.xxx, k, s);
}
float3 hue_turn(float3 c, float a) {
    const float3x3 toYiq = float3x3(0.299, 0.587, 0.114, 0.596, -0.274, -0.322, 0.211, -0.523, 0.312);
    const float3x3 toRgb = float3x3(1.0, 0.956, 0.621, 1.0, -0.272, -0.647, 1.0, -1.106, 1.703);
    float3 yiq = mul(toYiq, c);
    float sn, cs;
    sincos(a * kTau, sn, cs);
    yiq.yz = float2(yiq.y * cs - yiq.z * sn, yiq.y * sn + yiq.z * cs);
    return mul(toRgb, yiq);
}
float2 rot(float2 v, float a) { float sn, cs; sincos(a, sn, cs); return float2(v.x * cs - v.y * sn, v.x * sn + v.y * cs); }
// A stepped knob: 0..1 -> 0..n-1 (the same maths as himmel::fx::step_index in C++).
float step_of(float p, float n) { return min(n - 1, floor(saturate(p) * n)); }
float hash12(float2 p) { float3 q = frac(float3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }
float hash11(float p) { p = frac(p * 0.1031); p *= p + 33.33; p *= p + p; return frac(p); }
float vnoise1(float x) { float i = floor(x), f = frac(x); f = f * f * (3 - 2 * f); return lerp(hash11(i), hash11(i + 1), f); }
float vnoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), f.x), lerp(hash12(i + float2(0, 1)), hash12(i + 1), f.x), f.y);
}
// Generators paint over what is below them (an empty layer = transparent).
float4 over(float4 s, float3 rgb, float a) { a = saturate(a); return float4(lerp(s.rgb, rgb, a), max(s.a, a)); }
