// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// fake_engine: a stand-in for himmel_render so the GUI can be built before the
// real engine slice lands (S3). It creates the state, preview and command
// blocks exactly as the contracts say, with a fake catalog (14 clips,
// 7 effects), applies commands roughly (layers, FX, crossfader, TAKE / CUT,
// black, focus), validates them like the engine (so refusals show up), and
// draws moving CPU test pictures into the preview streams.
//
// It is NOT the engine: no GPU, no clips, no timing guarantees. Effects are
// rough CPU look-alikes, only so you can see that a parameter does something.
//
//   fake_engine [--fps 60] [--reject-rate]   Ctrl+C to stop.
#include "himmel/ui/shm_win.h"

#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "himmel/protocol/preview.h"
#include "himmel/protocol/state.h"

using namespace himmel::protocol;
using himmel::ui::ShmStatus;
using himmel::ui::ShmView;
using Clock = std::chrono::steady_clock;

namespace {

volatile bool g_quit = false;
BOOL WINAPI on_ctrl(DWORD) {
    g_quit = true;
    return TRUE;
}

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void copy_name(char* dst, size_t cap, const char* src) {
    std::snprintf(dst, cap, "%s", src);
}

// ---------------------------------------------------------------- catalog
struct FakeClip {
    const char* name;
    uint32_t rgb;
    int pattern;
    float seconds;
    float fps;
    uint8_t codec;
};
const FakeClip kClips[] = {
    {"Ocean Drift.mov", 0x1AA3A3, 0, 24, 30, 0},     {"Neon Tunnel.mov", 0xFF2E88, 1, 12, 60, 2},
    {"Sunrise Haze.mov", 0xFF9F1C, 2, 30, 30, 0},    {"Ink Bloom.mov", 0x6A5ACD, 1, 16, 30, 0},
    {"City Lines.mov", 0x2EC5FF, 3, 20, 30, 2},      {"Particles Alpha.mov", 0xE8ECF1, 2, 10, 60, 1},
    {"Lava Flow.mov", 0xFF5A1C, 0, 18, 30, 0},       {"Grid Pulse.mov", 0x30D158, 3, 8, 60, 2},
    {"Smoke Rings.mov", 0x9AA4B2, 1, 22, 30, 0},     {"Laser Fan.mov", 0xB78CFF, 0, 6, 60, 2},
    {"Rain Glass.mov", 0x4A90D9, 2, 26, 30, 0},      {"Logo Alpha.mov", 0xFFD60A, 3, 4, 30, 1},
    {"Broken file.mov", 0x803040, 0, 10, 30, 3},     // opens as "missing" (placeholder colour)
    {"Deep Space.mov", 0x203080, 2, 40, 30, 0},
};
constexpr int kClipCount = static_cast<int>(std::size(kClips));
constexpr int kMissingClip = 12;

enum Fx { FxHue, FxKaleido, FxMirror, FxInvert, FxStrobe, FxZoom, FxRgbSplit, FxCount };
struct FakeFx {
    const char* name;
    int n;
    const char* p[3];
    float d[3];
};
const FakeFx kFx[FxCount] = {
    {"Hue", 1, {"Turn"}, {0.25f}},
    {"Kaleido", 2, {"Segments", "Spin"}, {0.5f, 0.2f}},
    {"Mirror", 1, {"Axis"}, {0.f}},
    {"Invert", 1, {"Amount"}, {1.f}},
    {"Strobe", 2, {"Rate", "Duty"}, {0.5f, 0.5f}},
    {"Zoom", 2, {"Amount", "Pulse"}, {0.3f, 0.f}},
    {"RGB Split", 2, {"Distance", "Angle"}, {0.3f, 0.f}},
};

void fill_catalog(Catalog& c) {
    std::memset(&c, 0, sizeof c);
    c.clip_count = kClipCount;
    for (int k = 0; k < kClipCount; ++k) {
        ClipInfo& ci = c.clip[k];
        copy_name(ci.name, kNameLen, kClips[k].name);
        ci.width = 1920;
        ci.height = 1080;
        ci.fps = kClips[k].fps;
        ci.seconds = kClips[k].seconds;
        ci.codec = kClips[k].codec;
        ci.thumb_rgb = kClips[k].rgb;
    }
    c.effect_count = FxCount;
    for (int k = 0; k < FxCount; ++k) {
        EffectInfo& e = c.effect[k];
        copy_name(e.name, kNameLen, kFx[k].name);
        e.param_count = static_cast<uint8_t>(kFx[k].n);
        for (int p = 0; p < kFx[k].n; ++p) {
            copy_name(e.param_name[p], kParamNameLen, kFx[k].p[p]);
            e.param_default[p] = kFx[k].d[p];
        }
    }
}

void clear_fx(FxSlotState& f) {
    std::memset(&f, 0, sizeof f);
    f.effect = -1;
    f.mix = 1.f;
}

void init_live(LiveState& s) {
    std::memset(&s, 0, sizeof s);
    s.program_deck = 0;
    s.crossfader = 0;
    s.take_ms = 2000;
    s.focus_deck = 1;  // PREVIEW deck, layer 1 (D48-D53: start focus = PREP)
    s.focus_layer = 0;
    for (int d = 0; d < kDecks; ++d) {
        for (int l = 0; l < kLayersPerDeck; ++l) {
            LayerState& L = s.deck[d].layer[l];
            L.clip = -1;
            L.opacity = 1.f;
            L.speed = 1.f;
            L.playing = 1;
            for (auto& f : L.fx) clear_fx(f);
        }
        for (auto& f : s.deck[d].fx) clear_fx(f);
    }
    for (auto& f : s.master_fx) clear_fx(f);
    // Something to look at right away.
    s.deck[0].layer[0].clip = 0;
    s.deck[0].layer[1].clip = 5;
    s.deck[0].layer[1].opacity = 0.6f;
    s.deck[0].layer[1].blend = static_cast<uint8_t>(BlendMode::Screen);
    s.deck[1].layer[0].clip = 1;
}

// ---------------------------------------------------------------- commands
// The state v2 transport section, a light stand-in for the real M2 transport: settings are
// stored and shown, cue points / beat loop / HIT work roughly; modes are not simulated.
void init_transport_layer(LayerTransport& t) {
    std::memset(&t, 0, sizeof t);
    t.snap = kNone;
    t.slices = 8;
    t.step_frames = 1;
    t.autopilot_count = 4;
    t.scratch_snap = 1;
    t.catch_up = 1;
    t.out = 1.f;
    t.loop_out = 1.f;
    t.beats = 4.f;
    t.interval = 1.f;
    t.distance = 0.5f;
    t.step_div = 0.25f;
    t.glide = 1.f;
    t.fade_out = 0.5f;
    t.transition_time = 0.5f;
    t.gain = 1.f;
    t.transition_progress = 1.f;
    t.frame = -1;
    t.phase = static_cast<uint8_t>(PlayheadPhase::Empty);
    for (float& q : t.cue) q = -1.f;
}
void init_transport(TransportSection& t) {
    std::memset(&t, 0, sizeof t);
    t.clock.bpm = 120.f;
    for (auto& d : t.layer)
        for (auto& l : d) init_transport_layer(l);
    init_transport_layer(t.hit_transport);
    t.hit.clip = -1;
    t.hit.opacity = 1.f;
    t.hit.speed = 1.f;
    for (auto& f : t.hit.fx) clear_fx(f);
}

struct Model {
    LiveState s;
    TransportSection t;
    float cues[256][kCuePoints];  // per clip (cue points belong to the clip)
    // TAKE
    uint64_t take_start_us = 0;
    float take_from = 0, take_to = 1;
    // rate limit
    uint32_t count_this_second[kProducers] = {};
    uint64_t second_start_us = 0;
};

FxSlotState* fx_target(LiveState& s, const Command& c) {
    if (c.slot >= kFxSlots) return nullptr;
    if (c.deck == kMasterDeck) return c.layer == kNone ? &s.master_fx[c.slot] : nullptr;
    if (c.deck >= kDecks) return nullptr;
    if (c.layer == kNone) return &s.deck[c.deck].fx[c.slot];
    if (c.layer >= kLayersPerDeck) return nullptr;
    return &s.deck[c.deck].layer[c.layer].fx[c.slot];
}

uint8_t toggle(uint8_t cur, int32_t v) { return v == 2 ? static_cast<uint8_t>(!cur) : static_cast<uint8_t>(v != 0); }

Reject apply(Model& m, const Command& c, uint64_t now) {
    LiveState& s = m.s;
    const bool rel = (c.flags & kRelative) != 0;
    const float f = c.f[0];
    auto layer = [&]() -> LayerState* {
        if (c.deck == kHitDeck && c.layer == 0) return &m.t.hit;
        if (c.deck >= kDecks || c.layer >= kLayersPerDeck) return nullptr;
        return &s.deck[c.deck].layer[c.layer];
    };
    auto ltr = [&]() -> LayerTransport* {
        if (c.deck == kHitDeck && c.layer == 0) return &m.t.hit_transport;
        if (c.deck >= kDecks || c.layer >= kLayersPerDeck) return nullptr;
        return &m.t.layer[c.deck][c.layer];
    };
    auto fl = [&](float cur, float lo, float hi) { return std::clamp(rel ? cur + f : f, lo, hi); };
    switch (c.type) {
        case CommandType::Hello: return Reject::None;
        case CommandType::LayerSet: {
            LayerTransport* t = ltr();
            if (!t || c.param >= static_cast<uint8_t>(LayerParam::Count)) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            auto en = [&](int n, uint8_t& dst) { if (c.i < 0 || c.i >= n) return false; dst = static_cast<uint8_t>(c.i); return true; };
            bool ok = true;
            switch (static_cast<LayerParam>(c.param)) {
                case LayerParam::Mode: ok = en(static_cast<int>(PlayMode::Count), t->mode); break;
                case LayerParam::Direction: ok = en(2, t->backward); break;
                case LayerParam::In: t->in = fl(t->in, 0, 1); break;
                case LayerParam::Out: t->out = fl(t->out, 0, 1); break;
                case LayerParam::LoopIn: t->loop_in = fl(t->loop_in, 0, 1); break;
                case LayerParam::LoopOut: t->loop_out = fl(t->loop_out, 0, 1); break;
                case LayerParam::InnerLoop: t->inner_loop = toggle(t->inner_loop, c.i); break;
                case LayerParam::TimeBase: ok = en(static_cast<int>(TimeBase::Count), t->time_base); break;
                case LayerParam::Beats: t->beats = fl(t->beats, 0.25f, 64); break;
                case LayerParam::Interval: t->interval = fl(t->interval, 1.f / 16, 64); break;
                case LayerParam::Distance: t->distance = fl(t->distance, 0, 1); break;
                case LayerParam::Slices: t->slices = static_cast<uint8_t>(std::clamp(c.i, 2, 16)); break;
                case LayerParam::StepFrames: t->step_frames = static_cast<uint8_t>(std::clamp(c.i, 1, 64)); break;
                case LayerParam::StepDiv: t->step_div = fl(t->step_div, 1.f / 16, 16); break;
                case LayerParam::Glide: t->glide = fl(t->glide, 0, 16); break;
                case LayerParam::Trigger: ok = en(static_cast<int>(TriggerStyle::Count), t->trigger); break;
                case LayerParam::StartPos: ok = en(static_cast<int>(StartPos::Count), t->start); break;
                case LayerParam::FadeIn: t->fade_in = fl(t->fade_in, 0, 10); break;
                case LayerParam::FadeOut: t->fade_out = fl(t->fade_out, 0, 10); break;
                case LayerParam::Snap:
                    if (c.i == -1) t->snap = kNone;
                    else ok = en(static_cast<int>(GridDiv::Count), t->snap);
                    break;
                case LayerParam::Target: ok = en(static_cast<int>(ClipTarget::Count), t->target); break;
                case LayerParam::Autopilot: ok = en(static_cast<int>(AutopilotMode::Count), t->autopilot); break;
                case LayerParam::AutopilotCount: t->autopilot_count = static_cast<uint8_t>(std::clamp(c.i, 1, 64)); break;
                case LayerParam::Transition: ok = en(static_cast<int>(TransitionType::Count), t->transition); break;
                case LayerParam::TransitionTime: t->transition_time = fl(t->transition_time, 0, 10); break;
                case LayerParam::CatchUp: t->catch_up = toggle(t->catch_up, c.i); break;
                case LayerParam::ScratchSnap: t->scratch_snap = toggle(t->scratch_snap, c.i); break;
                case LayerParam::ReverseHold: ok = en(2, t->reverse_hold); break;
                case LayerParam::Seed: t->seed = static_cast<uint32_t>(c.i); break;
                default: break;
            }
            return ok ? Reject::None : Reject::BadValue;
        }
        case CommandType::LayerTrigger: {
            LayerState* L = layer();
            LayerTransport* t = ltr();
            if (!L || !t) return Reject::BadTarget;
            if (c.i < -1 || c.i >= kClipCount) return Reject::BadId;
            if (c.i == -1 && L->clip < 0) return Reject::BadValue;
            if (c.flags & kRelease) {
                if (c.deck == kHitDeck) L->clip = -1;  // GUESS: the fake drops the HIT clip at once (no fade)
                return Reject::None;
            }
            if (c.i >= 0) L->clip = static_cast<int16_t>(c.i);
            L->position = 0;
            L->playing = 1;
            t->velocity = f > 0 ? f : 1.f;
            if (c.deck == kHitDeck) L->opacity = t->velocity;
            return Reject::None;
        }
        case CommandType::LayerCue: {
            LayerState* L = layer();
            if (!L || c.slot >= kCuePoints) return Reject::BadTarget;
            if (L->clip < 0) return Reject::BadTarget;
            float* q = m.cues[L->clip];
            switch (static_cast<CueAction>(c.i)) {
                case CueAction::Jump: L->position = q[c.slot] >= 0 ? q[c.slot] : c.slot / 16.f; break;
                case CueAction::Set: q[c.slot] = f < 0 ? L->position : std::min(f, 1.f); break;
                case CueAction::Clear: q[c.slot] = -1.f; break;
                default: return Reject::BadValue;
            }
            return Reject::None;
        }
        case CommandType::LayerBeatLoop: {
            LayerTransport* t = ltr();
            if (!t) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            t->beat_loop = f > 0;
            t->beat_loop_beats = f > 0 ? std::clamp(f, 1.f / 16, 16.f) : 0.f;
            return Reject::None;
        }
        case CommandType::LayerScratch: {
            LayerState* L = layer();
            LayerTransport* t = ltr();
            if (!L || !t) return Reject::BadTarget;
            if ((c.i != 0 && c.i != 1) || !std::isfinite(f)) return Reject::BadValue;
            t->scratching = static_cast<uint8_t>(c.i);
            if (!rel) L->position = std::clamp(f, 0.f, 1.f);
            return Reject::None;
        }
        case CommandType::Clock: {
            ClockState& k = m.t.clock;
            if (!std::isfinite(f)) return Reject::BadValue;
            switch (static_cast<ClockParam>(c.param)) {
                case ClockParam::Tap: k.source = 1; ++k.taps; break;
                case ClockParam::Bpm: k.bpm = std::clamp(rel ? k.bpm + f : f, 20.f, 300.f); break;
                case ClockParam::Nudge: k.beat += std::clamp(f, -1.f, 1.f); break;
                case ClockParam::Resync: k.beat = 0; break;
                case ClockParam::Grid:
                    if (c.i < 0 || c.i >= static_cast<int>(GridDiv::Count)) return Reject::BadValue;
                    k.grid = static_cast<uint8_t>(c.i);
                    break;
                default: return Reject::BadTarget;
            }
            return Reject::None;
        }
        case CommandType::FxPress: {
            FxSlotState* fx = fx_target(s, c);
            if (!fx) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            fx->press = static_cast<uint8_t>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f);
            return Reject::None;
        }
        case CommandType::LayerClip: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (c.i < -1 || c.i >= kClipCount) return Reject::BadId;
            L->clip = static_cast<int16_t>(c.i);
            L->position = 0;
            L->missing = c.i == kMissingClip;
            return Reject::None;
        }
        case CommandType::LayerOpacity: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            L->opacity = std::clamp(rel ? L->opacity + f : f, 0.f, 1.f);
            return Reject::None;
        }
        case CommandType::LayerBlend: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (c.i < 0 || c.i >= static_cast<int>(BlendMode::Count)) return Reject::BadValue;
            L->blend = static_cast<uint8_t>(c.i);
            return Reject::None;
        }
        case CommandType::LayerMute: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (c.i < 0 || c.i > 2) return Reject::BadValue;
            L->mute = toggle(L->mute, c.i);
            return Reject::None;
        }
        case CommandType::LayerPlay: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (c.i < 0 || c.i >= static_cast<int>(PlayAction::Count)) return Reject::BadValue;
            if (c.i == static_cast<int>(PlayAction::Stop)) {
                L->clip = -1;  // GUESS: the fake empties at once (the engine fades out first)
            } else if (c.i == static_cast<int>(PlayAction::Release)) {
                if (LayerTransport* t = ltr()) t->inner_loop = 0;
            } else if (c.i == 3) {
                L->position = L->speed < 0 ? 1.f : 0.f;
                L->playing = 1;
            } else {
                L->playing = toggle(L->playing, c.i);
            }
            return Reject::None;
        }
        case CommandType::LayerSpeed: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            L->speed = std::clamp(rel ? L->speed + f : f, -4.f, 4.f);
            return Reject::None;
        }
        case CommandType::LayerJump: {
            LayerState* L = layer();
            if (!L) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            L->position = std::clamp(f, 0.f, 1.f);
            return Reject::None;
        }
        case CommandType::FxSet: {
            FxSlotState* fx = fx_target(s, c);
            if (!fx) return Reject::BadTarget;
            if (c.i < -1 || c.i >= FxCount) return Reject::BadId;
            clear_fx(*fx);
            fx->effect = static_cast<int16_t>(c.i);
            if (c.i >= 0)
                for (int p = 0; p < kFx[c.i].n; ++p) fx->param[p] = kFx[c.i].d[p];
            return Reject::None;
        }
        case CommandType::FxParam: {
            FxSlotState* fx = fx_target(s, c);
            if (!fx || c.param >= kFxParams) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            fx->param[c.param] = std::clamp(rel ? fx->param[c.param] + f : f, 0.f, 1.f);
            return Reject::None;
        }
        case CommandType::FxBypass: {
            FxSlotState* fx = fx_target(s, c);
            if (!fx) return Reject::BadTarget;
            if (c.i < 0 || c.i > 2) return Reject::BadValue;
            fx->bypass = toggle(fx->bypass, c.i);
            return Reject::None;
        }
        case CommandType::FxMix: {
            FxSlotState* fx = fx_target(s, c);
            if (!fx) return Reject::BadTarget;
            if (!std::isfinite(f)) return Reject::BadValue;
            fx->mix = std::clamp(rel ? fx->mix + f : f, 0.f, 1.f);
            return Reject::None;
        }
        case CommandType::Crossfader: {
            if (!std::isfinite(f)) return Reject::BadValue;
            s.take_active = 0;
            s.crossfader = std::clamp(rel ? s.crossfader + f : f, 0.f, 1.f);
            if (s.crossfader <= 0.f) s.program_deck = 0;
            if (s.crossfader >= 1.f) s.program_deck = 1;
            return Reject::None;
        }
        case CommandType::Take: {
            if (c.i < 0) return Reject::BadValue;
            if (s.take_active) return Reject::None;  // GUESS: a TAKE while one travels is ignored
            s.take_ms = c.i > 0 ? static_cast<uint32_t>(c.i) : 2000u;
            m.take_from = s.crossfader;
            m.take_to = s.program_deck == 0 ? 1.f : 0.f;
            m.take_start_us = now;
            s.take_active = 1;
            s.take_progress = 0;
            return Reject::None;
        }
        case CommandType::Cut: {
            s.take_active = 0;
            s.program_deck = static_cast<uint8_t>(1 - s.program_deck);
            s.crossfader = s.program_deck == 1 ? 1.f : 0.f;
            return Reject::None;
        }
        case CommandType::Black: {
            if (c.i < 0 || c.i > 2) return Reject::BadValue;
            s.black = toggle(s.black, c.i);
            return Reject::None;
        }
        case CommandType::Focus: {
            if (c.deck >= kDecks || c.layer >= kLayersPerDeck) return Reject::BadTarget;
            s.focus_deck = c.deck;
            s.focus_layer = c.layer;
            return Reject::None;
        }
        default: return Reject::UnknownType;
    }
}

