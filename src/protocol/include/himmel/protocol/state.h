// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The engine state block: engine -> GUI / weapons / tools (docs/interfaces/engine-state.md).
//
// Shared block "Local\HimmelState". Written only by the render process, read by
// anyone. Two seqlock sections:
//   * live     - the whole show model as it is on air now, written at most once
//                per frame (only when something changed, plus every 250 ms);
//   * catalog  - clips and effects the engine can play, written when it changes.
// Readers copy a section with seq_read and use the copy; they never hold it.
#pragma once

#include <cstdint>

#include "himmel/protocol/commands.h"

namespace himmel::protocol {

inline constexpr const wchar_t* kStateShmName = L"Local\\HimmelState";
inline constexpr uint32_t kStateMagic = 0x54534D48;  // "HMST"
inline constexpr uint32_t kStateVersion = 1;

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
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace himmel::protocol
