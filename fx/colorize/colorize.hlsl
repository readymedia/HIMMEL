// Colorize / duotone: dark hue, light hue, saturation, contrast of the mapping.
float4 fx(float2 uv, float4 s) {
    float l = saturate((luma(s.rgb) - 0.5) * exp2((P.w - 0.5) * 2) + 0.5);
    float3 d = hsv(P.x, P.z, 0.12 + 0.25 * P.z);
    float3 b = hsv(P.y, P.z * 0.7, 1.0);
    return float4(lerp(d, b, smoothstep(0, 1, l)) * (0.2 + 0.8 * smoothstep(0, 0.35, l)), s.a);
}
