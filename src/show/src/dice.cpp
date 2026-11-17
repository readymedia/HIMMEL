// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/dice.h"

#include <cctype>
#include <cstring>

#include "himmel/show/look_ops.h"

namespace himmel::show {

using namespace himmel::protocol;

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

namespace {

struct Rng {
    uint64_t s;
    uint64_t next() { return s = mix64(s); }
    float uni() { return static_cast<float>(next() >> 40) / static_cast<float>(1u << 24); }  // [0,1)
    float range(float a, float b) { return a + (b - a) * uni(); }
    int below(int n) { return n > 0 ? static_cast<int>(next() % static_cast<uint64_t>(n)) : 0; }
    bool chance(float p) { return uni() < p; }
};

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool contains(const char* hay, const char* needle) {
    char low[kParamNameLen + 1] = {};
    for (int i = 0; i < kParamNameLen && hay[i]; ++i) low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i])));
    return std::strstr(low, needle) != nullptr;
}

// A random effect (kind 0 = effect, never a generator: SOURCE decides generators), -1 = none in the catalog.
int pick_effect(const Catalog& cat, Rng& r) {
    int n = 0;
    for (uint32_t i = 0; i < cat.effect_count; ++i) n += cat.effect[i].reserved[0] == 0 ? 1 : 0;
    if (n == 0) return -1;
    int k = r.below(n);
    for (uint32_t i = 0; i < cat.effect_count; ++i)
        if (cat.effect[i].reserved[0] == 0 && k-- == 0) return static_cast<int>(i);
    return -1;
}

int pick_clip(const Catalog& cat, Rng& r, int avoid) {
    if (cat.clip_count == 0) return -1;
    int c = r.below(static_cast<int>(cat.clip_count));
    if (c == avoid && cat.clip_count > 1) c = (c + 1 + r.below(static_cast<int>(cat.clip_count) - 1)) % static_cast<int>(cat.clip_count);
    return c;
}

// New params around the defaults (+-0.3), inside the locks.
void roll_params(FxSlotState& s, const FxSlotState& keep, const Catalog& cat, uint32_t locks, Rng& r) {
    if (s.effect < 0 || s.effect >= static_cast<int>(cat.effect_count)) return;
    const EffectInfo& e = cat.effect[s.effect];
    for (int p = 0; p < kFxParams; ++p) {
        if (p >= e.param_count) {
            s.param[p] = 0.0f;
            continue;
        }
        const uint32_t guard = param_lock(e.param_name[p]);
        if ((guard & locks) && keep.effect == s.effect) {
            s.param[p] = keep.param[p];
            continue;
        }
        s.param[p] = clamp01(e.param_default[p] + r.range(-0.3f, 0.3f));
    }
    s.mix = r.chance(0.7f) ? 1.0f : r.range(0.5f, 1.0f);
    s.bypass = 0;
    s.press = 0;
}

void roll_slot(FxSlotState& s, const FxSlotState& cur, const Catalog& cat, uint32_t locks, Rng& r, float p_on) {
    if (locks & kLockFx) {  // same effect, new params (inside the other locks)
        s = cur;
        roll_params(s, cur, cat, locks, r);
        return;
    }
    std::memset(&s, 0, sizeof(s));
    s.mix = 1.0f;
    s.effect = static_cast<int16_t>(r.chance(p_on) ? pick_effect(cat, r) : -1);
    roll_params(s, cur, cat, locks, r);
}

void roll_layer(Look& out, const Look& cur, int l, const Catalog& cat, uint32_t locks, Rng& r, bool on) {
    LayerState& o = out.layer[l];
    const LayerState& c = cur.layer[l];
    if (!(locks & kLockClip)) {
        const int clip = on ? pick_clip(cat, r, c.clip) : -1;
        o.clip = static_cast<int16_t>(clip);
        copy_str(out.clip_name[l], kNameLen, clip >= 0 ? cat.clip[clip].name : "");
    }
    if (!(locks & kLockLayout)) {
        o.opacity = l == 0 ? 1.0f : r.range(0.5f, 1.0f);
        static const BlendMode kBlends[] = {BlendMode::Add, BlendMode::Screen, BlendMode::Normal, BlendMode::Lighten};
        o.blend = static_cast<uint8_t>(l == 0 ? BlendMode::Normal : kBlends[r.below(4)]);
        o.mute = 0;
    }
    if (!(locks & kLockMotion)) {
        // GUESS: most rolls keep normal speed; a third drift between half and 1.5x.
        o.speed = r.chance(0.66f) ? 1.0f : r.range(0.5f, 1.5f);
        out.transport[l].backward = static_cast<uint8_t>(r.chance(0.15f) ? 1 : 0);
    }
    for (int s = 0; s < kFxSlots; ++s)  // GUESS: slot 1 often, slot 2 sometimes, 3-4 stay free for the player
        roll_slot(o.fx[s], c.fx[s], cat, locks, r, s == 0 ? 0.6f : (s == 1 ? 0.25f : 0.0f));
}

