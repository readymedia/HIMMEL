// RGB split: distance (0..5 % of the width), angle, spin.
float4 fx(float2 uv, float4 s) {
    float sn, cs;
    sincos((P.y + (P.z - 0.5) * I.y) * kTau, sn, cs);
    float2 o = float2(cs, sn * aspect()) * P.x * 0.05;
    float4 r = tex(uv + o), b = tex(uv - o);
    return float4(r.r, s.g, b.b, max(s.a, max(r.a, b.a)));
}
