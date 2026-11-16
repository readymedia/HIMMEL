// Hue / Sat: hue turn (+ a spin speed), saturation 0..2x.
float4 fx(float2 uv, float4 s) {
    float a = P.x + (P.z - 0.5) * I.y;
    float3 c = hue_turn(s.rgb, a);
    c = lerp(luma(c).xxx, c, P.y * 2);
    return float4(c, s.a);
}