void nudge_slot(FxSlotState& s, const Catalog& cat, uint32_t locks, Rng& r, float amt) {
    if (s.effect < 0 || s.effect >= static_cast<int>(cat.effect_count)) return;
    const EffectInfo& e = cat.effect[s.effect];
    for (int p = 0; p < e.param_count && p < kFxParams; ++p) {
        if (param_lock(e.param_name[p]) & locks) continue;
        s.param[p] = clamp01(s.param[p] + r.range(-amt, amt));
    }
}

}  // namespace

uint32_t param_lock(const char* n) {
    if (!n) return 0;
    static const char* kColour[] = {"hue", "col", "sat", "tint", "palet", "bright", "contr", "gamma", "invert", "posteriz"};
    static const char* kMotion[] = {"speed", "rate", "spin", "zoom", "rot", "drift", "shake", "freq", "tempo", "wave"};
    for (const char* k : kColour)
        if (contains(n, k)) return kLockColour;
    for (const char* k : kMotion)
        if (contains(n, k)) return kLockMotion;
    return 0;
}

void dice_roll(const Look& cur, Look& out, const Catalog& cat, uint32_t locks, DiceScope scope, uint64_t seed) {
    out = cur;
    Rng r{mix64(seed ^ 0xD1CE0000ull)};
    if (scope.layer < 0 && scope.slot >= 0) {  // a deck FX slot
        roll_slot(out.fx[scope.slot], cur.fx[scope.slot], cat, locks, r, 1.0f);
        return;
    }
    if (scope.layer >= 0 && scope.layer < kLayers) {
        if (scope.slot >= 0 && scope.slot < kFxSlots) {
            roll_slot(out.layer[scope.layer].fx[scope.slot], cur.layer[scope.layer].fx[scope.slot], cat, locks, r, 1.0f);
            return;
        }
        roll_layer(out, cur, scope.layer, cat, locks, r, true);
        return;
    }
    // DECK: 1-4 layers (GUESS: 2-3 most of the time, until moods set the range), deck FX sometimes.
    static const int kCounts[] = {1, 2, 2, 3, 3, 3, 4};
    const int count = kCounts[r.below(7)];
    for (int l = 0; l < kLayers; ++l) roll_layer(out, cur, l, cat, locks, r, l < count);
    for (int s = 0; s < kFxSlots; ++s) roll_slot(out.fx[s], cur.fx[s], cat, locks, r, s == 0 ? 0.3f : 0.0f);
}

void dice_mutate(const Look& cur, Look& out, const Catalog& cat, uint32_t locks, DiceScope scope, uint64_t seed,
                 float amount) {
    out = cur;
    Rng r{mix64(seed ^ 0x3A7E0000ull)};
    const float amt = amount > 0.0f ? (amount > 1.0f ? 1.0f : amount) : 0.2f;
    for (int l = 0; l < kLayers; ++l) {
        if (scope.layer >= 0 && scope.layer != l) continue;
        if (scope.layer < 0 && scope.slot >= 0) break;  // a deck FX slot only
        LayerState& o = out.layer[l];
        if (scope.slot < 0) {
            if (!(locks & kLockLayout) && l > 0 && o.clip >= 0) o.opacity = clamp01(o.opacity + r.range(-amt, amt));
            if (!(locks & kLockMotion) && o.clip >= 0) {
                float sp = o.speed + r.range(-amt, amt);
                o.speed = sp < -4.0f ? -4.0f : (sp > 4.0f ? 4.0f : sp);
            }
        }
        for (int s = 0; s < kFxSlots; ++s)
            if (scope.slot < 0 || scope.slot == s) nudge_slot(o.fx[s], cat, locks, r, amt);
    }
    if (scope.layer < 0)
        for (int s = 0; s < kFxSlots; ++s)
            if (scope.slot < 0 || scope.slot == s) nudge_slot(out.fx[s], cat, locks, r, amt);
}

}  // namespace himmel::show
