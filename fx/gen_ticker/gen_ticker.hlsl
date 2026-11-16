// Text ticker: aux = a text strip (R8, X.z x X.w px, one row of 5x7 glyphs, made at
// start-up). Size 5..45 % of the height, speed -/+1 screen/s, Y position, hue.
// Bilinear + smoothstep turns the blocky pixels into soft, rounded ones at any size.
float4 fx(float2 uv, float4 s) {
    if (X.z < 1) return s;
    float h = 0.05 + P.x * 0.4;                          // text height, share of the canvas height
    float asp = aspect();
    float2 p = float2(uv.x * asp, uv.y);
    float y = (p.y - (P.z * (1 - h) + h * 0.5)) / h + 0.5; // 0..1 inside the text row
    if (y < 0 || y > 1) return s;
    float strip_w = h * X.z / X.w;                       // text width in height units
    float gap = asp * 0.25;                              // GUESS: a quarter screen between repeats
    float x = frac((p.x + I.y * (P.y - 0.5) * 2 * asp) / (strip_w + gap)) * (strip_w + gap);
    if (x > strip_w) return s;
    float v = aux.SampleLevel(sLin, float2(x / strip_w, y), 0).r;
    float a = smoothstep(0.35, 0.65, v);
    return over(s, hsv(P.w, 0.35, 1), a);
}
