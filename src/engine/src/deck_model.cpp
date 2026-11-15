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

bool layer_target(const Command& c) { return c.deck < kDecks && c.layer < kLayersPerDeck; }
bool fx_target(const Command& c) {
    if (c.slot >= kFxSlots) return false;
    if (c.deck == kMasterDeck) return c.layer == kNone;
    return c.deck < kDecks && (c.layer == kNone || c.layer < kLayersPerDeck);
}

bool names_equal(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n) == 0; }

}  // namespace

DeckModel::DeckModel(const Catalog& catalog) : catalog_(catalog) { reset(); }

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
    for (auto& d : live_.deck) {
        for (auto& l : d.layer) {
            l.clip = -1;
            l.blend = static_cast<uint8_t>(BlendMode::Normal);
            l.playing = 1;
            l.opacity = 1.0f;
            l.speed = 1.0f;
            for (auto& f : l.fx) reset_fx(f, -1);
        }
        for (auto& f : d.fx) reset_fx(f, -1);
    }
    for (auto& f : live_.master_fx) reset_fx(f, -1);
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
    switch (c.type) {
        case CommandType::LayerClip:
            if (!layer_target(c)) return Reject::BadTarget;
            if (c.i < -1 || c.i >= static_cast<int32_t>(catalog_.clip_count)) return Reject::BadId;
            return Reject::None;
        case CommandType::LayerOpacity:
        case CommandType::LayerSpeed:
        case CommandType::LayerJump:
            if (!layer_target(c)) return Reject::BadTarget;
            return finite(c.f[0]) ? Reject::None : Reject::BadValue;
        case CommandType::LayerBlend:
            if (!layer_target(c)) return Reject::BadTarget;
            return c.i >= 0 && c.i < static_cast<int32_t>(BlendMode::Count) ? Reject::None : Reject::BadValue;
        case CommandType::LayerMute:
            if (!layer_target(c)) return Reject::BadTarget;
            return is_toggle(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::LayerPlay:
            if (!layer_target(c)) return Reject::BadTarget;
            return c.i >= 0 && c.i <= static_cast<int32_t>(PlayAction::Restart) ? Reject::None : Reject::BadValue;
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
            return layer_target(c) ? Reject::None : Reject::BadTarget;
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

Reject DeckModel::apply(const Command& c, uint64_t now_us) {
    dirty_ = true;
    switch (c.type) {
        case CommandType::LayerClip: {
            LayerState& l = live_.deck[c.deck].layer[c.layer];
            l.clip = static_cast<int16_t>(c.i);
            l.missing = 0;
            l.position = 0.0f;
            l.playing = 1;
            jump_to_[c.deck][c.layer] = 0.0f;
            ++jump_gen_[c.deck][c.layer];
            break;
        }
        case CommandType::LayerOpacity: {
            LayerState& l = live_.deck[c.deck].layer[c.layer];
            l.opacity = clamp01(with_delta(l.opacity, c));
            break;
        }
        case CommandType::LayerBlend: live_.deck[c.deck].layer[c.layer].blend = static_cast<uint8_t>(c.i); break;
        case CommandType::LayerMute: {
            LayerState& l = live_.deck[c.deck].layer[c.layer];
            l.mute = toggled(l.mute, c.i);
            break;
        }
        case CommandType::LayerPlay: {
            LayerState& l = live_.deck[c.deck].layer[c.layer];
            switch (static_cast<PlayAction>(c.i)) {
                case PlayAction::Pause: l.playing = 0; break;
                case PlayAction::Play: l.playing = 1; break;
                case PlayAction::Toggle: l.playing = static_cast<uint8_t>(!l.playing); break;
                case PlayAction::Restart:
                    l.playing = 1;
                    l.position = 0.0f;
                    jump_to_[c.deck][c.layer] = 0.0f;
                    ++jump_gen_[c.deck][c.layer];
                    break;
            }
            break;
        }
        case CommandType::LayerSpeed: {
            LayerState& l = live_.deck[c.deck].layer[c.layer];
            l.speed = std::clamp(with_delta(l.speed, c), -4.0f, 4.0f);
            break;
        }
        case CommandType::LayerJump: {
            const float p = clamp01(c.f[0]);
            live_.deck[c.deck].layer[c.layer].position = p;
            jump_to_[c.deck][c.layer] = p;
            ++jump_gen_[c.deck][c.layer];
            break;
        }
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
        default: return Reject::UnknownType;
    }
    return Reject::None;
}

void DeckModel::tick(uint64_t now_us) {
    if (!live_.take_active) return;
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

void DeckModel::set_position(int deck, int layer, float pos01) { live_.deck[deck].layer[layer].position = pos01; }

void DeckModel::set_missing(int deck, int layer, bool missing) {
    uint8_t& m = live_.deck[deck].layer[layer].missing;
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

int DeckModel::restore(const LiveState& old, const Catalog& old_cat) {
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
            t.clip = clip_id(s.clip);
            t.blend = s.blend < static_cast<uint8_t>(BlendMode::Count) ? s.blend : 0;
            t.mute = s.mute ? 1 : 0;
            t.playing = s.playing ? 1 : 0;
            t.opacity = finite(s.opacity) ? clamp01(s.opacity) : 1.0f;
            t.speed = finite(s.speed) ? std::clamp(s.speed, -4.0f, 4.0f) : 1.0f;
            t.position = finite(s.position) && t.clip >= 0 ? clamp01(s.position) : 0.0f;
            jump_to_[d][l] = t.position;
            ++jump_gen_[d][l];
            for (int f = 0; f < kFxSlots; ++f) fx(t.fx[f], s.fx[f]);
        }
        for (int f = 0; f < kFxSlots; ++f) fx(live_.deck[d].fx[f], old.deck[d].fx[f]);
    }
    for (int f = 0; f < kFxSlots; ++f) fx(live_.master_fx[f], old.master_fx[f]);
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
    static constexpr const char* kNames[] = {"nop",   "clip",       "opacity", "blend", "mute",  "play",
                                             "speed", "jump",       "fx",      "param", "bypass", "mix",
                                             "xfade", "take",       "cut",     "black", "focus", "hello"};
    const auto i = static_cast<size_t>(t);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

}  // namespace himmel::engine