void drain(Model& m, CommandBlock& cb, uint64_t now) {
    if (now - m.second_start_us >= 1000000) {
        m.second_start_us = now;
        std::memset(m.count_this_second, 0, sizeof m.count_this_second);
    }
    for (int p = 0; p < kProducers; ++p) {
        Command c;
        for (int n = 0; n < 256 && cb.rings[p].pop(c); ++n) {
            Reject r = Reject::None;
            if (++m.count_this_second[p] > kMaxCommandsPerSecond) r = Reject::RateLimited;
            else r = apply(m, c, now);
            if (r != Reject::None) {
                ++m.s.rejected[p];
                m.s.last_reject[p] = static_cast<uint8_t>(r);
                std::printf("[fake_engine] refused command type %u from producer %d (reason %u)\n",
                            static_cast<unsigned>(c.type), p, static_cast<unsigned>(r));
            }
            m.s.acked_seq[p] = c.seq;
        }
        const uint64_t hb = cb.producers[p].heartbeat_us.load(std::memory_order_acquire);
        m.s.producer_alive[p] = (hb != 0 && now >= hb && now - hb < 2000000) ? 1 : 0;
    }
}

void step(Model& m, double dt, uint64_t now) {
    LiveState& s = m.s;
    for (int d = 0; d < kDecks; ++d)
        for (int l = 0; l < kLayersPerDeck; ++l) {
            LayerState& L = s.deck[d].layer[l];
            if (L.clip < 0 || !L.playing) continue;
            const float len = kClips[L.clip].seconds;
            float p = L.position + static_cast<float>(dt) * L.speed / len;
            p -= std::floor(p);
            L.position = p;
        }
    // Transport status (state v2).
    TransportSection& T = m.t;
    T.clock.beat += dt / 60.0 * T.clock.bpm;
    T.clock.time_us = now;
    auto status = [&](LayerState& L, LayerTransport& t) {
        if (L.clip < 0) {
            t.phase = static_cast<uint8_t>(PlayheadPhase::Empty);
            t.frame = -1;
            for (float& q : t.cue) q = -1.f;
            return;
        }
        t.phase = static_cast<uint8_t>(!L.playing ? PlayheadPhase::Paused
                                        : t.inner_loop ? PlayheadPhase::InnerLoop
                                                       : PlayheadPhase::Playing);
        t.frame = static_cast<int32_t>(L.position * kClips[L.clip].seconds * 30.f);
        t.speed_now = L.speed;
        t.gain = 1.f;
        std::memcpy(t.cue, m.cues[L.clip], sizeof t.cue);
    };
    for (int d = 0; d < kDecks; ++d)
        for (int l = 0; l < kLayersPerDeck; ++l) status(s.deck[d].layer[l], T.layer[d][l]);
    if (T.hit.clip >= 0 && T.hit.playing) {
        T.hit.position += static_cast<float>(dt) * T.hit.speed / kClips[T.hit.clip].seconds;
        if (T.hit.position >= 1.f) T.hit.clip = -1;  // a one-shot
    }
    status(T.hit, T.hit_transport);
    if (s.take_active) {
        const float t = std::clamp(static_cast<float>(now - m.take_start_us) / (s.take_ms * 1000.f), 0.f, 1.f);
        s.take_progress = t;
        s.crossfader = m.take_from + (m.take_to - m.take_from) * t;
        if (t >= 1.f) {
            s.take_active = 0;
            s.take_progress = 0;
            s.program_deck = static_cast<uint8_t>(1 - s.program_deck);
            s.crossfader = s.program_deck == 1 ? 1.f : 0.f;
        }
    }
}

