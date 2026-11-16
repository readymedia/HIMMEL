// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Per-frame resolution of one FX slot: knob -> scope -> PRESS -> reaction -> flash cap.
// Runs in the render loop: no allocation, no locks, no I/O.
#include <algorithm>
#include <cmath>

#include "himmel/fx/fx.h"

namespace himmel::fx {

namespace {

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }

float shape(Curve c, float x) {
    x = clamp01(x);
    switch (c) {
        case Curve::Soft: return 1.0f - (1.0f - x) * (1.0f - x);
        case Curve::Hard: return x * x;
        case Curve::Steps: return std::floor(x * 4.0f + 1e-4f) / 4.0f;
        case Curve::Exp: return (std::exp2(x * 4.0f) - 1.0f) / 15.0f;
        default: return x;
    }
}

float snap(const ParamMeta& pm, float v) {
    v = clamp01(v);
    return pm.steps >= 2 ? step_center(step_index(v, pm.steps), pm.steps) : v;
}

// One-pole follower with separate attack / release times.
float follow(float env, float target, float attack_ms, float release_ms, float dt) {
    const float tau = target > env ? attack_ms : release_ms;
    if (tau <= 0.0f) return target;
    const float k = 1.0f - std::exp(-dt * 1000.0f / tau);
    return env + (target - env) * k;
}

// A flash counts as "on edge"; with the cap on, two edges are never closer than this.
double min_flash_interval(float cap_hz) { return 1.0 / cap_hz + 0.001; }

}  // namespace

int step_index(float p, int n) {
    if (n <= 1) return 0;
    const int i = static_cast<int>(std::floor(clamp01(p) * static_cast<float>(n)));
    return std::min(i, n - 1);
}
float step_center(int i, int n) { return n <= 1 ? 0.0f : (static_cast<float>(i) + 0.5f) / static_cast<float>(n); }

float strobe_flash_hz(const EffectMeta& m, float rate01, float bpm) {
    if (m.flash.mode != FlashMode::Rate || m.flash.steps == 0) return 0.0f;
    const int i = step_index(rate01, m.flash.steps);
    return bpm / 60.0f / m.flash.beats[i];
}

