// Blur: one pass, 24 bilinear taps on a golden-angle spiral (round) or along a line.
// Size 0..3 % of the height (linear). A small per-pixel turn of the pattern hides banding.
float4 fx(float2 uv, float4 s) {
    float r = P.x * 0.03;
    if (r < 0.0003) return s;
    float2 sc = float2(S.y * S.z, 1) * r;        // height units -> uv
    float jitter = hash12(uv * S.xy) * 2.39996;   // within one tap step: neighbours keep their taps together (cache)
    float4 acc = s;
    float wsum = 1;
    if (step_of(P.z, 2) < 0.5) {
        [unroll] for (int k = 1; k <= 24; ++k) {
            float rr = sqrt(k / 24.0);
            float a = k * 2.39996 + jitter;
            float w = 1.2 - rr;
            acc += tex(uv + float2(cos(a), sin(a)) * rr * sc) * w;
            wsum += w;
        }
    } else {
        float sn, cs;
        sincos(P.y * kPi, sn, cs);
        float o = hash12(uv * S.xy + 7) - 0.5;
        [unroll] for (int k = 1; k <= 24; ++k) {
            float t = ((k + o) / 24.0) * 2 - 1;
            acc += tex(uv + float2(cs, sn) * t * sc);
            wsum += 1;
        }
    }
    return acc / wsum;
}
