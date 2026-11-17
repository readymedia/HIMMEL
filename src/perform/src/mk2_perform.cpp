// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// MK2 Perform mapper, M4: docs/design/perform.md v4 + hardware-mapping.md §3.
// The mapping table (and every GUESS) is in docs/agents/handoff/control.md,
// "Perform mapping (M4)".
//
// Runs in the render loop: no heap, no locks, no I/O, no Windows calls. All
// text is formatted into fixed stack buffers; the screens are drawn on a 4 KB
// stack canvas.
#include "himmel/perform/mk2_perform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "himmel/engine/clip_directory.h"
#include "himmel/mk2/pages.h"

namespace himmel::perform {

using namespace himmel::protocol;
using mk2::Button;
namespace ui = mk2::ui;

namespace {

// ---- tuning (all GUESS: until the owner has played it) ----------------------
constexpr float kKnobUnitsPerTurn = 1000.0f;   // one knob turn ~1008 raw units (measured)
constexpr float kSpeedPerTurn = 2.0f;          // GUESS: one turn = 2x speed
constexpr float kFadePerTurn = 5.0f;           // GUESS: one turn = 5 s of fade in / out
constexpr float kBpmPerTurn = 40.0f;           // GUESS: MIX knob 2, one turn = 40 BPM
constexpr float kFineScale = 0.1f;             // GUESS: SHIFT + knob = fine (1/10)
constexpr float kWheelDetentsPerTake = 30.0f;  // hardware-mapping §3.1: one turn (30 detents) = done
constexpr int kStepUnits = 100;                // GUESS: stepped knobs (scroll, choose, mode), raw units per step
constexpr int kDiceUnits = 150;                // GUESS: knob 2 DICE, raw units per roll
constexpr float kMinPadOpacity = 0.2f;         // GUESS: softest pad hit still shows the clip at 20 %
constexpr uint64_t kTouchedUs = 800'000;       // knob column stays inverted 0.8 s after a turn
constexpr uint64_t kDoubleTapUs = 350'000;     // GUESS: two presses of one group within this = double-tap
constexpr uint64_t kFlashUs = 1'500'000;       // a screen message stays 1.5 s
constexpr int kPressDeadband = 64;             // pressure changes below this (of 4095) are not sent
constexpr int kSliceLoopFrom = 1200;           // GUESS: SLICE pressure above this starts a beat loop
constexpr uint32_t kTakeMsMin = 100, kTakeMsMax = 10000;
constexpr float kScratchPerDetent = 0.05f;     // GUESS: SELECT + wheel, clip seconds per detent
constexpr float kStutterBeats = 0.25f;         // GUESS: NOTE REPEAT = a 1/4-beat loop on PROGRAM
constexpr float kBpmPerDetent = 0.5f;          // GUESS: TEMPO + wheel, BPM per detent

// Master FX slots the mapper uses: 0 = MASTER FX (SHIFT + knob 8), 2 = FX PADS, 3 = STROBE (Top 6).
constexpr uint8_t kMasterFxSlot = 0;
constexpr uint8_t kFxPadSlot = 2;
constexpr uint8_t kStrobeSlot = 3;

// LED levels (0..127).
constexpr uint8_t kLedFull = 127;
constexpr uint8_t kLedDim = 28;
constexpr uint8_t kLedFaint = 12;
constexpr uint8_t kPadInBank = 16;   // clip is only in the catalog

constexpr int kPadsPerPage = mk2::kPads;

constexpr const char* kPageName[] = {"PLAY", "CLIP", "LAYER", "FX", "MIX"};
constexpr const char* kModeName[] = {"CLIPS", "SLICE", "LOOKS", "FX PADS", "MOODS"};
constexpr const char* kPlayModeName[] = {"LOOP", "PINGPONG", "ONCE", "EJECT", "NEXT", "RANDOM", "CHOP", "STEP", "SCRATCH"};
constexpr const char* kBlendName[] = {"NORMAL", "ADD", "SCREEN", "MULTIPLY", "DIFF", "LIGHTEN"};
constexpr const char* kGridName[] = {"OFF", "1/4", "BEAT", "BAR", "2 BARS", "4 BARS"};
constexpr const char* kTargetName[] = {"SELECTED", "LAYER 1", "LAYER 2", "LAYER 3", "LAYER 4", "ALL", "FREE", "HIT"};
// Top 1-4 = what the dice may change (SHIFT = the second four), hardware-mapping §3.2.
constexpr uint32_t kTopLock[2][4] = {{kLockClip, kLockFx, kLockColour, kLockMotion},
                                     {kLockLayout, kLockSource, kLockReact, kLockMood}};
constexpr const char* kLockName[8] = {"CLIP", "FX", "COLOUR", "MOTION", "LAYOUT", "SOURCE", "REACT", "MOOD"};

enum PadHoldKind : uint8_t { kHoldNone = 0, kHoldClip = 1, kHoldHit = 2, kHoldSlice = 3, kHoldFx = 4, kHoldScene = 5, kHoldDone = 6 };

uint8_t program_of(const LiveState& s) { return s.program_deck < kDecks ? s.program_deck : 0; }
uint8_t preview_of(const LiveState& s) { return static_cast<uint8_t>(program_of(s) ^ 1); }

int clip_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.clip_count, kMaxClips)); }
int effect_count(const Catalog& cat) { return static_cast<int>(std::min<uint32_t>(cat.effect_count, kMaxEffects)); }
bool valid_effect(const Catalog& cat, int id) { return id >= 0 && id < effect_count(cat); }

