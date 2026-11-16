// Invert: amount, luma only (0 = invert RGB, 1 = invert the brightness, keep the hue).
float4 fx(float2 uv, float4 s) {
    float3 inv = lerp(1 - s.rgb, s.rgb + (1 - 2 * luma(s.rgb)), P.y);
    return float4(lerp(s.rgb, inv, P.x), s.a);
}
