// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Lightpad Block Perform mapper (M9). See the header and docs/agents/handoff/control.md,
// "Lightpad mapping (M9)". Runs in the render loop: no heap, no locks, no I/O.
#include "himmel/perform/lightpad_perform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::perform {

using namespace himmel::protocol;

namespace {

// ---- tuning (GUESS: until the owner has played it on the block) -------------
constexpr uint64_t kDoubleTapUs = 400'000;   // two Mode presses within this = next mode
constexpr float kPickup = 0.03f;             // MIX: a finger this close to the crossfader catches it at once
constexpr float kTakeoverGap = 0.03f;        // MIX: the crossfader this far from what we sent = someone else moved it
constexpr uint64_t kStateLagUs = 120'000;    // MIX: our own command may not be in the state for this long
constexpr int kEdgeRow = 4096 / 15;          // MIX: the top / bottom LED row (TAKE / CUT)
constexpr int kTouchZonesMax = 5;            // TOUCH: up to 5 effects across X
constexpr uint8_t kTouchSlot = 1;            // TOUCH: master FX slot 2 (slot 1 = MK2 MASTER FX, 3 = FX PADS, 4 = STROBE)

uint8_t program_of(const LiveState& s) { return s.program_deck < kDecks ? s.program_deck : 0; }
int effect_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.effect_count, kMaxEffects)); }
int clip_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.clip_count, kMaxClips)); }
bool valid_effect(const Catalog& cat, int id) { return id >= 0 && id < effect_count(cat); }
float unit(int v) { return std::clamp(static_cast<float>(v) / 4095.0f, 0.0f, 1.0f); }

