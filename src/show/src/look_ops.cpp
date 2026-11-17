// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/look_ops.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace himmel::show {

using namespace himmel::protocol;

namespace {

constexpr float kEps = 1e-5f;
bool differ(float a, float b) { return std::fabs(a - b) > kEps; }

Command layer_cmd(CommandType t, int deck, int layer, int32_t i = 0, float f = 0.0f) {
    return make(t, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), i, f);
}

Command set_cmd(int deck, int layer, LayerParam p, int32_t i, float f) {
    Command c = make(CommandType::LayerSet, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), i, f);
    c.param = static_cast<uint8_t>(p);
    return c;
}

// The FX slot state right after FxSet (the deck model resets it like this).
void after_set(FxSlotState& s, int effect, const Catalog& cat) {
    std::memset(&s, 0, sizeof(s));
    s.effect = static_cast<int16_t>(effect);
    s.mix = 1.0f;
    if (effect >= 0 && effect < static_cast<int>(cat.effect_count)) {
        const EffectInfo& e = cat.effect[effect];
        for (int p = 0; p < kFxParams; ++p) {
            float v = p < e.param_count ? e.param_default[p] : 0.0f;
            s.param[p] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
}

void diff_slot(const FxSlotState& cur0, const FxSlotState& want, uint8_t deck, uint8_t layer, uint8_t slot,
               const Catalog& cat, CommandList& out) {
    FxSlotState cur = cur0;
    if (cur.effect != want.effect) {
        out.push(make_fx(CommandType::FxSet, deck, layer, slot, kNone, want.effect));
        after_set(cur, want.effect, cat);
    }
    if (want.effect < 0) return;
    for (int p = 0; p < kFxParams; ++p)
        if (differ(cur.param[p], want.param[p]))
            out.push(make_fx(CommandType::FxParam, deck, layer, slot, static_cast<uint8_t>(p), 0, want.param[p]));
    if (differ(cur.mix, want.mix)) out.push(make_fx(CommandType::FxMix, deck, layer, slot, kNone, 0, want.mix));
    if (cur.bypass != want.bypass) out.push(make_fx(CommandType::FxBypass, deck, layer, slot, kNone, want.bypass ? 1 : 0));
}

void diff_transport(const LayerTransport& a, const LayerTransport& b, int d, int l, CommandList& out) {
    auto si = [&](LayerParam p, int ai, int bi) {
        if (ai != bi) out.push(set_cmd(d, l, p, bi, 0.0f));
    };
    auto sf = [&](LayerParam p, float af, float bf) {
        if (differ(af, bf)) out.push(set_cmd(d, l, p, 0, bf));
    };
    si(LayerParam::Mode, a.mode, b.mode);
    si(LayerParam::Direction, a.backward, b.backward);
    sf(LayerParam::In, a.in, b.in);
    sf(LayerParam::Out, a.out, b.out);
    sf(LayerParam::LoopIn, a.loop_in, b.loop_in);
    sf(LayerParam::LoopOut, a.loop_out, b.loop_out);
    si(LayerParam::InnerLoop, a.inner_loop, b.inner_loop);
    si(LayerParam::TimeBase, a.time_base, b.time_base);
    sf(LayerParam::Beats, a.beats, b.beats);
    sf(LayerParam::Interval, a.interval, b.interval);
    sf(LayerParam::Distance, a.distance, b.distance);
    si(LayerParam::Slices, a.slices, b.slices);
    si(LayerParam::StepFrames, a.step_frames, b.step_frames);
    sf(LayerParam::StepDiv, a.step_div, b.step_div);
    sf(LayerParam::Glide, a.glide, b.glide);
    si(LayerParam::Trigger, a.trigger, b.trigger);
    si(LayerParam::StartPos, a.start, b.start);
    sf(LayerParam::FadeIn, a.fade_in, b.fade_in);
    sf(LayerParam::FadeOut, a.fade_out, b.fade_out);
    if (a.snap != b.snap) out.push(set_cmd(d, l, LayerParam::Snap, b.snap == kNone ? -1 : b.snap, 0.0f));
    si(LayerParam::Target, a.target, b.target);
    si(LayerParam::Autopilot, a.autopilot, b.autopilot);
    si(LayerParam::AutopilotCount, a.autopilot_count, b.autopilot_count);
    si(LayerParam::Transition, a.transition, b.transition);
    sf(LayerParam::TransitionTime, a.transition_time, b.transition_time);
    si(LayerParam::CatchUp, a.catch_up, b.catch_up);
    si(LayerParam::ScratchSnap, a.scratch_snap, b.scratch_snap);
    if (a.seed != b.seed) out.push(set_cmd(d, l, LayerParam::Seed, static_cast<int32_t>(b.seed), 0.0f));
}

bool same_set(const ReactKnob& a, const ReactKnob& b) {
    return a.signal == b.signal && a.mode == b.mode && a.curve == b.curve && a.quantise == b.quantise &&
           a.hold_q == b.hold_q && !differ(a.amount, b.amount) && !differ(a.attack_ms, b.attack_ms) &&
           !differ(a.release_ms, b.release_ms) && !differ(a.threshold, b.threshold);
}

bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

void diff_react(const ReactRow& cur0, const ReactRow& want, bool effect_changed, int effect, uint8_t deck,
                uint8_t layer, uint8_t slot, const Catalog& cat, CommandList& out) {
    if (effect < 0 || effect >= static_cast<int>(cat.effect_count)) return;
    const int n = cat.effect[effect].param_count < kReactParams ? cat.effect[effect].param_count : kReactParams;
    constexpr auto kDefault = static_cast<uint8_t>(ReactSource::Default);
    constexpr auto kOffSrc = static_cast<uint8_t>(ReactSource::Off);
    constexpr auto kCustom = static_cast<uint8_t>(ReactSource::Custom);
    for (int p = 0; p < n; ++p) {
        ReactKnob c{};  // all zero = Default (after an FxSet)
        if (!effect_changed) c = cur0.k[p];
        const ReactKnob& w = want.k[p];
        if (std::memcmp(&c, &w, sizeof(c)) == 0) continue;
        const auto knob = static_cast<uint8_t>(p);
        ReactArgs a;
        if (w.source == kOffSrc || w.source != kCustom) {
            a.op = w.source == kOffSrc ? ReactOp::Off : ReactOp::Default;
            if (c.source != w.source) out.push(make_react(deck, layer, slot, knob, a));
            continue;
        }
        // Custom: Set (keeps the range), then the range when it differs or is not known (from Off).
        if (c.source != kCustom || !same_set(c, w)) {
            a.op = ReactOp::Set;
            a.signal = static_cast<ReactSignal>(w.signal);
            a.mode = static_cast<ReactMode>(w.mode);
            a.curve = static_cast<ReactCurve>(w.curve);
            a.quantise = static_cast<ReactQuantise>(w.quantise);
            a.hold_q = w.hold_q;
            Command x = make_react(deck, layer, slot, knob, a, w.amount);
            x.f[1] = w.attack_ms, x.f[2] = w.release_ms, x.f[3] = w.threshold;
            out.push(x);
        }
        const bool range_known = c.source == kCustom || c.source == kDefault;  // Default = the param's good range
        const bool range_same = c.ranged == w.ranged && (!w.ranged || (!differ(c.range_lo, w.range_lo) && !differ(c.range_hi, w.range_hi)));
        if (!range_known || !range_same) {
            a.op = ReactOp::Range;
            Command x = make_react(deck, layer, slot, knob, a, w.ranged ? w.range_lo : -1.0f);
            x.f[1] = w.ranged ? w.range_hi : 0.0f;
            out.push(x);
        }
    }
}

void diff_look(const Look& cur, const Look& target, int deck, const Catalog& cat, CommandList& out, uint32_t parts,
               uint32_t layer_mask, bool with_deck_fx) {
    const auto d = static_cast<uint8_t>(deck);
    for (int l = 0; l < kLayers; ++l) {
        if (!(layer_mask & (1u << l))) continue;
        const LayerState& a = cur.layer[l];
        const LayerState& b = target.layer[l];
        // Settings first, so the scene's transition / fades apply to its own clip.
        if (parts & kDiffTransport) diff_transport(cur.transport[l], target.transport[l], deck, l, out);
        if ((parts & kDiffClips) && a.clip != b.clip) out.push(layer_cmd(CommandType::LayerClip, deck, l, b.clip));
        if (parts & kDiffMix) {
            if (differ(a.opacity, b.opacity)) out.push(layer_cmd(CommandType::LayerOpacity, deck, l, 0, b.opacity));
            if (a.blend != b.blend) out.push(layer_cmd(CommandType::LayerBlend, deck, l, b.blend));
            if (a.mute != b.mute) out.push(layer_cmd(CommandType::LayerMute, deck, l, b.mute ? 1 : 0));
            if (differ(a.speed, b.speed)) out.push(layer_cmd(CommandType::LayerSpeed, deck, l, 0, b.speed));
        }
        if (parts & kDiffFx)
            for (int s = 0; s < kFxSlots; ++s)
                diff_slot(a.fx[s], b.fx[s], d, static_cast<uint8_t>(l), static_cast<uint8_t>(s), cat, out);
    }
    if (with_deck_fx && (parts & kDiffFx))
        for (int s = 0; s < kFxSlots; ++s) diff_slot(cur.fx[s], target.fx[s], d, kNone, static_cast<uint8_t>(s), cat, out);
    // Reactions last (D131): every FxSet above has reset its slot to Default by now.
    if (!(parts & kDiffFx)) return;
    for (int l = 0; l < kLayers; ++l) {
        if (!(layer_mask & (1u << l))) continue;
        for (int s = 0; s < kFxSlots; ++s) {
            const int e = target.layer[l].fx[s].effect;
            diff_react(cur.layer_react[l][s], target.layer_react[l][s], cur.layer[l].fx[s].effect != e, e, d,
                       static_cast<uint8_t>(l), static_cast<uint8_t>(s), cat, out);
        }
    }
    if (with_deck_fx)
        for (int s = 0; s < kFxSlots; ++s)
            diff_react(cur.fx_react[s], target.fx_react[s], cur.fx[s].effect != target.fx[s].effect, target.fx[s].effect, d,
                       kNone, static_cast<uint8_t>(s), cat, out);
}

void diff_master(const FxSlotState cur[kFxSlots], const ReactRow cur_react[kFxSlots], const FxSlotState target[kFxSlots],
                 const ReactRow target_react[kFxSlots], const Catalog& cat, CommandList& out) {
    for (int s = 0; s < kFxSlots; ++s) diff_slot(cur[s], target[s], kMasterDeck, kNone, static_cast<uint8_t>(s), cat, out);
    for (int s = 0; s < kFxSlots; ++s)
        diff_react(cur_react[s], target_react[s], cur[s].effect != target[s].effect, target[s].effect, kMasterDeck, kNone,
                   static_cast<uint8_t>(s), cat, out);
}

void lerp_look(const Look& a, const Look& b, float t, Look& out) {
    const bool second = t >= 0.5f;
    out = second ? b : a;
    for (int l = 0; l < kLayers; ++l) {
        const LayerState& la = a.layer[l];
        const LayerState& lb = b.layer[l];
        LayerState& o = out.layer[l];
        if (la.clip == lb.clip) {
            o.opacity = lerpf(la.opacity, lb.opacity, t);
            o.speed = lerpf(la.speed, lb.speed, t);
        } else if (!second) {
            // The source changes at the middle: the old one fades out on the first half ...
            o.opacity = la.clip >= 0 ? lerpf(la.opacity, 0.0f, t * 2.0f) : la.opacity;
        } else {
            // ... and the new one fades in on the second half.
            o.opacity = lb.clip >= 0 ? lerpf(0.0f, lb.opacity, (t - 0.5f) * 2.0f) : lb.opacity;
        }
        for (int s = 0; s < kFxSlots; ++s) {
            const FxSlotState& fa = la.fx[s];
            const FxSlotState& fb = lb.fx[s];
            FxSlotState& fo = o.fx[s];
            if (fa.effect == fb.effect && fa.effect >= 0) {
                for (int p = 0; p < kFxParams; ++p) fo.param[p] = lerpf(fa.param[p], fb.param[p], t);
                fo.mix = lerpf(fa.mix, fb.mix, t);
            }
        }
    }
    for (int s = 0; s < kFxSlots; ++s) {
        const FxSlotState& fa = a.fx[s];
        const FxSlotState& fb = b.fx[s];
        if (fa.effect == fb.effect && fa.effect >= 0) {
            for (int p = 0; p < kFxParams; ++p) out.fx[s].param[p] = lerpf(fa.param[p], fb.param[p], t);
            out.fx[s].mix = lerpf(fa.mix, fb.mix, t);
        }
    }
}

int find_clip(const Catalog& cat, const char* name) {
    if (!name || !*name) return -1;
    for (uint32_t i = 0; i < cat.clip_count && i < static_cast<uint32_t>(kMaxClips); ++i)
        if (ieq(cat.clip[i].name, name)) return static_cast<int>(i);
    return -1;
}

int find_effect(const Catalog& cat, const char* name) {
    if (!name || !*name) return -1;
    for (uint32_t i = 0; i < cat.effect_count && i < static_cast<uint32_t>(kMaxEffects); ++i)
        if (ieq(cat.effect[i].name, name)) return static_cast<int>(i);
    return -1;
}

int bind_clips(Look& look, const Catalog& cat) {
    int missing = 0;
    for (int l = 0; l < kLayers; ++l) {
        if (!look.clip_name[l][0]) {
            look.layer[l].clip = -1;
            continue;
        }
        const int id = find_clip(cat, look.clip_name[l]);
        look.layer[l].clip = static_cast<int16_t>(id);
        if (id < 0) ++missing;
    }
    return missing;
}

}  // namespace himmel::show
