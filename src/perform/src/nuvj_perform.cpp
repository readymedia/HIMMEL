// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// NuVJ Perform mapper (M10). See the header and docs/agents/handoff/control.md, "NuVJ mapping (M10)".
// Runs in the render loop: no heap, no locks, no I/O.
#include "himmel/perform/nuvj_perform.h"

#include "himmel/engine/clip_directory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "himmel/protocol/show.h"

namespace himmel::perform {

using namespace himmel::protocol;

namespace {

// Control indices: weapons/nuvj.weapon.json "controls" order (deck B = deck A + 17).
enum : int {
    kBank = 0, kPmtr1 = 1, kPmtr2 = 2, kLatch = 3, kAuto = 4, kSync = 5, kPadsFirst = 6, kJog = 15, kScratch = 16,
    kDeckStride = 17,
    kInstaA = 34, kInstaB = 35, kFxLevel = 36, kMasterFx = 37, kMasterFxSelect = 38, kMasterPmtr1 = 39,
    kInstaBlack = 40, kContrast = 41, kMasterPmtr2 = 42, kBpmTap = 43, kBright = 44, kSpeedA = 45, kSpeedB = 46,
    kCrossfader = 47, kFaderMode = 48, kAutoFadeSpeed = 49, kFaderStart = 50, kBankPushA = 51, kBankPushB = 52,
};
// Pad n (1..9, the number on the plate) -> offset from the deck's first pad control (7 8 9 / 4 5 6 / 1 2 3).
constexpr int kPadOffset[9] = {6, 7, 8, 3, 4, 5, 0, 1, 2};
int pad_control(int deck, int n) { return kPadsFirst + deck * kDeckStride + kPadOffset[n - 1]; }

// ---- tuning (GUESS: until the owner has played the NuVJ) ----------------------
constexpr uint64_t kJogIdleUs = 120'000;   // perform.md §5.2: let go = no movement for ~120 ms (no touch sensor)
constexpr float kBendPerTick = 0.02f;      // BEND: +2 % speed per tick while turning
constexpr float kBendMax = 0.5f;
constexpr float kScratchPerTick = 0.03f;   // SCRATCH / scrub: clip seconds per tick
constexpr float kFxPerTick = 0.04f;        // FX WHEEL: mix per tick
constexpr float kTrimPerTick = 0.005f;     // hold pad + wheel: in / out per tick (part of the clip)
constexpr float kKnobPerUnit = 1.0f / 1000.0f;  // KnobDelta units (1000 = the knob's range)
constexpr uint64_t kAutoLongUs = 600'000;  // AUTO held this long = mutate
constexpr float kPickup = 0.03f;           // crossfader soft takeover (as the Lightpad)
constexpr float kTakeoverGap = 0.03f;
constexpr uint64_t kStateLagUs = 120'000;
constexpr uint32_t kFadeMaxMs = 8000;      // AUTO FADE SPEED fully right
// SYNC + wheel: beat-loop lengths 1/16 .. 4 beats.
constexpr float kLoopBeats[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
constexpr int kLoopSteps = 7;

constexpr uint8_t kLedOn = 127, kLedOff = 0;

uint8_t program_of(const LiveState& s) { return s.program_deck < kDecks ? s.program_deck : 0; }
int clip_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.clip_count, kMaxClips)); }
int effect_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.effect_count, kMaxEffects)); }
bool valid_effect(const Catalog& cat, int id) { return id >= 0 && id < effect_count(cat); }

void copy_text(char* dst, size_t cap, const char* src, size_t n) {
    size_t k = 0;
    for (; k < n && k + 1 < cap && src[k]; ++k) {
        char c = src[k];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        dst[k] = c >= 32 && c < 127 ? c : ' ';
    }
    dst[k] = 0;
}

}  // namespace

void NuvjPerform::send(Command c, CommandSink& sink) {
    c.seq = ++seq_;
    sink.push(c);
}

uint8_t NuvjPerform::layer_of(const LiveState& s, int deck) const {
    return s.focus_deck == deck && s.focus_layer < kLayersPerDeck ? s.focus_layer : 0;
}

int NuvjPerform::clip_pages(const Catalog& cat) const {
    return std::max(1, (nclips(cat) + kPadsPerDeck - 1) / kPadsPerDeck);
}

