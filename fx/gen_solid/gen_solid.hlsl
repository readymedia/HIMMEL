// Solid colour / gradient: hue, angle, hue spread across the picture, brightness.
float4 fx(float2 uv, float4 s) {
    float sn, cs;
    sincos(P.y * kTau, sn, cs);
    float t = dot((uv - 0.5) * float2(aspect(), 1), float2(cs, sn));
    float3 c = hsv(P.x + t * P.z, 0.75, P.w);
    return over(s, c, 1);
}