std::string_view cstr(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return {s, n};
}
// Clip name without its extension ("sunset.mov" -> "sunset"); null = "".
std::string_view clip_label(const ClipInfo* ci) {
    if (!ci) return {};
    std::string_view n = cstr(ci->name, kNameLen);
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
bool has_param(const Catalog& cat, int effect, int p) { return valid_effect(cat, effect) && p < cat.effect[effect].param_count; }
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
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
    Text& fixed(float x, int decimals) {  // "-1.25" / "2.5"
        int scale = decimals == 1 ? 10 : 100;
        int c = static_cast<int>(std::lround(x * static_cast<float>(scale)));
        if (c < 0) {
            add('-');
            c = -c;
        }
        num(c / scale).add('.');
        if (decimals == 2) add(static_cast<char>('0' + (c / 10) % 10));
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
void rgb(uint8_t out[3], int r, int g, int b) {
    out[0] = static_cast<uint8_t>(std::clamp(r, 0, 127));
    out[1] = static_cast<uint8_t>(std::clamp(g, 0, 127));
    out[2] = static_cast<uint8_t>(std::clamp(b, 0, 127));
}

void set_button_led(mk2::LedState& leds, Button b, uint8_t v) {
    const mk2::LedSlot slot = mk2::led_of(b);
    if (slot.kind == 1 && slot.index < mk2::kButtonLeds) leds.buttons[slot.index] = v;
    else if (slot.kind == 2 && slot.index < 8) leds.transport[slot.index] = v;
}

// The scene on a deck as "3b" (pad + variation), "--" for none, "*" when edited.
void scene_tag(Text& t, const ShowStatus* show, int deck) {
    if (!show || deck < 0 || deck >= kDecks) return;
    const DeckSceneInfo& d = show->deck[deck];
    if (d.scene < 0) return;
    if (d.pad >= 0) t.num(d.pad + 1);
    else t.add('S');
    if (d.variation) t.add(d.variation);
    if (d.edited) t.add('*');
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
void layer_cell(ui::Screen& c, int side, int l, const LayerState& L, std::string_view name, bool focused) {
    const ui::Column col = ui::kKnobColumn(side * 4 + l);
    Text t;
    t.num(l + 1).add(' ');
    if (L.clip < 0) t.add("--");
    else t.add(name).add(L.missing ? "!" : "");
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
    bool bar = true;
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
        if (!v.bar) {
        } else if (v.bipolar) {
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

void knob_text(KnobView& v, std::string_view label, std::string_view value) {
    v.used = true;
    v.bar = false;
    v.label.add(label);
    v.value.add(value);
}
void knob_pct(KnobView& v, std::string_view label, float x) {
    v.used = true;
    v.label.add(label);
    v.value.pct(x);
    v.norm = x;
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
uint8_t Mk2Perform::focus_deck(const LiveState& s) const { return side_deck(s, focus_side_); }
uint8_t Mk2Perform::side_deck(const LiveState& s, uint8_t side) const { return side == kPlay ? program_of(s) : preview_of(s); }

int Mk2Perform::pad_pages(const Catalog& cat) const { return std::max(1, (nclips(cat) + kPadsPerPage - 1) / kPadsPerPage); }

// ---- every clip (wave 8): the engine's ClipDirectory when the renderer set it, else the catalog window
int Mk2Perform::nclips(const Catalog& cat) const { return clips_ ? static_cast<int>(clips_->count()) : clip_count(cat); }
const ClipInfo* Mk2Perform::clip_at(const Catalog& cat, int id) const {
    if (id < 0 || id >= nclips(cat)) return nullptr;
    return clips_ ? &clips_->info(id) : &cat.clip[id];
}
ClipState Mk2Perform::clip_state(int id) const { return clips_ && clips_->valid(id) ? clips_->state(id) : ClipState::Ready; }
int64_t Mk2Perform::clip_lib(int id) const { return clips_ && clips_->valid(id) ? clips_->entry(id).library_id : 0; }
bool Mk2Perform::loadable(const Catalog& cat, int id) const {
    const ClipState st = clip_state(id);
    return clip_at(cat, id) && st != ClipState::Removed && st != ClipState::Empty;
}
// A library clip loads by its library id (LayerClipLib: the engine refuses it while it is not
// ready, instead of a placeholder); media/ and --clip clips by engine id.
void Mk2Perform::load_clip(uint8_t deck, uint8_t layer, int id, CommandSink& sink) {
    const int64_t lib = clip_lib(id);
    if (lib > 0 && lib <= INT32_MAX) send(make(CommandType::LayerClipLib, deck, layer, static_cast<int32_t>(lib)), sink);
    else send(make(CommandType::LayerClip, deck, layer, id), sink);
}

void Mk2Perform::send(Command c, CommandSink& sink) {
    c.seq = ++seq_;
    sink.push(c);
}

void Mk2Perform::send_focus(const LiveState& s, CommandSink& sink) {
    send(make(CommandType::Focus, focus_deck(s), focus_layer_), sink);
}

void Mk2Perform::say(const char* text, uint64_t now_us) {
    std::size_t n = 0;
    for (; text[n] && n < sizeof(flash_) - 1; ++n) flash_[n] = text[n];
    flash_[n] = 0;
    flash_until_us_ = now_us + kFlashUs;
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

// ============================================================================ helpers
void Mk2Perform::cycle_fx(uint8_t deck, uint8_t layer, uint8_t slot, const FxSlotState& fx, int dir, const Catalog& cat,
                          CommandSink& sink) {
    const int n = effect_count(cat);
    if (n == 0 || dir == 0) return;
    // Cycle through "empty, 0, 1, ... n-1" (n + 1 states).
    const int cur = valid_effect(cat, fx.effect) ? fx.effect + 1 : 0;
    const int next = ((cur + dir) % (n + 1) + (n + 1)) % (n + 1);
    send(make_fx(CommandType::FxSet, deck, layer, slot, kNone, next - 1), sink);
}

int Mk2Perform::strobe_effect(const Catalog& cat) const {
    for (int i = 0; i < effect_count(cat); ++i)
        if (ieq(cstr(cat.effect[i].name, kNameLen), "strobe")) return i;
    return -1;
}

// FREEZE (what = 0): pause / resume every playing PROGRAM layer. NOTE REPEAT (what = 1):
// a short beat loop on every PROGRAM layer with a clip, off on release.
void Mk2Perform::hold_program_layers(const LiveState& s, CommandSink& sink, bool on, int what) {
    const uint8_t d = program_of(s);
    for (int l = 0; l < kLayersPerDeck; ++l) {
        const LayerState& L = s.deck[d].layer[l];
        if (L.clip < 0) continue;
        if (what == 0) send(make(CommandType::LayerPlay, d, static_cast<uint8_t>(l), static_cast<int32_t>(on ? PlayAction::Pause : PlayAction::Play)), sink);
        else send(make(CommandType::LayerBeatLoop, d, static_cast<uint8_t>(l), 0, on ? kStutterBeats : 0.0f), sink);
    }
}

int Mk2Perform::steps(int k, int delta, int units) {
    knob_acc_[k] += delta;
    const int n = knob_acc_[k] / units;
    knob_acc_[k] -= n * units;
    return n;
}

// ============================================================================ knobs
void Mk2Perform::on_knob(int k, int delta, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    const float scale = shift_ ? kFineScale : 1.0f;
    const float d = static_cast<float>(delta) / kKnobUnitsPerTurn * scale;
    const uint8_t deck = focus_deck(s), layer = focus_layer_;
    const LayerState& L = s.deck[deck].layer[layer];
    const LayerTransport* T = transport_ ? &transport_->layer[deck][layer] : nullptr;

    if (browse_ && k == 0) {  // BROWSE: knob 1 scrolls the list (hardware-mapping §3.1); SHIFT = a page
        browse_move(cat, steps(k, delta, kStepUnits / 2) * (shift_ ? kBrowsePage : 1));
        return;
    }
    if (browse_ && k == 1) {  // BROWSE: knob 2 = the order (ADDED / NAME / NEWEST / ENERGY), D200
        const int n = steps(k, delta, kStepUnits);
        if (n == 0) return;
        const int m = static_cast<int>(BrowseSort::Count);
        browse_sort_ = static_cast<BrowseSort>(((static_cast<int>(browse_sort_) + n) % m + m) % m);
        refresh_order(cat);  // keeps the selected clip
        return;
    }
    if (k >= 4) {  // knobs 5-8: always the PROGRAM scene's macros (perform.md §3)
        if (k == 7 && shift_) {  // SHIFT + knob 8 = choose the master effect (hardware-mapping §3.3)
            cycle_fx(kMasterDeck, kNone, kMasterFxSlot, s.master_fx[kMasterFxSlot], steps(k, delta, kStepUnits), cat, sink);
            return;
        }
        Command c = make(CommandType::MacroValue, kNone, kNone, 0, d, kRelative);
        c.slot = static_cast<uint8_t>(k - 4);
        send(c, sink);
        return;
    }
    switch (page_) {
        case Page::Play:
            switch (k) {
                case 0: {  // SCROLL: the next / previous clip loads at once (Removed clips skipped)
                    const int n = steps(k, delta, kStepUnits), count = nclips(cat);
                    if (n == 0 || count == 0) break;
                    const int cur = L.clip >= 0 ? L.clip : (n > 0 ? -1 : 0);
                    int next = ((cur + n) % count + count) % count;
                    for (int tries = 0; tries < count && !loadable(cat, next); ++tries)
                        next = ((next + (n > 0 ? 1 : -1)) % count + count) % count;
                    if (loadable(cat, next)) load_clip(deck, layer, next, sink);
                    break;
                }
                case 1: {  // DICE: right = a new look, left = back through the history (ALL = the whole deck)
                    const int n = steps(k, delta, kDiceUnits);
                    if (n == 0) break;
                    Command c = make(CommandType::Dice, deck, held(Button::All) ? kNone : layer, 0);
                    c.param = static_cast<uint8_t>(n > 0 ? DiceVerb::Roll : DiceVerb::Back);
                    send(c, sink);
                    break;
                }
                case 2:  // FX amount = the mix of the layer's main effect
                    if (valid_effect(cat, L.fx[0].effect))
                        send(make_fx(CommandType::FxMix, deck, layer, 0, kNone, 0, d, kRelative), sink);
                    break;
                default:  // SPEED
                    send(make(CommandType::LayerSpeed, deck, layer, 0, d * kSpeedPerTurn, kRelative), sink);
                    break;
            }
            break;
        case Page::Clip:
            if (L.clip < 0 && k < 3) break;
            if (k == 0 || k == 1) {  // IN / OUT
                Command c = make(CommandType::LayerSet, deck, layer, 0, d, kRelative);
                c.param = static_cast<uint8_t>(k == 0 ? LayerParam::In : LayerParam::Out);
                send(c, sink);
            } else if (k == 2) {  // MODE
                const int n = steps(k, delta, kStepUnits);
                if (n == 0) break;
                const int count = static_cast<int>(PlayMode::Count);
                const int cur = T ? T->mode : 0;
                Command c = make(CommandType::LayerSet, deck, layer, ((cur + n) % count + count) % count);
                c.param = static_cast<uint8_t>(LayerParam::Mode);
                send(c, sink);
            } else {
                send(make(CommandType::LayerSpeed, deck, layer, 0, d * kSpeedPerTurn, kRelative), sink);
            }
            break;
        case Page::Layer:
            if (k == 0) {
                send(make(CommandType::LayerOpacity, deck, layer, 0, d, kRelative), sink);
            } else if (k == 1) {  // BLEND
                const int n = steps(k, delta, kStepUnits);
                if (n == 0) break;
                const int count = static_cast<int>(BlendMode::Count);
                send(make(CommandType::LayerBlend, deck, layer, ((L.blend + n) % count + count) % count), sink);
            } else {  // FADE IN / FADE OUT (seconds)
                Command c = make(CommandType::LayerSet, deck, layer, 0, d * kFadePerTurn, kRelative);
                c.param = static_cast<uint8_t>(k == 2 ? LayerParam::FadeIn : LayerParam::FadeOut);
                send(c, sink);
            }
            break;
        case Page::Fx:
            if (k == 0) {
                cycle_fx(deck, layer, 0, L.fx[0], steps(k, delta, kStepUnits), cat, sink);
            } else if (k <= 2) {
                if (has_param(cat, L.fx[0].effect, k - 1))
                    send(make_fx(CommandType::FxParam, deck, layer, 0, static_cast<uint8_t>(k - 1), 0, d, kRelative), sink);
            } else if (valid_effect(cat, L.fx[0].effect)) {
                send(make_fx(CommandType::FxMix, deck, layer, 0, kNone, 0, d, kRelative), sink);
            }
            break;
        default:  // MIX
            if (k == 0) {  // TAKE time (the mapper's own; 0 = the show default)
                const int n = steps(k, delta, kStepUnits / 2);
                if (n == 0) break;
                const int cur = take_ms_ ? static_cast<int>(take_ms_) : (s.take_ms ? static_cast<int>(s.take_ms) : 2000);
                const int step = cur < 1000 ? 100 : cur < 4000 ? 250 : 500;
                take_ms_ = static_cast<uint32_t>(std::clamp(cur + n * step, static_cast<int>(kTakeMsMin), static_cast<int>(kTakeMsMax)));
            } else if (k == 1) {  // BPM
                Command c = make(CommandType::Clock, kNone, kNone, 0, d * kBpmPerTurn, kRelative);
                c.param = static_cast<uint8_t>(ClockParam::Bpm);
                send(c, sink);
            } else if (k == 2) {  // GRID (the global beat snap)
                const int n = steps(k, delta, kStepUnits);
                if (n == 0) break;
                const int cur = transport_ ? transport_->clock.grid : 0;
                Command c = make(CommandType::Clock, kNone, kNone, std::clamp(cur + n, 0, static_cast<int>(GridDiv::Count) - 1));
                c.param = static_cast<uint8_t>(ClockParam::Grid);
                send(c, sink);
            } else if (valid_effect(cat, s.master_fx[kMasterFxSlot].effect)) {  // MASTER FX mix
                send(make_fx(CommandType::FxMix, kMasterDeck, kNone, kMasterFxSlot, kNone, 0, d, kRelative), sink);
            }
            break;
    }
}

// ============================================================================ buttons
void Mk2Perform::on_group(int g, const LiveState& s, CommandSink& sink, uint64_t now_us) {
    const uint8_t side = g >= 4 ? kPlay : kPrep;
    const uint8_t l = static_cast<uint8_t>(g & 3);
    const uint8_t d = side_deck(s, side);
    Text t;
    if (held(Button::Erase)) {  // hold ERASE + group = reset that layer to the scene (perform.md §3)
        erase_used_ = true;
        Command c = make(CommandType::SceneReset, d, l, 0, 0.0f);
        send(c, sink);
        t.add("RESET ").add(static_cast<char>('A' + g));
        say(t.s, now_us);
        return;
    }
    if (pads_held_ && (pad_mode_ == PadMode::Clips)) {  // hold pad + group = that pad's target (D71)
        for (int p = 0; p < mk2::kPads; ++p)
            if (pads_held_ & (1u << p)) {
                pad_target_[p] = static_cast<uint8_t>(static_cast<uint8_t>(PadTarget::Layer1) + l);
                pad_[p].kind = kHoldDone;
                t.n = 0, t.s[0] = 0;
                t.add("PAD ").num(p + 1).add(" > ").add(kTargetName[pad_target_[p]]);
            }
        say(t.s, now_us);
        return;
    }
    if (shift_) {  // SHIFT + group = mute that layer (hardware-mapping §3.5), focus unchanged
        send(make(CommandType::LayerMute, d, l, kToggle), sink);
        return;
    }
    // Double-tap = the same layer on the other side (A <-> E), perform.md §1.
    int eff = g;
    if (last_group_ == g && now_us - last_group_us_ < kDoubleTapUs) {
        eff = g ^ 4;
        last_group_ = -1;
    } else {
        last_group_ = g;
        last_group_us_ = now_us;
    }
    focus_side_ = eff >= 4 ? kPlay : kPrep;
    focus_layer_ = static_cast<uint8_t>(eff & 3);
    send_focus(s, sink);
}

void Mk2Perform::on_button(Button b, const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    const uint8_t deck = focus_deck(s), layer = focus_layer_;
    const LayerState& L = s.deck[deck].layer[layer];
    const uint8_t prog = program_of(s), prev = preview_of(s);
    const int bi = static_cast<int>(b);
    Text t;

    // ==== ENGINE (quit): in the SYSTEM menu the wheel push starts the QUIT hold. Any other button
    // closes the menu and does its normal job, so the menu never stands in the way of the show. ====
    if (sys_ && b != Button::Shift) {
        if (b == Button::EncoderPush) {
            if (!quit_sent_) quit_hold_us_ = std::max<uint64_t>(now_us, 1);
            return;
        }
        sys_ = false;
        quit_hold_us_ = 0;
    }
    // ==== end ENGINE (quit) ====
    if (bi >= static_cast<int>(Button::GroupA) && bi <= static_cast<int>(Button::GroupH)) {
        on_group(bi - static_cast<int>(Button::GroupA), s, sink, now_us);
        return;
    }
    if (bi >= static_cast<int>(Button::Top1) && bi <= static_cast<int>(Button::Top4)) {  // the dice locks
        const int k = bi - static_cast<int>(Button::Top1);
        Command c = make(CommandType::Locks, kNone, kNone, static_cast<int32_t>(kTopLock[shift_ ? 1 : 0][k]));
        c.param = static_cast<uint8_t>(LockOp::Toggle);
        send(c, sink);
        const bool now_held = show_ ? !(show_->locks & kTopLock[shift_ ? 1 : 0][k]) : true;
        t.add(kLockName[(shift_ ? 4 : 0) + k]).add(now_held ? " HELD" : " FREE");
        say(t.s, now_us);
        return;
    }
    switch (b) {
        case Button::Shift: shift_ = true; break;
        case Button::Top5:  // FREEZE while held, SHIFT = latch
            if (shift_) {
                freeze_latched_ = !freeze_latched_;
                if (freeze_latched_ != freeze_) freeze_ = freeze_latched_, hold_program_layers(s, sink, freeze_, 0);
            } else if (!freeze_) {
                freeze_ = true;
                hold_program_layers(s, sink, true, 0);
            }
            break;
        case Button::Top6: {  // STROBE while held (master FX slot 4), SHIFT = latch
            const int fx = strobe_effect(cat);
            if (fx < 0) {
                say("NO STROBE EFFECT", now_us);
                break;
            }
            const bool want = shift_ ? !strobe_latched_ : true;
            if (shift_) strobe_latched_ = want;
            if (want != strobe_) {
                strobe_ = want;
                send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kStrobeSlot, kNone, want ? fx : -1), sink);
            }
            break;
        }
        case Button::Top7:  // BLACK while held, SHIFT = latch
            if (shift_) black_latched_ = !black_latched_, send(make(CommandType::Black, kNone, kNone, black_latched_ ? kOn : kOff), sink);
            else send(make(CommandType::Black, kNone, kNone, kOn), sink);
            break;
        case Button::Top8:  // ASSIST: GUESS stub until the assist exists (SHOW / AUDIO)
            assist_ = !assist_;
            say(assist_ ? "ASSIST ON (STUB)" : "ASSIST OFF", now_us);
            break;
        case Button::Browse:  // LIBRARY: the catalog as a list (hardware-mapping §3.5); BROWSE again = back
            browse_ = !browse_;
            if (browse_) {
                refresh_order(cat);
                browse_select(cat, L.clip >= 0 ? static_cast<int>(L.clip) : browse_sel_);
            }
            break;
        case Button::EncoderPush:
            if (browse_) {  // in BROWSE the wheel push loads the selected clip
                browse_load(s, cat, sink, now_us);
                break;
            }
            [[fallthrough]];
        case Button::Play:
            send(make(CommandType::Take, kNone, kNone, static_cast<int32_t>(take_ms_)), sink);
            break;
        case Button::Restart: send(make(CommandType::Cut), sink); break;
        case Button::PageLeft:
        case Button::PageRight: {
            const int n = static_cast<int>(Page::Count);
            if (shift_) page_ = Page::Play;
            else page_ = static_cast<Page>(std::clamp(static_cast<int>(page_) + (b == Button::PageRight ? 1 : -1), 0, n - 1));
            break;
        }
        case Button::Pattern:
            if (shift_) {
                say("KIT: BANK (SCENE KITS LATER)", now_us);  // GUESS: scene kits need the kit in the show block
            } else {
                pad_mode_ = pad_mode_ == PadMode::Clips ? PadMode::Slice : PadMode::Clips;
            }
            break;
        case Button::Scene:
            pad_mode_ = shift_ ? PadMode::Moods : PadMode::Looks;
            if (shift_) say("MOODS (STUB)", now_us);
            break;
        case Button::PadMode: pad_mode_ = PadMode::FxPads; break;
        case Button::MasterLeft:
        case Button::MasterRight: {  // CLIPS: the clip page; LOOKS: the bank / song
            const int dir = b == Button::MasterRight ? 1 : -1;
            if (pad_mode_ == PadMode::Looks) {
                Command c = make(CommandType::ShowSelect, kNone, kNone, dir, 0.0f, kRelative);
                c.param = static_cast<uint8_t>(show_ && show_->use_setlist ? SelectWhat::Song : SelectWhat::Bank);
                send(c, sink);
            } else {
                pad_page_ = std::clamp(pad_page_ + dir, 0, pad_pages(cat) - 1);
            }
            break;
        }
        case Button::TransportLeft:
        case Button::TransportRight: {  // dice back / forward on the focus deck; SHIFT + > = mutate
            Command c = make(CommandType::Dice, deck, held(Button::All) ? kNone : layer, 0);
            c.param = static_cast<uint8_t>(b == Button::TransportLeft ? DiceVerb::Back
                                           : shift_                   ? DiceVerb::Mutate
                                                                      : DiceVerb::Forward);
            send(c, sink);
            break;
        }
        case Button::Grid: {  // the global beat snap: off <-> 1 beat
            const int cur = transport_ ? transport_->clock.grid : 0;
            Command c = make(CommandType::Clock, kNone, kNone,
                             static_cast<int32_t>(cur == static_cast<int>(GridDiv::Off) ? GridDiv::Beat : GridDiv::Off));
            c.param = static_cast<uint8_t>(ClockParam::Grid);
            send(c, sink);
            break;
        }
        case Button::Rec: rec_used_ = false; break;      // acts on release (REC + pad = that pad)
        case Button::Erase: erase_used_ = false; break;  // acts on release (ERASE + group = reset)
        case Button::All:
            if (held(Button::Erase)) {  // ERASE + ALL = reset the whole scene
                erase_used_ = true;
                send(make(CommandType::SceneReset, deck, kNone, 0, 0.0f), sink);
                say("RESET SCENE", now_us);
            } else if (pads_held_ && pad_mode_ == PadMode::Clips) {
                for (int p = 0; p < mk2::kPads; ++p)
                    if (pads_held_ & (1u << p)) pad_target_[p] = static_cast<uint8_t>(PadTarget::All), pad_[p].kind = kHoldDone;
                say("PAD > ALL", now_us);
            }
            break;
        case Button::Solo:
            if (pads_held_ && pad_mode_ == PadMode::Clips) {
                for (int p = 0; p < mk2::kPads; ++p)
                    if (pads_held_ & (1u << p)) pad_target_[p] = static_cast<uint8_t>(PadTarget::Hit), pad_[p].kind = kHoldDone;
                say("PAD > HIT", now_us);
            }
            break;
        case Button::Select:
            if (pads_held_ && pad_mode_ == PadMode::Clips) {
                for (int p = 0; p < mk2::kPads; ++p)
                    if (pads_held_ & (1u << p)) pad_target_[p] = static_cast<uint8_t>(PadTarget::Selected), pad_[p].kind = kHoldDone;
                say("PAD > SELECTED", now_us);
            }
            break;
        case Button::Enter: {  // GO: the next scene live (scene's transition); SHIFT = into PREVIEW (cut)
            if (browse_) {  // in BROWSE, ENTER loads the selected clip (GUESS: GO waits until BROWSE is left)
                browse_load(s, cat, sink, now_us);
                break;
            }
            Command c = make(CommandType::SceneRecall, shift_ ? prev : prog, kNone, kSceneNext, shift_ ? 0.0f : -1.0f);
            send(c, sink);
            break;
        }
        case Button::NoteRepeat:
            if (!repeat_) repeat_ = true, hold_program_layers(s, sink, true, 1);
            break;
        case Button::Tempo: tempo_turned_ = false; break;  // a tap on release unless the wheel turned
        case Button::Mute:
            if (shift_) {  // PANIC: GUESS = master black on (latched) until a safe scene exists
                black_latched_ = true;
                send(make(CommandType::Black, kNone, kNone, kOn), sink);
                say("PANIC: BLACK", now_us);
            } else {
                send(make(CommandType::LayerMute, deck, layer, kToggle), sink);
            }
            break;
        case Button::Navigate: say("OUTPUTS: LATER (M6)", now_us); break;
        default: break;
    }
    (void)L;
}

void Mk2Perform::on_button_up(Button b, const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    (void)cat;
    if (b == Button::EncoderPush && quit_hold_us_) {  // ENGINE (quit): let go before 2 s = no quit
        quit_hold_us_ = 0;
        if (!quit_sent_) say("QUIT CANCELLED", now_us);
        return;
    }
    switch (b) {
        case Button::Shift: shift_ = false; break;
        case Button::Top5:
            if (freeze_ && !freeze_latched_) freeze_ = false, hold_program_layers(s, sink, false, 0);
            break;
        case Button::Top6:
            if (strobe_ && !strobe_latched_) {
                strobe_ = false;
                send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kStrobeSlot, kNone, -1), sink);
            }
            break;
        case Button::Top7:
            if (!black_latched_) send(make(CommandType::Black, kNone, kNone, kOff), sink);
            break;
        case Button::Rec:
            if (!rec_used_) {  // PREP: save the preview scene to the next free pad; PLAY: a new variation
                const bool play = focus_side_ == kPlay;
                Command c = make(CommandType::SceneSave, focus_deck(s), kNone, -1, 0.0f, play ? kVariation : 0);
                send(c, sink);
            }
            break;
        case Button::Erase:
            if (!erase_used_) {
                send(make(CommandType::Undo, kNone, kNone, shift_ ? 1 : 0), sink);
                say(shift_ ? "REDO" : "UNDO", now_us);
            }
            break;
        case Button::NoteRepeat:
            if (repeat_) repeat_ = false, hold_program_layers(s, sink, false, 1);
            break;
        case Button::Tempo:
            if (!tempo_turned_) {
                Command c = make(CommandType::Clock);
                c.param = static_cast<uint8_t>(ClockParam::Tap);
                send(c, sink);
            }
            break;
        default: break;
    }
}

// ============================================================================ pads
void Mk2Perform::on_pad(int pad, int velocity, const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    const int p = pad - 1;
    PadHold& h = pad_[p];
    h = PadHold{};
    h.down_us = now_us;
    const float vel = std::clamp((velocity - 1) / 126.0f, 0.0f, 1.0f);
    const uint8_t deck = focus_deck(s), flayer = focus_layer_;
    const uint8_t prog = program_of(s), prev = preview_of(s);
    Text t;

    if (held(Button::Erase)) erase_used_ = true;
    if (held(Button::Rec)) rec_used_ = true;

    switch (pad_mode_) {
        case PadMode::Clips: {
            const int clip = std::min(pad_page_, pad_pages(cat) - 1) * kPadsPerPage + p;
            if (!loadable(cat, clip)) break;
            PadTarget target = static_cast<PadTarget>(pad_target_[p]);
            if (held(Button::Solo)) target = PadTarget::Hit;  // SOLO + pad = HIT from either focus (§4.3)
            if (target == PadTarget::Hit) {
                send(make(CommandType::LayerTrigger, kHitDeck, 0, clip, std::max(0.05f, vel)), sink);
                h.kind = kHoldHit;
                h.deck = kHitDeck;
                h.layer_mask = 1;
                break;
            }
            uint8_t mask = 0;
            switch (target) {
                case PadTarget::Layer1: case PadTarget::Layer2: case PadTarget::Layer3: case PadTarget::Layer4:
                    mask = static_cast<uint8_t>(1u << (static_cast<int>(target) - 1));
                    break;
                case PadTarget::All: mask = 0xF; break;
                case PadTarget::Free: {
                    for (int l = 0; l < kLayersPerDeck && !mask; ++l)
                        if (s.deck[deck].layer[l].clip < 0) mask = static_cast<uint8_t>(1u << l);
                    if (!mask) mask = static_cast<uint8_t>(1u << flayer);
                    break;
                }
                default: mask = static_cast<uint8_t>(1u << flayer); break;
            }
            for (int l = 0; l < kLayersPerDeck; ++l) {
                if (!(mask & (1u << l))) continue;
                send(make(CommandType::LayerTrigger, deck, static_cast<uint8_t>(l), clip, std::max(0.05f, vel)), sink);
                // Velocity = punch-in on PREVIEW. GUESS: on PROGRAM the opacity is left alone, so a
                // launch waiting for the grid never changes the live picture early.
                if (focus_side_ == kPrep)
                    send(make(CommandType::LayerOpacity, deck, static_cast<uint8_t>(l), 0,
                              kMinPadOpacity + (1.0f - kMinPadOpacity) * vel), sink);
            }
            h.kind = kHoldClip;
            h.deck = deck;
            h.layer_mask = mask;
            if (shift_ && focus_side_ == kPrep) send(make(CommandType::Take, kNone, kNone, static_cast<int32_t>(take_ms_)), sink);
            break;
        }
        case PadMode::Slice: {
            const LayerState& L = s.deck[deck].layer[flayer];
            if (L.clip < 0) break;
            Command c = make(CommandType::LayerCue, deck, flayer, static_cast<int32_t>(CueAction::Jump));
            c.slot = static_cast<uint8_t>(p);
            if (held(Button::Rec)) c.i = static_cast<int32_t>(CueAction::Set), c.f[0] = -1.0f;  // REC + pad = store a cue here
            else if (held(Button::Erase)) c.i = static_cast<int32_t>(CueAction::Clear);         // ERASE + pad = clear it
            send(c, sink);
            h.kind = c.i == static_cast<int32_t>(CueAction::Jump) ? kHoldSlice : kHoldDone;
            h.deck = deck;
            h.layer_mask = static_cast<uint8_t>(1u << flayer);
            break;
        }
        case PadMode::Looks: {
            if (held(Button::Rec)) {  // REC + pad = save the focus deck's look to that pad
                send(make(CommandType::SceneSave, deck, kNone, p), sink);
                h.kind = kHoldDone;
                break;
            }
            if (!show_ || !show_->pad[p].used) break;
            // Scenes land where the focus is (D70): PREP = into PREVIEW at once, PLAY = live with the
            // scene's transition; SHIFT = the other way (PREP: live now as a cut, PLAY: into PREVIEW).
            const bool live = (focus_side_ == kPlay) != shift_;
            const float morph = focus_side_ == kPlay && !shift_ ? -1.0f : 0.0f;
            send(make(CommandType::SceneRecall, live ? prog : prev, kNone, p, morph), sink);
            h.kind = kHoldScene;
            t.add("SCENE ").num(p + 1).add(live ? " LIVE" : " > PREVIEW");
            say(t.s, now_us);
            break;
        }
        case PadMode::FxPads: {
            // 16 momentary effects on PROGRAM (master FX slot 3): on while held, SHIFT = latch.
            // GUESS: pad n = effect n of the catalog; velocity = the mix (soft = subtle).
            if (!valid_effect(cat, p)) break;
            if (shift_ && fx_pad_ == p && fx_pad_latched_) {  // SHIFT on the latched pad = off
                send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kFxPadSlot, kNone, -1), sink);
                fx_pad_ = -1;
                fx_pad_latched_ = false;
                h.kind = kHoldDone;
                break;
            }
            send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kFxPadSlot, kNone, p), sink);
            send(make_fx(CommandType::FxMix, kMasterDeck, kNone, kFxPadSlot, kNone, 0, 0.35f + 0.65f * vel), sink);
            fx_pad_ = p;
            fx_pad_latched_ = shift_;
            h.kind = kHoldFx;
            h.deck = kMasterDeck;
            break;
        }
        case PadMode::Moods: {
            t.add("MOOD ").num(p + 1).add(" (STUB)");  // moods arrive with SHOW's mood map
            say(t.s, now_us);
            h.kind = kHoldDone;
            break;
        }
        default: break;
    }
}

