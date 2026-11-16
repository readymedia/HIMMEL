// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The command protocol: GUI / weapons / OSC -> engine (docs/interfaces/command-protocol.md).
//
// Shared block "Local\HimmelCommands": one SPSC ring per producer slot. The
// engine is the only consumer. Commands are 32-byte PODs; the engine validates
// every field (range, ids, rate) before it touches the model, and applies them
// at frame boundaries. Append-only: never renumber a CommandType.
#pragma once

#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::protocol {

inline constexpr const wchar_t* kCommandShmName = L"Local\\HimmelCommands";
inline constexpr uint32_t kCommandMagic = 0x444D4348;  // "HCMD"
inline constexpr uint32_t kCommandVersion = 1;
inline constexpr uint32_t kCommandRingSize = 1024;     // per producer, power of two

// Model limits shared by every track (D46: 4 layers per deck).
inline constexpr int kDecks = 2;            // 0 = A, 1 = B
inline constexpr int kLayersPerDeck = 4;
inline constexpr int kFxSlots = 4;          // per layer, per deck and on master
inline constexpr int kFxParams = 8;
inline constexpr uint8_t kNone = 0xFF;      // "not used" for deck / layer / slot / param
inline constexpr uint8_t kMasterDeck = 0xFE; // deck field of master-level FX commands
inline constexpr uint8_t kHitDeck = 0xFD;    // deck field of the HIT layer (layer must be 0); M2, clips-and-playback.md §3.3
inline constexpr int kCuePoints = 16;        // cue points per clip (clips-and-playback.md §2.4)

// Fixed producer slots (one ring each; exactly one process writes a slot).
enum class Producer : uint8_t {
    Perform = 0,   // weapon mappers inside the engine process (MK2 Perform, ...)
    Gui = 1,       // himmel_ui bridge
    Osc = 2,       // OSC in (M13)
    Tool = 3,      // tests, scripts, himmel_cmd
    Spare4, Spare5, Spare6, Spare7,
    Count
};
inline constexpr int kProducers = static_cast<int>(Producer::Count);

enum class CommandType : uint16_t {
    Nop = 0,
    // ---- layer ----            deck, layer
    LayerClip = 1,      // i = clip id from the catalog, -1 = empty layer
    LayerOpacity = 2,   // f[0] 0..1 (Relative: delta)
    LayerBlend = 3,     // i = BlendMode
    LayerMute = 4,      // i = Toggle value (0 off, 1 on, 2 toggle)
    LayerPlay = 5,      // i = PlayAction
    LayerSpeed = 6,     // f[0] -4..4 (1 = normal; Relative: delta)
    LayerJump = 7,      // f[0] position 0..1 of the clip (in..out later)
    // ---- effects ----          deck (or kMasterDeck), layer (kNone = deck level), slot
    FxSet = 8,          // i = effect id from the catalog, -1 = empty slot
    FxParam = 9,        // param, f[0] 0..1 (Relative: delta)
    FxBypass = 10,      // i = Toggle value
    FxMix = 11,         // f[0] 0..1 dry/wet (Relative: delta)
    // ---- mixer ----
    Crossfader = 12,    // f[0] 0 (A) .. 1 (B) (Relative: delta). Moving it by hand cancels a running TAKE.
    Take = 13,          // i = duration in ms (0 = the show default); the crossfader travels to the other deck
    Cut = 14,           // jump the crossfader to the other deck now
    Black = 15,         // i = Toggle value: master black
    // ---- perform state (shared by weapons and the GUI) ----
    Focus = 16,         // deck, layer: the focused layer (D48-D53 "last group pressed")
    // ---- housekeeping ----
    Hello = 17,         // i = producer's pid; marks the producer alive (also every 1 s)
    // ---- clip transport (M2, design/clips-and-playback.md §2-3) ----   deck (or kHitDeck), layer
    LayerSet = 18,      // param = LayerParam; i or f[0] (float params: Relative = delta)
    LayerTrigger = 19,  // i = clip id (-1 = the layer's clip), f[0] = velocity 0..1 (0 = full); flags kRelease = pad up
    LayerCue = 20,      // slot = cue 0..15, i = CueAction; Set: f[0] = position 0..1 of the clip, < 0 = the playhead
    LayerBeatLoop = 21, // f[0] = loop length in beats (1/16..16), 0 = off (catch-up per LayerParam::CatchUp)
    LayerScratch = 22,  // i = 1 hold / move, 0 = let go; f[0] = position 0..1 of the clip (Relative: delta in clip seconds)
    Clock = 23,         // param = ClockParam; i / f[0]
    // ---- effects (M5) ----     deck (or kMasterDeck), layer (kNone = deck level), slot
    FxPress = 24,       // f[0] 0..1 pressure on the slot's PRESS target (0 = released); docs/interfaces/effect-metadata.md §5
    Count
};