Resolved resolve(const EffectMeta& m, int effect_id, const protocol::FxSlotState& slot, SlotRuntime& rt,
                 const Globals& globals, const FrameCtx& ctx) {
    if (rt.effect != effect_id) {
        rt = SlotRuntime{};
        rt.effect = static_cast<int16_t>(effect_id);
    }
    Resolved out;
    out.mix = clamp01(slot.mix);
    const int n = std::min(m.params, kParams);
    const float dt = std::clamp(ctx.dt, 0.0f, 0.25f);
    const float bpm = ctx.bpm > 1.0f ? ctx.bpm : 120.0f;
    const bool gid = effect_id >= 0 && effect_id < kMaxEffects;

    // 1. Scope: SLOT (the knob in this slot), LOOK (per scene), GLOBAL (one per effect), MOVE.
    float v[kParams] = {};
    for (int p = 0; p < n; ++p) {
        Scope s = static_cast<Scope>(rt.scope[p] & 3);
        if (!(m.param[p].scopes & scope_bit(s))) s = Scope::Slot;
        v[p] = slot.param[p];
        if (s == Scope::Look && rt.look_set[p]) v[p] = rt.look[p];
        if (s == Scope::Global && gid && globals.set[effect_id][p]) v[p] = globals.v[effect_id][p];
        v[p] = clamp01(v[p]);
    }

    // 2. PRESS: velocity sets where it starts (the first value), aftertouch moves it,
    //    release brings it back over release_beats (clips-and-playback.md §8.2).
    const float press_in = static_cast<float>(slot.press) / 255.0f;
    if (press_in >= rt.press_env) {
        rt.press_env = press_in;
    } else {
        const float rel_s = std::max(0.01f, m.press.release_beats * 60.0f / bpm);
        rt.press_env = std::max(press_in, rt.press_env - dt / rel_s);
    }
    out.press = rt.press_env;
    if (rt.press_env > 0.0f && m.press.param >= 0 && m.press.param < n) {
        const int pp = m.press.param;
        float pv = m.press.lo + (m.press.hi - m.press.lo) * shape(m.press.curve == Curve::Steps ? Curve::Linear : m.press.curve,
                                                                   rt.press_env);
        if (m.press.curve == Curve::Steps || m.param[pp].steps >= 2) pv = snap(m.param[pp], pv);
        const float w = std::min(1.0f, rt.press_env / 0.05f);  // fade in over the first 5 % of pressure
        v[pp] = v[pp] + (pv - v[pp]) * w;
    }

    // 3. Reaction (music.md §4): signal -> gate -> envelope -> curve -> ADD / FOLLOW / TRIGGER.
    const Reaction& re = m.reaction;
    if (re.signal != Signal::None && re.param >= 0 && re.param < n) {
        const float sig = clamp01(ctx.sig[re.signal]);
        const float x = sig <= re.threshold ? 0.0f : (sig - re.threshold) / std::max(1e-3f, 1.0f - re.threshold);
        if (re.mode == ReactMode::Trigger) {
            const bool edge = sig > re.threshold && rt.prev_sig <= re.threshold;
            bool fire = edge;
            if (fire && re.quantise_beats > 0.0f) {
                // Only on the grid: an edge waits for the next grid line (kept as a pending trigger).
                rt.last_trigger_beat = std::ceil(ctx.beats / re.quantise_beats) * re.quantise_beats;
                fire = false;
            }
            if (!fire && re.quantise_beats > 0.0f && rt.last_trigger_beat > -1e8 && ctx.beats >= rt.last_trigger_beat) {
                fire = true;
                rt.last_trigger_beat = -1e9;
            }
            if (fire) rt.trigger_until_beat = ctx.beats + re.hold_beats;
            const bool up = fire || ctx.beats < rt.trigger_until_beat;
            rt.react_env = follow(rt.react_env, up ? 1.0f : 0.0f, re.attack_ms, re.release_ms, dt);
            if (fire && re.attack_ms <= 0.0f) rt.react_env = 1.0f;
        } else {
            rt.react_env = follow(rt.react_env, x, re.attack_ms, re.release_ms, dt);
        }
        rt.prev_sig = sig;
        const float k = shape(re.curve, rt.react_env);
        const int rp = re.param;
        const bool move = (rt.scope[rp] & 3) == static_cast<uint8_t>(Scope::Move) && (m.param[rp].scopes & scope_bit(Scope::Move));
        const float lo = re.range_lo, hi = re.range_hi;
        if (move || re.mode == ReactMode::Follow) {
            const float a = move ? 1.0f : std::fabs(re.amount);
            const float target = re.amount >= 0.0f || move ? lo + (hi - lo) * k : hi - (hi - lo) * k;
            v[rp] = v[rp] + (target - v[rp]) * a;
        } else {
            v[rp] += re.amount * k * (hi - lo);
        }
    }

    for (int p = 0; p < n; ++p) v[p] = snap(m.param[p], v[p]);

    // 4. Flash cap (D65, off by default): strobe rate and flash level never exceed it.
    const float cap = ctx.flash_cap_hz;
    const Flash& fl = m.flash;
    if (fl.mode == FlashMode::Rate && fl.param >= 0 && fl.param < n && fl.steps > 0) {
        int idx = step_index(v[fl.param], fl.steps);
        if (cap > 0.0f)
            while (idx > 0 && bpm / 60.0f / fl.beats[idx] > cap) --idx;  // slower steps first
        v[fl.param] = step_center(idx, fl.steps);
        const double div = fl.beats[idx];
        const float duty = fl.duty >= 0 && fl.duty < n ? 0.05f + 0.9f * v[fl.duty] : 0.5f;
        const double ph = ctx.beats / div - std::floor(ctx.beats / div);
        bool on = ph < duty;
        if (on && !rt.flash_on) {
            if (cap > 0.0f && ctx.time_s - rt.last_flash_s < min_flash_interval(cap)) on = false;  // too soon: skip it
            else rt.last_flash_s = ctx.time_s;
        }
        rt.flash_on = on;
        out.gate = on ? 1.0f : 0.0f;
    } else if (fl.mode == FlashMode::Level && fl.param >= 0 && fl.param < n) {
        float lv = v[fl.param];
        if (lv > 0.5f && !rt.level_high) {
            if (cap > 0.0f && ctx.time_s - rt.last_flash_s < min_flash_interval(cap)) lv = std::min(lv, rt.level_prev);
            else rt.last_flash_s = ctx.time_s;
        }
        rt.level_high = lv > 0.5f;
        rt.level_prev = lv;
        v[fl.param] = lv;
    }

    for (int p = 0; p < kParams; ++p) out.p[p] = p < n ? v[p] : 0.0f;
    return out;
}

}  // namespace himmel::fx