void Mk2Perform::on_pad_up(int pad, const LiveState& s, CommandSink& sink) {
    const int p = pad - 1;
    PadHold& h = pad_[p];
    const int clip = pad_page_ * kPadsPerPage + p;
    switch (h.kind) {
        case kHoldClip:
        case kHoldHit:
            for (int l = 0; l < kLayersPerDeck; ++l)
                if (h.layer_mask & (1u << l)) {
                    send(make(CommandType::LayerTrigger, h.deck, static_cast<uint8_t>(l), clip, 0.0f, kRelease), sink);
                    if (h.press_sent && h.deck < kDecks && s.deck[h.deck].layer[l].fx[0].effect >= 0)
                        send(make_fx(CommandType::FxPress, h.deck, static_cast<uint8_t>(l), 0, kNone, 0, 0.0f), sink);
                }
            break;
        case kHoldSlice:
            if (h.loop_step) {
                for (int l = 0; l < kLayersPerDeck; ++l)
                    if (h.layer_mask & (1u << l)) send(make(CommandType::LayerBeatLoop, h.deck, static_cast<uint8_t>(l), 0, 0.0f), sink);
            }
            break;
        case kHoldFx:
            if (fx_pad_ == p && !fx_pad_latched_) {
                send(make_fx(CommandType::FxSet, kMasterDeck, kNone, kFxPadSlot, kNone, -1), sink);
                fx_pad_ = -1;
            } else if (h.press_sent) {
                send(make_fx(CommandType::FxPress, kMasterDeck, kNone, kFxPadSlot, kNone, 0, 0.0f), sink);
            }
            break;
        default: break;
    }
    h = PadHold{};
}

