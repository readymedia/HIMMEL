// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The show model in memory (M3, docs/interfaces/show-files.md §2).
//
// Fixed-size PODs only: the frame loop owns one ShowData and edits it in place
// (no heap); the save worker gets a copy. Clip and effect ids are the engine's
// catalog ids at run time; on disk they are names (show-files.md §3), and
// `Look::clip_name` keeps the name of a clip the catalog does not have, so a
// missing clip survives a save and can be relinked later.
#pragma once

#include <cstdint>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"

namespace himmel::show {

namespace proto = himmel::protocol;

inline constexpr int kLayers = proto::kLayersPerDeck;
inline constexpr int kPads = proto::kScenePads;
inline constexpr int kMacros = 4;
inline constexpr int kMacroBinds = 8;
inline constexpr int kMaxScenes = 256;     // scenes in one show, variations included
inline constexpr int kMaxBanks = 16;
inline constexpr int kMaxSongs = 64;
inline constexpr int kMaxVariations = 8;   // a .. h
inline constexpr int kUndoDepth = 64;      // D72
inline constexpr int kDiceHistory = 64;    // per deck (presets-and-shuffle.md §2)

// One FX knob's audio reaction as a look stores it (D131), in FxReact terms
// (command-protocol.md section 4d). Canonical form, so two looks compare bitwise:
// Default (the effect's own reaction) and Off keep every other field zero; only
// Custom fills them. All zero = Default, so an old look has no reactions.
struct ReactKnob {
    uint8_t source;        // proto::ReactSource
    uint8_t signal;        // proto::ReactSignal
    uint8_t mode;          // proto::ReactMode
    uint8_t curve;         // proto::ReactCurve
    uint8_t quantise;      // proto::ReactQuantise
    uint8_t hold_q;        // TRIGGER hold in quarter beats 0..kReactHoldMax
    uint8_t ranged;        // 1 = range_lo / hi set with FxReact Range; 0 = the param's good range
    uint8_t reserved;
    float amount;          // -1..1
    float attack_ms, release_ms;
    float threshold;
    float range_lo, range_hi;   // 0 when !ranged
};
struct ReactRow {
    ReactKnob k[proto::kReactParams];   // one per FX knob 1-4 of the slot
};

// One deck as a scene stores it: 4 layers (source, mix, transport settings, FX)
// plus the deck FX, and each FX slot's knob reactions. Status fields (position,
// missing, playing, playhead status) are always zero in a Look.
struct Look {
    proto::LayerState layer[kLayers];
    proto::LayerTransport transport[kLayers];
    proto::FxSlotState fx[proto::kFxSlots];
    char clip_name[kLayers][proto::kNameLen];   // the clip's name ("" = empty); kept when the clip is missing
    ReactRow layer_react[kLayers][proto::kFxSlots];   // = layer[l].fx[s]
    ReactRow fx_react[proto::kFxSlots];               // = fx[s]
};

// What both decks and the master hold: the undo stack, the journal and live.json.
struct LiveSnapshot {
    Look deck[proto::kDecks];
    proto::FxSlotState master_fx[proto::kFxSlots];
    ReactRow master_react[proto::kFxSlots];
    float crossfader;
    uint8_t program_deck;
    uint8_t black;
    uint8_t reserved[2];
};

struct KitPad {
    uint8_t kind;          // proto::PadKind
    uint8_t target;        // proto::PadTarget
    uint8_t slot;          // Effect: the FX slot it uses; Cue: the cue 0..15
    uint8_t reserved;
    int32_t id;            // clip id, effect id, scene pad or PadAction (-1 = none)
    char name[proto::kNameLen];   // clip / effect name on disk (kept when missing)
};

struct MacroBind {
    uint8_t used;
    uint8_t layer;         // 0..3, kNone = deck FX
    uint8_t slot;          // FX slot 0..3
    uint8_t param;         // 0..7
    float min, max;        // the param range the macro sweeps (0..1); max < min = inverted
};

struct Macro {
    char name[12];
    float value;           // 0..1
    MacroBind bind[kMacroBinds];
};

enum class TransitionKind : uint8_t { Cut = 0, Morph = 1, Take = 2, Count };
struct Transition {
    uint8_t kind;          // TransitionKind
    uint8_t on_beat;       // 1 = waits for the grid (M4 applies it)
    uint8_t reserved[2];
    float seconds;         // morph / take time
};

// Where a scene lives: a bank pad or a song position.
enum class Owner : uint8_t { None = 0, Bank = 1, Song = 2 };

struct Scene {
    uint8_t used;
    uint8_t owner;         // Owner
    uint8_t owner_index;   // bank 0..15 / song 0..63
    uint8_t pad;           // 0..15 in its bank / song
    int16_t parent;        // -1 = a main scene; else the main scene this is a variation of
    uint8_t variation;     // 0 = main ('a'), 1 = 'b' ...
    uint8_t reserved;
    char name[proto::kSceneNameLen];
    Look look;
    KitPad kit[kPads];
    Macro macro[kMacros];
    Transition transition;
};

struct Song {
    uint8_t used;
    uint8_t scene_count;
    uint8_t reserved[2];
    float bpm;             // 0 = none
    char name[proto::kSceneNameLen];
    int16_t scene[kPads];  // scene ids (main scenes), -1 = empty position
};

enum class OutputPreset : uint8_t { P720 = 0, P1080 = 1, P1440 = 2, P4K = 3, Custom = 4, Count };
struct OutputConfig {
    uint8_t count;         // 1..8
    uint8_t preset;        // OutputPreset (projects.md §5)
    uint8_t span;          // 1 = one canvas, 0 = mirror
    uint8_t reserved;
    uint16_t custom_w, custom_h;
    float refresh_hz;      // 0 = follow the display
};

struct ShowData {
    char name[proto::kNameLen];
    uint64_t created_unix;
    OutputConfig outputs;
    uint8_t start_focus;   // 0 = PREP, 1 = PLAY (D75)
    uint8_t use_setlist;
    uint8_t bank;          // current bank
    uint8_t reserved;
    int16_t song;          // current song in the setlist (-1 = none)
    uint16_t setlist_count;
    uint32_t locks;        // LockBit
    uint32_t default_take_ms;
    Scene scene[kMaxScenes];
    int16_t bank_pad[kMaxBanks][kPads];   // scene ids, -1 = empty
    Song songs[kMaxSongs];
    int16_t setlist[kMaxSongs];           // song ids in play order
};

// ---- helpers (pure, no heap) ----
void clear_look(Look& l);                         // every layer empty, defaults
void clear_scene(Scene& s);
void clear_show(ShowData& s, int screens = 3);    // the "new project" template
void default_macros(Scene& s);                    // INTENSITY, COLOUR, SPACE, MASTER (perform.md §2)

// The scene ids on page pad 0..15 (bank or current song), -1 = empty.
int page_scene(const ShowData& s, int pad);
// A free scene id, -1 = full.
int alloc_scene(const ShowData& s);
// The variations of a main scene, in order (main first). Returns the count (<= kMaxVariations).
int variations_of(const ShowData& s, int main_scene, int out[kMaxVariations]);
int main_of(const ShowData& s, int scene);        // the main scene of a variation (or itself)
int scene_count(const ShowData& s);

// Deck model state -> Look / LiveSnapshot (status fields zeroed, clip names from the catalog).
// `react` = the model's reaction table (nullptr = every knob Default).
void look_from_live(const proto::LiveState& live, const proto::TransportSection& tr, const proto::Catalog& cat,
                    int deck, Look& out, const proto::ReactionSection* react = nullptr);
void snapshot_from_live(const proto::LiveState& live, const proto::TransportSection& tr, const proto::Catalog& cat,
                        LiveSnapshot& out, const proto::ReactionSection* react = nullptr);

// One reaction table entry -> its canonical stored form (ReactKnob).
void react_from_entry(const proto::ReactionEntry& e, ReactKnob& out);
void react_from_row(const proto::SlotReactions& row, int effect, ReactRow& out);   // effect < 0: all Default
// A slot whose effect changed between `ref` and `look` gets Default reactions
// (FxSet resets them; the dice uses this).
void reset_changed_react(Look& look, const Look& ref);
bool same_look(const Look& a, const Look& b);    // settings only (bitwise on the zeroed form)

// Copy a NUL-terminated string into a fixed buffer (always terminated).
void copy_str(char* dst, int cap, const char* src);

}  // namespace himmel::show
