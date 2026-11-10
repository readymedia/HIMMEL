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