// Pressure goes out at most once per tick per pad, and only when it moved (pads stream at ~750 Hz).
void Mk2Perform::send_pressure(const LiveState& s, CommandSink& sink) {
    for (int p = 0; p < mk2::kPads; ++p) {
        if (!(pads_held_ & (1u << p))) continue;
        PadHold& h = pad_[p];
        if (std::abs(static_cast<int>(h.press) - static_cast<int>(h.press_sent)) < kPressDeadband) continue;
        const float x = std::clamp(h.press / 4095.0f, 0.0f, 1.0f);
        switch (h.kind) {
            case kHoldClip:  // PRESS: the layer's main effect (clips-and-playback.md §8.2)
                for (int l = 0; l < kLayersPerDeck; ++l)
                    if ((h.layer_mask & (1u << l)) && h.deck < kDecks && s.deck[h.deck].layer[l].fx[0].effect >= 0)
                        send(make_fx(CommandType::FxPress, h.deck, static_cast<uint8_t>(l), 0, kNone, 0, x), sink);
                break;
            case kHoldFx:
                if (fx_pad_ == p) send(make_fx(CommandType::FxPress, kMasterDeck, kNone, kFxPadSlot, kNone, 0, x), sink);
                break;
            case kHoldSlice: {  // harder = shorter beat loop: 1, 1/2, 1/4 ... 1/16 beat
                int step = 0;
                if (h.press > kSliceLoopFrom)
                    step = 1 + std::min(4, (h.press - kSliceLoopFrom) * 5 / (4096 - kSliceLoopFrom));
                if (step != h.loop_step) {
                    h.loop_step = static_cast<uint8_t>(step);
                    const float beats = step ? 1.0f / static_cast<float>(1 << (step - 1)) : 0.0f;
                    for (int l = 0; l < kLayersPerDeck; ++l)
                        if (h.layer_mask & (1u << l))
                            send(make(CommandType::LayerBeatLoop, h.deck, static_cast<uint8_t>(l), 0, beats), sink);
                }
                break;
            }
            default: break;
        }
        h.press_sent = h.press ? h.press : 1;
        if (!h.press) h.press_sent = 0;
    }
}

