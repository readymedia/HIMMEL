// Tile: 1..8 tiles per side, mirror every other tile, scroll speed.
float4 fx(float2 uv, float4 s) {
    float n = 1 + step_of(P.x, 8);
    float2 q = uv * n + float2((P.z - 0.5) * I.y, 0);
    float2 cell = floor(q);
    float2 f = frac(q);
    if (step_of(P.y, 2) > 0.5) f = lerp(f, 1 - f, abs(fmod(cell, 2)));
    return tex(f);
}
