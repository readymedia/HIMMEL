// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Effect metadata (docs/interfaces/effect-metadata.md) and its resolution:
// what an FX slot's knobs + scope + PRESS + reaction + the show's flash cap
// turn into, every frame, as the 4 values the shader gets.
//
// Pure: no GPU, no Windows. The Library is filled once at start-up (it parses
// the JSON embedded from fx/); after that nothing here allocates, locks or
// does I/O, so resolve() is safe in the render loop.
#pragma once

#include <cstdint>
#include <string>

#include "himmel/protocol/state.h"

namespace himmel::fx {

inline constexpr int kParams = 4;          // knobs 1-4 of an FX page; the shader gets float4
inline constexpr int kMaxPresets = 8;
inline constexpr int kMaxFlashSteps = 8;
inline constexpr int kIdLen = 24;
inline constexpr int kHelpLen = 96;
inline constexpr int kMaxEffects = protocol::kMaxEffects;

enum class Kind : uint8_t { Effect = 0, Generator = 1 };
enum class State : uint8_t { None = 0, Feedback = 1, History = 2 };

// The signals a reaction can listen to (music.md §4). Index into Signals::v.
enum class Signal : uint8_t {
    None = 0, Level, Bass, Mid, High, Kick, Snare, Hat, Onset, Beat, Bar, Energy, Build, Drop, Count
};
inline constexpr int kSignals = static_cast<int>(Signal::Count);

enum class ReactMode : uint8_t { Add = 0, Follow = 1, Trigger = 2 };
enum class Curve : uint8_t { Linear = 0, Soft = 1, Hard = 2, Steps = 3, Exp = 4 };

// Parameter scopes (clips-and-playback.md §4.2), as bits in ParamMeta::scopes.
enum class Scope : uint8_t { Slot = 0, Look = 1, Global = 2, Move = 3 };
inline constexpr uint8_t scope_bit(Scope s) { return static_cast<uint8_t>(1u << static_cast<int>(s)); }
inline constexpr uint8_t kAllScopes = 0x0F;

// Levels an effect may sit on (clips-and-playback.md §4.1), bits in EffectMeta::levels.
enum class Level : uint8_t { Clip = 0, Layer = 1, Deck = 2, Master = 3, Output = 4 };
inline constexpr uint8_t level_bit(Level l) { return static_cast<uint8_t>(1u << static_cast<int>(l)); }
inline constexpr uint8_t kAllLevels = 0x1F;

struct ParamMeta {
    char id[kIdLen];
    char name[protocol::kParamNameLen];
    char unit[16];
    float min = 0, max = 1;      // display range (the engine works in 0..1)
    float def = 0;               // knob position 0..1
    float good_lo = 0, good_hi = 1;
    uint8_t steps = 0;           // 0 = continuous, n = n stepped positions
    uint8_t scopes = kAllScopes; // allowed scopes
    bool speed = false;          // a speed: can lock to the clock (knob 7 SYNC)
    bool exp_curve = false;      // display mapping min..max is exponential
};

struct Reaction {
    Signal signal = Signal::None;
    int8_t param = -1;
    float amount = 0;            // -1..1
    float attack_ms = 10, release_ms = 200;
    float threshold = 0;         // gate 0..1
    float range_lo = 0, range_hi = 1;  // default: the param's good range
    ReactMode mode = ReactMode::Add;
    Curve curve = Curve::Linear;
    float quantise_beats = 0;    // TRIGGER: 0 = off, else fire only on this grid
    float hold_beats = 0;        // TRIGGER: keep it up this long
};

struct Press {
    int8_t param = -1;
    float lo = 0, hi = 1;        // knob positions 0..1
    Curve curve = Curve::Linear;
    float release_beats = 0.5f;
};

struct Preset {
    char name[16];
    float v[kParams];
};

// How the optional per-show flash cap (D65) reaches this effect.
enum class FlashMode : uint8_t { None = 0, Rate = 1, Level = 2 };
struct Flash {
    FlashMode mode = FlashMode::None;
    int8_t param = -1;           // Rate: the stepped rate knob; Level: the brightness / amount knob
    int8_t duty = -1;            // Rate: the duty knob (0..1 -> 5..95 %), -1 = 50 %
    uint8_t steps = 0;
    float beats[kMaxFlashSteps] = {};  // Rate: beats per flash for each knob step
};

struct EffectMeta {
    char id[kIdLen];
    char name[protocol::kNameLen];
    char family[16];
    Kind kind = Kind::Effect;
    uint8_t tier = 1;
    bool builtin = true;
    int params = 0;
    ParamMeta param[kParams];
    int8_t main[2] = {0, 1};
    Reaction reaction;
    Press press;
    int presets = 0;
    Preset preset[kMaxPresets];
    char cost_class = 'S';
    int passes = 1;
    State state = State::None;
    Flash flash;
    uint8_t levels = kAllLevels;
    char help[2][kHelpLen];

