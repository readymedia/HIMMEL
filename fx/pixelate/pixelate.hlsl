// Pixelate: cell 1..64 px (on a 1080-high canvas, scaled with the height), posterize.
float4 fx(float2 uv, float4 s) {
    float cell = (1 + floor(P.x * P.x * 63)) * S.y / 1080;
    float2 px = (floor(uv * S.xy / cell) + 0.5) * cell * S.zw;
    float4 c = src.SampleLevel(sPnt, px, 0);
    if (P.y > 0.01) {
        float lv = 2 + floor((1 - P.y) * 14);
        c.rgb = floor(c.rgb * lv) / (lv - 1);
    }
    return c;
}
