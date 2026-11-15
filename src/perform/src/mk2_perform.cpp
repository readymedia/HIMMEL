// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// MK2 Perform mapper for the playable slice (docs/roadmap.md §0a, task S2): a
// subset of docs/design/perform.md v4. The mapping table is in
// docs/agents/handoff/control.md, "Slice mapping (2026-11)".
//
// Runs in the render loop: no heap, no locks, no I/O, no Windows calls. All
// text is formatted into fixed stack buffers; the screens are drawn on a 4 KB
// stack canvas.
#include "himmel/perform/mk2_perform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "himmel/mk2/pages.h"

namespace himmel::perform {

using namespace himmel::protocol;
using mk2::Button;
namespace ui = mk2::ui;

namespace {

// ---- tuning (all GUESS: until the owner has played it) ----------------------
constexpr float kKnobUnitsPerTurn = 1000.0f;   // one knob turn ~1008 raw units (measured)
constexpr float kSpeedPerTurn = 2.0f;          // GUESS: one turn of knob 2 = 2x speed
constexpr float kFineScale = 0.1f;             // GUESS: SHIFT + knob = fine (1/10)
constexpr float kWheelDetentsPerTake = 30.0f;  // hardware-mapping §3.1: one turn (30 detents) = done
constexpr int kChooseUnits = 100;              // GUESS: SHIFT + knob 8, raw units per effect step
constexpr float kMinPadOpacity = 0.2f;         // GUESS: softest pad hit still shows the clip at 20 %
constexpr uint64_t kTouchedUs = 800'000;       // knob column stays inverted 0.8 s after a turn

// LED levels (0..127).
constexpr uint8_t kLedFull = 127;
constexpr uint8_t kLedDim = 28;
constexpr uint8_t kLedFaint = 12;
constexpr uint8_t kPadOnDeck = 56;   // clip is on some other layer of either deck
constexpr uint8_t kPadInBank = 16;   // clip is only in the catalog

constexpr int kPadsPerPage = mk2::kPads;

bool is(const mk2::Event& e, Button b) { return e.id == static_cast<uint8_t>(b); }

uint8_t program_of(const LiveState& s) { return s.program_deck < kDecks ? s.program_deck : 0; }
uint8_t preview_of(const LiveState& s) { return static_cast<uint8_t>(program_of(s) ^ 1); }

bool valid_clip(const Catalog& cat, int id) { return id >= 0 && id < static_cast<int>(std::min<uint32_t>(cat.clip_count, kMaxClips)); }
bool valid_effect(const Catalog& cat, int id) {
    return id >= 0 && id < static_cast<int>(std::min<uint32_t>(cat.effect_count, kMaxEffects));
}
int effect_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.effect_count, kMaxEffects)); }
int clip_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.clip_count, kMaxClips)); }

std::string_view cstr(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return {s, n};
}
// Clip name without its extension ("sunset.mov" -> "sunset").
std::string_view clip_name(const Catalog& cat, int id) {
    if (!valid_clip(cat, id)) return {};
    std::string_view n = cstr(cat.clip[id].name, kNameLen);
    const size_t dot = n.rfind('.');
    if (dot != std::string_view::npos && dot > 0) n = n.substr(0, dot);
    return n;
}
std::string_view effect_name(const Catalog& cat, int id) {
    return valid_effect(cat, id) ? cstr(cat.effect[id].name, kNameLen) : std::string_view{};
}
std::string_view param_name(const Catalog& cat, int effect, int p) {
    if (!valid_effect(cat, effect) || p >= cat.effect[effect].param_count) return {};
    return cstr(cat.effect[effect].param_name[p], kParamNameLen);
}
bool has_param(const Catalog& cat, int effect, int p) {
    return valid_effect(cat, effect) && p < cat.effect[effect].param_count;
}

