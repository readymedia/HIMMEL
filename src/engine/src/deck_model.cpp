// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/deck_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::engine {

using namespace himmel::protocol;

namespace {

bool finite(float v) { return std::isfinite(v); }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
bool is_toggle(int32_t v) { return v >= kOff && v <= kToggle; }
uint8_t toggled(uint8_t cur, int32_t v) { return v == kToggle ? static_cast<uint8_t>(!cur) : static_cast<uint8_t>(v == kOn); }
float with_delta(float cur, const Command& c) { return (c.flags & kRelative) ? cur + c.f[0] : c.f[0]; }

// A real deck layer (Focus, FX): never the HIT layer.
bool deck_layer(const Command& c) { return c.deck < kDecks && c.layer < kLayersPerDeck; }
// Any layer slot incl. the HIT layer (deck = kHitDeck, layer 0); -1 = none.
int cmd_slot(const Command& c) {
    if (deck_layer(c)) return slot_of(c.deck, c.layer);
    if (c.deck == kHitDeck && c.layer == 0) return kHitSlot;
    return -1;
}
bool fx_target(const Command& c) {
    if (c.slot >= kFxSlots) return false;
    if (c.deck == kMasterDeck) return c.layer == kNone;
    return c.deck < kDecks && (c.layer == kNone || c.layer < kLayersPerDeck);
}
template <class E>
bool enum_ok(int32_t v) { return v >= 0 && v < static_cast<int32_t>(E::Count); }

bool names_equal(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n) == 0; }

// Start positions for load_clip / change_clip besides 0..1.
constexpr double kFromStart = -2.0;  // the range start (or its end when backward)

}  // namespace

void default_transport(LayerTransport& t, int slot) {
    std::memset(&t, 0, sizeof(t));
    t.mode = static_cast<uint8_t>(PlayMode::Loop);
    t.time_base = static_cast<uint8_t>(TimeBase::Free);
    t.trigger = static_cast<uint8_t>(slot == kHitSlot ? TriggerStyle::Piano : TriggerStyle::Normal);
    t.start = static_cast<uint8_t>(StartPos::Restart);
    t.target = static_cast<uint8_t>(slot == kHitSlot ? ClipTarget::Hit : ClipTarget::Own);
    t.autopilot = static_cast<uint8_t>(AutopilotMode::Off);
    t.transition = static_cast<uint8_t>(TransitionType::Cut);
    t.snap = kNone;  // follow the global grid
    t.inner_loop = 0;
    t.catch_up = 1;      // design default (§2.6)
    t.scratch_snap = 0;  // design default: stay (§2.5)
    t.slices = 8;        // GUESS: chop default 8 slices
    t.step_frames = 1;
    t.autopilot_count = 4;  // GUESS: 4 loops before autopilot moves on
    t.in = 0.0f;
    t.out = 1.0f;
    t.loop_in = 0.25f;   // GUESS: the inner loop starts as the middle half
    t.loop_out = 0.75f;
    t.beats = 4.0f;      // GUESS: a BPM-synced range lasts 4 beats until analysis says otherwise (M7)
    t.interval = 1.0f;
    t.distance = 0.25f;
    t.step_div = 0.25f;
    t.glide = 1.0f;      // design default: 1 beat (§2.7)
    t.fade_in = 0.0f;
    t.fade_out = slot == kHitSlot ? 0.2f : 0.0f;  // GUESS: a hit fades out over 0.2 s on release
    t.transition_time = 0.5f;  // GUESS
    t.seed = static_cast<uint32_t>(slot + 1);
    t.gain = 1.0f;
    t.transition_progress = 1.0f;
    t.velocity = 1.0f;
    t.frame = -1;
    for (auto& q : t.cue) q = -1.0f;
}

DeckModel::DeckModel(const Catalog& catalog) : catalog_(catalog) { reset(); }

LayerState& DeckModel::L(int slot) { return slot < kHitSlot ? live_.deck[slot / 4].layer[slot % 4] : transport_.hit; }
LayerTransport& DeckModel::T(int slot) {
    return slot < kHitSlot ? transport_.layer[slot / 4][slot % 4] : transport_.hit_transport;
}
const LayerState& DeckModel::layer(int slot) const {
    return slot < kHitSlot ? live_.deck[slot / 4].layer[slot % 4] : transport_.hit;
}
const LayerTransport& DeckModel::layer_transport(int slot) const {
    return slot < kHitSlot ? transport_.layer[slot / 4][slot % 4] : transport_.hit_transport;
}

void DeckModel::reset_fx(FxSlotState& s, int effect) {
    std::memset(&s, 0, sizeof(s));
    s.effect = static_cast<int16_t>(effect);
    s.mix = 1.0f;
    if (effect >= 0 && effect < static_cast<int>(catalog_.effect_count)) {
        const EffectInfo& e = catalog_.effect[effect];
        for (int p = 0; p < kFxParams; ++p) s.param[p] = p < e.param_count ? clamp01(e.param_default[p]) : 0.0f;
    }
}

void DeckModel::reset() {
    const uint32_t epoch = live_.epoch;
    std::memset(&live_, 0, sizeof(live_));
    live_.epoch = epoch;
    live_.crossfader = 0.0f;
    live_.program_deck = 0;
    live_.focus_deck = 0;
    live_.focus_layer = 0;
    live_.take_ms = default_take_ms;
    auto empty_layer = [&](LayerState& l) {
        l.clip = -1;
        l.blend = static_cast<uint8_t>(BlendMode::Normal);
        l.playing = 1;
        l.opacity = 1.0f;
        l.speed = 1.0f;
        for (auto& f : l.fx) reset_fx(f, -1);
    };
    for (auto& d : live_.deck) {
        for (auto& l : d.layer) empty_layer(l);
        for (auto& f : d.fx) reset_fx(f, -1);
    }
    for (auto& f : live_.master_fx) reset_fx(f, -1);

    std::memset(&transport_, 0, sizeof(transport_));
    empty_layer(transport_.hit);
    transport_.hit.blend = static_cast<uint8_t>(BlendMode::Add);  // design: the HIT layer adds by default (§3.3)
    for (int s = 0; s < kLayerSlots; ++s) {
        default_transport(T(s), s);
        ph_[s].clear();
        pending_[s] = Pending{};
        fade_[s] = Fade{};
        piano_clip_[s] = -1;
        trans_t_[s] = 0.0;
    }
    clock_.reset(last_tick_us_);
    last_beat_ = clock_.beat(last_tick_us_);
    transport_.clock.bpm = static_cast<float>(clock_.bpm());
    transport_.clock.grid = static_cast<uint8_t>(GridDiv::Off);  // GUESS: no global snap until the owner turns GRID on
    for (auto& c : cues_)
        for (auto& q : c) q = -1.0f;
    std::memset(resume_, 0, sizeof(resume_));
    std::memset(jump_gen_, 0, sizeof(jump_gen_));
    std::memset(jump_to_, 0, sizeof(jump_to_));
    dirty_ = true;
}