// ============================================================================ events
void Mk2Perform::on_event(const mk2::Event& e, const LiveState& s, const Catalog& cat, CommandSink& sink) {
    if (e.type == mk2::EventType::Connected || e.type == mk2::EventType::Disconnected) {
        reset();
        return;
    }
    sync(s, sink);
    last_event_us_ = e.t_us;

    switch (e.type) {
        case mk2::EventType::ButtonDown:
            if (e.id < 64) held_ |= 1ull << e.id;
            on_button(static_cast<Button>(e.id), s, cat, sink, e.t_us);
            break;
        case mk2::EventType::ButtonUp:
            if (e.id < 64) held_ &= ~(1ull << e.id);
            on_button_up(static_cast<Button>(e.id), s, cat, sink, e.t_us);
            break;
        case mk2::EventType::KnobDelta:
            if (e.id < mk2::kKnobs) {
                touched_knob_ = e.id;
                touched_us_ = e.t_us;
                on_knob(e.id, e.value, s, cat, sink);
            }
            break;
        case mk2::EventType::EncoderDelta: {
            if (browse_ && shift_ && !sys_) {  // BROWSE: SHIFT + wheel = a page (D200; the SYSTEM menu after BROWSE)
                browse_move(cat, e.value * kBrowsePage);
                break;
            }
            // ==== ENGINE (quit): SHIFT + wheel opens the SYSTEM menu; in it the wheel never crossfades ====
            if (shift_ && !sys_) {
                sys_ = true;
                quit_hold_us_ = 0;
                browse_ = false;
            }
            if (sys_) break;
            // ==== end ENGINE (quit) ====
            if (browse_) {  // BROWSE: the wheel scrolls the list; nothing is crossfaded (hardware-mapping §3.1)
                browse_move(cat, e.value);
                break;
            }
            if (held(Button::Tempo)) {  // TEMPO + wheel = BPM nudge
                tempo_turned_ = true;
                Command c = make(CommandType::Clock, kNone, kNone, 0, static_cast<float>(e.value) * kBpmPerDetent, kRelative);
                c.param = static_cast<uint8_t>(ClockParam::Bpm);
                send(c, sink);
                break;
            }
            if (held(Button::Select)) {  // SELECT + wheel = scratch the focused layer
                const uint8_t d = focus_deck(s);
                if (s.deck[d].layer[focus_layer_].clip >= 0)
                    send(make(CommandType::LayerScratch, d, focus_layer_, 1, static_cast<float>(e.value) * kScratchPerDetent, kRelative), sink);
                break;
            }
            // Endless T-bar: clockwise always blends PREVIEW in, whichever deck that is.
            const float step = static_cast<float>(e.value) / kWheelDetentsPerTake;
            send(make(CommandType::Crossfader, kNone, kNone, 0, program_of(s) == 0 ? step : -step, kRelative), sink);
            break;
        }
        case mk2::EventType::PadHit:
            if (e.id < 1 || e.id > mk2::kPads) break;
            pads_held_ = static_cast<uint16_t>(pads_held_ | (1u << (e.id - 1)));
            on_pad(e.id, e.value, s, cat, sink, e.t_us);
            break;
        case mk2::EventType::PadPressure:
            if (e.id >= 1 && e.id <= mk2::kPads && (pads_held_ & (1u << (e.id - 1))))
                pad_[e.id - 1].press = static_cast<uint16_t>(std::clamp<int>(e.value, 0, 4095));
            break;
        case mk2::EventType::PadRelease:
            if (e.id < 1 || e.id > mk2::kPads) break;
            if (pads_held_ & (1u << (e.id - 1))) on_pad_up(e.id, s, sink);
            pads_held_ = static_cast<uint16_t>(pads_held_ & ~(1u << (e.id - 1)));
            // SELECT let go after a scratch: let the playhead go.
            break;
        default:
            break;
    }
    if (e.type == mk2::EventType::ButtonUp && e.id == static_cast<uint8_t>(Button::Select)) {
        const uint8_t d = focus_deck(s);
        if (s.deck[d].layer[focus_layer_].clip >= 0 && transport_ && transport_->layer[d][focus_layer_].scratching)
            send(make(CommandType::LayerScratch, d, focus_layer_, 0, 0.0f, kRelative), sink);
    }
}