// Wave 8: the engine's ClipDirectory when set (every clip), else the catalog window.
int NuvjPerform::nclips(const Catalog& cat) const { return clips_ ? static_cast<int>(clips_->count()) : clip_count(cat); }
const ClipInfo* NuvjPerform::clip_at(const Catalog& cat, int id) const {
    if (id < 0 || id >= nclips(cat)) return nullptr;
    return clips_ ? &clips_->info(id) : &cat.clip[id];
}
bool NuvjPerform::playable(const Catalog& cat, int id) const {
    if (!clip_at(cat, id)) return false;
    return !clips_ || (clips_->state(id) != ClipState::Removed && clips_->state(id) != ClipState::Empty);
}

// ============================================================================ crossfader
// The crossfader decides PROGRAM (perform.md §5.2): it is sent as is (0 = A .. 1 = B), but only
// after the physical fader has caught the engine's crossfader (soft takeover against the MK2
// wheel, a TAKE or INSTA). FADER START: leaving one end restarts the other deck's clip.
void NuvjPerform::on_crossfader(float x, const LiveState& s, CommandSink& sink, uint64_t now_us) {
    const float xf = s.crossfader;
    if (!picked_) {
        const bool crossed = fader_last_ >= 0.0f && (fader_last_ - xf) * (x - xf) <= 0.0f;
        if (crossed || std::fabs(x - xf) < kPickup) picked_ = true;
    }
    if (picked_ && fader_start_ && fader_last_ >= 0.0f) {
        auto restart = [&](int d) {
            const uint8_t l = layer_of(s, d);
            if (s.deck[d].layer[l].clip >= 0)
                send(make(CommandType::LayerPlay, static_cast<uint8_t>(d), l, static_cast<int32_t>(PlayAction::Restart)), sink);
        };
        if (fader_last_ <= 0.02f && x > 0.02f) restart(1);       // away from A: towards B
        else if (fader_last_ >= 0.98f && x < 0.98f) restart(0);  // away from B: towards A
    }
    fader_last_ = x;
    if (picked_ && std::fabs(x - sent_x_) > 0.002f) {
        send(make(CommandType::Crossfader, kNone, kNone, 0, x), sink);
        sent_x_ = x;
        sent_us_ = now_us;
    }
}

// ============================================================================ wheels
void NuvjPerform::on_jog(int d, int ticks, const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    Jog& j = jog_[d];
    const uint8_t deck = static_cast<uint8_t>(d), l = layer_of(s, d);
    const LayerState& L = s.deck[d].layer[l];
    const bool start = !j.active;
    j.active = true;
    j.last_us = now_us;
    last_deck_ = d;
    if (sync_held_[d]) {  // hold SYNC + turn = the deck's beat-loop length
        sync_used_[d] = true;
        if (L.clip < 0) return;
        const int step = std::clamp((sync_loop_[d] < 0 ? 4 : sync_loop_[d]) + (ticks > 0 ? 1 : -1) * (sync_loop_[d] < 0 ? 0 : 1),
                                    0, kLoopSteps - 1);
        sync_loop_[d] = step;
        send(make(CommandType::LayerBeatLoop, deck, l, 0, kLoopBeats[step]), sink);
        return;
    }
    if (pad_held_[d] >= 0) {  // hold a pad + turn = trim the in point; the out point moves with it
        if (L.clip < 0) return;
        j.trimmed = true;
        const float dv = static_cast<float>(ticks) * kTrimPerTick;
        Command in = make(CommandType::LayerSet, deck, l, 0, dv, kRelative);
        in.param = static_cast<uint8_t>(LayerParam::In);
        Command out = make(CommandType::LayerSet, deck, l, 0, dv, kRelative);
        out.param = static_cast<uint8_t>(LayerParam::Out);
        if (ticks > 0) send(out, sink), send(in, sink);  // move the far end first so the range never collapses
        else send(in, sink), send(out, sink);
        return;
    }
    if (L.clip < 0) return;
    switch (wheel_[d]) {
        case Wheel::Bend:
            if (start) {
                j.scrub = !L.playing;
                j.base = L.speed;
                j.bend = 0.0f;
            }
            if (j.scrub) {  // a paused clip: scrub frame by frame
                send(make(CommandType::LayerScratch, deck, l, 1, static_cast<float>(ticks) * kScratchPerTick, kRelative), sink);
            } else {
                j.bend = std::clamp(j.bend + static_cast<float>(ticks) * kBendPerTick, -kBendMax, kBendMax);
                send(make(CommandType::LayerSpeed, deck, l, 0, j.base * (1.0f + j.bend)), sink);
            }
            break;
        case Wheel::Scratch:
            send(make(CommandType::LayerScratch, deck, l, 1, static_cast<float>(ticks) * kScratchPerTick, kRelative), sink);
            break;
        default: {  // FX WHEEL: sweep the deck's main effect amount, springs back on let go
            const FxSlotState& f = L.fx[0];
            if (!valid_effect(cat, f.effect)) break;
            if (start) j.base = f.mix;
            send(make_fx(CommandType::FxMix, deck, l, 0, kNone, 0, static_cast<float>(ticks) * kFxPerTick, kRelative), sink);
            break;
        }
    }
}