const FxSlotState* DeckModel::fx_slot(const Command& c) const {
    if (c.deck == kMasterDeck) return &live_.master_fx[c.slot];
    if (c.layer == kNone) return &live_.deck[c.deck].fx[c.slot];
    return &live_.deck[c.deck].layer[c.layer].fx[c.slot];
}
FxSlotState* DeckModel::fx_slot(const Command& c) {
    return const_cast<FxSlotState*>(static_cast<const DeckModel*>(this)->fx_slot(c));
}

Reject DeckModel::validate(const Command& c) const {
    const auto t = static_cast<uint16_t>(c.type);
    if (t == 0 || t >= static_cast<uint16_t>(CommandType::Count)) return Reject::UnknownType;
    const int slot = cmd_slot(c);
    switch (c.type) {
        case CommandType::LayerClip:
            if (slot < 0) return Reject::BadTarget;
            if (c.i < -1 || c.i >= static_cast<int32_t>(catalog_.clip_count)) return Reject::BadId;
            return Reject::None;
        case CommandType::LayerOpacity:
        case CommandType::LayerSpeed:
        case CommandType::LayerJump:
        case CommandType::LayerBeatLoop:
            if (slot < 0) return Reject::BadTarget;
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::LayerBlend:
            if (slot < 0) return Reject::BadTarget;
            return c.i >= 0 && c.i < static_cast<int32_t>(BlendMode::Count) ? Reject::None : Reject::BadValue;
        case CommandType::LayerMute:
            if (slot < 0) return Reject::BadTarget;
            return is_toggle(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::LayerPlay:
            if (slot < 0) return Reject::BadTarget;
            return enum_ok<PlayAction>(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::FxSet:
            if (!fx_target(c)) return Reject::BadTarget;
            if (c.i < -1 || c.i >= static_cast<int32_t>(catalog_.effect_count)) return Reject::BadId;
            return Reject::None;
        case CommandType::FxParam: {
            if (!fx_target(c) || c.param >= kFxParams) return Reject::BadTarget;
            // A parameter of an empty slot, or one the effect does not have, is not a real target.
            const FxSlotState* s = fx_slot(c);
            if (s->effect < 0 || c.param >= catalog_.effect[s->effect].param_count) return Reject::BadTarget;
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        }
        case CommandType::FxBypass:
            if (!fx_target(c)) return Reject::BadTarget;
            return is_toggle(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::FxMix:
        case CommandType::FxPress:
            if (!fx_target(c)) return Reject::BadTarget;
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::Crossfader:
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::Take:
            return c.i >= 0 ? Reject::None : Reject::BadValue;
        case CommandType::Cut:
        case CommandType::Hello:
            return Reject::None;
        case CommandType::Black:
            return is_toggle(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::Focus:
            if (c.deck == kNone) return Reject::None;  // no focus
            return deck_layer(c) ? Reject::None : Reject::BadTarget;
        // ---- M2 transport ----
        case CommandType::LayerSet: {
            if (slot < 0 || c.param >= static_cast<uint8_t>(LayerParam::Count)) return Reject::BadTarget;
            if (!finite(c.f[0])) return Reject::BadValue;
            switch (static_cast<LayerParam>(c.param)) {
                case LayerParam::Mode: return enum_ok<PlayMode>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::TimeBase: return enum_ok<TimeBase>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Trigger: return enum_ok<TriggerStyle>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::StartPos: return enum_ok<StartPos>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Target: return enum_ok<ClipTarget>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Autopilot: return enum_ok<AutopilotMode>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Transition: return enum_ok<TransitionType>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Snap: return c.i == -1 || enum_ok<GridDiv>(c.i) ? Reject::None : Reject::BadValue;
                case LayerParam::Direction:
                case LayerParam::ReverseHold: return c.i == 0 || c.i == 1 ? Reject::None : Reject::BadValue;
                case LayerParam::InnerLoop:
                case LayerParam::CatchUp:
                case LayerParam::ScratchSnap: return is_toggle(c.i) ? Reject::None : Reject::BadValue;
                default: return Reject::None;  // numbers: clamped
            }
        }
        case CommandType::LayerTrigger:
            if (slot < 0) return Reject::BadTarget;
            if (c.i < -1 || c.i >= static_cast<int32_t>(catalog_.clip_count)) return Reject::BadId;
            if (c.i == -1 && layer(slot).clip < 0) return Reject::BadValue;  // "the layer's clip" on an empty layer
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::LayerCue:
            if (slot < 0 || c.slot >= kCuePoints) return Reject::BadTarget;
            if (!enum_ok<CueAction>(c.i) || !finite(c.f[0])) return Reject::BadValue;
            return layer(slot).clip >= 0 ? Reject::None : Reject::BadTarget;  // cue points belong to a clip
        case CommandType::LayerScratch:
            if (slot < 0) return Reject::BadTarget;
            return (c.i == 0 || c.i == 1) && finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::Clock:
            if (c.param >= static_cast<uint8_t>(ClockParam::Count)) return Reject::BadTarget;
            if (!finite(c.f[0])) return Reject::BadValue;
            if (static_cast<ClockParam>(c.param) == ClockParam::Grid && !enum_ok<GridDiv>(c.i)) return Reject::BadValue;
            return Reject::None;
        default:
            return Reject::UnknownType;
    }
}

Reject DeckModel::submit(int producer, const Command& c, uint64_t now_us) {
    if (producer < 0 || producer >= kProducers) return Reject::BadTarget;
    // §5.5: at most kMaxCommandsPerSecond per producer per one-second window.
    if (now_us - rate_window_us_[producer] >= 1000000 || now_us < rate_window_us_[producer]) {
        rate_window_us_[producer] = now_us;
        rate_count_[producer] = 0;
    }
    Reject r = ++rate_count_[producer] > kMaxCommandsPerSecond ? Reject::RateLimited : validate(c);
    if (r == Reject::None) r = apply(c, now_us);
    if (r == Reject::None) {
        live_.acked_seq[producer] = c.seq;
    } else {
        ++live_.rejected[producer];
        live_.last_reject[producer] = static_cast<uint8_t>(r);
        dirty_ = true;
    }
    return r;
}

bool DeckModel::should_log_reject(int producer, uint64_t now_us) {
    if (producer < 0 || producer >= kProducers) return false;
    if (log_window_us_[producer] && now_us - log_window_us_[producer] < 1000000) return false;
    log_window_us_[producer] = now_us;
    return true;
}

void DeckModel::set_crossfader(float v) {
    live_.crossfader = clamp01(v);
    // program_deck = the side the crossfader rests on. In between, the last end
    // it touched stays PROGRAM. GUESS: hysteresis (only the ends flip); owner may
    // prefer "past the middle" for a manual fader (confidence medium).
    if (live_.crossfader >= 1.0f) live_.program_deck = 1;
    else if (live_.crossfader <= 0.0f) live_.program_deck = 0;
}

void DeckModel::land_take() {
    live_.take_active = 0;
    live_.take_progress = 0.0f;
    set_crossfader(take_to_);
    live_.program_deck = take_to_ >= 0.5f ? 1 : 0;
}

// ---------------------------------------------------------------- transport helpers
ClipGeom DeckModel::geom(int clip) const {
    ClipGeom g;
    if (clip < 0 || clip >= static_cast<int>(catalog_.clip_count)) return g;
    const ClipInfo& ci = catalog_.clip[clip];
    if (ci.fps > 0 && ci.seconds > 0) {
        g.fps = ci.fps;
        g.frames = std::max<int64_t>(1, std::llround(static_cast<double>(ci.seconds) * ci.fps));
    }
    return g;
}

bool DeckModel::waits_for_grid(int slot) const {
    // PREVIEW never waits; only launches on PROGRAM (and the HIT layer, which sits on PROGRAM) do (§3.2).
    if (slot != kHitSlot && slot / kLayersPerDeck != live_.program_deck) return false;
    const LayerTransport& t = layer_transport(slot);
    const auto g = static_cast<GridDiv>(t.snap == kNone ? transport_.clock.grid : t.snap);
    return grid_beats(g) > 0.0;
}

double DeckModel::next_grid_beat(int slot) const {
    const LayerTransport& t = layer_transport(slot);
    const double gb = grid_beats(static_cast<GridDiv>(t.snap == kNone ? transport_.clock.grid : t.snap));
    const double b = clock_.beat(last_tick_us_);
    if (gb <= 0) return b;
    return std::ceil(b / gb - 1e-9) * gb;
}

double DeckModel::cue_frac(int slot, int cue) const {
    const LayerTransport& t = layer_transport(slot);
    if (t.cue[cue] >= 0.0f) return t.cue[cue];
    // No cue point: the 16 equal slices of in..out (§2.4).
    const ClipGeom g = geom(layer(slot).clip);
    if (g.frames <= 0) {
        const double a = std::min(t.in, t.out), b = std::max(t.in, t.out);
        return a + cue * (b - a) / kCuePoints;
    }
    double a, b;
    Playhead::range(g, t, a, b);
    return (a + cue * (b - a) / kCuePoints) / static_cast<double>(g.frames);
}

void DeckModel::warm_frames(int slot, int64_t out[kCuePoints]) const {
    const ClipGeom g = geom(layer(slot).clip);
    for (int k = 0; k < kCuePoints; ++k) {
        if (g.frames <= 0) {
            out[k] = -1;
            continue;
        }
        // Exactly the arithmetic of a jump (jump_now -> Playhead::jump -> frame()).
        const double pos = cue_frac(slot, k) * static_cast<double>(g.frames);
        out[k] = std::clamp<int64_t>(static_cast<int64_t>(std::floor(std::clamp(pos, 0.0, static_cast<double>(g.frames) - 1e-6))),
                                     0, g.frames - 1);
    }
}

void DeckModel::load_clip(int slot, int clip, double frac, float velocity) {
    LayerState& l = L(slot);
    LayerTransport& t = T(slot);
    if (l.clip >= 0 && l.clip < kMaxClips) resume_[l.clip] = l.position;
    l.clip = static_cast<int16_t>(clip);
    l.missing = 0;
    l.playing = 1;
    t.loops = 0;
    t.velocity = velocity;
    t.frame = -1;
    ++jump_gen_[slot];
    dirty_ = true;
    if (clip < 0) {
        ph_[slot].clear();
        l.position = 0.0f;
        jump_to_[slot] = 0.0f;
        for (auto& q : t.cue) q = -1.0f;
        return;
    }
    std::memcpy(t.cue, cues_[clip], sizeof(t.cue));
    const ClipGeom g = geom(clip);
    if (frac < 0.0) {
        if (frac > kFromStart && t.start == static_cast<uint8_t>(StartPos::Continue)) {
            frac = resume_[clip];
        } else if (g.frames > 0) {
            double a, b;
            Playhead::range(g, t, a, b);
            frac = (t.backward ? b - 1e-6 : a) / static_cast<double>(g.frames);
        } else {
            frac = t.backward ? t.out : t.in;
        }
    }
    frac = std::clamp(frac, 0.0, 1.0);
    ph_[slot].seed(t.seed ^ (static_cast<uint32_t>(clip + 1) * 2654435761u));
    if (g.frames > 0) ph_[slot].start(g, t, frac * static_cast<double>(g.frames), l.speed);
    else ph_[slot].clear();
    l.position = static_cast<float>(frac);
    jump_to_[slot] = static_cast<float>(frac);
}

void DeckModel::start_fade(int slot, float from, float to, double dur, FadeThen then, int clip, double frac,
                           float velocity) {
    Fade& f = fade_[slot];
    f.active = true;
    f.from = from;
    f.to = to;
    f.t = 0.0;
    f.dur = dur;
    f.then = then;
    f.clip = static_cast<int16_t>(clip);
    f.frac = frac;
    f.velocity = velocity;
    T(slot).gain = from;
    if (dur <= 0.0) finish_fade(slot);
}

void DeckModel::finish_fade(int slot) {
    Fade& f = fade_[slot];
    f.active = false;
    T(slot).gain = f.to;
    switch (f.then) {
        case FadeThen::Eject:
            load_clip(slot, -1, 0.0, 1.0f);
            T(slot).gain = 1.0f;
            break;
        case FadeThen::Load: {  // the black-dip's second half: new clip, fade back in
            const double half = f.dur;
            load_clip(slot, f.clip, f.frac, f.velocity);
            start_fade(slot, 0.0f, 1.0f, half, FadeThen::Nothing);
            break;
        }
        default: break;
    }
}

void DeckModel::change_clip(int slot, int clip, double frac, float velocity) {
    LayerTransport& t = T(slot);
    const bool had = L(slot).clip >= 0;
    const auto type = static_cast<TransitionType>(t.transition);
    const double tt = t.transition_time;
    fade_[slot].active = false;
    t.gain = 1.0f;
    if (had && clip >= 0 && type == TransitionType::Fade && tt > 0.0) {
        // The renderer keeps the old picture (transition_gen) and crossfades to the new clip.
        load_clip(slot, clip, frac, velocity);
        ++trans_gen_[slot];
        trans_t_[slot] = 0.0;
        t.transition_progress = 0.0f;
    } else if (had && type == TransitionType::BlackDip && tt > 0.0) {
        // Out to transparent over half the time, swap, back in over the other half.
        if (clip >= 0) start_fade(slot, 1.0f, 0.0f, tt * 0.5, FadeThen::Load, clip, frac, velocity);
        else start_fade(slot, 1.0f, 0.0f, tt * 0.5, FadeThen::Eject);
    } else if (had && clip < 0 && type == TransitionType::Fade && tt > 0.0) {
        start_fade(slot, 1.0f, 0.0f, tt, FadeThen::Eject);
    } else {
        load_clip(slot, clip, frac, velocity);
        t.transition_progress = 1.0f;
        // A clip on an empty layer fades in by its fade-in time (§3.1); 0 = cut.
        if (!had && clip >= 0 && t.fade_in > 0.0f) start_fade(slot, 0.0f, 1.0f, t.fade_in, FadeThen::Nothing);
    }
}

void DeckModel::launch(int slot, int clip, bool legato, float velocity) {
    if (waits_for_grid(slot)) {
        Pending& p = pending_[slot];
        p.kind = PendingKind::Launch;
        p.clip = static_cast<int16_t>(clip);
        p.legato = legato;
        p.velocity = velocity;
        p.due_beat = next_grid_beat(slot);
        T(slot).pending = 1;
        dirty_ = true;
        return;
    }
    pending_[slot].kind = PendingKind::None;
    T(slot).pending = 0;
    // LEGATO: start where the clip it replaces was (same layer).
    const double frac = legato && L(slot).clip >= 0 ? static_cast<double>(L(slot).position) : -1.0;
    change_clip(slot, clip, frac, velocity);
}

void DeckModel::jump_now(int slot, double frac) {
    LayerState& l = L(slot);
    frac = std::clamp(frac, 0.0, 1.0);
    const ClipGeom g = geom(l.clip);
    if (g.frames > 0) ph_[slot].jump(g, T(slot), frac * static_cast<double>(g.frames));
    l.position = static_cast<float>(frac);
    jump_to_[slot] = static_cast<float>(frac);
    ++jump_gen_[slot];
}

void DeckModel::stop_layer(int slot, float fade_s) {
    pending_[slot].kind = PendingKind::None;
    T(slot).pending = 0;
    if (L(slot).clip < 0) return;
    start_fade(slot, T(slot).gain, 0.0f, fade_s, FadeThen::Eject);
}

int DeckModel::pick_clip(int slot, AutopilotMode how) {
    const int n = static_cast<int>(catalog_.clip_count);
    const int cur = L(slot).clip;
    if (n <= 0) return -1;
    switch (how) {
        case AutopilotMode::Next: return cur < 0 ? 0 : (cur + 1) % n;
        case AutopilotMode::Previous: return cur < 0 ? n - 1 : (cur + n - 1) % n;
        case AutopilotMode::Random: {
            // GUESS: uniform from the whole catalog until banks and mood fit exist (M7); never the same clip twice.
            if (n == 1) return 0;
            const int r = static_cast<int>(ph_[slot].rng_next() % static_cast<uint32_t>(n - 1));
            return cur < 0 ? r : (r >= cur ? r + 1 : r);
        }
        default: return cur;
    }
}

void DeckModel::autopilot(int slot, AutopilotMode how) {
    if (how == AutopilotMode::Off || how == AutopilotMode::Same) return;
    if (pending_[slot].kind == PendingKind::Launch) return;  // already waiting for the beat
    const int next = pick_clip(slot, how);
    if (next < 0) return;
    // On PROGRAM the change is on the beat (§3.4); PREVIEW changes now.
    if (slot == kHitSlot || slot / kLayersPerDeck == live_.program_deck) {
        Pending& p = pending_[slot];
        p.kind = PendingKind::Launch;
        p.clip = static_cast<int16_t>(next);
        p.legato = false;
        p.velocity = T(slot).velocity;
        const double b = clock_.beat(last_tick_us_);
        p.due_beat = std::ceil(b - 1e-9);
        T(slot).pending = 1;
        dirty_ = true;
    } else {
        change_clip(slot, next, -1.0, T(slot).velocity);
    }
}

void DeckModel::on_ended(int slot) {
    switch (static_cast<PlayMode>(T(slot).mode)) {
        case PlayMode::OnceEject: stop_layer(slot, T(slot).fade_out); break;
        case PlayMode::OnceNext: autopilot(slot, AutopilotMode::Next); break;
        default: break;  // ONCE · HOLD keeps the last frame
    }
    dirty_ = true;
}

Reject DeckModel::apply_layer_set(int slot, const Command& c) {
    LayerTransport& t = T(slot);
    const ClipGeom g = geom(L(slot).clip);
    auto fl = [&](float cur, float lo, float hi) { return std::clamp(with_delta(cur, c), lo, hi); };
    switch (static_cast<LayerParam>(c.param)) {
        case LayerParam::Mode:
            t.mode = static_cast<uint8_t>(c.i);
            ph_[slot].mode_changed();
            break;
        case LayerParam::Direction: t.backward = static_cast<uint8_t>(c.i); break;
        case LayerParam::In: t.in = fl(t.in, 0.0f, 1.0f); break;
        case LayerParam::Out: t.out = fl(t.out, 0.0f, 1.0f); break;
        case LayerParam::LoopIn: t.loop_in = fl(t.loop_in, 0.0f, 1.0f); break;
        case LayerParam::LoopOut: t.loop_out = fl(t.loop_out, 0.0f, 1.0f); break;
        case LayerParam::InnerLoop:
            t.inner_loop = toggled(t.inner_loop, c.i);
            if (g.frames > 0) ph_[slot].refresh_inner(g, t);
            break;
        case LayerParam::TimeBase: t.time_base = static_cast<uint8_t>(c.i); break;
        case LayerParam::Beats: t.beats = fl(t.beats, 0.25f, 64.0f); break;
        case LayerParam::Interval: t.interval = fl(t.interval, 1.0f / 16.0f, 64.0f); break;
        case LayerParam::Distance: t.distance = fl(t.distance, 0.0f, 1.0f); break;
        case LayerParam::Slices: t.slices = static_cast<uint8_t>(std::clamp(c.i, 2, 16)); break;
        case LayerParam::StepFrames: t.step_frames = static_cast<uint8_t>(std::clamp(c.i, 1, 64)); break;
        case LayerParam::StepDiv: t.step_div = fl(t.step_div, 1.0f / 16.0f, 16.0f); break;
        case LayerParam::Glide: t.glide = fl(t.glide, 0.0f, 16.0f); break;
        case LayerParam::Trigger: t.trigger = static_cast<uint8_t>(c.i); break;
        case LayerParam::StartPos: t.start = static_cast<uint8_t>(c.i); break;
        case LayerParam::FadeIn: t.fade_in = fl(t.fade_in, 0.0f, 10.0f); break;
        case LayerParam::FadeOut: t.fade_out = fl(t.fade_out, 0.0f, 10.0f); break;
        case LayerParam::Snap: t.snap = c.i < 0 ? kNone : static_cast<uint8_t>(c.i); break;
        case LayerParam::Target: t.target = static_cast<uint8_t>(c.i); break;
        case LayerParam::Autopilot: t.autopilot = static_cast<uint8_t>(c.i); break;
        case LayerParam::AutopilotCount: t.autopilot_count = static_cast<uint8_t>(std::clamp(c.i, 1, 64)); break;
        case LayerParam::Transition: t.transition = static_cast<uint8_t>(c.i); break;
        case LayerParam::TransitionTime: t.transition_time = fl(t.transition_time, 0.0f, 10.0f); break;
        case LayerParam::CatchUp: t.catch_up = toggled(t.catch_up, c.i); break;
        case LayerParam::ScratchSnap: t.scratch_snap = toggled(t.scratch_snap, c.i); break;
        case LayerParam::ReverseHold:
            t.reverse_hold = static_cast<uint8_t>(c.i);
            ph_[slot].reverse(c.i != 0);
            break;
        case LayerParam::Seed:
            t.seed = static_cast<uint32_t>(c.i);
            ph_[slot].seed(t.seed);
            break;
        default: return Reject::BadTarget;
    }
    return Reject::None;
}

Reject DeckModel::apply_trigger(int s0, const Command& c) {
    const LayerTransport& t0 = T(s0);
    const int clip = c.i >= 0 ? c.i : L(s0).clip;
    const float vel = c.f[0] > 0.0f ? clamp01(c.f[0]) : 1.0f;
    const bool release = (c.flags & kRelease) != 0;
    // GUESS: the trigger style, target and snap follow the layer the pad addresses (the design
    // keeps them per clip, which needs the library, M7); fades follow the layer that plays it.
    const auto style = static_cast<TriggerStyle>(t0.trigger);
    int s = s0;
    if (s0 != kHitSlot) {
        if (t0.target == static_cast<uint8_t>(ClipTarget::Hit)) {
            s = kHitSlot;
        } else if (t0.target == static_cast<uint8_t>(ClipTarget::Free)) {
            const int base = (s0 / kLayersPerDeck) * kLayersPerDeck;
            int found = -1;
            for (int l = 0; l < kLayersPerDeck && found < 0; ++l)  // a release finds the layer its press used
                if (release ? piano_clip_[base + l] == clip : (L(base + l).clip < 0 && pending_[base + l].kind == PendingKind::None))
                    found = base + l;
            s = found >= 0 ? found : s0;
        }
    }
    const bool momentary = style == TriggerStyle::Piano || s == kHitSlot;
    if (release) {
        if (momentary && piano_clip_[s] == clip) {
            piano_clip_[s] = -1;
            if (pending_[s].kind == PendingKind::Launch && pending_[s].clip == clip) {
                pending_[s].kind = PendingKind::None;  // let go before the beat: never starts
                T(s).pending = 0;
            } else {
                stop_layer(s, T(s).fade_out);
            }
        }
        return Reject::None;
    }
    if (style == TriggerStyle::Toggle && L(s).clip == clip && !(fade_[s].active && fade_[s].to <= 0.0f)) {
        stop_layer(s, T(s).fade_out);
        return Reject::None;
    }
    if (s == kHitSlot) L(s).opacity = vel;  // a hit: velocity = start opacity (§8.1)
    piano_clip_[s] = momentary ? static_cast<int16_t>(clip) : static_cast<int16_t>(-1);
    launch(s, clip, style == TriggerStyle::Legato, vel);
    return Reject::None;
}

Reject DeckModel::apply(const Command& c, uint64_t now_us) {
    dirty_ = true;
    const int slot = cmd_slot(c);
    switch (c.type) {
        case CommandType::LayerClip:
            // A plain load (not a pad launch): never waits for the grid; uses the layer transition.
            pending_[slot].kind = PendingKind::None;
            T(slot).pending = 0;
            piano_clip_[slot] = -1;
            // GUESS: a plain load starts at the range start, whatever StartPos says (CONTINUE is for pad launches).
            change_clip(slot, c.i, kFromStart, 1.0f);
            break;
        case CommandType::LayerOpacity: {
            LayerState& l = L(slot);
            l.opacity = clamp01(with_delta(l.opacity, c));
            break;
        }
        case CommandType::LayerBlend: L(slot).blend = static_cast<uint8_t>(c.i); break;
        case CommandType::LayerMute: {
            LayerState& l = L(slot);
            l.mute = toggled(l.mute, c.i);
            break;
        }
        case CommandType::LayerPlay: {
            LayerState& l = L(slot);
            switch (static_cast<PlayAction>(c.i)) {
                case PlayAction::Pause: l.playing = 0; break;
                case PlayAction::Play: l.playing = 1; break;
                case PlayAction::Toggle: l.playing = static_cast<uint8_t>(!l.playing); break;
                case PlayAction::Restart: {
                    l.playing = 1;
                    ph_[slot].mode_changed();
                    const ClipGeom g = geom(l.clip);
                    double frac = T(slot).in;
                    if (g.frames > 0) {
                        double a, b;
                        Playhead::range(g, T(slot), a, b);
                        frac = (T(slot).backward ? b - 1e-6 : a) / static_cast<double>(g.frames);
                    }
                    jump_now(slot, frac);
                    break;
                }
                case PlayAction::Release: ph_[slot].release_inner(); break;
                case PlayAction::Stop: stop_layer(slot, T(slot).fade_out); break;
                default: break;
            }
            break;
        }
        case CommandType::LayerSpeed: {
            LayerState& l = L(slot);  // the target; the playhead glides to it (§2.7)
            l.speed = std::clamp(with_delta(l.speed, c), -4.0f, 4.0f);
            break;
        }
        case CommandType::LayerJump: jump_now(slot, clamp01(c.f[0])); break;
        case CommandType::FxSet: reset_fx(*fx_slot(c), c.i); break;
        case CommandType::FxParam: {
            FxSlotState* s = fx_slot(c);
            s->param[c.param] = clamp01(with_delta(s->param[c.param], c));
            break;
        }
        case CommandType::FxBypass: {
            FxSlotState* s = fx_slot(c);
            s->bypass = toggled(s->bypass, c.i);
            break;
        }
        case CommandType::FxMix: {
            FxSlotState* s = fx_slot(c);
            s->mix = clamp01(with_delta(s->mix, c));
            break;
        }
        case CommandType::FxPress: {  // absolute only; the renderer shapes velocity / release (himmel::fx::resolve)
            FxSlotState* s = fx_slot(c);
            s->press = static_cast<uint8_t>(std::lround(clamp01(c.f[0]) * 255.0f));
            break;
        }
        case CommandType::Crossfader:
            live_.take_active = 0;  // a manual move cancels a running TAKE
            live_.take_progress = 0.0f;
            set_crossfader(with_delta(live_.crossfader, c));
            break;
        case CommandType::Take:
            if (live_.take_active) {
                // GUESS: TAKE during a TAKE finishes it now (like a second press on a
                // vision mixer's AUTO); confidence low, owner to decide.
                land_take();
                break;
            }
            live_.take_ms = c.i == 0 ? default_take_ms : std::min<uint32_t>(static_cast<uint32_t>(c.i), kMaxTakeMs);
            take_from_ = live_.crossfader;
            take_to_ = live_.program_deck == 0 ? 1.0f : 0.0f;
            take_start_us_ = now_us;
            live_.take_active = 1;
            live_.take_progress = 0.0f;
            break;
        case CommandType::Cut:
            take_to_ = live_.program_deck == 0 ? 1.0f : 0.0f;
            land_take();
            break;
        case CommandType::Black: live_.black = toggled(live_.black, c.i); break;
        case CommandType::Focus:
            live_.focus_deck = c.deck;
            live_.focus_layer = c.deck == kNone ? kNone : c.layer;
            break;
        case CommandType::Hello: dirty_ = false; break;  // only keeps the producer alive
        // ---- M2 transport ----
        case CommandType::LayerSet: return apply_layer_set(slot, c);
        case CommandType::LayerTrigger: return apply_trigger(slot, c);
        case CommandType::LayerCue: {
            LayerTransport& t = T(slot);
            const int k = c.slot;
            switch (static_cast<CueAction>(c.i)) {
                case CueAction::Jump:
                    // Tap = jump: on the grid on PROGRAM, at once on PREVIEW (§8.3). A jump ends a beat loop.
                    if (waits_for_grid(slot)) {
                        Pending& p = pending_[slot];
                        p.kind = PendingKind::Jump;
                        p.cue = static_cast<uint8_t>(k);
                        p.due_beat = next_grid_beat(slot);
                        t.pending = 1;
                    } else {
                        jump_now(slot, cue_frac(slot, k));
                    }
                    break;
                case CueAction::Set: {
                    const float v = c.f[0] < 0.0f ? L(slot).position : clamp01(c.f[0]);
                    t.cue[k] = v;
                    cues_[L(slot).clip][k] = v;
                    break;
                }
                case CueAction::Clear:
                    t.cue[k] = -1.0f;
                    cues_[L(slot).clip][k] = -1.0f;
                    break;
                default: break;
            }
            break;
        }
        case CommandType::LayerBeatLoop: {
            const ClipGeom g = geom(L(slot).clip);
            if (c.f[0] <= 0.0f) ph_[slot].beat_loop_off(T(slot).catch_up != 0);
            else if (g.frames > 0)
                ph_[slot].beat_loop(g, T(slot), std::clamp(static_cast<double>(c.f[0]), 1.0 / 16.0, 16.0), clock_.bpm());
            break;
        }
        case CommandType::LayerScratch: {
            const ClipGeom g = geom(L(slot).clip);
            if (g.frames <= 0) break;
            if (c.i == 0) {
                ph_[slot].scratch_release(T(slot).scratch_snap != 0);
                break;
            }
            const double p = (c.flags & kRelative) ? ph_[slot].pos() + static_cast<double>(c.f[0]) * g.fps
                                                   : static_cast<double>(clamp01(c.f[0])) * static_cast<double>(g.frames);
            ph_[slot].scratch_move(g, T(slot), p);
            break;
        }
        case CommandType::Clock:
            switch (static_cast<ClockParam>(c.param)) {
                case ClockParam::Tap: clock_.tap(now_us); break;
                case ClockParam::Bpm: {
                    const float cur = static_cast<float>(clock_.bpm());
                    clock_.set_bpm(with_delta(cur, c), now_us);
                    break;
                }
                case ClockParam::Nudge: clock_.nudge(std::clamp(static_cast<double>(c.f[0]), -1.0, 1.0)); break;
                case ClockParam::Resync: clock_.resync(now_us); break;
                case ClockParam::Grid: transport_.clock.grid = static_cast<uint8_t>(c.i); break;
                default: break;
            }
            last_beat_ = clock_.beat(now_us);  // a clock change is not a jump of every beat timer
            break;
        default: return Reject::UnknownType;
    }
    return Reject::None;
}

void DeckModel::tick(uint64_t now_us) {
    if (live_.take_active) {
        const uint32_t ms = std::max<uint32_t>(1, live_.take_ms);
        const float p = clamp01(static_cast<float>((now_us - std::min(now_us, take_start_us_)) / 1000.0 / ms));
        if (p >= 1.0f) {
            land_take();
        } else {
            live_.take_progress = p;
            live_.crossfader = take_from_ + (take_to_ - take_from_) * p;
        }
        dirty_ = true;
    }
    tick_transport(now_us);
}

void DeckModel::tick_transport(uint64_t now_us) {
    // GUESS: a stall longer than 250 ms (debugger, sleep) is not played through; catch-up stays in phase otherwise.
    const double dt = last_tick_us_ && now_us > last_tick_us_ ? std::min(0.25, (now_us - last_tick_us_) / 1e6) : 0.0;
    last_tick_us_ = now_us;
    TickIn in;
    in.dt = dt;
    in.beat1 = clock_.beat(now_us);
    in.beat0 = dt > 0.0 ? std::min(last_beat_, in.beat1) : in.beat1;  // no beat events on the first tick
    in.bpm = clock_.bpm();
    last_beat_ = in.beat1;

    for (int s = 0; s < kLayerSlots; ++s) {
        LayerState& l = L(s);
        LayerTransport& t = T(s);
        // Launches and jumps waiting for the beat snap.
        Pending& p = pending_[s];
        if (p.kind != PendingKind::None) {
            t.pending_beats = static_cast<float>(std::max(0.0, p.due_beat - in.beat1));
            if (in.beat1 >= p.due_beat - 1e-9) {
                const Pending fire = p;
                p.kind = PendingKind::None;
                t.pending = 0;
                t.pending_beats = 0.0f;
                if (fire.kind == PendingKind::Launch) {
                    const double frac = fire.legato && l.clip >= 0 ? static_cast<double>(l.position) : -1.0;
                    change_clip(s, fire.clip, frac, fire.velocity);
                } else if (l.clip >= 0) {
                    jump_now(s, cue_frac(s, fire.cue));
                }
                dirty_ = true;
            }
        }
        // Fades (trigger fade in / out, black-dip, eject).
        Fade& f = fade_[s];
        if (f.active) {
            f.t += dt;
            if (f.t >= f.dur) {
                finish_fade(s);
                dirty_ = true;
            } else {
                t.gain = f.from + (f.to - f.from) * static_cast<float>(f.t / f.dur);
            }
        }
        // FADE transition progress.
        if (t.transition_progress < 1.0f) {
            trans_t_[s] += dt;
            const double tt = std::max(1e-3, static_cast<double>(t.transition_time));
            t.transition_progress = static_cast<float>(std::min(1.0, trans_t_[s] / tt));
            if (t.transition_progress >= 1.0f) dirty_ = true;
        }
        // The playhead.
        if (l.clip < 0) {
            t.phase = static_cast<uint8_t>(PlayheadPhase::Empty);
            t.frame = -1;
            continue;
        }
        const ClipGeom g = geom(l.clip);
        if (g.frames <= 0) continue;  // length unknown: nothing to play (the renderer shows the placeholder)
        if (!ph_[s].active()) ph_[s].start(g, t, l.position * static_cast<double>(g.frames), l.speed);
        const uint32_t ev = ph_[s].advance(g, t, l.playing != 0, l.speed, in);
        if (ev & kEvLooped) {
            ++t.loops;
            const auto ap = static_cast<AutopilotMode>(t.autopilot);
            if (ap != AutopilotMode::Off && t.loops >= t.autopilot_count) autopilot(s, ap);
        }
        if (ev & kEvEnded) on_ended(s);
        if (l.clip < 0) continue;  // ejected by a cut fade
        l.position = static_cast<float>(ph_[s].pos() / static_cast<double>(g.frames));
        t.frame = static_cast<int32_t>(ph_[s].frame());
        t.speed_now = static_cast<float>(ph_[s].speed_now());
        const bool holding = ph_[s].ended() && t.mode == static_cast<uint8_t>(PlayMode::OnceHold);
        t.phase = static_cast<uint8_t>(holding ? PlayheadPhase::Holding : ph_[s].phase(l.playing != 0));
        t.beat_loop = ph_[s].beat_looping() ? 1 : 0;
        t.beat_loop_beats = static_cast<float>(ph_[s].beat_loop_beats());
        t.scratching = ph_[s].scratching() ? 1 : 0;
    }
    ClockState& cs = transport_.clock;
    cs.bpm = static_cast<float>(clock_.bpm());
    cs.beat = in.beat1;
    cs.time_us = now_us;
    cs.taps = clock_.taps();
    cs.source = clock_.tapped() ? 1 : 0;
}

void DeckModel::set_position(int deck, int layer, float pos01) { live_.deck[deck].layer[layer].position = pos01; }

void DeckModel::set_missing(int deck, int layer, bool missing) { set_missing_slot(slot_of(deck, layer), missing); }

void DeckModel::set_missing_slot(int slot, bool missing) {
    uint8_t& m = L(slot).missing;
    if (m != static_cast<uint8_t>(missing)) m = static_cast<uint8_t>(missing), dirty_ = true;
}

void DeckModel::set_frame(uint64_t frame, uint64_t time_us, float fps) {
    live_.frame = frame;
    live_.time_us = time_us;
    live_.fps = fps;
}

void DeckModel::set_producer_alive(int producer, bool alive) {
    uint8_t& a = live_.producer_alive[producer];
    if (a != static_cast<uint8_t>(alive)) a = static_cast<uint8_t>(alive), dirty_ = true;
}

namespace {
// Settings from an old block, each one checked (a reader never trusts shared memory).
void copy_settings(LayerTransport& d, const LayerTransport& s) {
    auto e = [](uint8_t v, int count, uint8_t def) { return v < count ? v : def; };
    auto f = [](float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
    d.mode = e(s.mode, static_cast<int>(PlayMode::Count), d.mode);
    d.time_base = e(s.time_base, static_cast<int>(TimeBase::Count), d.time_base);
    d.trigger = e(s.trigger, static_cast<int>(TriggerStyle::Count), d.trigger);
    d.start = e(s.start, static_cast<int>(StartPos::Count), d.start);
    d.target = e(s.target, static_cast<int>(ClipTarget::Count), d.target);
    d.autopilot = e(s.autopilot, static_cast<int>(AutopilotMode::Count), d.autopilot);
    d.transition = e(s.transition, static_cast<int>(TransitionType::Count), d.transition);
    d.snap = s.snap == kNone ? kNone : e(s.snap, static_cast<int>(GridDiv::Count), d.snap);
    d.inner_loop = s.inner_loop ? 1 : 0;
    d.catch_up = s.catch_up ? 1 : 0;
    d.scratch_snap = s.scratch_snap ? 1 : 0;
    d.backward = s.backward ? 1 : 0;
    d.slices = static_cast<uint8_t>(std::clamp<int>(s.slices, 2, 16));
    d.step_frames = static_cast<uint8_t>(std::clamp<int>(s.step_frames, 1, 64));
    d.autopilot_count = static_cast<uint8_t>(std::clamp<int>(s.autopilot_count, 1, 64));
    d.in = f(s.in, 0, 1, d.in);
    d.out = f(s.out, 0, 1, d.out);
    d.loop_in = f(s.loop_in, 0, 1, d.loop_in);
    d.loop_out = f(s.loop_out, 0, 1, d.loop_out);
    d.beats = f(s.beats, 0.25f, 64, d.beats);
    d.interval = f(s.interval, 1.0f / 16, 64, d.interval);
    d.distance = f(s.distance, 0, 1, d.distance);
    d.step_div = f(s.step_div, 1.0f / 16, 16, d.step_div);
    d.glide = f(s.glide, 0, 16, d.glide);
    d.fade_in = f(s.fade_in, 0, 10, d.fade_in);
    d.fade_out = f(s.fade_out, 0, 10, d.fade_out);
    d.transition_time = f(s.transition_time, 0, 10, d.transition_time);
    d.seed = s.seed;
}
}  // namespace

int DeckModel::restore(const LiveState& old, const Catalog& old_cat, const TransportSection* old_tr) {
    reset();
    int lost = 0;
    auto clip_id = [&](int16_t id) -> int16_t {
        if (id < 0 || id >= static_cast<int>(old_cat.clip_count) || id >= kMaxClips) return -1;
        for (uint32_t i = 0; i < catalog_.clip_count; ++i)
            if (names_equal(catalog_.clip[i].name, old_cat.clip[id].name, kNameLen)) return static_cast<int16_t>(i);
        ++lost;
        return -1;
    };
    auto fx = [&](FxSlotState& dst, const FxSlotState& src) {
        int16_t id = -1;
        if (src.effect >= 0 && src.effect < static_cast<int>(old_cat.effect_count) && src.effect < kMaxEffects)
            for (uint32_t i = 0; i < catalog_.effect_count; ++i)
                if (names_equal(catalog_.effect[i].name, old_cat.effect[src.effect].name, kNameLen))
                    id = static_cast<int16_t>(i);
        reset_fx(dst, id);
        if (id < 0) return;
        dst.bypass = src.bypass ? 1 : 0;
        dst.mix = finite(src.mix) ? clamp01(src.mix) : 1.0f;
        for (int p = 0; p < kFxParams; ++p) dst.param[p] = finite(src.param[p]) ? clamp01(src.param[p]) : dst.param[p];
    };
    for (int d = 0; d < kDecks; ++d) {
        for (int l = 0; l < kLayersPerDeck; ++l) {
            const LayerState& s = old.deck[d].layer[l];
            LayerState& t = live_.deck[d].layer[l];
            const int slot = slot_of(d, l);
            t.clip = clip_id(s.clip);
            t.blend = s.blend < static_cast<uint8_t>(BlendMode::Count) ? s.blend : 0;
            t.mute = s.mute ? 1 : 0;
            t.playing = s.playing ? 1 : 0;
            t.opacity = finite(s.opacity) ? clamp01(s.opacity) : 1.0f;
            t.speed = finite(s.speed) ? std::clamp(s.speed, -4.0f, 4.0f) : 1.0f;
            t.position = finite(s.position) && t.clip >= 0 ? clamp01(s.position) : 0.0f;
            jump_to_[slot] = t.position;
            ++jump_gen_[slot];
            for (int f = 0; f < kFxSlots; ++f) fx(t.fx[f], s.fx[f]);
            if (old_tr) {
                const LayerTransport& ot = old_tr->layer[d][l];
                copy_settings(T(slot), ot);
                if (t.clip >= 0)
                    for (int k = 0; k < kCuePoints; ++k)
                        cues_[t.clip][k] = finite(ot.cue[k]) && ot.cue[k] >= 0.0f ? clamp01(ot.cue[k]) : -1.0f;
            }
            if (t.clip >= 0) {
                std::memcpy(T(slot).cue, cues_[t.clip], sizeof(T(slot).cue));
                const ClipGeom g = geom(t.clip);
                if (g.frames > 0) ph_[slot].start(g, T(slot), t.position * static_cast<double>(g.frames), t.speed);
            }
        }
        for (int f = 0; f < kFxSlots; ++f) fx(live_.deck[d].fx[f], old.deck[d].fx[f]);
    }
    for (int f = 0; f < kFxSlots; ++f) fx(live_.master_fx[f], old.master_fx[f]);
    if (old_tr) {
        if (finite(old_tr->clock.bpm)) clock_.set_bpm(old_tr->clock.bpm, last_tick_us_);
        transport_.clock.bpm = static_cast<float>(clock_.bpm());
        transport_.clock.grid = old_tr->clock.grid < static_cast<uint8_t>(GridDiv::Count) ? old_tr->clock.grid : 0;
    }
    live_.crossfader = finite(old.crossfader) ? clamp01(old.crossfader) : 0.0f;
    live_.program_deck = old.program_deck ? 1 : 0;
    live_.black = old.black ? 1 : 0;
    const bool focus_ok = old.focus_deck < kDecks && old.focus_layer < kLayersPerDeck;
    live_.focus_deck = focus_ok ? old.focus_deck : kNone;
    live_.focus_layer = focus_ok ? old.focus_layer : kNone;
    dirty_ = true;
    return lost;
}

const char* reject_name(Reject r) {
    switch (r) {
        case Reject::None: return "ok";
        case Reject::UnknownType: return "unknown type";
        case Reject::BadTarget: return "bad target";
        case Reject::BadId: return "bad id";
        case Reject::BadValue: return "bad value";
        case Reject::RateLimited: return "rate limited";
        default: return "?";
    }
}

const char* command_name(CommandType t) {
    static constexpr const char* kNames[] = {"nop",   "clip",    "opacity", "blend",    "mute",    "play",
                                             "speed", "jump",    "fx",      "param",    "bypass",  "mix",
                                             "xfade", "take",    "cut",     "black",    "focus",   "hello",
                                             "set",   "trigger", "cue",     "beatloop", "scratch", "clock",
                                             "press"};
    const auto i = static_cast<size_t>(t);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

}  // namespace himmel::engine
