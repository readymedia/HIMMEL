// Glow / bloom in one pass: 16 taps on a spiral, keep what is over the threshold,
// add it back with a soft falloff. Size 0..4 % of the height.
float4 fx(float2 uv, float4 s) {
    float r = 0.002 + P.z * P.z * 0.04;
    float2 sc = float2(S.y * S.z, 1) * r;
    float jitter = 0;   // no per-pixel jitter: neighbours share taps (cache); measured 0.35 vs 0.99 ms at size max
    float thr = P.y;
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int k = 0; k < 16; ++k) {
        float rr = sqrt((k + 0.5) / 16.0);
        float a = k * 2.39996 + jitter;
        float3 c = tex(uv + float2(cos(a), sin(a)) * rr * sc).rgb;
        float w = 1 - rr * 0.8;
        acc += max(0, c - thr) / max(0.05, 1 - thr) * w;
        wsum += w;
    }
    float3 g = acc / wsum;
    return float4(s.rgb + g * P.x * 2, max(s.a, saturate(luma(g) * P.x * 2)));
}