    int find_param(const char* pid) const;
    int find_preset(const char* pname) const;
};

// ---- the library -------------------------------------------------------------

class Library {
public:
    // Parses the effects embedded from fx/ in catalog order. Start-up only (allocates).
    // Returns false (and the reason) if any effect is malformed or breaks a rule.
    bool load_builtin(std::string* error = nullptr);

    // Parses one effect.json into `out` and checks it (validate()). Start-up / tests only.
    static bool parse(const char* json, EffectMeta& out, std::string* error);
    // The rules every effect must follow (effect-metadata.md §3), e.g. a reaction.
    static bool validate(const EffectMeta& m, std::string* error);

    int count() const { return count_; }
    const EffectMeta& meta(int id) const { return meta_[(id >= 0 && id < count_) ? id : 0]; }
    int find(const char* id) const;
    // HLSL body of effect `id` (fx/<id>/<id>.hlsl), and the shared prelude / entry.
    const char* hlsl(int id) const { return (id >= 0 && id < count_) ? hlsl_[id] : ""; }
    static const char* common_hlsl();
    static const char* main_hlsl();

    // The engine catalog (Local\HimmelState): name, knob names, defaults.
    void fill_catalog(protocol::Catalog& catalog) const;

private:
    EffectMeta meta_[kMaxEffects];
    const char* hlsl_[kMaxEffects] = {};
    int count_ = 0;
};

// The built-in library, loaded on first use (thread-safe static; start-up only).
const Library& builtin();

// ---- resolution (per frame, per active slot) ------------------------------------

struct Signals {
    float v[kSignals] = {};      // 0..1 each; Signal::None stays 0
    float operator[](Signal s) const { return v[static_cast<int>(s)]; }
};

struct FrameCtx {
    double time_s = 0;           // engine clock
    double beats = 0;            // show clock in beats
    float bpm = 120;
    float dt = 1.0f / 60;        // seconds since the last frame (clamped by the engine)
    float flash_cap_hz = 0;      // D65: 0 = off (default); else max flashes per second
    Signals sig;
};

// Per-slot runtime (the engine keeps one per FX slot; plain data, no heap).
struct SlotRuntime {
    int16_t effect = -1;         // the effect this state belongs to (reset when it changes)
    uint8_t scope[kParams] = {}; // Scope per param (default SLOT)
    float look[kParams] = {};    // LOOK values (M3 scenes fill them; until then = the slot value)
    bool look_set[kParams] = {};
    float press_env = 0;         // PRESS after velocity / release shaping
    // One reaction per knob (D131): each has its own envelope and TRIGGER state.
    float react_env[kParams] = {};             // the reaction's envelope
    float prev_sig[kParams] = {};              // for TRIGGER edges
    double trigger_until_beat[kParams] = {-1, -1, -1, -1};
    double last_trigger_beat[kParams] = {-1e9, -1e9, -1e9, -1e9};
    float out[kParams] = {};                   // the last resolved values (state block "knob ghosts")
    uint64_t frame = 0;                        // FxGpu frame of the last resolve (0 = never)
    // flash cap
    bool flash_on = false;
    double last_flash_s = -1e9;
    float level_prev = 0;
    bool level_high = false;
};

// GLOBAL scope: one value per effect and param, shared by every instance.
struct Globals {
    float v[kMaxEffects][kParams] = {};
    bool set[kMaxEffects][kParams] = {};
};

struct Resolved {
    float p[kParams] = {};       // what the shader gets in P, 0..1
    float mix = 1;               // the slot's dry / wet (0 = skip the pass)
    float gate = 0;              // flash gate (X.x) for FlashMode::Rate
    float press = 0;             // PRESS envelope (X.y)
};

// Knob step: 0..1 -> 0..n-1 (the same maths as step_of() in fx/_common.hlsl).
int step_index(float p, int n);
float step_center(int i, int n);

// Turns one slot into shader values. Updates `rt` (envelopes, flash limiter).
// PRESS comes from FxSlotState::press. `reacts`: the slot's reaction per knob
// (kParams entries, D131; signal None = no reaction on that knob, `param` is
// ignored), or nullptr = only the effect's default reaction (m.reaction).
Resolved resolve(const EffectMeta& m, int effect_id, const protocol::FxSlotState& slot, SlotRuntime& rt,
                 const Globals& globals, const FrameCtx& ctx, const Reaction* reacts = nullptr);

// D131: the state block's reaction table (protocol::ReactionEntry, numbered like
// Signal / ReactMode / Curve) -> the resolver's per-knob reactions. An Off entry,
// a bad enum or a non-finite number gives "no reaction" on that knob.
void reactions_from(const protocol::SlotReactions& in, Reaction out[kParams]);
// The table entry an effect starts with on knob `p` (ReactSource::Default): its
// default reaction when it targets `p`, else signal None; the range is the
// reaction's range, else the param's good range.
void default_entry(const EffectMeta& m, int p, protocol::ReactionEntry& out);

// Flashes per second the strobe makes at knob `rate01` and `bpm` (no cap).
float strobe_flash_hz(const EffectMeta& m, float rate01, float bpm);

// Names for JSON / logs.
const char* signal_name(Signal s);
bool signal_from_name(const char* name, Signal& out);

}  // namespace himmel::fx
