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
    // ---- effects (M5) ----     deck (or kMasterDeck), layer (kNone = deck level), slot
    FxPress = 18,       // f[0] 0..1 pressure on the slot's PRESS target (0 = released); docs/interfaces/effect-metadata.md §5
    Count
};

enum class BlendMode : int32_t { Normal = 0, Add = 1, Screen = 2, Multiply = 3, Difference = 4, Lighten = 5, Count };
enum class PlayAction : int32_t { Pause = 0, Play = 1, Toggle = 2, Restart = 3 };
enum ToggleValue : int32_t { kOff = 0, kOn = 1, kToggle = 2 };

// Command.flags
inline constexpr uint16_t kRelative = 1u << 0;   // f[0] is a delta, not an absolute value

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
