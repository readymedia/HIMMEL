// Strobe: the on/off gate is computed on the CPU (himmel::fx::resolve, so the
// optional flash cap is exact); X.x = 1 while flashing. COLOR black..white, LEVEL.
float4 fx(float2 uv, float4 s) {
    float g = X.x * P.w;
    return float4(lerp(s.rgb, P.y.xxx, g), max(s.a, g));
}
