// Brightness -/+0.5, contrast 0.25..4x round mid-grey, gamma 0.35..2.8.
float4 fx(float2 uv, float4 s) {
    float3 c = (s.rgb - 0.5) * exp2((P.y - 0.5) * 4) + 0.5 + (P.x - 0.5);
    c = pow(saturate(c), exp2((P.z - 0.5) * 3));
    return float4(c, s.a);
}
