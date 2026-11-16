// Posterize: 2..16 levels per channel, smoothing of the steps.
float4 fx(float2 uv, float4 s) {
    float lv = 2 + step_of(P.x, 15);
    float3 q = saturate(s.rgb) * (lv - 1);
    float3 f = frac(q);
    float w = 0.001 + P.y * 0.5;
    float3 c = (floor(q) + smoothstep(0.5 - w, 0.5 + w, f)) / (lv - 1);
    return float4(c, s.a);
}
