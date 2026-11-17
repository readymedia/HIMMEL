// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/analysis.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace himmel::library {

namespace {

constexpr double kPi = 3.14159265358979323846;

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

struct Lab { float l, a, b; };

float srgb_lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float lin_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

Lab to_lab(float r, float g, float b) {
    r = srgb_lin(r), g = srgb_lin(g), b = srgb_lin(b);
    float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
    float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; };
    x = f(x), y = f(y), z = f(z);
    return {116.0f * y - 16.0f, 500.0f * (x - y), 200.0f * (y - z)};
}

uint32_t from_lab(const Lab& c) {
    const float fy = (c.l + 16.0f) / 116.0f, fx = fy + c.a / 500.0f, fz = fy - c.b / 200.0f;
    auto inv = [](float t) { return t * t * t > 0.008856f ? t * t * t : (t - 16.0f / 116.0f) / 7.787f; };
    const float x = inv(fx) * 0.95047f, y = inv(fy), z = inv(fz) * 1.08883f;
    const float r = 3.2406f * x - 1.5372f * y - 0.4986f * z;
    const float g = -0.9689f * x + 1.8758f * y + 0.0415f * z;
    const float b = 0.0557f * x - 0.2040f * y + 1.0570f * z;
    auto byte = [](float v) { return static_cast<uint32_t>(std::lround(clamp01(lin_srgb(clamp01(v))) * 255.0f)); };
    return (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

float mean(const std::vector<float>& v) { return v.empty() ? 0.0f : std::accumulate(v.begin(), v.end(), 0.0f) / v.size(); }

float median_of(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

int64_t to_hap(const ProxyClip& c, int proxy_frame) {
    if (c.hap_frames <= 0) return 0;
    const double f = proxy_frame * (c.hap_fps / std::max(0.001f, c.fps));
    return std::clamp<int64_t>(std::llround(f), 0, c.hap_frames - 1);
}

}  // namespace

int pow2_round(double v) {
    if (v <= 1.0) return 1;
    const double l = std::log2(v);
    return 1 << static_cast<int>(std::lround(l));
}

void luma_of(const uint8_t* p, int w, int h, std::vector<float>& y) {
    y.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < y.size(); ++i)
        y[i] = (0.2126f * p[i * 3] + 0.7152f * p[i * 3 + 1] + 0.0722f * p[i * 3 + 2]) / 255.0f;
}

float ssim(const std::vector<float>& a, const std::vector<float>& b, int w, int h) {
    constexpr float c1 = 0.01f * 0.01f, c2 = 0.03f * 0.03f;
    const int win = std::max(1, std::min({8, w, h}));
    double sum = 0;
    int n = 0;
    for (int y0 = 0; y0 + win <= h; y0 += win / 2 > 0 ? win / 2 : 1) {
        for (int x0 = 0; x0 + win <= w; x0 += win / 2 > 0 ? win / 2 : 1) {
            double ma = 0, mb = 0;
            for (int y = y0; y < y0 + win; ++y)
                for (int x = x0; x < x0 + win; ++x) ma += a[y * w + x], mb += b[y * w + x];
            const double cnt = static_cast<double>(win) * win;
            ma /= cnt, mb /= cnt;
            double va = 0, vb = 0, cov = 0;
            for (int y = y0; y < y0 + win; ++y)
                for (int x = x0; x < x0 + win; ++x) {
                    const double da = a[y * w + x] - ma, db = b[y * w + x] - mb;
                    va += da * da, vb += db * db, cov += da * db;
                }
            va /= cnt, vb /= cnt, cov /= cnt;
            sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    }
    return n ? static_cast<float>(sum / n) : 1.0f;
}

float hist_distance(const uint8_t* a, const uint8_t* b, int pixels) {
    int ha[64] = {}, hb[64] = {};
    for (int i = 0; i < pixels; ++i) {
        ha[(a[i * 3] >> 6) * 16 + (a[i * 3 + 1] >> 6) * 4 + (a[i * 3 + 2] >> 6)]++;
        hb[(b[i * 3] >> 6) * 16 + (b[i * 3 + 1] >> 6) * 4 + (b[i * 3 + 2] >> 6)]++;
    }
    int d = 0;
    for (int k = 0; k < 64; ++k) d += std::abs(ha[k] - hb[k]);
    return pixels ? d / (2.0f * pixels) : 0.0f;
}

std::vector<PaletteEntry> palette_kmeans(const ProxyClip& clip, int k) {
    const int n = clip.frames(), px = clip.w * clip.h;
    std::vector<Lab> s;
    if (n <= 0 || px <= 0) return {};
    const int use_frames = std::min(n, 32);
    const int per_frame = std::max(1, 4096 / use_frames);
    const int stride = std::max(1, px / per_frame);
    s.reserve(static_cast<size_t>(use_frames) * per_frame + 16);
    for (int f = 0; f < use_frames; ++f) {
        const uint8_t* p = clip.frame(use_frames == n ? f : static_cast<int>(static_cast<int64_t>(f) * n / use_frames));
        for (int i = 0; i < px; i += stride) s.push_back(to_lab(p[i * 3] / 255.0f, p[i * 3 + 1] / 255.0f, p[i * 3 + 2] / 255.0f));
    }
    auto d2 = [](const Lab& x, const Lab& y) { return (x.l - y.l) * (x.l - y.l) + (x.a - y.a) * (x.a - y.a) + (x.b - y.b) * (x.b - y.b); };
    // Deterministic farthest-point start: the sample nearest the mean, then the farthest ones.
    std::vector<Lab> c;
    Lab m{0, 0, 0};
    for (const Lab& v : s) m.l += v.l, m.a += v.a, m.b += v.b;
    m.l /= s.size(), m.a /= s.size(), m.b /= s.size();
    c.push_back(*std::min_element(s.begin(), s.end(), [&](const Lab& x, const Lab& y) { return d2(x, m) < d2(y, m); }));
    std::vector<float> best(s.size());
    for (size_t i = 0; i < s.size(); ++i) best[i] = d2(s[i], c[0]);
    while (static_cast<int>(c.size()) < k) {
        const size_t far = static_cast<size_t>(std::max_element(best.begin(), best.end()) - best.begin());
        if (best[far] < 4.0f) break;  // fewer distinct colours than k (dE < 2)
        c.push_back(s[far]);
        for (size_t i = 0; i < s.size(); ++i) best[i] = std::min(best[i], d2(s[i], c.back()));
    }
    std::vector<int> assign(s.size(), 0);
    std::vector<double> acc(c.size() * 4);
    for (int iter = 0; iter < 12; ++iter) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (size_t i = 0; i < s.size(); ++i) {
            int bi = 0;
            float bd = d2(s[i], c[0]);
            for (size_t j = 1; j < c.size(); ++j)
                if (const float d = d2(s[i], c[j]); d < bd) bd = d, bi = static_cast<int>(j);
            assign[i] = bi;
            acc[bi * 4] += s[i].l, acc[bi * 4 + 1] += s[i].a, acc[bi * 4 + 2] += s[i].b, acc[bi * 4 + 3] += 1;
        }
        for (size_t j = 0; j < c.size(); ++j)
            if (acc[j * 4 + 3] > 0)
                c[j] = {static_cast<float>(acc[j * 4] / acc[j * 4 + 3]), static_cast<float>(acc[j * 4 + 1] / acc[j * 4 + 3]),
                        static_cast<float>(acc[j * 4 + 2] / acc[j * 4 + 3])};
    }
    std::vector<PaletteEntry> out;
    for (size_t j = 0; j < c.size(); ++j)
        if (acc[j * 4 + 3] > 0) out.push_back({from_lab(c[j]), static_cast<float>(acc[j * 4 + 3] / s.size())});
    std::sort(out.begin(), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) { return a.weight > b.weight; });
    return out;
}