// ---- tiny fixed-buffer text building (no heap, no locale) -------------------
struct Text {
    char s[64]{};
    int n = 0;
    Text& add(std::string_view v) {
        for (char c : v)
            if (n < 63) s[n++] = c;
        s[n] = 0;
        return *this;
    }
    Text& add(char c) { return add(std::string_view(&c, 1)); }
    Text& num(int v) {
        if (v < 0) {
            add('-');
            v = -v;
        }
        char d[12];
        int k = 0;
        do {
            d[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && k < 11);
        while (k) add(d[--k]);
        return *this;
    }
    Text& pct(float x) { return num(static_cast<int>(std::lround(std::clamp(x, 0.0f, 1.0f) * 100.0f))).add('%'); }
    Text& fixed2(float x) {  // "-1.25"
        int c = static_cast<int>(std::lround(x * 100.0f));
        if (c < 0) {
            add('-');
            c = -c;
        }
        num(c / 100).add('.');
        add(static_cast<char>('0' + (c / 10) % 10));
        return add(static_cast<char>('0' + c % 10));
    }
    std::string_view view() const { return {s, static_cast<size_t>(n)}; }
};

// Scales an 0xRRGGBB colour so its brightest channel is `level` (0..127). Very
// dark placeholder colours become grey, so the pad still shows something.
void pad_colour(uint32_t rgb, uint8_t level, uint8_t out[3]) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int mx = std::max({r, g, b});
    if (mx < 16) r = g = b = mx = 255;
    out[0] = static_cast<uint8_t>(r * level / mx);
    out[1] = static_cast<uint8_t>(g * level / mx);
    out[2] = static_cast<uint8_t>(b * level / mx);
}

void set_button_led(mk2::LedState& leds, Button b, uint8_t v) {
    const mk2::LedSlot slot = mk2::led_of(b);
    if (slot.kind == 1 && slot.index < mk2::kButtonLeds) leds.buttons[slot.index] = v;
    else if (slot.kind == 2 && slot.index < 8) leds.transport[slot.index] = v;
}

// ---- screen drawing ----------------------------------------------------------
// Header line: inverted on the focus side (perform.md §1), plain + underline on the other.
void header(ui::Screen& c, int side, std::string_view left, std::string_view right, bool focus) {
    const int x0 = side * mk2::kScreenW, x1 = x0 + mk2::kScreenW;
    const bool on = !focus;
    if (focus) c.fill(x0, 0, mk2::kScreenW, ui::kHeaderH);
    else c.fill(x0, ui::kHeaderH - 1, mk2::kScreenW, 1);
    int left_end = x1 - 2;
    if (!right.empty()) left_end = ui::text_in(c, x0 + 2, x1 - 2, 1, right, ui::Align::Right, on) - 6;
    ui::text_in(c, x0 + 2, left_end, 1, left, ui::Align::Left, on);
}

// A small bar: outline + fill (h >= 3).
void bar(ui::Screen& c, int x, int y, int w, int h, float v) {
    c.rect(x, y, w, h);
    c.fill(x + 1, y + 1, static_cast<int>(std::lround((w - 2) * std::clamp(v, 0.0f, 1.0f))), h - 2);
}

constexpr int kLayerNameY = 11;
constexpr int kLayerBarY = 20;
constexpr int kRow3Y = 27;
constexpr int kKnobLabelY = 36;
constexpr int kKnobValueY = 45;
constexpr int kKnobBarY = 53;

