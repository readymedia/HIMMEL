// Luma key: alpha from brightness (straight alpha; the layer composite uses it).
// On a deck / master there is nothing below, so keyed parts go dark instead.
float4 fx(float2 uv, float4 s) {
    float l = luma(s.rgb);
    float w = 0.002 + P.y * 0.5;
    float k = smoothstep(P.x - w, P.x + w, l);
    if (step_of(P.z, 2) > 0.5) k = 1 - k;
    return float4(s.rgb, s.a * k);
}
