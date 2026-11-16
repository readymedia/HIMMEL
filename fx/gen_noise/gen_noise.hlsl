// Noise / fBm clouds: 3 octaves of value noise with a slow one-sample domain warp.
float4 fx(float2 uv, float4 s) {
    float2 p = (uv - 0.5) * float2(aspect(), 1) * (1 + P.x * 12);
    float t = I.y * P.y * 0.8;
    float w = vnoise(p * 0.5 + t * 0.3);
    p += float2(w, 1 - w) * 1.5;
    float n = 0, a = 0.5;
    [unroll] for (int k = 0; k < 3; ++k) {
        n += vnoise(p + float2(t, -0.7 * t)) * a;
        p = p * 2.03 + 3.7;
        a *= 0.5;
    }
    n = saturate((n / 0.875 - 0.5) * exp2((P.w - 0.5) * 3) + 0.5);
    float3 c = hsv(P.z + n * 0.15, 0.65, n);
    return over(s, c, 1);
}