// One layer cell in the knob column of layer l on `side`: "2 NAME" + opacity bar.
void layer_cell(ui::Screen& c, int side, int l, const LayerState& L, const Catalog& cat, bool focused) {
    const ui::Column col = ui::kKnobColumn(side * 4 + l);
    Text t;
    t.num(l + 1).add(' ');
    if (L.clip < 0) t.add("--");
    else t.add(clip_name(cat, L.clip)).add(L.missing ? "!" : "");
    // A 4 px gap to the next cell; whole characters only.
    const int x0 = ui::text_in(c, col.x0 + 1, col.x1 - 4, kLayerNameY, t.view());
    if (L.mute) {  // muted: struck through
        const int w = std::min(ui::text_width(t.view()), col.x1 - 4 - x0);
        c.fill(x0, kLayerNameY + 3, w, 1);
    }
    bar(c, col.x0 + 2, kLayerBarY, col.width() - 4, 5, L.clip < 0 ? 0.0f : L.opacity);
    if (focused) c.invert(col.x0, kLayerNameY - 1, col.width(), 9);
}

struct KnobView {
    Text label, value;
    float norm = 0.0f;
    bool bipolar = false;
    bool used = false;
};

void knob_cell(ui::Screen& c, int k, const KnobView& v, bool hot) {
    const ui::Column col = ui::kKnobColumn(k);
    const int ax = ui::kKnobX[k];
    const int tx0 = col.x0 % mk2::kScreenW == 0 ? col.x0 : col.x0 + 1;
    const int tx1 = col.x1 % mk2::kScreenW == 0 ? col.x1 : col.x1 - 1;
    c.fill(ax - 2, 62, 5, 1);  // tick over the physical knob (as pages.cpp)
    c.fill(ax - 1, 61, 3, 1);
    c.pixel(ax, 60);
    if (v.used) {
        ui::text_anchored(c, tx0, tx1, kKnobLabelY, ax, v.label.view());
        ui::text_anchored(c, tx0, tx1, kKnobValueY, ax, v.value.view());
        const int bx = col.x0 + 2, bw = col.width() - 4;
        if (v.bipolar) {
            c.rect(bx, kKnobBarY, bw, 6);
            const int inner = bw - 2, mid = bx + 1 + inner / 2;
            const int to = bx + 1 + static_cast<int>(std::lround(inner * std::clamp(v.norm, 0.0f, 1.0f)));
            c.fill(std::min(mid, to), kKnobBarY + 1, std::max(1, std::abs(to - mid)), 4);
        } else {
            bar(c, bx, kKnobBarY, bw, 6, v.norm);
        }
    }
    if (hot) c.invert(col.x0, kKnobLabelY - 1, col.width(), kKnobBarY + 6 - kKnobLabelY + 1);
}

void fx_param_knob(KnobView& v, const FxSlotState& slot, int p, const Catalog& cat) {
    if (!has_param(cat, slot.effect, p)) return;
    v.used = true;
    v.label.add(param_name(cat, slot.effect, p));
    if (v.label.n == 0) v.label.add("P").num(p + 1);
    v.value.pct(slot.param[p]);
    v.norm = slot.param[p];
}

}  // namespace

// ============================================================================ state sync
uint8_t Mk2Perform::focus_deck(const LiveState& s) const { return focus_side_ == kPlay ? program_of(s) : preview_of(s); }

int Mk2Perform::pad_pages(const Catalog& cat) const { return std::max(1, (clip_count(cat) + kPadsPerPage - 1) / kPadsPerPage); }

void Mk2Perform::send(Command c, CommandSink& sink) {
    c.seq = ++seq_;
    sink.push(c);
}

void Mk2Perform::send_focus(const LiveState& s, CommandSink& sink) {
    send(make(CommandType::Focus, focus_deck(s), focus_layer_), sink);
}

