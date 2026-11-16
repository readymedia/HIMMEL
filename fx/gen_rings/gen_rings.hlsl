// Rings / circles: count 1..25 over the half-height, a pulse on every beat, speed, width.
float4 fx(float2 uv, float4 s) {
    float r = length((uv - 0.5) * float2(aspect(), 1)) * 2;
    float n = 1 + P.x * 24;
    float v = frac(r * n - I.y * (P.z - 0.5) * 2);
    float pulse = P.y * exp(-frac(I.z) * 5);
    float w = saturate(max(0.01, P.w) * 0.5 + pulse * 0.3);
    float aa = n * S.w * 3;
    float a = smoothstep(w + aa, w, abs(v - 0.5));
    return over(s, 1.0.xxx, a);
}