// ---------------------------------------------------------------- pictures
constexpr int W = static_cast<int>(kPreviewW), H = static_cast<int>(kPreviewH), N = W * H;
struct Px {
    uint8_t b, g, r, a;
};
using Img = std::vector<Px>;

uint8_t u8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 255.f)); }

void draw_clip(Img& out, int clip, float t, bool missing) {
    const FakeClip& fc = kClips[clip];
    const float br = static_cast<float>((fc.rgb >> 16) & 255), bg = static_cast<float>((fc.rgb >> 8) & 255),
                bb = static_cast<float>(fc.rgb & 255);
    if (missing) {
        for (auto& p : out) p = {u8(bb), u8(bg), u8(br), 255};
        return;
    }
    const int shift = static_cast<int>(t * 60.f);
    const int bar = static_cast<int>(std::fmod(t * 0.25f, 1.f) * W);  // a sweeping bar: motion you can see
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float k;
            switch (fc.pattern) {
                case 0: k = static_cast<float>(((x + y + shift) >> 3) & 15) / 15.f; break;               // stripes
                case 1: {                                                                                // rings
                    const int dx = x - W / 2, dy = y - H / 2;
                    k = static_cast<float>((((dx * dx + dy * dy) >> 7) - shift) & 31) / 31.f;
                    break;
                }
                case 2: k = 0.5f + 0.5f * std::sin(x * 0.03f + t * 2.f) * std::cos(y * 0.04f - t * 1.3f); break;  // waves
                default: k = static_cast<float>((((x + shift) >> 5) ^ ((y + shift / 2) >> 5)) & 1); break;   // checker
            }
            float r = br * (0.25f + 0.75f * k), g = bg * (0.25f + 0.75f * k), b = bb * (0.25f + 0.75f * k);
            if (std::abs(x - bar) < 3) r = g = b = 255;
            out[y * W + x] = {u8(b), u8(g), u8(r), 255};
        }
}

