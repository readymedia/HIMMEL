// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Entry point appended after the effect body (see _common.hlsl).
float4 ps_fx(VSOut i) : SV_Target {
    float4 s = src.SampleLevel(sLin, i.uv, 0);
    float4 c = fx(i.uv, s);
    return lerp(s, saturate(c), I.x);
}