enum class BlendMode : int32_t { Normal = 0, Add = 1, Screen = 2, Multiply = 3, Difference = 4, Lighten = 5, Count };
enum class PlayAction : int32_t {
    Pause = 0, Play = 1, Toggle = 2, Restart = 3,
    Release = 4,   // M2: leave the inner loop and play the outro (loop-out -> out)
    Stop = 5,      // M2: fade out (the layer's fade-out time), then the layer is empty
    Count
};

// ---- clip transport (M2) ---------------------------------------------------
// LayerSet.param. Settings live per layer (and the HIT layer); they are shown in
// the state block's transport section (TransportSection, state.h).
enum class LayerParam : uint8_t {
    Mode = 0,           // i = PlayMode
    Direction = 1,      // i = 0 forward, 1 backward
    In = 2,             // f 0..1 of the clip (the range keeps at least one frame)
    Out = 3,            // f 0..1 of the clip
    LoopIn = 4,         // f 0..1 of the clip: inner loop start
    LoopOut = 5,        // f 0..1 of the clip: inner loop end
    InnerLoop = 6,      // i = Toggle value: the 4-point loop on / off
    TimeBase = 7,       // i = TimeBase
    Beats = 8,          // f 0.25..64: BPM time base, the in..out range lasts this many beats
    Interval = 9,       // f 1/16..64: random / chop interval (beats with BPM time base, else seconds)
    Distance = 10,      // f 0..1: random jump distance, part of the range
    Slices = 11,        // i 2..16: chop slices
    StepFrames = 12,    // i 1..64: step mode, frames per step
    StepDiv = 13,       // f 1/16..16: step mode, one step every (beats with BPM time base, else seconds)
    Glide = 14,         // f 0..16 beats: speed changes glide over this time (0 = jump)
    Trigger = 15,       // i = TriggerStyle
    StartPos = 16,      // i = StartPos
    FadeIn = 17,        // f 0..10 s: a triggered clip fades in (0 = cut)
    FadeOut = 18,       // f 0..10 s: stop / release / eject fades out (0 = cut)
    Snap = 19,          // i = GridDiv, or -1 = follow the global grid
    Target = 20,        // i = ClipTarget
    Autopilot = 21,     // i = AutopilotMode
    AutopilotCount = 22,// i 1..64 loops
    Transition = 23,    // i = TransitionType (when the layer gets a new clip)
    TransitionTime = 24,// f 0..10 s
    CatchUp = 25,       // i = Toggle value: beat loop release returns to where the clip would be
    ScratchSnap = 26,   // i = Toggle value: on scratch release, snap back (1) or stay (0)
    ReverseHold = 27,   // i = 1 held / 0 let go: momentary reverse with catch-up
    Seed = 28,          // i: random / chop / autopilot seed (same seed = same pattern)
    Count
};
enum class PlayMode : uint8_t { Loop = 0, PingPong = 1, OnceHold = 2, OnceEject = 3, OnceNext = 4, Random = 5, Chop = 6, Step = 7, Scratch = 8, Count };
enum class TimeBase : uint8_t { Free = 0, Bpm = 1, Count };
enum class TriggerStyle : uint8_t { Normal = 0, Toggle = 1, Piano = 2, Legato = 3, Count };
enum class StartPos : uint8_t { Restart = 0, Continue = 1, Count };
enum class ClipTarget : uint8_t { Own = 0, Free = 1, Hit = 2, Count };
enum class AutopilotMode : uint8_t { Off = 0, Next = 1, Previous = 2, Random = 3, Same = 4, Count };
// Later types (wipes, push, zoom, luma, noise, blur, flash) are appended.
enum class TransitionType : uint8_t { Cut = 0, Fade = 1, BlackDip = 2, Count };
// Beat snap: off, 1/4 beat, 1 beat, 1 bar (4 beats), 2 bars, 4 bars.
enum class GridDiv : uint8_t { Off = 0, Quarter = 1, Beat = 2, Bar = 3, TwoBars = 4, FourBars = 5, Count };
enum class CueAction : int32_t { Jump = 0, Set = 1, Clear = 2, Count };
// Clock.param. The internal clock (tap / set); audio and Link clocks come in M8.
enum class ClockParam : uint8_t {
    Tap = 0,            // a tap: tempo from the last taps, the tap lands on a beat
    Bpm = 1,            // f 20..300 (Relative: delta)
    Nudge = 2,          // f -1..1 beats: shift the phase
    Resync = 3,         // beat 0 is now
    Grid = 4,           // i = GridDiv: the global beat snap
    Count
};
enum ToggleValue : int32_t { kOff = 0, kOn = 1, kToggle = 2 };