void apply_fx(Img& img, Img& tmp, const FxSlotState& f, uint64_t frame, float t) {
    if (f.effect < 0 || f.bypass || f.mix <= 0.f) return;
    const float mix = f.mix;
    const float* p = f.param;
    tmp = img;
    auto sample = [&](int x, int y) -> const Px& {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        return tmp[y * W + x];
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            Px src = tmp[y * W + x], o = src;
            switch (f.effect) {
                case FxHue: {  // rough: rotate the channels
                    const float a = p[0];
                    const float r = src.r, g = src.g, b = src.b;
                    const float c0 = 1.f - a, c1 = a;
                    o = {u8(b * c0 + g * c1), u8(g * c0 + r * c1), u8(r * c0 + b * c1), 255};
                    break;
                }
                case FxKaleido: {
                    const int seg = 2 + static_cast<int>(p[0] * 6.f);
                    const int cw = W / seg;
                    int xx = x % (2 * cw);
                    if (xx >= cw) xx = 2 * cw - 1 - xx;
                    int yy = y < H / 2 ? y : H - 1 - y;
                    xx += static_cast<int>(t * p[1] * 100.f) % W;
                    o = sample(xx, yy);
                    break;
                }
                case FxMirror: o = p[0] < 0.5f ? sample(x < W / 2 ? x : W - 1 - x, y) : sample(x, y < H / 2 ? y : H - 1 - y); break;
                case FxInvert: {
                    const float a = p[0];
                    o = {u8(src.b + (255 - 2 * src.b) * a), u8(src.g + (255 - 2 * src.g) * a), u8(src.r + (255 - 2 * src.r) * a), 255};
                    break;
                }
                case FxStrobe: {
                    const uint64_t period = 2 + static_cast<uint64_t>((1.f - p[0]) * 20.f);
                    const bool off = static_cast<float>(frame % period) >= p[1] * period;
                    if (off) o = {0, 0, 0, 255};
                    break;
                }
                case FxZoom: {
                    const float z = 1.f + p[0] * 2.f + p[1] * 0.5f * (0.5f + 0.5f * std::sin(t * 6.f));
                    o = sample(W / 2 + static_cast<int>((x - W / 2) / z), H / 2 + static_cast<int>((y - H / 2) / z));
                    break;
                }
                case FxRgbSplit: {
                    const float d = p[0] * 30.f, ang = p[1] * 6.2832f;
                    const int ox = static_cast<int>(std::cos(ang) * d), oy = static_cast<int>(std::sin(ang) * d);
                    o = {sample(x - ox, y - oy).b, src.g, sample(x + ox, y + oy).r, 255};
                    break;
                }
                default: break;
            }
            Px& d = img[y * W + x];
            d = {u8(src.b + (o.b - src.b) * mix), u8(src.g + (o.g - src.g) * mix), u8(src.r + (o.r - src.r) * mix), 255};
        }
}

