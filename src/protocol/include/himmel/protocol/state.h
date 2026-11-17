// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The engine state block: engine -> GUI / weapons / tools (docs/interfaces/engine-state.md).
//
// Shared block "Local\HimmelState". Written only by the render process, read by
// anyone. Two seqlock sections:
//   * live     - the whole show model as it is on air now, written at most once
//                per frame (only when something changed, plus every 250 ms);
//   * catalog  - clips and effects the engine can play, written when it changes.
//   * transport (v2) and react (v3) - see below.
// Readers copy a section with seq_read and use the copy; they never hold it.
#pragma once

#include <cstdint>

#include "himmel/protocol/commands.h"

namespace himmel::protocol {

inline constexpr const wchar_t* kStateShmName = L"Local\\HimmelState";
inline constexpr uint32_t kStateMagic = 0x54534D48;  // "HMST"
inline constexpr uint32_t kStateVersion = 3;  // 2: + transport section (M2); 3: + reaction table (AUDIO, D131)

inline constexpr int kMaxClips = 256;
inline constexpr int kMaxEffects = 64;
inline constexpr int kNameLen = 48;        // UTF-8, NUL-terminated
inline constexpr int kParamNameLen = 16;

// ---- live section ----------------------------------------------------------
struct FxSlotState {
    int16_t effect;            // effect id, -1 = empty
    uint8_t bypass;            // 1 = bypassed
    uint8_t press;             // PRESS 0..255 (FxPress; 0 = released). The engine shapes the release (effect-metadata.md §5)
    float mix;                 // 0..1 dry/wet
    float param[kFxParams];    // 0..1, normalised; meaning from the catalog
};

struct LayerState {
    int16_t clip;              // clip id, -1 = empty
    uint8_t blend;             // BlendMode
    uint8_t mute;
    uint8_t playing;           // 1 = playing, 0 = paused
    uint8_t missing;           // 1 = the clip failed to open (placeholder shown, build-v1 §4.4)
    uint8_t reserved[2];
    float opacity;             // 0..1
    float speed;               // -4..4
    float position;            // 0..1 of the clip
    FxSlotState fx[kFxSlots];
};

struct DeckState {
    LayerState layer[kLayersPerDeck];
    FxSlotState fx[kFxSlots];  // deck level
};

struct LiveState {
    uint64_t frame;            // engine frame counter
    uint64_t time_us;          // engine steady clock at this write
    uint32_t epoch;            // changes every time the render process starts (GUI: resync)
    float fps;                 // measured over the last second
    float crossfader;          // 0 = A .. 1 = B
    uint8_t program_deck;      // the deck on air (0 = A, 1 = B); the other one is PREVIEW
    uint8_t take_active;       // 1 while a TAKE is travelling
    uint8_t black;             // 1 = master black
    uint8_t focus_deck;        // focused layer (Focus command; kNone = none)
    uint8_t focus_layer;
    uint8_t reserved[3];
    float take_progress;       // 0..1 while take_active
    uint32_t take_ms;          // duration of the running (or default) TAKE
    DeckState deck[kDecks];
    FxSlotState master_fx[kFxSlots];
    // Per producer: last command seq applied, and refusals (docs: command-protocol.md §5).
    uint32_t acked_seq[kProducers];
    uint32_t rejected[kProducers];
    uint8_t last_reject[kProducers];   // Reject
    uint8_t producer_alive[kProducers]; // 1 = heartbeat seen in the last 2 s
};

// ---- catalog section -------------------------------------------------------
struct ClipInfo {
    char name[kNameLen];       // file name without folder
    uint32_t width, height;
    float fps;
    float seconds;
    uint8_t codec;             // 0 = Hap, 1 = Hap Alpha, 2 = Hap Q, 3 = other / unknown
    uint8_t reserved[3];
    uint32_t thumb_rgb;        // 0xRRGGBB average colour (placeholder colour, build-v1 §4.4)
};

struct EffectInfo {
    char name[kNameLen];
    uint8_t param_count;       // 0..kFxParams
    uint8_t reserved[3];       // [0] = kind: 0 effect, 1 generator (effect-metadata.md)
    char param_name[kFxParams][kParamNameLen];
    float param_default[kFxParams];
};

struct Catalog {
    uint32_t clip_count;
    uint32_t effect_count;
    ClipInfo clip[kMaxClips];
    EffectInfo effect[kMaxEffects];
};

// ---- transport section (M2, design/clips-and-playback.md §2-3) --------------
enum class PlayheadPhase : uint8_t {
    Empty = 0,      // no clip
    Playing = 1,
    Paused = 2,
    Holding = 3,    // ONCE · HOLD reached the end
    Intro = 4,      // inner loop on: in -> loop-out, then the inner loop
    InnerLoop = 5,  // looping between loop-in and loop-out
    Outro = 6,      // released: loop-out -> out
    Count
};

struct LayerTransport {
    // Settings (LayerSet; LayerParam names the field).
    uint8_t mode;              // PlayMode
    uint8_t time_base;         // TimeBase
    uint8_t trigger;           // TriggerStyle
    uint8_t start;             // StartPos
    uint8_t target;            // ClipTarget
    uint8_t autopilot;         // AutopilotMode
    uint8_t transition;        // TransitionType
    uint8_t snap;              // GridDiv, kNone = the global grid
    uint8_t inner_loop;        // 1 = 4-point loop on
    uint8_t catch_up;          // 1 = beat loop release returns to the shadow playhead
    uint8_t scratch_snap;      // 1 = snap back after a scratch, 0 = stay
    uint8_t slices;            // chop: 2..16
    uint8_t step_frames;       // step: 1..64
    uint8_t autopilot_count;   // 1..64 loops
    uint8_t backward;          // Direction: 1 = backward
    uint8_t reverse_hold;      // 1 while REVERSE is held
    float in, out;             // 0..1 of the clip
    float loop_in, loop_out;   // 0..1 of the clip
    float beats;               // BPM time base: range length in beats
    float interval;            // random / chop interval
    float distance;            // random distance 0..1 of the range
    float step_div;            // step interval
    float glide;               // beats
    float fade_in, fade_out;   // seconds
    float transition_time;     // seconds
    uint32_t seed;
    // Status (the engine writes these every frame a clip plays).
    uint8_t phase;             // PlayheadPhase
    uint8_t beat_loop;         // 1 = a beat loop is running
    uint8_t scratching;        // 1 = held by a scratch
    uint8_t pending;           // 1 = a launch / jump waits for the beat snap
    float beat_loop_beats;     // its length
    float speed_now;           // the speed after the glide (LayerState.speed is the target)
    float gain;                // fade envelope 0..1 (multiplies the opacity)
    float transition_progress; // 0..1 while a layer transition runs, 1 when done
    float pending_beats;       // beats until the waiting launch fires
    float velocity;            // of the last trigger, 0..1
    uint32_t loops;            // loops since the clip started (autopilot counts these)
    int32_t frame;             // the frame on screen (-1 = none)
    float cue[kCuePoints];     // the loaded clip's cue points, 0..1 of the clip; < 0 = not set
};

struct ClockState {
    float bpm;                 // 20..300
    uint8_t grid;              // GridDiv: the global beat snap
    uint8_t source;            // 0 = internal, 1 = tapped, 2 = MIDI clock leads (M8, D130)
    uint8_t reserved[2];
    double beat;               // running beat count at time_us (beat 0 = the last resync)
    uint64_t time_us;          // engine clock of this sample
    uint32_t taps;             // taps in the current tap run
    uint32_t reserved2;
};

struct TransportSection {
    ClockState clock;
    LayerTransport layer[kDecks][kLayersPerDeck];
    LayerState hit;            // the HIT layer (on top of PROGRAM, never saved in a look; fx unused)
    LayerTransport hit_transport;
};

// ---- reaction table (version 3, AUDIO, D131; design/music.md §4) -------------
// One reaction per FX knob (kReactParams) per slot, set with FxReact (commands.h).
// Every entry always shows the reaction in use: a Default entry mirrors the
// effect's default reaction (effect.json) when it targets this knob, else its
// signal is None. FxSet resets the slot's 4 entries to Default.
enum class ReactSource : uint8_t {
    Default = 0,   // the effect's own default (from its metadata)
    Custom = 1,    // set with FxReact (Set / Amount / Signal / Range)
    Off = 2,       // switched off with FxReact Off (not even the default moves it)
    Count
};
inline constexpr uint8_t kReactRangeSet = 1u << 0;  // ReactionEntry.flags: range from FxReact Range

struct ReactionEntry {
    uint8_t source;            // ReactSource
    uint8_t signal;            // ReactSignal (None = nothing moves this knob)
    uint8_t mode;              // ReactMode
    uint8_t curve;             // ReactCurve
    uint8_t flags;             // kReactRangeSet; else range = the param's good range
    uint8_t reserved[3];
    float amount;              // -1..1
    float attack_ms;           // 0..2000
    float release_ms;          // 0..2000
    float threshold;           // 0..1 gate
    float range_lo, range_hi;  // knob positions 0..1 the reaction moves between
    float quantise_beats;      // TRIGGER grid in beats (0 = off)
    float hold_beats;          // TRIGGER hold in beats
    // Live (the engine writes these with the section; readers draw the "knob ghost"):
    float env;                 // the reaction's envelope 0..1 now (0 when the slot is not on air)
    float value;               // the knob as the shader gets it now, 0..1 (after scope, PRESS, reaction)
};
static_assert(sizeof(ReactionEntry) == 48);

struct SlotReactions {
    ReactionEntry p[kReactParams];
};

struct ReactionSection {
    SlotReactions layer[kDecks][kLayersPerDeck][kFxSlots];  // = LayerState::fx
    SlotReactions deck[kDecks][kFxSlots];                   // = DeckState::fx
    SlotReactions master[kFxSlots];                         // = LiveState::master_fx
    uint32_t gen;              // bumped on every change of a setting (not of env / value)
    uint32_t reserved;
};

struct EngineStatusMirror {
    std::atomic<uint64_t> heartbeat_us;   // engine clock at the last live write
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> reserved;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct StateBlock {
    BlockHeader header;                 // kStateMagic, kStateVersion, sizeof(StateBlock)
    EngineStatusMirror status;
    alignas(64) std::atomic<uint32_t> live_seq;
    LiveState live;
    alignas(64) std::atomic<uint32_t> catalog_seq;
    Catalog catalog;
    alignas(64) std::atomic<uint32_t> transport_seq;   // version 2
    TransportSection transport;
    alignas(64) std::atomic<uint32_t> react_seq;       // version 3
    ReactionSection react;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace himmel::protocol
