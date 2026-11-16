// Mirror: mode (left / right / top / bottom / quad), centre of the fold.
float4 fx(float2 uv, float4 s) {
    int m = (int)step_of(P.x, 5);
    float c = P.y;
    float2 q = uv;
    if (m == 0 || m == 4) q.x = q.x > c ? 2 * c - q.x : q.x;
    if (m == 1) q.x = q.x < c ? 2 * c - q.x : q.x;
    if (m == 2 || m == 4) q.y = q.y > c ? 2 * c - q.y : q.y;
    if (m == 3) q.y = q.y < c ? 2 * c - q.y : q.y;
    return tex(saturate(q));
}
