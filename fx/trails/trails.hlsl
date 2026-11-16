// Trails: aux = this slot's previous output (a feedback texture the engine keeps).
// Decay 0.8..0.99 per frame, drift = zoom of the old picture, mode lighten / blend.
float4 fx(float2 uv, float4 s) {
    float decay = 0.8 + P.x * 0.19;
    float z = 1 + (P.y - 0.5) * 0.04;
    float4 prev = aux.SampleLevel(sLin, (uv - 0.5) / z + 0.5, 0) * decay;
    if (step_of(P.z, 2) > 0.5) return lerp(s, prev, decay * 0.9);
    return max(s, prev);
}