// ============================================================================ outputs
void Mk2Perform::tick(uint64_t now_us, const LiveState& s, const Catalog& cat, CommandSink& sink, mk2::LedState& leds,
                      uint8_t screens[2][mk2::kScreenBytes]) {
    sync(s, sink);
    send_pressure(s, sink);
    pad_page_ = std::min(pad_page_, pad_pages(cat) - 1);  // the catalog may have shrunk
    if (show_ && show_->message_seq != show_msg_seq_) {  // the show's own message ("3c saved", "Saved")
        const bool first = show_msg_seq_ == 0;
        show_msg_seq_ = show_->message_seq;
        if (!first && show_->message[0]) {
            char m[sizeof(show_->message)];
            std::memcpy(m, show_->message, sizeof(m));
            m[sizeof(m) - 1] = 0;
            for (char* c = m; *c; ++c)
                if (*c >= 'a' && *c <= 'z' && !(c > m && c[-1] >= '0' && c[-1] <= '9')) *c = static_cast<char>(*c - 32);
            say(m, now_us);
        }
    }
    const uint8_t prog = program_of(s), prev = preview_of(s);
    const uint8_t fdeck = focus_deck(s), flayer = focus_layer_;
    const LayerState& F = s.deck[fdeck].layer[flayer];
    const LayerTransport* FT = transport_ ? &transport_->layer[fdeck][flayer] : nullptr;
    // Beat phase 0..1 (for the red pulse of PROGRAM pads), from the engine's clock.
    float beat_frac = 0.0f;
    if (transport_ && transport_->clock.bpm > 0.0f) {
        const double dt = now_us >= transport_->clock.time_us ? (now_us - transport_->clock.time_us) / 1e6 : 0.0;
        const double beat = transport_->clock.beat + dt * transport_->clock.bpm / 60.0;
        beat_frac = static_cast<float>(beat - std::floor(beat));
    }
    const bool blink = (now_us / 250'000) & 1;

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
            rgb(out, kLedFull, kLedFull, kLedFull);
            continue;
        }
        switch (pad_mode_) {
            case PadMode::Clips: {
                const int clip = pad_page_ * kPadsPerPage + p;
                if (!loadable(cat, clip)) break;  // dark = empty (or removed)
                const uint32_t thumb = clip_at(cat, clip)->thumb_rgb;
                bool on_prog = false, on_prev = false;
                for (int l = 0; l < kLayersPerDeck; ++l) {
                    on_prog = on_prog || s.deck[prog].layer[l].clip == clip;
                    on_prev = on_prev || s.deck[prev].layer[l].clip == clip;
                }
                const bool mine = F.clip == clip;
                if (on_prog) {  // red, pulsing on the beat (hardware-mapping §3.4)
                    const int lvl = static_cast<int>((mine ? 127.0f : 96.0f) * (1.0f - 0.6f * beat_frac));
                    rgb(out, lvl, 0, 0);
                } else if (on_prev) {  // green-tinted bright
                    uint8_t c[3];
                    pad_colour(thumb, mine ? 127 : 72, c);
                    rgb(out, c[0] / 3, std::max<int>(c[1], mine ? 127 : 72), c[2] / 3);
                } else {
                    pad_colour(thumb, kPadInBank, out);
                }
                break;
            }
            case PadMode::Slice: {
                if (F.clip < 0) break;
                const bool set = FT && FT->cue[p] >= 0.0f;
                uint8_t c[3];
                pad_colour(clip_at(cat, F.clip) ? clip_at(cat, F.clip)->thumb_rgb : 0, set ? 100 : 20, c);
                rgb(out, c[0], c[1], c[2]);
                break;
            }
            case PadMode::Looks: {
                if (!show_ || !show_->pad[p].used) break;
                const uint8_t on = show_->pad[p].on_deck;
                if (on & (1u << prog)) rgb(out, static_cast<int>(127 * (1.0f - 0.6f * beat_frac)), 0, 0);
                else if (on & (1u << prev)) rgb(out, 10, 127, 10);
                else pad_colour(show_->pad[p].thumb_rgb, 24, out);
                break;
            }
            case PadMode::FxPads:
                if (!valid_effect(cat, p)) break;
                if (fx_pad_ == p) rgb(out, 127, 20, 127);
                else rgb(out, 18, 6, 24);
                break;
            case PadMode::Moods: {  // the 4 x 4 map: energy up the rows, style across
                const int row = p / 4, col = p % 4;
                rgb(out, 10 + row * 30, 8 + (3 - row) * 8, 10 + col * 25);
                break;
            }
            default: break;
        }
    }
    const uint32_t locks = show_ ? show_->locks : 0;
    for (int k = 0; k < 4; ++k)  // lit = free, dark = held
        set_button_led(leds, static_cast<Button>(static_cast<int>(Button::Top1) + k),
                       (locks & kTopLock[shift_ ? 1 : 0][k]) ? 0 : kLedFull);
    set_button_led(leds, Button::Top5, freeze_ ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Top6, strobe_ ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Top7, s.black ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Top8, assist_ ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Shift, shift_ ? kLedFull : 0);
    set_button_led(leds, Button::Play, s.take_active ? kLedFull : kLedDim);
    set_button_led(leds, Button::Restart, kLedDim);
    set_button_led(leds, Button::PageLeft, page_ != Page::Play ? kLedFull : 0);
    set_button_led(leds, Button::PageRight, static_cast<int>(page_) < static_cast<int>(Page::Count) - 1 ? kLedFull : 0);
    const bool looks = pad_mode_ == PadMode::Looks;
    set_button_led(leds, Button::MasterLeft, looks || pad_page_ > 0 ? kLedFull : 0);
    set_button_led(leds, Button::MasterRight, looks || pad_page_ < pages - 1 ? kLedFull : 0);
    set_button_led(leds, Button::TransportLeft, kLedDim);
    set_button_led(leds, Button::TransportRight, kLedDim);
    set_button_led(leds, Button::Pattern, pad_mode_ == PadMode::Clips ? kLedFull : pad_mode_ == PadMode::Slice ? (blink ? kLedFull : kLedFaint) : 0);
    set_button_led(leds, Button::Scene, looks ? kLedFull : pad_mode_ == PadMode::Moods ? (blink ? kLedFull : kLedFaint) : 0);
    set_button_led(leds, Button::PadMode, pad_mode_ == PadMode::FxPads ? kLedFull : 0);
    set_button_led(leds, Button::Rec, held(Button::Rec) ? kLedFull : kLedDim);
    set_button_led(leds, Button::Erase, held(Button::Erase) ? kLedFull : show_ && show_->undo_depth ? kLedDim : kLedFaint);
    set_button_led(leds, Button::Enter, show_ && show_->scene_count ? kLedDim : 0);
    set_button_led(leds, Button::Grid, transport_ && transport_->clock.grid ? kLedFull : kLedFaint);
    set_button_led(leds, Button::NoteRepeat, repeat_ ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Mute, F.mute ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Solo, held(Button::Solo) ? kLedFull : kLedFaint);
    set_button_led(leds, Button::All, held(Button::All) ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Select, held(Button::Select) ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Tempo, held(Button::Tempo) ? kLedFull : kLedFaint);
    set_button_led(leds, Button::Browse, browse_ ? kLedFull : kLedFaint);
    // ==== ENGINE (quit): the QUIT hold; held 2 s -> one confirmed SystemQuit ====
    if (sys_) {
        if (quit_hold_us_ && !held(Button::EncoderPush)) quit_hold_us_ = 0;  // missed the release
        if (quit_hold_us_ && !quit_sent_ && now_us >= quit_hold_us_ + kQuitHoldUs) {
            send(make(CommandType::SystemQuit, kNone, kNone, 0, 1.0f), sink);
            quit_sent_ = true;
        }
        draw_system(now_us, screens);
        return;
    }
    // ==== end ENGINE (quit) ====
    if (browse_) {
        draw_browse(now_us, s, cat, screens);
        return;
    }

    // ---- screens: left = PREVIEW deck, right = PROGRAM deck ----
    ui::Screen c;
    c.clear();
    const char deck_letter[2] = {'A', 'B'};
    {
        Text l, r;
        l.add("PREVIEW ").add(deck_letter[prev]).add(' ');
        scene_tag(l, show_, prev);
        if (shift_) r.add("SHIFT");
        else {
            r.add(kModeName[static_cast<int>(pad_mode_)]);
            if (pad_mode_ == PadMode::Clips) r.add(' ').num(pad_page_ + 1).add('/').num(pages);
            if (looks && show_) {
                if (show_->use_setlist) r.add(" SONG ").num(show_->song + 1);
                else r.add(" BANK ").num(show_->bank + 1);
            }
        }
        header(c, 0, l.view(), r.view(), focus_side_ == kPrep);
    }
    {
        Text l, r;
        l.add("PROGRAM ").add(deck_letter[prog]).add(' ');
        scene_tag(l, show_, prog);
        if (s.black) r.add("BLACK");
        else if (freeze_) r.add("FREEZE");
        else if (s.take_active) r.add("TAKE");
        else if (show_ && show_->deck[prog].morphing) r.add("MORPH");
        else r.add("ON AIR");
        header(c, 1, l.view(), r.view(), focus_side_ == kPlay);
    }
    for (int side = 0; side < 2; ++side) {
        const uint8_t d = side == 0 ? prev : prog;
        for (int l = 0; l < kLayersPerDeck; ++l)
            layer_cell(c, side, l, s.deck[d].layer[l], clip_label(clip_at(cat, s.deck[d].layer[l].clip)),
                       (side == 0 ? kPrep : kPlay) == focus_side_ && l == flayer);
    }
    {  // left row 3: a flash message, else the page and the focused layer
        Text t;
        if (flash_[0] && now_us < flash_until_us_) {
            t.add(flash_);
        } else {
            t.add(kPageName[static_cast<int>(page_)]).add(' ').add(focus_side_ == kPlay ? "LIVE " : "").add('L').num(flayer + 1).add(' ');
            if (valid_effect(cat, F.fx[0].effect)) t.add(effect_name(cat, F.fx[0].effect)).add(F.fx[0].bypass ? " (BYP)" : "");
            else t.add("NO FX");
        }
        ui::text_in(c, 2, mk2::kScreenW - 2, kRow3Y, t.view());
        if (flash_[0] && now_us < flash_until_us_) c.invert(0, kRow3Y - 1, mk2::kScreenW, 9);
    }
    {  // right row 3: TAKE bar + % (how much PREVIEW is blended in) + BPM
        const float xf = std::clamp(s.take_active ? (prog == 0 ? s.take_progress : 1.0f - s.take_progress) : s.crossfader,
                                    0.0f, 1.0f);
        const float into_preview = prog == 0 ? xf : 1.0f - xf;
        const int x0 = mk2::kScreenW;
        ui::text_in(c, x0 + 2, x0 + 30, kRow3Y, "TAKE");
        bar(c, x0 + 30, kRow3Y, 130, 7, into_preview);
        Text p;
        p.pct(into_preview);
        ui::text_in(c, x0 + 164, x0 + 190, kRow3Y, p.view());
        Text b;
        if (transport_ && transport_->clock.bpm > 0.0f) b.num(static_cast<int>(std::lround(transport_->clock.bpm))).add(" BPM");
        ui::text_in(c, x0 + 192, x0 + mk2::kScreenW - 2, kRow3Y, b.view(), ui::Align::Right);
        if (transport_ && transport_->clock.bpm > 0.0f && beat_frac < 0.15f) c.fill(x0 + 190, kRow3Y + 2, 3, 3);  // beat dot
    }
    // Knobs 1-4: the page on the focus layer.
    KnobView kv[mk2::kKnobs];
    switch (page_) {
        case Page::Play:
            knob_text(kv[0], "SCROLL", F.clip >= 0 ? clip_label(clip_at(cat, F.clip)) : std::string_view("--"));
            {
                Text v;
                if (show_) v.num(show_->deck[fdeck].dice_pos).add('/').num(show_->deck[fdeck].dice_count);
                knob_text(kv[1], held(Button::All) ? "DICE ALL" : "DICE", v.view());
            }
            if (valid_effect(cat, F.fx[0].effect)) knob_pct(kv[2], "FX AMT", F.fx[0].mix);
            kv[3].used = true;
            kv[3].label.add("SPEED");
            kv[3].value.fixed(F.speed, 2).add('x');
            kv[3].norm = (F.speed + 4.0f) / 8.0f;
            kv[3].bipolar = true;
            break;
        case Page::Clip:
            if (F.clip >= 0 && FT) {
                knob_pct(kv[0], "IN", FT->in);
                knob_pct(kv[1], "OUT", FT->out);
                knob_text(kv[2], "MODE", kPlayModeName[std::min<int>(FT->mode, 8)]);
            }
            kv[3].used = true;
            kv[3].label.add("SPEED");
            kv[3].value.fixed(F.speed, 2).add('x');
            kv[3].norm = (F.speed + 4.0f) / 8.0f;
            kv[3].bipolar = true;
            break;
        case Page::Layer:
            knob_pct(kv[0], "OPACITY", F.opacity);
            knob_text(kv[1], "BLEND", kBlendName[std::min<int>(F.blend, 5)]);
            if (FT) {
                kv[2].used = kv[3].used = true;
                kv[2].label.add("FADE IN");
                kv[2].value.fixed(FT->fade_in, 1).add('s');
                kv[2].norm = FT->fade_in / 10.0f;
                kv[3].label.add("FADE OUT");
                kv[3].value.fixed(FT->fade_out, 1).add('s');
                kv[3].norm = FT->fade_out / 10.0f;
            }
            break;
        case Page::Fx:
            knob_text(kv[0], "EFFECT", valid_effect(cat, F.fx[0].effect) ? effect_name(cat, F.fx[0].effect) : std::string_view("NONE"));
            fx_param_knob(kv[1], F.fx[0], 0, cat);
            fx_param_knob(kv[2], F.fx[0], 1, cat);
            if (valid_effect(cat, F.fx[0].effect)) knob_pct(kv[3], "MIX", F.fx[0].mix);
            break;
        default: {
            Text v;
            if (take_ms_) v.fixed(take_ms_ / 1000.0f, 1).add('s');
            else v.add("SHOW");
            knob_text(kv[0], "TAKE", v.view());
            Text b;
            if (transport_) b.fixed(transport_->clock.bpm, 1);
            knob_text(kv[1], "BPM", b.view());
            knob_text(kv[2], "GRID", kGridName[transport_ ? std::min<int>(transport_->clock.grid, 5) : 0]);
            if (valid_effect(cat, s.master_fx[kMasterFxSlot].effect)) knob_pct(kv[3], "MFX MIX", s.master_fx[kMasterFxSlot].mix);
            break;
        }
    }
    // Knobs 5-8: the PROGRAM scene's macros (names and values from the show).
    static constexpr const char* kMacroDefault[4] = {"INTENSITY", "COLOUR", "SPACE", "MASTER FX"};
    for (int m = 0; m < 4; ++m) {
        KnobView& v = kv[4 + m];
        v.used = true;
        std::string_view name = show_ ? cstr(show_->macro_name[m], sizeof(show_->macro_name[m])) : std::string_view{};
        v.label.add(name.empty() ? std::string_view(kMacroDefault[m]) : name);
        const float x = show_ ? show_->macro[m] : 0.0f;
        v.value.pct(x);
        v.norm = x;
    }
    if (shift_) {  // where the master effect is chosen
        kv[7] = KnobView{};
        knob_text(kv[7], "CHOOSE", valid_effect(cat, s.master_fx[kMasterFxSlot].effect)
                                       ? effect_name(cat, s.master_fx[kMasterFxSlot].effect)
                                       : std::string_view("NONE"));
    }
    const bool recent = touched_knob_ >= 0 && now_us >= touched_us_ && now_us - touched_us_ < kTouchedUs;
    for (int k = 0; k < mk2::kKnobs; ++k) knob_cell(c, k, kv[k], recent && k == touched_knob_);
    c.split(screens[0], screens[1]);
}

