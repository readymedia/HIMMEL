// Kaleido: 2..16 segments, turn, zoom 0.25..4x, spin.
float4 fx(float2 uv, float4 s) {
    float asp = aspect();
    float2 d = (uv - 0.5) * float2(asp, 1);
    float n = 2 + step_of(P.x, 15);
    float seg = kTau / n;
    float a = atan2(d.y, d.x) + (P.y + (P.w - 0.5) * I.y) * kTau;
    a = abs(fmod(a + 64 * kPi, seg) - seg * 0.5);
    float r = length(d) / exp2((P.z - 0.5) * 4);
    float2 q = float2(cos(a), sin(a)) * r;
    return tex_mirror(q / float2(asp, 1) + 0.5);
}