Analysis analyse(const ProxyClip& clip, const AnalysisRules& r) {
    Analysis a;
    a.version = kAnalysisVersion;
    const int n = clip.frames(), w = clip.w, h = clip.h, px = w * h;
    a.proxy_w = w, a.proxy_h = h, a.proxy_frames = n, a.proxy_fps = clip.fps;
    if (n <= 0) return a;

    // ---- frame statistics
    std::vector<std::vector<float>> y(static_cast<size_t>(n));
    std::vector<float> frame_luma(static_cast<size_t>(n)), red(static_cast<size_t>(n));
    double sr = 0, sg = 0, sb = 0, sat = 0, sy = 0, syy = 0;
    for (int f = 0; f < n; ++f) {
        const uint8_t* p = clip.frame(f);
        luma_of(p, w, h, y[f]);
        double fl = 0;
        int reds = 0;
        for (int i = 0; i < px; ++i) {
            const float R = p[i * 3] / 255.0f, G = p[i * 3 + 1] / 255.0f, B = p[i * 3 + 2] / 255.0f;
            sr += R, sg += G, sb += B;
            const float mx = std::max({R, G, B}), mn = std::min({R, G, B});
            sat += mx > 0 ? (mx - mn) / mx : 0;
            const float l = y[f][i];
            fl += l, sy += l, syy += static_cast<double>(l) * l;
            if (R > 0.3f && R / (R + G + B + 1e-6f) >= 0.8f) ++reds;
        }
        frame_luma[f] = static_cast<float>(fl / px);
        red[f] = static_cast<float>(reds) / px;
    }
    const double total = static_cast<double>(n) * px;
    auto byte = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    a.avg_rgb = (byte(sr / total) << 16) | (byte(sg / total) << 8) | byte(sb / total);
    a.brightness = static_cast<float>(sy / total);
    a.contrast = clamp01(static_cast<float>(std::sqrt(std::max(0.0, syy / total - (sy / total) * (sy / total)))) * 2.0f);
    a.saturation = clamp01(static_cast<float>(sat / total));
    for (float l : frame_luma) a.black_frames += l < r.black_luma ? 1 : 0;

    // ---- palette + warm / cool
    a.palette = palette_kmeans(clip);
    {
        double warm = 0;
        for (const auto& e : a.palette) {
            const Lab l = to_lab(((e.rgb >> 16) & 255) / 255.0f, ((e.rgb >> 8) & 255) / 255.0f, (e.rgb & 255) / 255.0f);
            warm += e.weight * (0.8 * l.b + 0.3 * l.a) / 50.0;  // GUESS: warm = yellow / red, cool = blue
        }
        a.warmth = std::clamp(static_cast<float>(warm), -1.0f, 1.0f);
    }

    // ---- cuts (histogram distance against the local median)
    std::vector<float> hd(static_cast<size_t>(n), 0.0f);
    for (int f = 1; f < n; ++f) hd[f] = hist_distance(clip.frame(f - 1), clip.frame(f), px);
    std::vector<bool> is_cut(static_cast<size_t>(n), false);
    for (int f = 1; f < n; ++f) {
        std::vector<float> around;
        for (int k = std::max(1, f - 5); k <= std::min(n - 1, f + 5); ++k)
            if (k != f) around.push_back(hd[k]);
        const float med = median_of(around);
        if (hd[f] > r.cut_min && hd[f] > r.cut_over_median * med + 0.05f) {
            is_cut[f] = true;
            a.cuts.push_back(to_hap(clip, f));
        }
    }
    const double seconds = clip.hap_frames > 0 && clip.hap_fps > 0 ? clip.hap_frames / clip.hap_fps : n / std::max(0.001f, clip.fps);
    a.cuts_per_min = seconds > 0 ? static_cast<float>(a.cuts.size() * 60.0 / seconds) : 0.0f;

    // ---- motion: mean |dY| (per 1/12.5 s) + the best global shift (direction)
    a.motion_curve.assign(static_cast<size_t>(n), 0.0f);
    double dir_x = 0, dir_y = 0;
    int dir_n = 0;
    const int S = std::min(r.motion_search, std::max(0, std::min(w, h) / 4));
    for (int f = 1; f < n; ++f) {
        const auto& p0 = y[f - 1];
        const auto& p1 = y[f];
        double d0 = 0;
        for (int i = 0; i < px; ++i) d0 += std::fabs(p1[i] - p0[i]);
        d0 /= px;
        float m = clamp01(static_cast<float>(d0 * (clip.fps / 12.5) * r.motion_gain));
        if (is_cut[f]) m = a.motion_curve[f - 1];
        a.motion_curve[f] = m;
        if (is_cut[f] || d0 < 0.004 || S == 0) continue;
        double best = 1e9, inner0 = 0;
        int bx = 0, by = 0;
        for (int dy = -S; dy <= S; ++dy)
            for (int dx = -S; dx <= S; ++dx) {
                double sad = 0;
                for (int yy = S; yy < h - S; ++yy)
                    for (int xx = S; xx < w - S; ++xx) sad += std::fabs(p1[(yy + dy) * w + xx + dx] - p0[yy * w + xx]);
                if (dx == 0 && dy == 0) inner0 = sad;
                if (sad < best - 1e-9) best = sad, bx = dx, by = dy;
            }
        if ((bx || by) && best < 0.8 * inner0) dir_x += bx, dir_y -= by, ++dir_n;  // up = +y
    }
    if (n > 1) a.motion_curve[0] = a.motion_curve[1];
    a.motion = n > 1 ? mean(a.motion_curve) : 0.0f;
    if (dir_n > 0 && (dir_x != 0 || dir_y != 0)) {
        double deg = std::atan2(dir_y, dir_x) * 180.0 / kPi;
        if (deg < 0) deg += 360.0;
        a.motion_dir = static_cast<float>(deg);
    }
    if (n > 2) {
        double var = 0;
        for (float m : a.motion_curve) var += (m - a.motion) * (m - a.motion);
        const float sd = static_cast<float>(std::sqrt(var / n));
        a.steadiness = clamp01(1.0f - 0.5f * sd / (a.motion + 0.05f));
    }

    // ---- loop quality, best loop points, style
    a.loop_out = std::max<int64_t>(0, clip.hap_frames - 1);
    if (n == 1) {
        a.loop_quality = 1.0f, a.loop_score = 1.0f;
    } else {
        // SSIM sees luma only; the histogram term catches a jump in colour at the same brightness.
        auto similar = [&](int p, int q) { return ssim(y[p], y[q], w, h) * (1.0f - hist_distance(clip.frame(p), clip.frame(q), px)); };
        a.loop_quality = similar(n - 1, 0);
        const int win = std::max(1, static_cast<int>(n * r.loop_search));
        const int step = std::max(1, win / 24);
        float best = -2.0f;
        int bi = 0, bo = n - 1;
        for (int i = 0; i <= win && i < n; i += step)
            for (int o = n - 1; o >= n - 1 - win && o > i; o -= step) {
                const float s = similar(o, i);
                if (s > best + 1e-4f) best = s, bi = i, bo = o;
            }
        a.loop_score = best;
        a.loop_in = to_hap(clip, bi);
        a.loop_out = clip.hap_frames > 0 ? (bo == n - 1 ? clip.hap_frames - 1 : to_hap(clip, bo)) : bo;
    }
    a.needs_crossfade = a.loop_score < r.crossfade_below;
    if (a.loop_score >= r.loopable_score) a.loop_style = "loopable";
    else if (a.cuts.empty() && a.steadiness >= 0.5f) a.loop_style = "ping-pong";  // GUESS
    else a.loop_style = "one-shot";

    // ---- slices: the cuts, or equal 16ths
    if (static_cast<int>(a.cuts.size()) >= r.min_cuts_for_slices) {
        a.slices.push_back(0);
        const size_t nc = a.cuts.size();
        const size_t take = std::min<size_t>(nc, kMaxSlices - 1);
        for (size_t k = 0; k < take; ++k) a.slices.push_back(a.cuts[take == nc ? k : k * nc / take]);
    } else if (clip.hap_frames > 1) {
        for (int k = 0; k < kMaxSlices; ++k) a.slices.push_back(static_cast<int64_t>(k) * clip.hap_frames / kMaxSlices);
        a.slices.erase(std::unique(a.slices.begin(), a.slices.end()), a.slices.end());
    } else {
        a.slices.push_back(0);
    }

    // ---- natural beats (v1: from the length at 120 BPM) and bars
    // GUESS: motion periodicity (autocorrelation) is not used yet; length only.
    a.natural_beats = std::clamp(pow2_round(seconds * 2.0), 1, 64);
    {
        const double beats = seconds * 2.0;
        a.bars_at_120 = beats >= 4.0 && std::fabs(beats - a.natural_beats) / a.natural_beats < 0.15 ? a.natural_beats / 4 : 0;
    }

    // ---- flicker: luminance and red transitions in any 1 s window
    if (n > 2 && clip.fps >= 6.0f) {
        std::vector<int> trans(static_cast<size_t>(n), 0);
        for (int f = 1; f < n; ++f) {
            const float d = frame_luma[f] - frame_luma[f - 1];
            const bool lum = std::fabs(d) >= r.flash_delta && std::min(frame_luma[f], frame_luma[f - 1]) < r.flash_dark_max;
            const bool rd = std::fabs(red[f] - red[f - 1]) >= 0.25f;
            trans[f] = lum || rd ? 1 : 0;
        }
        const int window = std::max(2, static_cast<int>(std::lround(clip.fps)));
        int run = 0, worst = 0;
        for (int f = 1; f < n; ++f) {
            run += trans[f];
            if (f - window >= 1) run -= trans[f - window];
            worst = std::max(worst, run);
        }
        const float flashes = worst / 2.0f;  // a flash = two opposing transitions
        a.flicker = flashes > r.flashes_bad ? "bad" : flashes >= r.flashes_warn ? "warn" : "ok";
    }

    // ---- energy + moods (rules; GUESS weights until tuned on the starter library)
    const float e = clamp01(0.45f * a.motion + 0.25f * std::min(1.0f, a.cuts_per_min / 30.0f) + 0.15f * a.contrast +
                            0.15f * a.saturation);
    a.energy = std::clamp(1 + static_cast<int>(std::lround(e * 4.0f)), 1, 5);
    const float flick = a.flicker == "ok" ? 0.0f : a.flicker == "warn" ? 0.5f : 1.0f;
    a.mood_chill = clamp01((1.0f - 1.8f * e) * (1.0f - 0.5f * flick));
    a.mood_groove = clamp01(1.0f - std::fabs(e - 0.35f) * 3.0f);
    a.mood_techno = clamp01(1.0f - std::fabs(e - 0.6f) * 3.0f + 0.2f * a.contrast - 0.1f * a.saturation);
    a.mood_crazy = clamp01((e - 0.55f) * 2.5f + 0.3f * flick);
    return a;
}

}  // namespace himmel::library