void put(uint8_t px[][3], int col, int row, int r, int g, int b) {
    if (col < 0 || col >= LightpadPerform::kGrid || row < 0 || row >= LightpadPerform::kGrid) return;
    uint8_t* p = px[row * LightpadPerform::kGrid + col];
    p[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
    p[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
    p[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
}
void rgb_of(uint32_t c, int level, int out[3]) {  // 0xRRGGBB scaled so the brightest channel is `level`
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int mx = std::max({r, g, b});
    if (mx < 16) r = g = b = mx = 255;
    out[0] = r * level / mx, out[1] = g * level / mx, out[2] = b * level / mx;
}
void hue(float h, int level, int out[3]) {
    h -= std::floor(h);
    const float r = std::clamp(std::fabs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    const float g = std::clamp(2.0f - std::fabs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    const float b = std::clamp(2.0f - std::fabs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    out[0] = static_cast<int>(r * level), out[1] = static_cast<int>(g * level), out[2] = static_cast<int>(b * level);
}
int col_of(float x) { return std::clamp(static_cast<int>(std::lround(x * (LightpadPerform::kGrid - 1))), 0, LightpadPerform::kGrid - 1); }

}  // namespace

void LightpadPerform::send(Command c, CommandSink& sink) {
    c.seq = ++seq_;
    sink.push(c);
}

int LightpadPerform::fingers_down() const {
    int n = 0;
    for (const auto& f : finger_) n += f.down;
    return n;
}
int LightpadPerform::first_finger() const {
    for (int i = 0; i < kFingers; ++i)
        if (finger_[i].down) return i;
    return -1;
}

// ============================================================================ TOUCH
int LightpadPerform::touch_zones(const Catalog& cat) const {
    int n = 0;
    for (int i = 0; i < effect_count(cat) && n < kTouchZonesMax; ++i) n += cat.effect[i].reserved[0] == 0;
    return n;
}
// GUESS: the zones are the first effects (not generators) of the catalog, left to right.
int LightpadPerform::touch_effect(const Catalog& cat, int zone) const {
    int n = 0;
    for (int i = 0; i < effect_count(cat); ++i)
        if (cat.effect[i].reserved[0] == 0 && n++ == zone) return i;
    return -1;
}

// X = which effect, Y = amount (the mix, top = full), pressure = PRESS, a second finger's X = param 2.
void LightpadPerform::touch_fx(const LiveState& s, const Catalog& cat, CommandSink& sink, bool start) {
    (void)s;
    const int fi = first_finger();
    const int zones = touch_zones(cat);
    if (fi < 0 || zones == 0) return;
    const Finger& f = finger_[fi];
    const int zone = std::min(zones - 1, f.x * zones / 4096);
    const int eff = touch_effect(cat, zone);
    if (eff != touch_fx_) {
        send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kTouchSlot, kNone, eff), sink);
        touch_fx_ = eff;
        touch_mix_sent_ = touch_press_sent_ = touch_p2_sent_ = -1.0f;
    }
    if (touch_fx_ < 0) return;
    const float mix = 1.0f - unit(f.y);
    if (start || std::fabs(mix - touch_mix_sent_) > 0.01f) {
        send(make_fx(CommandType::FxMix, kMasterDeck, kNone, kTouchSlot, kNone, 0, mix), sink);
        touch_mix_sent_ = mix;
    }
    const float press = f.z / 255.0f;
    if (std::fabs(press - touch_press_sent_) > 0.02f) {
        send(make_fx(CommandType::FxPress, kMasterDeck, kNone, kTouchSlot, kNone, 0, press), sink);
        touch_press_sent_ = press;
    }
    for (int i = fi + 1; i < kFingers; ++i)
        if (finger_[i].down) {
            const float p2 = unit(finger_[i].x);
            if (cat.effect[touch_fx_].param_count > 1 && std::fabs(p2 - touch_p2_sent_) > 0.01f) {
                send(make_fx(CommandType::FxParam, kMasterDeck, kNone, kTouchSlot, 1, 0, p2), sink);
                touch_p2_sent_ = p2;
            }
            break;
        }
}

// ============================================================================ MIX
// Soft takeover: the finger moves the crossfader only after it has caught it (touched
// it, or crossed it). If the MK2 wheel (or a TAKE) moves the fader under a caught
// finger, the finger lets go until it catches the fader again.
void LightpadPerform::mix_move(float x, const LiveState& s, CommandSink& sink, uint64_t now_us) {
    const float xf = s.crossfader;
    if (!picked_) {
        const bool crossed = mix_last_x_ >= 0.0f && (mix_last_x_ - xf) * (x - xf) <= 0.0f;
        if (crossed || std::fabs(x - xf) < kPickup) picked_ = true, took_ = false;
    }
    mix_last_x_ = x;
    if (picked_ && std::fabs(x - mix_sent_) > 0.002f) {
        send(make(CommandType::Crossfader, kNone, kNone, 0, x), sink);
        mix_sent_ = x;
        mix_sent_us_ = now_us;
    }
}

// ============================================================================ SCRUB
bool LightpadPerform::scrub_target(const LiveState& s, uint8_t& deck, uint8_t& layer) const {
    if (s.focus_deck >= kDecks || s.focus_layer >= kLayersPerDeck) return false;
    deck = s.focus_deck, layer = s.focus_layer;
    return s.deck[deck].layer[layer].clip >= 0;
}

void LightpadPerform::scrub_move(const Finger& f, const LiveState& s, CommandSink& sink) {
    if (scrub_deck_ >= kDecks) return;
    float in = 0.0f, out = 1.0f;
    if (transport_) in = transport_->layer[scrub_deck_][scrub_layer_].in, out = transport_->layer[scrub_deck_][scrub_layer_].out;
    if (out <= in) in = 0.0f, out = 1.0f;
    const float pos = in + unit(f.x) * (out - in);
    if (std::fabs(pos - scrub_sent_) > 0.001f) {
        send(make(CommandType::LayerScratch, scrub_deck_, scrub_layer_, 1, pos), sink);
        scrub_sent_ = pos;
    }
    // Pressure = the layer's PRESS target (FX slot 1), only if it has an effect.
    const float press = f.z / 255.0f;
    if (s.deck[scrub_deck_].layer[scrub_layer_].fx[0].effect >= 0 && std::fabs(press - scrub_press_sent_) > 0.02f) {
        send(make_fx(CommandType::FxPress, scrub_deck_, scrub_layer_, 0, kNone, 0, press), sink);
        scrub_press_sent_ = press;
    }
}

// ============================================================================ modes
void LightpadPerform::end_all(const LiveState& s, CommandSink& sink) {
    if (touch_fx_ >= 0) send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kTouchSlot, kNone, -1), sink);
    touch_fx_ = -1;
    hold_ = false;
    if (scrub_finger_ >= 0 && scrub_deck_ < kDecks) {
        send(make(CommandType::LayerScratch, scrub_deck_, scrub_layer_, 0, 0.0f), sink);
        if (scrub_press_sent_ > 0.0f && s.deck[scrub_deck_].layer[scrub_layer_].fx[0].effect >= 0)
            send(make_fx(CommandType::FxPress, scrub_deck_, scrub_layer_, 0, kNone, 0, 0.0f), sink);
    }
    scrub_finger_ = -1;
    scrub_deck_ = scrub_layer_ = kNone;
    mix_finger_ = -1;
    picked_ = false;
}

void LightpadPerform::set_mode(Mode m, const LiveState& s, CommandSink& sink) {
    end_all(s, sink);
    mode_ = m;
}

// ============================================================================ events
void LightpadPerform::on_event(const mk2::Event& e, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    last_us_ = e.t_us;
    switch (e.type) {
        case mk2::EventType::Connected:
        case mk2::EventType::Disconnected:
            end_all(s, sink);
            reset();
            return;
        case mk2::EventType::ButtonDown: {
            if (e.id != kModeControl) return;
            if (mode_tap_us_ && e.t_us - mode_tap_us_ < kDoubleTapUs) {  // double-tap: the next mode
                mode_tap_us_ = 0;
                hold_ = hold_before_tap_;
                set_mode(static_cast<Mode>((static_cast<int>(mode_) + 1) % static_cast<int>(Mode::Count)), s, sink);
                return;
            }
            mode_tap_us_ = e.t_us;
            if (mode_ == Mode::Touch) {  // TOUCH: Mode = hold
                hold_before_tap_ = hold_;
                hold_ = !hold_;
                if (!hold_ && fingers_down() == 0 && touch_fx_ >= 0) {
                    send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kTouchSlot, kNone, -1), sink);
                    touch_fx_ = -1;
                }
            }
            return;
        }
        case mk2::EventType::TouchStart:
        case mk2::EventType::TouchMove:
        case mk2::EventType::TouchEnd:
            break;
        default:
            return;  // the 4 x 4 zone pads and the rest: the touches carry it all
    }
    if (e.id >= kFingers) return;
    Finger& f = finger_[e.id];
    const bool start = e.type == mk2::EventType::TouchStart, end = e.type == mk2::EventType::TouchEnd;
    if (!start && !f.down) return;  // a move / end of a finger we never saw start
    f.down = !end;
    f.x = static_cast<uint16_t>(std::clamp<int>(e.value, 0, 4095));
    f.y = static_cast<uint16_t>(std::clamp<int>(e.raw, 0, 4095));
    f.z = static_cast<uint8_t>(e.reserved & 0xFF);

    switch (mode_) {
        case Mode::Touch:
            if (end && fingers_down() == 0) {
                if (hold_) {
                    if (touch_press_sent_ > 0.0f)
                        send(make_fx(CommandType::FxPress, kMasterDeck, kNone, kTouchSlot, kNone, 0, 0.0f), sink);
                    touch_press_sent_ = 0.0f;
                } else if (touch_fx_ >= 0) {
                    send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kTouchSlot, kNone, -1), sink);
                    touch_fx_ = -1;
                }
            } else if (!end) {
                touch_fx(s, cat, sink, start);
            }
            break;
        case Mode::Mix:
            if (start) {
                if (f.y < kEdgeRow) {  // the top row = TAKE
                    send(make(CommandType::Take, kNone, kNone, 0), sink);
                } else if (f.y > 4095 - kEdgeRow) {  // the bottom row = CUT
                    send(make(CommandType::Cut), sink);
                } else if (mix_finger_ < 0) {
                    mix_finger_ = e.id;
                    picked_ = false;
                    mix_last_x_ = -1.0f;
                    mix_sent_ = -1.0f;
                    mix_move(unit(f.x), s, sink, e.t_us);
                }
            } else if (e.id == mix_finger_) {
                if (end) {
                    mix_finger_ = -1;
                    picked_ = false;
                } else {
                    mix_move(unit(f.x), s, sink, e.t_us);
                }
            }
            break;
        default:  // SCRUB
            if (start && scrub_finger_ < 0) {
                uint8_t d, l;
                if (!scrub_target(s, d, l)) break;
                scrub_finger_ = e.id;
                scrub_deck_ = d, scrub_layer_ = l;
                scrub_sent_ = scrub_press_sent_ = -1.0f;
                scrub_move(f, s, sink);
            } else if (e.id == scrub_finger_) {
                if (end) {  // lift = back to the play mode
                    send(make(CommandType::LayerScratch, scrub_deck_, scrub_layer_, 0, 0.0f), sink);
                    if (scrub_press_sent_ > 0.0f && s.deck[scrub_deck_].layer[scrub_layer_].fx[0].effect >= 0)
                        send(make_fx(CommandType::FxPress, scrub_deck_, scrub_layer_, 0, kNone, 0, 0.0f), sink);
                    scrub_finger_ = -1;
                    scrub_deck_ = scrub_layer_ = kNone;
                } else {
                    scrub_move(f, s, sink);
                }
            }
            break;
    }
}

