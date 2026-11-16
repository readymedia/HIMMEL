// RotoZoom: zoom 0.25..4x, angle, spin (centre = still, +/- 0.5 turn/s), tile (clamp / repeat).
float4 fx(float2 uv, float4 s) {
    float asp = aspect();
    float z = exp2((P.x - 0.5) * 4);
    float2 d = rot((uv - 0.5) * float2(asp, 1) / z, (P.y + (P.z - 0.5) * I.y) * kTau);
    float2 q = d / float2(asp, 1) + 0.5;
    return step_of(P.w, 2) > 0.5 ? tex_mirror(q) : tex_clip(q);
}