// Command.flags
inline constexpr uint16_t kRelative = 1u << 0;   // f[0] is a delta, not an absolute value
inline constexpr uint16_t kRelease = 1u << 1;    // LayerTrigger: the pad / key was let go

struct Command {
    CommandType type;   // 2
    uint16_t flags;     // 2
    uint8_t deck;       // 0..kDecks-1, kMasterDeck, or kNone
    uint8_t layer;      // 0..kLayersPerDeck-1 or kNone
    uint8_t slot;       // 0..kFxSlots-1 or kNone
    uint8_t param;      // 0..kFxParams-1 or kNone
    uint32_t seq;       // producer's running number (1, 2, 3 ...); acked in the state block
    int32_t i;          // integer argument
    float f[4];         // float arguments (only f[0] is used in version 1)
};
static_assert(sizeof(Command) == 32);

using CommandRing = SpscRing<Command, kCommandRingSize>;

struct ProducerInfo {
    std::atomic<uint32_t> pid;            // writer's process id (0 = nobody)
    std::atomic<uint64_t> heartbeat_us;   // writer's clock (steady, microseconds) at its last push / Hello
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas padding is intended
#endif
struct CommandBlock {
    BlockHeader header;           // kCommandMagic, kCommandVersion, sizeof(CommandBlock)
    ProducerInfo producers[kProducers];
    CommandRing rings[kProducers];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Why the engine refused a command (counted per producer in the state block).
enum class Reject : uint8_t {
    None = 0,
    UnknownType,
    BadTarget,      // deck / layer / slot / param out of range
    BadId,          // clip or effect id not in the catalog
    BadValue,       // NaN, inf, or an enum out of range (values in range are clamped, not refused)
    RateLimited,    // more than kMaxCommandsPerSecond from one producer
    Count
};
inline constexpr uint32_t kMaxCommandsPerSecond = 4000;

// Convenience builders (no allocation). Producers fill seq themselves.
inline Command make(CommandType t, uint8_t deck = kNone, uint8_t layer = kNone, int32_t i = 0, float f0 = 0.f,
                    uint16_t flags = 0) {
    Command c{};
    c.type = t;
    c.flags = flags;
    c.deck = deck;
    c.layer = layer;
    c.slot = kNone;
    c.param = kNone;
    c.i = i;
    c.f[0] = f0;
    return c;
}
inline Command make_fx(CommandType t, uint8_t deck, uint8_t layer, uint8_t slot, uint8_t param = kNone, int32_t i = 0,
                       float f0 = 0.f, uint16_t flags = 0) {
    Command c = make(t, deck, layer, i, f0, flags);
    c.slot = slot;
    c.param = param;
    return c;
}

}  // namespace himmel::protocol