// ============================================================================ tick + LEDs
void LightpadPerform::tick(uint64_t now_us, const LiveState& s, const Catalog& cat, CommandSink& sink,
                           uint8_t pixels[kPixels][3]) {
    (void)sink;
    // MIX: the wheel / a TAKE moved the crossfader under a caught finger -> let go (soft takeover).
    if (mode_ == Mode::Mix && picked_ && mix_finger_ >= 0 && now_us - std::min(now_us, mix_sent_us_) > kStateLagUs &&
        (std::fabs(s.crossfader - mix_sent_) > kTakeoverGap || s.take_active)) {
        picked_ = false;
        took_ = true;
        mix_last_x_ = unit(finger_[mix_finger_].x);
    }

    std::memset(pixels, 0, kPixels * 3);
    const uint8_t prog = program_of(s);
    const bool blink = (now_us / 200'000) & 1;
    int c[3];
    switch (mode_) {
        case Mode::Touch: {
            const int zones = touch_zones(cat);
            const int fi = first_finger();
            const int active = touch_fx_ >= 0 && fi >= 0 ? std::min(zones - 1, finger_[fi].x * zones / 4096) : -1;
            for (int z = 0; z < zones; ++z) {  // a dim colour band per effect, the active one filled to its amount
                const int x0 = z * kGrid / zones, x1 = (z + 1) * kGrid / zones;
                hue(static_cast<float>(z) / zones, z == active ? 200 : 40, c);
                const int fill = z == active ? static_cast<int>(std::lround(touch_mix_sent_ * (kGrid - 2))) : 0;
                for (int x = x0; x < x1; ++x)
                    for (int y = 1; y < kGrid - 1; ++y) {
                        const bool lit = z == active && (kGrid - 2 - y) < fill;
                        put(pixels, x, y, lit ? c[0] : c[0] / 4, lit ? c[1] : c[1] / 4, lit ? c[2] : c[2] / 4);
                    }
            }
            // The border is red: TOUCH plays PROGRAM. Full while held (Mode = hold).
            const int r = hold_ ? 255 : 110;
            for (int i = 0; i < kGrid; ++i) put(pixels, i, 0, r, 0, 0), put(pixels, i, kGrid - 1, r, 0, 0);
            for (int k = 0; k < kFingers; ++k)
                if (finger_[k].down) put(pixels, col_of(unit(finger_[k].x)), col_of(unit(finger_[k].y)), 255, 255, 255);
            break;
        }
        case Mode::Mix: {
            // Top row = TAKE (amber, full while a TAKE runs), bottom row = CUT (dim red).
            for (int i = 0; i < kGrid; ++i) {
                put(pixels, i, 0, s.take_active ? 255 : 90, s.take_active ? 160 : 50, 0);
                put(pixels, i, kGrid - 1, 90, 0, 0);
            }
            // The track: deck A on the left, deck B on the right; PROGRAM red-tinted, PREVIEW green.
            const int fx = col_of(s.crossfader);
            for (int x = 0; x < kGrid; ++x) {
                const bool side_b = x > fx;
                const bool is_prog = (side_b ? 1 : 0) == prog;
                for (int y = 1; y < kGrid - 1; ++y) put(pixels, x, y, is_prog ? 40 : 0, is_prog ? 0 : 40, 0);
            }
            for (int y = 1; y < kGrid - 1; ++y) put(pixels, fx, y, 255, 255, 255);  // the crossfader
            if (mix_finger_ >= 0) {
                const int finger = col_of(unit(finger_[mix_finger_].x));
                if (!picked_) {  // not caught yet: the finger blinks blue, go to the white line
                    if (blink) for (int y = 1; y < kGrid - 1; ++y) put(pixels, finger, y, 0, 60, 255);
                } else {
                    for (int y = 1; y < kGrid - 1; ++y) put(pixels, finger, y, 255, 255, 255);
                }
            }
            break;
        }
        default: {  // SCRUB: the focused clip's colour, the playhead column, the finger
            uint8_t d = 0, l = 0;
            const bool ok = scrub_target(s, d, l);
            if (!ok) {
                for (int i = 0; i < kGrid; ++i) put(pixels, i, i, 40, 40, 40), put(pixels, kGrid - 1 - i, i, 40, 40, 40);
                break;
            }
            const LayerState& L = s.deck[d].layer[l];
            const uint32_t col = L.clip < clip_count(cat) ? cat.clip[L.clip].thumb_rgb : 0x808080;
            float in = 0.0f, out = 1.0f;
            if (transport_) in = transport_->layer[d][l].in, out = transport_->layer[d][l].out;
            if (out <= in) in = 0.0f, out = 1.0f;
            rgb_of(col, 50, c);
            for (int x = 0; x < kGrid; ++x)
                for (int y = 3; y < kGrid - 3; ++y) put(pixels, x, y, c[0], c[1], c[2]);
            rgb_of(col, 255, c);
            const int ph = col_of(std::clamp((L.position - in) / (out - in), 0.0f, 1.0f));
            for (int y = 3; y < kGrid - 3; ++y) put(pixels, ph, y, c[0], c[1], c[2]);
            if (scrub_finger_ >= 0)
                for (int y = 0; y < kGrid; ++y) put(pixels, col_of(unit(finger_[scrub_finger_].x)), y, 255, 255, 255);
            // Top / bottom rows: PREVIEW green or PROGRAM red, which deck the focus is on.
            for (int i = 0; i < kGrid; ++i) {
                const bool live = d == prog;
                put(pixels, i, 0, live ? 160 : 0, live ? 0 : 160, 0);
                put(pixels, i, kGrid - 1, live ? 160 : 0, live ? 0 : 160, 0);
            }
            break;
        }
    }
}

void LightpadPerform::reset() {
    for (auto& f : finger_) f = Finger{};
    mix_finger_ = -1;
    picked_ = false;
    scrub_finger_ = -1;
    scrub_deck_ = scrub_layer_ = kNone;
    mode_tap_us_ = 0;
}

}  // namespace himmel::perform
