// Zoom pulse: amount (0..60 % zoom), a pulse on the show clock (off / 4 / 2 / 1 / 1/2 beats),
// centre. With BEAT off it is a steady zoom that the kick reaction pumps.
float4 fx(float2 uv, float4 s) {
    float b = step_of(P.y, 5);
    float pulse = 1;
    if (b > 0.5) {
        float div = exp2(3 - b);
        pulse = exp(-frac(I.z / div) * 5);
    }
    float2 c = float2(P.z, P.w);
    return tex_clip((uv - c) / (1 + P.x * 0.6 * pulse) + c);
}