void NuvjPerform::jog_release(int d, const LiveState& s, CommandSink& sink) {
    Jog& j = jog_[d];
    const uint8_t deck = static_cast<uint8_t>(d), l = layer_of(s, d);
    if (!j.trimmed && !sync_held_[d] && s.deck[d].layer[l].clip >= 0) {
        switch (wheel_[d]) {
            case Wheel::Bend:
                if (j.scrub) send(make(CommandType::LayerScratch, deck, l, 0, 0.0f), sink);
                else send(make(CommandType::LayerSpeed, deck, l, 0, j.base), sink);  // back to normal speed
                break;
            case Wheel::Scratch:  // let go: plays on (catch-up per the layer's ScratchSnap)
                send(make(CommandType::LayerScratch, deck, l, 0, 0.0f), sink);
                break;
            default:
                if (s.deck[d].layer[l].fx[0].effect >= 0) send(make_fx(CommandType::FxMix, deck, l, 0, kNone, 0, j.base), sink);
                break;
        }
    }
    j = Jog{};
}

// ============================================================================ buttons
void NuvjPerform::on_button(int c, bool down, const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    auto msg = [&](const char* m) {
        copy_text(lcd_msg_, sizeof(lcd_msg_), m, 11);
        lcd_until_us_ = now_us + 1'200'000;
    };
    const int d = c >= kDeckStride && c < 2 * kDeckStride ? 1 : 0;
    const int local = c < 2 * kDeckStride ? c - d * kDeckStride : -1;
    const uint8_t deck = static_cast<uint8_t>(d), l = layer_of(s, d);
    if (local >= 0) last_deck_ = d;
    auto dice = [&](DiceVerb v) {
        if (latch_[d]) return msg("LATCHED");
        Command x = make(CommandType::Dice, deck, kNone, 0);
        x.param = static_cast<uint8_t>(v);
        send(x, sink);
    };
    if (local == kScratch && down) {  // SCRATCH cycles the wheel mode: off / lit / blinking
        wheel_[d] = static_cast<Wheel>((static_cast<int>(wheel_[d]) + 1) % static_cast<int>(Wheel::Count));
        static constexpr const char* kName[] = {"BEND", "SCRATCH", "FX WHEEL"};
        msg(kName[static_cast<int>(wheel_[d])]);
        return;
    }
    if (local == kLatch && down) {  // LATCH = lock the deck's scene (no dice)
        latch_[d] = !latch_[d];
        msg(latch_[d] ? "LATCH ON" : "LATCH OFF");
        return;
    }
    if (local == kAuto) {  // AUTO = dice; a long press = mutate (on release)
        if (down) auto_down_us_[d] = now_us;
        else if (auto_down_us_[d]) dice(now_us - auto_down_us_[d] >= kAutoLongUs ? DiceVerb::Mutate : DiceVerb::Roll),
            auto_down_us_[d] = 0;
        return;
    }
    if (local == kSync) {  // SYNC = the clip on the beat (BPM time base); hold + wheel = beat loop
        if (down) {
            sync_held_[d] = true;
            sync_used_[d] = false;
            return;
        }
        sync_held_[d] = false;
        if (sync_used_[d]) {  // release after a loop: loop off (catch-up per the layer)
            if (sync_loop_[d] >= 0 && s.deck[d].layer[l].clip >= 0) send(make(CommandType::LayerBeatLoop, deck, l, 0, 0.0f), sink);
            sync_loop_[d] = -1;
        } else if (s.deck[d].layer[l].clip >= 0) {
            const bool bpm = transport_ && transport_->layer[d][l].time_base == static_cast<uint8_t>(TimeBase::Bpm);
            Command x = make(CommandType::LayerSet, deck, l, bpm ? 0 : 1);
            x.param = static_cast<uint8_t>(LayerParam::TimeBase);
            send(x, sink);
            msg(bpm ? "SYNC OFF" : "SYNC ON");
        }
        return;
    }
    if (!down) return;
    switch (c) {
        case kBankPushA:
        case kBankPushB:  // BANK push = dice the deck
            last_deck_ = c == kBankPushB;
            if (latch_[last_deck_]) return msg("LATCHED");
            {
                Command x = make(CommandType::Dice, static_cast<uint8_t>(last_deck_), kNone, 0);
                x.param = static_cast<uint8_t>(DiceVerb::Roll);
                send(x, sink);
            }
            return;
        case kInstaA:
        case kInstaB: {  // fade to that deck at the AUTO FADE speed (0 = a cut); nothing if it is on air
            const uint8_t to = c == kInstaB ? 1 : 0;
            if (program_of(s) == to && !s.take_active) return;
            if (fade_ms_ == 0) send(make(CommandType::Cut), sink);
            else send(make(CommandType::Take, kNone, kNone, static_cast<int32_t>(fade_ms_)), sink);
            return;
        }
        case kInstaBlack: send(make(CommandType::Black, kNone, kNone, kToggle), sink); return;
        case kMasterFx:  // master effect on / off (slot 1)
            if (valid_effect(cat, s.master_fx[0].effect)) send(make_fx(CommandType::FxBypass, kMasterDeck, kNone, 0, kNone, kToggle), sink);
            return;
        case kBpmTap: {
            Command x = make(CommandType::Clock);
            x.param = static_cast<uint8_t>(ClockParam::Tap);
            send(x, sink);
            return;
        }
        default: return;
    }
}

