// Wave: frequency 2..32, height 0..5 % of the height, speed -/+, direction.
float4 fx(float2 uv, float4 s) {
    float asp = aspect();
    float sn, cs;
    sincos(P.w * kPi, sn, cs);
    float2 dir = float2(cs, sn);
    float2 perp = float2(-dir.y, dir.x);
    float2 p = (uv - 0.5) * float2(asp, 1);
    float k = 2 + P.x * 30;
    float off = sin(dot(p, perp) * k + I.y * (P.z - 0.5) * 8) * P.y * 0.05;
    return tex_mirror(uv + dir * off / float2(asp, 1));
}