// Keeps the local focus role in step with the engine:
//   * first state (or a new engine epoch): adopt the engine's focus if it has one,
//     else announce ours (PREP layer 1);
//   * the decks swapped (TAKE / CUT / wheel landed): the focus stays on its role,
//     so re-send it for the deck that now has that role;
//   * someone else (the GUI) moved the focus: adopt it.
void Mk2Perform::sync(const LiveState& s, CommandSink& sink) {
    const bool engine_has_focus = s.focus_deck < kDecks && s.focus_layer < kLayersPerDeck;
    auto adopt = [&] {
        focus_side_ = s.focus_deck == program_of(s) ? kPlay : kPrep;
        focus_layer_ = s.focus_layer;
    };
    if (!synced_ || s.epoch != epoch_) {
        synced_ = true;
        epoch_ = s.epoch;
        last_program_ = program_of(s);
        seen_focus_deck_ = s.focus_deck;
        seen_focus_layer_ = s.focus_layer;
        if (engine_has_focus) adopt();
        else send_focus(s, sink);
        return;
    }
    if (program_of(s) != last_program_) {
        last_program_ = program_of(s);
        seen_focus_deck_ = s.focus_deck;
        seen_focus_layer_ = s.focus_layer;
        send_focus(s, sink);
        return;
    }
    if (s.focus_deck != seen_focus_deck_ || s.focus_layer != seen_focus_layer_) {
        seen_focus_deck_ = s.focus_deck;
        seen_focus_layer_ = s.focus_layer;
        if (engine_has_focus && (s.focus_deck != focus_deck(s) || s.focus_layer != focus_layer_)) adopt();
    }
}

// ============================================================================ events
void Mk2Perform::cycle_fx(uint8_t deck, uint8_t layer, const FxSlotState& slot, int dir, const Catalog& cat,
                          CommandSink& sink) {
    const int n = effect_count(cat);
    if (n == 0) return;
    // Cycle through "empty, 0, 1, ... n-1" (n + 1 states).
    const int cur = valid_effect(cat, slot.effect) ? slot.effect + 1 : 0;
    const int next = ((cur + dir) % (n + 1) + (n + 1)) % (n + 1);
    send(make_fx(CommandType::FxSet, deck, layer, 0, kNone, next - 1), sink);
}

void Mk2Perform::on_knob(int k, int delta, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    const float scale = shift_ ? kFineScale : 1.0f;
    const float d = static_cast<float>(delta) / kKnobUnitsPerTurn * scale;
    const uint8_t deck = focus_deck(s), layer = focus_layer_;
    const LayerState& L = s.deck[deck].layer[layer];
    switch (k) {
        case 0:
            send(make(CommandType::LayerOpacity, deck, layer, 0, d, kRelative), sink);
            break;
        case 1:
            send(make(CommandType::LayerSpeed, deck, layer, 0, d * kSpeedPerTurn, kRelative), sink);
            break;
        case 2:
        case 3:
            if (has_param(cat, L.fx[0].effect, k - 2))
                send(make_fx(CommandType::FxParam, deck, layer, 0, static_cast<uint8_t>(k - 2), 0, d, kRelative), sink);
            break;
        default: {  // knobs 5-8: master FX slot 1, params 1-4.
            // GUESS: stand-in macros until scenes (and their 4 macros) exist (perform.md §3).
            const FxSlotState& m = s.master_fx[0];
            if (k == 7 && shift_) {  // SHIFT + knob 8 = choose the master effect (hardware-mapping §3.3)
                master_choose_acc_ += delta;
                if (master_choose_acc_ >= kChooseUnits || master_choose_acc_ <= -kChooseUnits) {
                    const int dir = master_choose_acc_ > 0 ? 1 : -1;
                    master_choose_acc_ = 0;
                    cycle_fx(kMasterDeck, kNone, m, dir, cat, sink);
                }
                break;
            }
            const int p = k - 4;
            if (has_param(cat, m.effect, p))
                send(make_fx(CommandType::FxParam, kMasterDeck, kNone, 0, static_cast<uint8_t>(p), 0, d, kRelative), sink);
            break;
        }
    }
}