float blend1(float d, float s, int mode) {
    switch (static_cast<BlendMode>(mode)) {
        case BlendMode::Add: return std::min(1.f, d + s);
        case BlendMode::Screen: return 1.f - (1.f - d) * (1.f - s);
        case BlendMode::Multiply: return d * s;
        case BlendMode::Difference: return std::fabs(d - s);
        case BlendMode::Lighten: return std::max(d, s);
        default: return s;
    }
}

void composite(Img& dst, const Img& src, float a, int mode) {
    for (int k = 0; k < N; ++k) {
        Px& d = dst[k];
        const Px& s = src[k];
        auto ch = [&](uint8_t dv, uint8_t sv) {
            const float df = dv / 255.f, sf = sv / 255.f;
            return u8((df + (blend1(df, sf, mode) - df) * a) * 255.f);
        };
        d = {ch(d.b, s.b), ch(d.g, s.g), ch(d.r, s.r), 255};
    }
}

void render_deck(const LiveState& s, int d, Img& out, Img& layer, Img& tmp, uint64_t frame, float t) {
    std::fill(out.begin(), out.end(), Px{0, 0, 0, 255});
    for (int l = 0; l < kLayersPerDeck; ++l) {  // GUESS: layer 1 is the bottom layer
        const LayerState& L = s.deck[d].layer[l];
        if (L.clip < 0 || L.mute || L.opacity <= 0.f) continue;
        draw_clip(layer, L.clip, L.position * kClips[L.clip].seconds, L.missing != 0);
        for (const auto& f : L.fx) apply_fx(layer, tmp, f, frame, t);
        composite(out, layer, L.opacity, l == 0 ? 0 : L.blend);
    }
    for (const auto& f : s.deck[d].fx) apply_fx(out, tmp, f, frame, t);
}

