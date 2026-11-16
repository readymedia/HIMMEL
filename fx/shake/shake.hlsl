// Shake: amount (0..4 % of the height), speed 1..21 Hz, a small turn. Zooms in a
// little with the amount so the edges stay covered.
float4 fx(float2 uv, float4 s) {
    float asp = aspect();
    float t = I.y * (1 + P.y * 20);
    float2 o = (float2(vnoise1(t), vnoise1(t + 17.3)) * 2 - 1) * P.x * 0.04;
    float a = (vnoise1(t + 41.7) * 2 - 1) * P.z * 0.05;
    float2 d = rot((uv - 0.5) * float2(asp, 1), a) / (1 + P.x * 0.1);
    return tex_mirror(d / float2(asp, 1) + 0.5 + o);
}