void Mk2Perform::on_event(const mk2::Event& e, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    if (e.type == mk2::EventType::Connected || e.type == mk2::EventType::Disconnected) {
        reset();
        return;
    }
    sync(s, sink);
    last_event_us_ = e.t_us;
    const uint8_t deck = focus_deck(s), layer = focus_layer_;
    const LayerState& L = s.deck[deck].layer[layer];

    switch (e.type) {
        case mk2::EventType::ButtonDown: {
            if (is(e, Button::Shift)) {
                shift_ = true;
            } else if (e.id >= static_cast<uint8_t>(Button::GroupA) && e.id <= static_cast<uint8_t>(Button::GroupH)) {
                const int g = e.id - static_cast<uint8_t>(Button::GroupA);
                const uint8_t side = g >= 4 ? kPlay : kPrep;
                const uint8_t l = static_cast<uint8_t>(g & 3);
                if (shift_) {  // SHIFT + group = mute that layer (hardware-mapping §3.5), focus unchanged
                    const uint8_t d = side == kPlay ? program_of(s) : preview_of(s);
                    send(make(CommandType::LayerMute, d, l, kToggle), sink);
                } else {
                    focus_side_ = side;
                    focus_layer_ = l;
                    send_focus(s, sink);
                }
            } else if (is(e, Button::Play) || is(e, Button::EncoderPush)) {
                send(make(CommandType::Take, kNone, kNone, 0), sink);  // 0 = the show default (slice: 2000 ms)
            } else if (is(e, Button::Restart)) {
                send(make(CommandType::Cut), sink);
            } else if (is(e, Button::TransportLeft)) {
                pad_page_ = std::max(0, std::min(pad_page_, pad_pages(cat) - 1) - 1);
            } else if (is(e, Button::TransportRight)) {
                pad_page_ = std::min(pad_pages(cat) - 1, pad_page_ + 1);
            } else if (is(e, Button::PadMode)) {
                // GUESS (slice only): PAD MODE = next effect in FX slot 1 of the focused layer,
                // SHIFT + PAD MODE = previous. In 0.1 PAD MODE becomes FX PADS (hardware-mapping §3.4).
                cycle_fx(deck, layer, L.fx[0], shift_ ? -1 : 1, cat, sink);
            } else if (is(e, Button::Navigate)) {
                // GUESS (slice only): NAVIGATE = bypass FX slot 1 of the focused layer. In 0.1 NAVIGATE
                // becomes OUTPUTS (hardware-mapping §3.4).
                if (valid_effect(cat, L.fx[0].effect))
                    send(make_fx(CommandType::FxBypass, deck, layer, 0, kNone, kToggle), sink);
            } else if (is(e, Button::Mute)) {
                send(make(CommandType::LayerMute, deck, layer, kToggle), sink);
            }
            break;
        }
        case mk2::EventType::ButtonUp:
            if (is(e, Button::Shift)) {
                shift_ = false;
                master_choose_acc_ = 0;
            }
            break;
        case mk2::EventType::KnobDelta:
            if (e.id < mk2::kKnobs) {
                touched_knob_ = e.id;
                touched_us_ = e.t_us;
                on_knob(e.id, e.value, s, cat, sink);
            }
            break;
        case mk2::EventType::EncoderDelta: {
            if (shift_) break;  // SHIFT + wheel = system menu (not in the slice)
            // Endless T-bar: clockwise always blends PREVIEW in, whichever deck that is.
            // GUESS: the engine flips program_deck when a manual move reaches the far end
            // (engine-state.md: program_deck = "the side the crossfader rests on").
            const float step = static_cast<float>(e.value) / kWheelDetentsPerTake;
            send(make(CommandType::Crossfader, kNone, kNone, 0, program_of(s) == 0 ? step : -step, kRelative), sink);
            break;
        }
        case mk2::EventType::PadHit: {
            if (e.id < 1 || e.id > mk2::kPads) break;
            pads_held_ = static_cast<uint16_t>(pads_held_ | (1u << (e.id - 1)));
            // GUESS: clip order follows the MK2 pad numbers (pad 1 = bottom-left = first clip of the page).
            const int clip = std::min(pad_page_, pad_pages(cat) - 1) * kPadsPerPage + (e.id - 1);
            if (!valid_clip(cat, clip)) break;
            const float vel = std::clamp((e.value - 1) / 126.0f, 0.0f, 1.0f);
            send(make(CommandType::LayerClip, deck, layer, clip), sink);
            send(make(CommandType::LayerOpacity, deck, layer, 0, kMinPadOpacity + (1.0f - kMinPadOpacity) * vel), sink);
            break;
        }
        case mk2::EventType::PadRelease:
            if (e.id >= 1 && e.id <= mk2::kPads) pads_held_ = static_cast<uint16_t>(pads_held_ & ~(1u << (e.id - 1)));
            break;
        default:
            break;
    }
}

