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

static_assert(static_cast<int>(protocol::ReactSignal::Count) == kSignals);
static_assert(static_cast<int>(protocol::ReactMode::Trigger) == static_cast<int>(ReactMode::Trigger));
static_assert(static_cast<int>(protocol::ReactCurve::Exp) == static_cast<int>(Curve::Exp));
static_assert(protocol::kReactParams == kParams);

void reactions_from(const protocol::SlotReactions& in, Reaction out[kParams]) {
    for (int p = 0; p < kParams; ++p) {
        const protocol::ReactionEntry& e = in.p[p];
        Reaction r;
        r.param = static_cast<int8_t>(p);
        const bool nums = std::isfinite(e.amount) && std::isfinite(e.attack_ms) && std::isfinite(e.release_ms) &&
                          std::isfinite(e.threshold) && std::isfinite(e.range_lo) && std::isfinite(e.range_hi) &&
                          std::isfinite(e.quantise_beats) && std::isfinite(e.hold_beats);
        if (e.source != static_cast<uint8_t>(protocol::ReactSource::Off) && e.signal < kSignals &&
            e.mode < static_cast<uint8_t>(protocol::ReactMode::Count) &&
            e.curve < static_cast<uint8_t>(protocol::ReactCurve::Count) && nums) {
            r.signal = static_cast<Signal>(e.signal);
            r.mode = static_cast<ReactMode>(e.mode);
            r.curve = static_cast<Curve>(e.curve);
            r.amount = std::clamp(e.amount, -1.0f, 1.0f);
            r.attack_ms = std::clamp(e.attack_ms, 0.0f, protocol::kReactMaxMs);
            r.release_ms = std::clamp(e.release_ms, 0.0f, protocol::kReactMaxMs);
            r.threshold = clamp01(e.threshold);
            r.range_lo = clamp01(e.range_lo);
            r.range_hi = clamp01(e.range_hi);
            r.quantise_beats = std::clamp(e.quantise_beats, 0.0f, 16.0f);
            r.hold_beats = std::clamp(e.hold_beats, 0.0f, 16.0f);
        }
        out[p] = r;
    }
}

void default_entry(const EffectMeta& m, int p, protocol::ReactionEntry& e) {
    e = protocol::ReactionEntry{};
    e.source = static_cast<uint8_t>(protocol::ReactSource::Default);
    const bool has = p >= 0 && p < std::min(m.params, kParams);
    const Reaction& re = m.reaction;
    if (has && re.param == p && re.signal != Signal::None) {
        e.signal = static_cast<uint8_t>(re.signal);
        e.mode = static_cast<uint8_t>(re.mode);
        e.curve = static_cast<uint8_t>(re.curve);
        e.amount = re.amount;
        e.attack_ms = re.attack_ms;
        e.release_ms = re.release_ms;
        e.threshold = re.threshold;
        e.range_lo = re.range_lo;
        e.range_hi = re.range_hi;
        e.quantise_beats = re.quantise_beats;
        e.hold_beats = re.hold_beats;
        return;
    }
    // No default on this knob: the template a Signal / Amount op starts from.
    e.signal = static_cast<uint8_t>(Signal::None);
    e.mode = static_cast<uint8_t>(ReactMode::Add);
    e.curve = static_cast<uint8_t>(Curve::Linear);
    e.amount = 0.0f;
    e.attack_ms = 10.0f;
    e.release_ms = 200.0f;
    e.range_lo = has ? m.param[p].good_lo : 0.0f;
    e.range_hi = has ? m.param[p].good_hi : 1.0f;
}

float strobe_flash_hz(const EffectMeta& m, float rate01, float bpm) {
    if (m.flash.mode != FlashMode::Rate || m.flash.steps == 0) return 0.0f;
    const int i = step_index(rate01, m.flash.steps);
    return bpm / 60.0f / m.flash.beats[i];
}

Resolved resolve(const EffectMeta& m, int effect_id, const protocol::FxSlotState& slot, SlotRuntime& rt,
                 const Globals& globals, const FrameCtx& ctx, const Reaction* reacts) {
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

    // 3. Reactions (music.md §4): signal -> gate -> envelope -> curve -> ADD / FOLLOW / TRIGGER.
    //    One per knob (D131), each on its own envelope, in knob order.
    for (int rp = 0; rp < n; ++rp) {
        const Reaction* rr = nullptr;
        if (reacts) rr = &reacts[rp];
        else if (m.reaction.param == rp) rr = &m.reaction;
        if (!rr || rr->signal == Signal::None || rr->signal >= Signal::Count) {
            rt.react_env[rp] = 0.0f;
            continue;
        }
        const Reaction& re = *rr;
        float& env = rt.react_env[rp];
        const float sig = clamp01(ctx.sig[re.signal]);
        const float x = sig <= re.threshold ? 0.0f : (sig - re.threshold) / std::max(1e-3f, 1.0f - re.threshold);
        if (re.mode == ReactMode::Trigger) {
            const bool edge = sig > re.threshold && rt.prev_sig[rp] <= re.threshold;
            bool fire = edge;
            if (fire && re.quantise_beats > 0.0f) {
                // Only on the grid: an edge waits for the next grid line (kept as a pending trigger).
                rt.last_trigger_beat[rp] = std::ceil(ctx.beats / re.quantise_beats) * re.quantise_beats;
                fire = false;
            }
            if (!fire && re.quantise_beats > 0.0f && rt.last_trigger_beat[rp] > -1e8 && ctx.beats >= rt.last_trigger_beat[rp]) {
                fire = true;
                rt.last_trigger_beat[rp] = -1e9;
            }
            if (fire) rt.trigger_until_beat[rp] = ctx.beats + re.hold_beats;
            const bool up = fire || ctx.beats < rt.trigger_until_beat[rp];
            env = follow(env, up ? 1.0f : 0.0f, re.attack_ms, re.release_ms, dt);
            if (fire && re.attack_ms <= 0.0f) env = 1.0f;
        } else {
            env = follow(env, x, re.attack_ms, re.release_ms, dt);
        }
        rt.prev_sig[rp] = sig;
        const float k = shape(re.curve, env);
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

    for (int p = 0; p < kParams; ++p) out.p[p] = rt.out[p] = p < n ? v[p] : 0.0f;
    return out;
}

}  // namespace himmel::fx
