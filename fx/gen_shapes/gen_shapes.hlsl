// Shapes: a regular polygon outline (3..8 sides), spin, size, 1..6 nested copies
// (every other copy turns the other way). One atan2 for all copies.
float4 fx(float2 uv, float4 s) {
    float2 p = (uv - 0.5) * float2(aspect(), 1);
    float n = 3 + step_of(P.x, 6);
    float copies = 1 + step_of(P.w, 6);
    float size = (0.05 + P.z * 0.9) * 0.5;
    float len = length(p);
    float ang0 = atan2(p.y, p.x);
    float seg = kTau / n;
    float spin = (P.y - 0.5) * I.y * kTau;
    float lw = 0.006 + 0.004 * P.z;
    float a = 0;
    [loop] for (int k = 0; k < (int)copies; ++k) {
        float ang = ang0 - (spin * (k % 2 == 0 ? 1 : -1) + k * 0.3);
        float d = len * cos(fmod(ang + 64 * kPi, seg) - seg * 0.5);   // polygon distance
        float r = size * (1 - k / (copies + 0.5));
        a = max(a, smoothstep(lw + 1.5 * S.w, lw, abs(d - r)));
    }
    return over(s, 1.0.xxx, a);
}