// ============================================================================ outputs
void Mk2Perform::tick(uint64_t now_us, const LiveState& s, const Catalog& cat, CommandSink& sink, mk2::LedState& leds,
                      uint8_t screens[2][mk2::kScreenBytes]) {
    sync(s, sink);
    pad_page_ = std::min(pad_page_, pad_pages(cat) - 1);  // the catalog may have shrunk
    const uint8_t prog = program_of(s), prev = preview_of(s);
    const uint8_t fdeck = focus_deck(s), flayer = focus_layer_;
    const LayerState& F = s.deck[fdeck].layer[flayer];

    // ---- LEDs ----
    std::memset(&leds, 0, sizeof(leds));
    for (int g = 0; g < 8; ++g) {  // A-D green = PREVIEW, E-H red = PROGRAM
        const bool play_row = g >= 4;
        const uint8_t d = play_row ? prog : prev;
        const int l = g & 3;
        const bool focused = (play_row ? kPlay : kPrep) == focus_side_ && l == flayer;
        const uint8_t v = focused ? kLedFull : s.deck[d].layer[l].clip >= 0 ? kLedDim : 0;
        for (int side = 0; side < 2; ++side) leds.group_rgb[g][side][play_row ? 0 : 1] = v;
    }
    const int pages = pad_pages(cat);
    for (int p = 0; p < mk2::kPads; ++p) {
        uint8_t* out = leds.pad_rgb[p];
        if (pads_held_ & (1u << p)) {  // your finger: white
            out[0] = out[1] = out[2] = kLedFull;
            continue;
        }
        const int clip = pad_page_ * kPadsPerPage + p;
        if (!valid_clip(cat, clip)) continue;  // dark = empty
        uint8_t level = kPadInBank;
        for (int d = 0; d < kDecks && level < kPadOnDeck; ++d)
            for (int l = 0; l < kLayersPerDeck; ++l)
                if (s.deck[d].layer[l].clip == clip) level = kPadOnDeck;
        if (F.clip == clip) level = kLedFull;
        pad_colour(cat.clip[clip].thumb_rgb, level, out);
    }
    set_button_led(leds, Button::Shift, shift_ ? kLedFull : 0);
    set_button_led(leds, Button::Play, s.take_active ? kLedFull : kLedDim);
    set_button_led(leds, Button::Restart, kLedDim);
    set_button_led(leds, Button::TransportLeft, pad_page_ > 0 ? kLedFull : 0);
    set_button_led(leds, Button::TransportRight, pad_page_ < pages - 1 ? kLedFull : 0);
    set_button_led(leds, Button::Pattern, kLedFull);  // pads are in CLIPS mode
    const bool has_fx = valid_effect(cat, F.fx[0].effect);
    set_button_led(leds, Button::PadMode, has_fx ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Navigate, !has_fx ? 0 : F.fx[0].bypass ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Mute, F.mute ? kLedFull : kLedFaint);

    // ---- screens: left = PREVIEW deck, right = PROGRAM deck ----
    ui::Screen c;
    c.clear();
    const char deck_letter[2] = {'A', 'B'};
    {
        Text l, r;
        l.add("PREVIEW ").add(deck_letter[prev]);
        if (shift_) r.add("SHIFT");
        else r.add("PADS ").num(pad_page_ + 1).add('/').num(pages);
        header(c, 0, l.view(), r.view(), focus_side_ == kPrep);
    }
    {
        Text l, r;
        l.add("PROGRAM ").add(deck_letter[prog]);
        if (s.black) r.add("BLACK");
        else if (s.take_active) r.add("TAKE");
        else r.add("ON AIR");
        header(c, 1, l.view(), r.view(), focus_side_ == kPlay);
    }
    for (int side = 0; side < 2; ++side) {
        const uint8_t d = side == 0 ? prev : prog;
        for (int l = 0; l < kLayersPerDeck; ++l)
            layer_cell(c, side, l, s.deck[d].layer[l], cat, (side == 0 ? kPrep : kPlay) == focus_side_ && l == flayer);
    }
    {  // left row 3: the focused layer's FX slot 1 (knobs 3-4 act on it)
        Text t;
        t.add("L").num(flayer + 1).add(focus_side_ == kPlay ? " LIVE FX " : " FX ");
        if (has_fx) t.add(effect_name(cat, F.fx[0].effect)).add(F.fx[0].bypass ? " (BYP)" : "");
        else t.add("NONE");
        ui::text_in(c, 2, mk2::kScreenW - 2, kRow3Y, t.view());
    }
    {  // right row 3: TAKE / crossfader progress = how much of PREVIEW is blended in
        const float xf = std::clamp(s.take_active ? (prog == 0 ? s.take_progress : 1.0f - s.take_progress) : s.crossfader,
                                    0.0f, 1.0f);
        const float into_preview = prog == 0 ? xf : 1.0f - xf;
        const int x0 = mk2::kScreenW;
        ui::text_in(c, x0 + 2, x0 + 30, kRow3Y, "TAKE");
        bar(c, x0 + 30, kRow3Y, 190, 7, into_preview);
        Text p;
        p.pct(into_preview);
        ui::text_in(c, x0 + 224, x0 + mk2::kScreenW - 2, kRow3Y, p.view(), ui::Align::Right);
    }
    // Knobs: 1-4 = focused layer, 5-8 = master FX slot 1 (stand-in macros).
    KnobView kv[mk2::kKnobs];
    kv[0].used = true;
    kv[0].label.add("OPACITY");
    kv[0].value.pct(F.opacity);
    kv[0].norm = F.opacity;
    kv[1].used = true;
    kv[1].label.add("SPEED");
    kv[1].value.fixed2(F.speed).add('x');
    kv[1].norm = (F.speed + 4.0f) / 8.0f;
    kv[1].bipolar = true;
    fx_param_knob(kv[2], F.fx[0], 0, cat);
    fx_param_knob(kv[3], F.fx[0], 1, cat);
    const FxSlotState& M = s.master_fx[0];
    for (int p = 0; p < 4; ++p) fx_param_knob(kv[4 + p], M, p, cat);
    if (!valid_effect(cat, M.effect) || shift_) {  // hint where the master effect is chosen
        kv[7] = KnobView{};
        kv[7].used = true;
        kv[7].label.add(shift_ ? "CHOOSE" : "MFX");
        kv[7].value.add(valid_effect(cat, M.effect) ? effect_name(cat, M.effect) : std::string_view("NONE"));
    }
    const bool recent = touched_knob_ >= 0 && now_us >= touched_us_ && now_us - touched_us_ < kTouchedUs;
    for (int k = 0; k < mk2::kKnobs; ++k) knob_cell(c, k, kv[k], recent && k == touched_knob_);
    c.split(screens[0], screens[1]);
}

void Mk2Perform::reset() {
    shift_ = false;
    pads_held_ = 0;
    touched_knob_ = -1;
    master_choose_acc_ = 0;
    synced_ = false;  // re-read the engine's focus and re-announce ours
}

}  // namespace himmel::perform