void publish(PreviewStreamBlock& sb, const Img& img, uint64_t frame) {
    const uint32_t next = (sb.latest.load(std::memory_order_relaxed) + 1) % kPreviewBuffers;
    PreviewBuffer& b = sb.buffer[next];
    seq_write_bytes(b.seq, b.pixels, reinterpret_cast<const uint8_t*>(img.data()), static_cast<size_t>(N) * 4);
    b.frame = frame;
    sb.latest.store(next, std::memory_order_release);
    sb.published.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace

int main(int argc, char** argv) {
    int fps = 60;
    for (int k = 1; k < argc; ++k) {
        if (!std::strcmp(argv[k], "--fps") && k + 1 < argc) fps = std::clamp(std::atoi(argv[++k]), 10, 240);
        else if (!std::strcmp(argv[k], "--help")) {
            std::printf("fake_engine [--fps 60]\n  Fake Local\\HimmelState / HimmelPreview / HimmelCommands for GUI work.\n");
            return 0;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // logs show at once when redirected
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    timeBeginPeriod(1);

    ShmView state, preview, commands;
    const ShmStatus st = state.create_or_open(kStateShmName, sizeof(StateBlock), kStateMagic, kStateVersion);
    const ShmStatus pv = preview.create_or_open(kPreviewShmName, sizeof(PreviewBlock), kPreviewMagic, kPreviewVersion);
    ShmStatus cm = commands.create_or_open(kCommandShmName, sizeof(CommandBlock), kCommandMagic, kCommandVersion);
    for (int wait = 0; cm == ShmStatus::Starting && wait < 20; ++wait) {  // another creator is busy
        Sleep(50);
        cm = commands.check(sizeof(CommandBlock), kCommandMagic, kCommandVersion);
    }
    if (st == ShmStatus::Absent || st == ShmStatus::Version || pv == ShmStatus::Absent || pv == ShmStatus::Version ||
        cm != ShmStatus::Ok) {
        std::printf("[fake_engine] cannot get the blocks (state %s, preview %s, commands %s). Is the real engine running?\n",
                    himmel::ui::shm_status_name(st), himmel::ui::shm_status_name(pv), himmel::ui::shm_status_name(cm));
        return 2;
    }
    if (commands.created()) commands.write_header(sizeof(CommandBlock), kCommandMagic, kCommandVersion);

    auto* sb = state.as<StateBlock>();
    // Never fight a live engine for the blocks (the names are per user session, so another
    // agent's himmel_render on this PC uses the same ones).
    if (!state.created() && st == ShmStatus::Ok) {
        const uint64_t b1 = sb->status.heartbeat_us.load();
        Sleep(400);
        if (sb->status.heartbeat_us.load() != b1) {
            std::printf("[fake_engine] another engine (pid %u) is writing %ls; stop it first.\n",
                        sb->status.pid.load(), kStateShmName);
            return 3;
        }
    }
    auto* pb = preview.as<PreviewBlock>();
    auto* cb = commands.as<CommandBlock>();

    // Engine (re)start rules (command-protocol.md §7, engine-state.md §2).
    for (auto& r : cb->rings) r.skip_all();
    auto m = std::make_unique<Model>();
    const bool restore = !state.created() && st == ShmStatus::Ok;
    uint32_t old_epoch = 0;
    if (restore && seq_read(sb->live_seq, sb->live, m->s)) {
        old_epoch = m->s.epoch;
        m->s.take_active = 0;
        std::printf("[fake_engine] restored the show from the existing state block (epoch %u)\n", old_epoch);
    } else {
        init_live(m->s);
    }
    m->s.epoch = old_epoch ? old_epoch + 1 : (GetTickCount() & 0xFFFF) | 1;  // a new epoch every start
    init_transport(m->t);
    for (auto& c : m->cues)
        for (float& q : c) q = -1.f;

    auto cat = std::make_unique<Catalog>();
    fill_catalog(*cat);
    seq_write(sb->catalog_seq, sb->catalog, *cat);
    sb->status.pid.store(GetCurrentProcessId());
    seq_write(sb->live_seq, sb->live, m->s);
    state.write_header(sizeof(StateBlock), kStateMagic, kStateVersion);

    for (int k = 0; k < kPreviewStreams; ++k) {
        auto& s = pb->stream[k];
        const bool on = k <= static_cast<int>(PreviewStream::Output1);
        s.width = kPreviewW;
        s.height = kPreviewH;
        s.fps_target = 30;
        s.active.store(on ? 1u : 0u);
    }
    preview.write_header(sizeof(PreviewBlock), kPreviewMagic, kPreviewVersion);

    std::printf("[fake_engine] running: %d clips, %d effects, epoch %u, %d fps. Ctrl+C to stop.\n", kClipCount,
                static_cast<int>(FxCount), m->s.epoch, fps);

    Img deck[2] = {Img(N), Img(N)}, mainImg(N), layer(N), tmp(N);
    const auto period = std::chrono::microseconds(1000000 / fps);
    auto next = Clock::now();
    auto last = Clock::now();
    auto last_pic = Clock::now() - std::chrono::seconds(1);
    auto fps_t0 = Clock::now();
    int fps_frames = 0;
    uint64_t frame = 0;
    while (!g_quit) {
        next += period;
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        const uint64_t us = now_us();
        ++frame;

        drain(*m, *cb, us);
        step(*m, dt, us);
        m->s.frame = frame;
        m->s.time_us = us;
        if (++fps_frames, now - fps_t0 >= std::chrono::seconds(1)) {
            m->s.fps = static_cast<float>(fps_frames / std::chrono::duration<double>(now - fps_t0).count());
            fps_frames = 0;
            fps_t0 = now;
        }
        seq_write(sb->live_seq, sb->live, m->s);
        seq_write(sb->transport_seq, sb->transport, m->t);
        sb->status.heartbeat_us.store(us, std::memory_order_release);

        if (now - last_pic >= std::chrono::milliseconds(33)) {  // ~30 fps pictures
            last_pic = now;
            const float t = static_cast<float>(us % 1000000000ull) / 1e6f;
            for (int d = 0; d < kDecks; ++d) {
                render_deck(m->s, d, deck[d], layer, tmp, frame, t);
                publish(pb->stream[d], deck[d], frame);
            }
            const float xf = m->s.crossfader;
            for (int k = 0; k < N; ++k) {
                const Px& a = deck[0][k];
                const Px& b = deck[1][k];
                mainImg[k] = {u8(a.b + (b.b - a.b) * xf), u8(a.g + (b.g - a.g) * xf), u8(a.r + (b.r - a.r) * xf), 255};
            }
            for (const auto& f : m->s.master_fx) apply_fx(mainImg, tmp, f, frame, t);
            if (m->s.black) std::fill(mainImg.begin(), mainImg.end(), Px{0, 0, 0, 255});
            publish(pb->stream[static_cast<int>(PreviewStream::Main)], mainImg, frame);
            publish(pb->stream[static_cast<int>(PreviewStream::Output1)], mainImg, frame);
        }
        std::this_thread::sleep_until(next);
        if (Clock::now() - next > std::chrono::milliseconds(100)) next = Clock::now();
    }
    for (auto& s : pb->stream) s.active.store(0);
    std::printf("[fake_engine] stopped\n");
    timeEndPeriod(1);
    return 0;
}