// ============================================================================ BROWSE
namespace {
constexpr const char* kSortName[] = {"ADDED", "NAME", "NEWEST", "ENERGY"};
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
int name_cmp(const ClipInfo& a, const ClipInfo& b) {
    for (int i = 0; i < kNameLen; ++i) {
        const char x = lower(a.name[i]), y = lower(b.name[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
// Energy 1..5 from the library analysis, 0 = unknown (ClipInfo.reserved[0], D201).
int energy_of(const ClipInfo& ci) { return ci.reserved[0] <= 5 ? ci.reserved[0] : 0; }
}  // namespace

// Rebuilds order_ when the clips or the order changed. Removed clips are left out. No heap:
// std::sort on the fixed array. The signature sums the entries' generations, so a state change
// (Ready -> Removed) or a new clip rebuilds it; otherwise this is one pass over the counts.
void Mk2Perform::refresh_order(const Catalog& cat) {
    const int n = nclips(cat);
    uint64_t gens = 0;
    if (clips_)
        for (int i = 0; i < n; ++i) gens += clips_->entry(i).gen + (static_cast<uint64_t>(clips_->entry(i).state) << 32);
    const uint64_t sig = (static_cast<uint64_t>(n) | (static_cast<uint64_t>(browse_sort_) << 14)) ^ (gens * 0x9E3779B97F4A7C15ull);
    if (sig == order_sig_) return;
    order_sig_ = sig;
    order_n_ = 0;
    for (int i = 0; i < n; ++i)
        if (loadable(cat, i)) order_[order_n_++] = static_cast<uint16_t>(i);
    uint16_t* first = order_;
    uint16_t* last = order_ + order_n_;
    switch (browse_sort_) {
        case BrowseSort::Added: break;  // already in id order
        case BrowseSort::Newest: std::reverse(first, last); break;
        case BrowseSort::Name:
            std::sort(first, last, [&](uint16_t a, uint16_t b) {
                const int c = name_cmp(*clip_at(cat, a), *clip_at(cat, b));
                return c ? c < 0 : a < b;
            });
            break;
        case BrowseSort::Energy:
            std::sort(first, last, [&](uint16_t a, uint16_t b) {
                const int ea = energy_of(*clip_at(cat, a)), eb = energy_of(*clip_at(cat, b));
                if (ea != eb) return ea > eb;  // the hottest first, unknown last
                const int c = name_cmp(*clip_at(cat, a), *clip_at(cat, b));
                return c ? c < 0 : a < b;
            });
            break;
        default: break;
    }
    browse_select(cat, browse_sel_);  // keep the selected clip (its row may have moved)
}

void Mk2Perform::browse_select(const Catalog&, int clip_id) {
    for (int r = 0; r < order_n_; ++r)
        if (order_[r] == clip_id) {
            browse_pos_ = r;
            browse_sel_ = clip_id;
            return;
        }
    browse_pos_ = std::clamp(browse_pos_, 0, std::max(0, order_n_ - 1));
    browse_sel_ = order_n_ ? order_[browse_pos_] : -1;
}

void Mk2Perform::browse_move(const Catalog& cat, int rows) {
    refresh_order(cat);
    browse_pos_ = std::clamp(browse_pos_ + rows, 0, std::max(0, order_n_ - 1));
    browse_sel_ = order_n_ ? order_[browse_pos_] : -1;
}

void Mk2Perform::browse_load(const LiveState& s, const Catalog& cat, CommandSink& sink, uint64_t now_us) {
    if (!loadable(cat, browse_sel_)) return;
    const uint8_t d = focus_deck(s);
    load_clip(d, focus_layer_, browse_sel_, sink);
    Text t;
    t.add("LOADED > ").add(static_cast<char>('A' + d)).num(focus_layer_ + 1).add(focus_side_ == kPlay ? " LIVE" : "");
    say(t.s, now_us);
}

// Left: the list (selected row inverted) in the chosen order. Right: the selected clip (name,
// size, fps, length, codec, energy or state, a dithered swatch of its colour) and where it lands.
void Mk2Perform::draw_browse(uint64_t now_us, const LiveState& s, const Catalog& cat, uint8_t screens[2][mk2::kScreenBytes]) {
    refresh_order(cat);
    const int count = order_n_;
    ui::Screen c;
    c.clear();
    ui::ListModel list;
    {
        Text h;
        h.add("LIBRARY ").add(kSortName[static_cast<int>(browse_sort_)]);
        list.head.left.set(h.view());
        Text r;
        if (count) r.num(browse_pos_ + 1).add('/').num(count);
        else r.add("EMPTY");
        list.head.right.set(r.view());
    }
    // A window of up to kMax rows around the selection (the list may hold 8192).
    const int first = std::clamp(browse_pos_ - ui::ListModel::kMax / 2, 0, std::max(0, count - ui::ListModel::kMax));
    list.count = std::min(count - first, ui::ListModel::kMax);
    list.selected = browse_pos_ - first;
    for (int i = 0; i < list.count; ++i) {
        const int id = order_[first + i];
        list.items[static_cast<size_t>(i)].set(clip_label(clip_at(cat, id)));
        bool on_prog = false, on_prev = false;
        for (int l = 0; l < kLayersPerDeck; ++l) {
            on_prog = on_prog || s.deck[program_of(s)].layer[l].clip == id;
            on_prev = on_prev || s.deck[preview_of(s)].layer[l].clip == id;
        }
        const ClipState st = clip_state(id);
        list.tags[static_cast<size_t>(i)].set(on_prog ? "LIVE" : on_prev ? "PREV" : st == ClipState::Missing ? "MISSING"
                                              : st == ClipState::Busy ? "BUSY" : "");
    }
    if (count <= 0) list.count = 0;
    ui::draw_list(c, 0, list);

    ui::DetailModel det;
    const uint8_t d = focus_deck(s);
    {
        Text h;
        h.add("LOAD > ").add(static_cast<char>('A' + d)).num(focus_layer_ + 1).add(focus_side_ == kPlay ? " PROGRAM" : " PREVIEW");
        det.head.left.set(h.view());
        det.head.right.set("PUSH");
    }
    if (const ClipInfo* ci = loadable(cat, browse_sel_) ? clip_at(cat, browse_sel_) : nullptr) {
        det.lines[0].set(clip_label(ci));
        Text a;
        a.num(static_cast<int>(ci->width)).add('x').num(static_cast<int>(ci->height));
        det.lines[1].set(a.view());
        Text b;
        b.fixed(ci->fps, 1).add(" FPS  ");
        const int secs = static_cast<int>(std::lround(ci->seconds));
        if (secs >= 60) b.num(secs / 60).add(':').add(static_cast<char>('0' + secs % 60 / 10)).add(static_cast<char>('0' + secs % 10));
        else b.fixed(ci->seconds, 1).add('S');
        det.lines[2].set(b.view());
        Text e;
        static constexpr const char* kCodec[] = {"HAP", "HAP ALPHA", "HAP Q", "OTHER"};
        e.add(kCodec[std::min<int>(ci->codec, 3)]);
        const ClipState st = clip_state(browse_sel_);
        if (st == ClipState::Missing) e.add(" MISSING");
        else if (st == ClipState::Busy) e.add(" BUSY");
        else if (energy_of(*ci)) e.add(" ENERGY ").num(energy_of(*ci));
        det.lines[3].set(e.view());
        // The swatch: the clip's average colour as a grey level (thumb_rgb), dithered on the screen.
        const uint32_t rgb = ci->thumb_rgb;
        const int luma = static_cast<int>((((rgb >> 16) & 0xFF) * 77 + ((rgb >> 8) & 0xFF) * 150 + (rgb & 0xFF) * 29) >> 8);
        for (int y = 0; y < 27; ++y)
            for (int x = 0; x < 48; ++x) {
                const bool edge = x == 0 || y == 0 || x == 47 || y == 26;
                thumb_[y * 48 + x] = static_cast<uint8_t>(edge ? 255 : std::clamp(luma, 24, 232));
            }
        det.thumb = thumb_;
        det.thumb_w = 48;
        det.thumb_h = 27;
    }
    if (catalog_full_) {  // ready library clips the catalog cannot hold (library-db.md §6)
        Text f;
        f.add("FULL: +").num(static_cast<int>(catalog_full_)).add(" IN LIBRARY");
        det.lines[4].set(f.view());
    } else if (flash_[0] && now_us < flash_until_us_) {
        det.lines[4].set(std::string_view(flash_));
    } else if (count > kBrowsePage) {
        Text p;
        p.add("PAGE ").num(browse_pos_ / kBrowsePage + 1).add('/').num((count + kBrowsePage - 1) / kBrowsePage).add(" SHIFT");
        det.lines[4].set(p.view());
    }
    ui::draw_detail(c, 1, det);
    c.split(screens[0], screens[1]);
}

// ==== ENGINE (quit): left = the SYSTEM menu, right = what the push does (the countdown while held).
void Mk2Perform::draw_system(uint64_t now_us, uint8_t screens[2][mk2::kScreenBytes]) {
    ui::Screen c;
    c.clear();
    ui::ListModel list;
    list.head.left.set("SYSTEM");
    list.head.right.set("KEY = BACK");
    list.count = 1;
    list.selected = 0;
    list.items[0].set("QUIT HIMMEL");
    list.tags[0].set("HOLD");
    ui::draw_list(c, 0, list);
    if (quit_sent_) {
        ui::draw_message(c, 1, "QUIT HIMMEL", "BYE", "STOPPING EVERYTHING");
    } else if (quit_hold_us_) {
        const uint64_t held_us = now_us > quit_hold_us_ ? now_us - quit_hold_us_ : 0;
        const uint64_t left_us = held_us >= kQuitHoldUs ? 0 : kQuitHoldUs - held_us;
        Text big;
        big.fixed(static_cast<float>(left_us) / 1e6f, 1).add(" S");
        ui::draw_message(c, 1, "QUIT HIMMEL", big.view(), "KEEP HOLDING - LET GO = NO");
        const int bar_w = mk2::kScreenW - 16;  // the hold as a filling bar over a thin rail
        c.fill(mk2::kScreenW + 8, 47, bar_w, 1);
        c.fill(mk2::kScreenW + 8, 45, static_cast<int>(bar_w * static_cast<double>(held_us) / kQuitHoldUs), 5);
    } else {
        ui::draw_message(c, 1, "QUIT HIMMEL", "HOLD PUSH", "PUSH THE WHEEL FOR 2 S");
    }
    c.split(screens[0], screens[1]);
}
// ==== end ENGINE (quit) ====

void Mk2Perform::reset() {
    sys_ = false;  // ENGINE (quit): a reconnect closes the menu and drops a QUIT hold
    quit_hold_us_ = 0;
    shift_ = false;
    held_ = 0;
    pads_held_ = 0;
    for (auto& h : pad_) h = PadHold{};
    touched_knob_ = -1;
    for (int& a : knob_acc_) a = 0;
    repeat_ = false;
    tempo_turned_ = false;
    synced_ = false;  // re-read the engine's focus and re-announce ours
}

}  // namespace himmel::perform