// ============================================================================ events
void NuvjPerform::on_event(const mk2::Event& e, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    now_us_ = e.t_us;
    switch (e.type) {
        case mk2::EventType::Connected:
        case mk2::EventType::Disconnected:
            reset();
            return;
        case mk2::EventType::ButtonDown:
        case mk2::EventType::ButtonUp:
            if (e.id < kControls) on_button(e.id, e.type == mk2::EventType::ButtonDown, s, cat, sink, e.t_us);
            return;
        case mk2::EventType::PadHit: {  // pad numbers 1..9 = deck A, 10..18 = deck B
            if (e.id < 1 || e.id > 2 * kPadsPerDeck) return;
            const int d = e.id > kPadsPerDeck ? 1 : 0, n = e.id - d * kPadsPerDeck;
            pad_held_[d] = n - 1;
            last_deck_ = d;
            const int clip = bank_[d] * kPadsPerDeck + (n - 1);
            if (!playable(cat, clip)) return;
            send(make(CommandType::LayerTrigger, static_cast<uint8_t>(d), layer_of(s, d), clip, 1.0f), sink);
            return;
        }
        case mk2::EventType::PadRelease: {
            if (e.id < 1 || e.id > 2 * kPadsPerDeck) return;
            const int d = e.id > kPadsPerDeck ? 1 : 0, n = e.id - d * kPadsPerDeck;
            if (pad_held_[d] == n - 1) pad_held_[d] = -1;
            const int clip = bank_[d] * kPadsPerDeck + (n - 1);
            if (playable(cat, clip))  // piano / LATCH-off styles end on release (the engine's trigger style decides)
                send(make(CommandType::LayerTrigger, static_cast<uint8_t>(d), layer_of(s, d), clip, 0.0f, kRelease), sink);
            return;
        }
        case mk2::EventType::EncoderDelta: {  // BANK, MASTER FX select, jog wheels (ticks, + = clockwise)
            if (e.value == 0) return;
            if (e.id == kJog || e.id == kJog + kDeckStride) {
                on_jog(e.id == kJog ? 0 : 1, e.value, s, cat, sink, e.t_us);
            } else if (e.id == kBank || e.id == kBank + kDeckStride) {
                const int d = e.id == kBank ? 0 : 1;
                bank_[d] = std::clamp(bank_[d] + (e.value > 0 ? 1 : -1), 0, clip_pages(cat) - 1);
                last_deck_ = d;
            } else if (e.id == kMasterFxSelect) {
                const int n = effect_count(cat);
                if (n == 0) return;
                const int cur = valid_effect(cat, s.master_fx[0].effect) ? s.master_fx[0].effect + 1 : 0;
                const int next = ((cur + (e.value > 0 ? 1 : -1)) % (n + 1) + (n + 1)) % (n + 1);
                send(make_fx(CommandType::FxSet, kMasterDeck, kNone, 0, kNone, next - 1), sink);
            }
            return;
        }
        case mk2::EventType::KnobDelta: {  // endless knobs: deltas in knob units (1000 = the range)
            const float dv = static_cast<float>(e.value) * kKnobPerUnit;
            const int c = e.id;
            if (c == kPmtr1 || c == kPmtr2 || c == kPmtr1 + kDeckStride || c == kPmtr2 + kDeckStride) {
                const int d = c >= kDeckStride ? 1 : 0, p = (c - d * kDeckStride) == kPmtr1 ? 0 : 1;
                const uint8_t l = layer_of(s, d);
                const FxSlotState& f = s.deck[d].layer[l].fx[0];
                if (valid_effect(cat, f.effect) && p < cat.effect[f.effect].param_count)
                    send(make_fx(CommandType::FxParam, static_cast<uint8_t>(d), l, 0, static_cast<uint8_t>(p), 0, dv, kRelative), sink);
                last_deck_ = d;
            } else if (c == kFxLevel) {
                if (valid_effect(cat, s.master_fx[0].effect)) send(make_fx(CommandType::FxMix, kMasterDeck, kNone, 0, kNone, 0, dv, kRelative), sink);
            } else if (c == kMasterPmtr1 || c == kMasterPmtr2) {
                const int p = c == kMasterPmtr1 ? 0 : 1;
                const int fx = s.master_fx[0].effect;
                if (valid_effect(cat, fx) && p < cat.effect[fx].param_count)
                    send(make_fx(CommandType::FxParam, kMasterDeck, kNone, 0, static_cast<uint8_t>(p), 0, dv, kRelative), sink);
            } else if (c == kAutoFadeSpeed) {  // absolute: raw 0..127 = INSTANT .. SLOW
                fade_ms_ = static_cast<uint32_t>(std::lround(std::clamp(e.raw / 127.0f, 0.0f, 1.0f) * kFadeMaxMs / 100.0)) * 100;
            }
            // CONTRAST / BRIGHT: GUESS - no output contrast / brightness in the engine yet.
            return;
        }
        case mk2::EventType::Fader: {  // SPEED faders, crossfader: 0..16383
            const float x = std::clamp(e.value / 16383.0f, 0.0f, 1.0f);
            if (e.id == kCrossfader) {
                on_crossfader(x, s, sink, e.t_us);
            } else if (e.id == kSpeedA || e.id == kSpeedB) {  // centre = 1x, 0..2x (GUESS: no reverse on the fader)
                const int d = e.id == kSpeedB ? 1 : 0;
                const uint8_t l = layer_of(s, d);
                if (s.deck[d].layer[l].clip >= 0) send(make(CommandType::LayerSpeed, static_cast<uint8_t>(d), l, 0, x * 2.0f), sink);
                last_deck_ = d;
            }
            return;
        }
        case mk2::EventType::Switch:
            if (e.id == kFaderStart) fader_start_ = e.value != 0;
            else if (e.id == kFaderMode) fader_mode_ = static_cast<uint8_t>(std::clamp<int>(e.value, 0, 2));  // curve: later
            return;
        default:
            return;
    }
}

