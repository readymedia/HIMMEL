// Lines and stripes: count 1..41, speed -/+2 lines/s, angle, width (share of the gap).
float4 fx(float2 uv, float4 s) {
    float sn, cs;
    sincos(P.z * kPi, sn, cs);
    float n = 1 + P.x * 40;
    float d = dot((uv - 0.5) * float2(aspect(), 1), float2(-sn, cs)) + 0.5;
    float v = frac(d * n + I.y * (P.y - 0.5) * 4);
    float w = max(0.01, P.w) * 0.5;
    float aa = n * S.w * 1.5;
    float a = smoothstep(w + aa, w, abs(v - 0.5));
    return over(s, 1.0.xxx, a);
}
