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
inline constexpr uint32_t kStateVersion = 4;  // 2: + transport section (M2); 3: + reaction table (AUDIO, D131);
                                              // 4: + clip pages (catalog v3, ENGINE wave 7) + playback section

inline constexpr int kMaxClips = 256;      // the `catalog` section's window: clip ids 0..255 (catalog v3: see ClipPages)
inline constexpr int kMaxCatalogClips = 8192;  // catalog v3: every engine clip id 0..8191 (clip pages)
inline constexpr int kClipPageSize = 64;
inline constexpr int kClipPageCount = kMaxCatalogClips / kClipPageSize;  // 128
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
    uint8_t reserved[3];       // [0] = energy 1..5 from the library analysis, 0 = not analysed / not a
                               // library clip (CONTROL wave 8, D202; the MK2 BROWSE ENERGY order). [1..2] free
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

// ---- catalog v3: clip pages (version 4, ENGINE wave 7; engine-state.md §1, §2) ------------
// Every clip the engine can play, up to kMaxCatalogClips, in pages of kClipPageSize with one
// seqlock each. Clip id = index (append-only: an id never moves; a clip that is removed or
// whose file is gone keeps its id and says so in `state`). Ids 0..255 are also in the
// `catalog` section (unchanged since version 1). A reader keeps a copy, watches
// `ClipPagesHeader::gen`, and re-reads only the pages whose page_gen changed.
enum class ClipState : uint8_t {
    Empty = 0,     // no clip at this id (above count)
    Ready = 1,     // playable
    Missing = 2,   // the file is gone or cannot be opened (a layer shows the placeholder)
    Removed = 3,   // the clip left the library (MEDIA Remove); the id stays, loading it is refused
    Busy = 4,      // the file is locked or still being written; loading it is refused for now
    Count
};

struct ClipEntry {
    ClipInfo info;             // as in the catalog section
    int64_t library_id;        // the library DB id (library-db.md), 0 = not from the library (media/ or --clip)
    uint8_t state;             // ClipState
    uint8_t reserved[3];
    uint32_t gen;              // bumped when this entry changes
    uint32_t reserved2[2];
};
static_assert(sizeof(ClipEntry) == 96);

struct ClipPageData {
    uint32_t gen;              // = ClipPagesHeader::page_gen[page] at the write
    uint32_t used;             // entries in use on this page (0..kClipPageSize)
    ClipEntry e[kClipPageSize];
};

struct ClipPagesHeader {
    std::atomic<uint32_t> count;     // clip ids in use: 0..count-1 (only grows while the renderer runs)
    std::atomic<uint32_t> gen;       // bumped after any page changed (readers poll this one value)
    uint32_t capacity;               // kMaxCatalogClips
    uint32_t page_size;              // kClipPageSize
    std::atomic<uint32_t> full;      // clips the engine could not take (catalog full; they stay in the library)
    uint32_t reserved[3];
    std::atomic<uint32_t> page_gen[kClipPageCount];  // per page: bumped when that page changed
};

struct ClipPage {
    alignas(64) std::atomic<uint32_t> seq;
    ClipPageData data;
};

// ---- playback (version 4): frame smoothing for clips whose fps does not divide the output Hz --
enum class FrameSmooth : uint8_t {
    Auto = 0,      // Even when the output Hz is a whole multiple of the clip's fps (30 @ 60), else Blend
    Off = 1,       // the nearest frame by wall-clock time (versions <= 3)
    Blend = 2,     // mix the two nearest frames by the playhead's fraction (one extra texture per layer)
    Even = 3,      // the nearest frame on the display's refresh grid (no dt jitter: 2-2-2, 3-2-3-2)
    Count
};
inline constexpr uint8_t kSmoothFollow = 0xFF;  // a layer's setting: follow the global one

struct LayerPlayback {
    uint8_t smooth;            // the layer's setting: FrameSmooth or kSmoothFollow
    uint8_t in_use;            // the FrameSmooth the engine uses now (never Auto)
    uint8_t loading;           // 1 = a new clip is pre-rolling (the old picture stays until its first frame)
    uint8_t reserved;
    float clip_fps;            // fps of the clip on the layer (0 = none)
    float refreshes_per_frame; // output Hz / (clip fps x |speed|): 2 = 30 fps at 60 Hz
    float judder;              // cadence error over the last stats period: mean |hold - ideal| in refreshes (0 = even)
    uint32_t irregular;        // holds that were neither floor nor ceil of the ideal, last stats period
};

struct PlaybackSection {
    uint8_t smooth;            // the global setting: FrameSmooth (OutputConfig FrameSmooth)
    uint8_t reserved[3];
    float out_hz;              // the refresh the smoothing assumes (measured)
    LayerPlayback layer[kDecks * kLayersPerDeck + 1];  // deck * 4 + layer, then HIT
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
    alignas(64) std::atomic<uint32_t> playback_seq;    // version 4
    PlaybackSection playback;
    alignas(64) ClipPagesHeader clips;                 // version 4: catalog v3
    ClipPage clip_page[kClipPageCount];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace himmel::protocol