// ============================================================================ tick: let-go, LEDs, LCD
void NuvjPerform::tick(uint64_t now_us, const LiveState& s, const Catalog& cat, CommandSink& sink, uint8_t leds[128],
                       NuvjLcd& lcd) {
    now_us_ = now_us;
    for (int d = 0; d < 2; ++d)
        if (jog_[d].active && now_us - std::min(now_us, jog_[d].last_us) > kJogIdleUs) jog_release(d, s, sink);
    // The MK2 wheel, a TAKE or INSTA moved the crossfader under the physical fader: let go until it catches it.
    if (picked_ && now_us - std::min(now_us, sent_us_) > kStateLagUs &&
        (std::fabs(s.crossfader - sent_x_) > kTakeoverGap || s.take_active))
        picked_ = false;

    const uint8_t prog = program_of(s);
    const bool blink = (now_us / 250'000) & 1;
    double beat = 0.0;
    if (transport_ && transport_->clock.bpm > 0.0f) {
        const double dt = now_us >= transport_->clock.time_us ? (now_us - transport_->clock.time_us) / 1e6 : 0.0;
        beat = transport_->clock.beat + dt * transport_->clock.bpm / 60.0;
    }
    const bool on_beat = beat - std::floor(beat) < 0.2;

    std::memset(leds, 0, 128);
    for (int d = 0; d < 2; ++d) {
        const int base = d * kDeckStride;
        const uint8_t l = layer_of(s, d);
        for (int n = 1; n <= kPadsPerDeck; ++n) {
            const int clip = bank_[d] * kPadsPerDeck + (n - 1);
            bool on = false;
            for (int k = 0; k < kLayersPerDeck; ++k) on = on || s.deck[d].layer[k].clip == clip;
            if (pad_held_[d] == n - 1) on = true;
            if (playable(cat, clip)) leds[pad_control(d, n)] = on ? kLedOn : kLedOff;
        }
        leds[base + kLatch] = latch_[d] ? kLedOn : kLedOff;
        leds[base + kSync] = transport_ && transport_->layer[d][l].time_base == static_cast<uint8_t>(TimeBase::Bpm) ? kLedOn : kLedOff;
        leds[base + kAuto] = auto_down_us_[d] ? kLedOn : kLedOff;
        leds[base + kScratch] = wheel_[d] == Wheel::Bend ? kLedOff : wheel_[d] == Wheel::Scratch ? kLedOn : (blink ? kLedOn : kLedOff);
    }
    leds[kInstaA] = prog == 0 ? kLedOn : kLedOff;  // the tally: the deck on air
    leds[kInstaB] = prog == 1 ? kLedOn : kLedOff;
    leds[kInstaBlack] = s.black ? kLedOn : kLedOff;
    leds[kMasterFx] = valid_effect(cat, s.master_fx[0].effect) && !s.master_fx[0].bypass ? kLedOn : kLedOff;
    leds[kBpmTap] = on_beat ? kLedOn : kLedOff;

    // LCD: a short message, else the PROGRAM deck's clip; BPM; PRESET = the last deck's bank.
    lcd = NuvjLcd{};
    if (lcd_msg_[0] && now_us < lcd_until_us_) {
        copy_text(lcd.text, sizeof(lcd.text), lcd_msg_, 8);
    } else {
        const LayerState& L = s.deck[prog].layer[layer_of(s, prog)];
        if (const ClipInfo* ci = clip_at(cat, L.clip)) copy_text(lcd.text, sizeof(lcd.text), ci->name, 8);
        else copy_text(lcd.text, sizeof(lcd.text), "HIMMEL", 8);
    }
    {
        char a[16];
        std::snprintf(a, sizeof(a), "%c %s %s", prog == 0 ? 'A' : 'B', s.black ? "BLACK" : s.take_active ? "TAKE" : "LIVE",
                      wheel_[last_deck_] == Wheel::Bend ? "BND" : wheel_[last_deck_] == Wheel::Scratch ? "SCR" : "FXW");
        copy_text(lcd.alpha, sizeof(lcd.alpha), a, 11);
    }
    if (transport_ && transport_->clock.bpm > 0.0f)
        std::snprintf(lcd.number[0], sizeof(lcd.number[0]), "%3d", std::min(999, static_cast<int>(std::lround(transport_->clock.bpm))));
    std::snprintf(lcd.number[1], sizeof(lcd.number[1]), "%d", std::min(99, bank_[last_deck_] + 1));
    lcd.bar[0] = static_cast<int8_t>(std::lround(std::clamp(s.crossfader, 0.0f, 1.0f) * 7));  // LAST = the crossfader
    lcd.bar[1] = static_cast<int8_t>(std::lround(static_cast<double>(fade_ms_) / kFadeMaxMs * 7));  // DATA = the fade speed
    lcd.marks = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (prog == 0 ? (1u << 7) : (1u << 8));  // BPM PRESET LAST DATA, box A / B
}

void NuvjPerform::reset() {
    for (auto& j : jog_) j = Jog{};
    auto_down_us_[0] = auto_down_us_[1] = 0;
    sync_held_[0] = sync_held_[1] = false;
    sync_loop_[0] = sync_loop_[1] = -1;
    pad_held_[0] = pad_held_[1] = -1;
    picked_ = false;
    fader_last_ = -1.0f;
    sent_x_ = -1.0f;
}

}  // namespace himmel::perform
