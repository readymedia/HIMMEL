// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The show protocol (M3, SHOW track): the enums of the show commands (types
// 25..36 in commands.h) and the show block "Local\HimmelShow"
// (docs/interfaces/show-block.md, command-protocol.md §4b).
//
// The block has three parts:
//   * status  - written by the render process (the show runtime) at most once
//               per frame, when something changed: project, scenes on the page,
//               the scene on each deck, undo depth, locks, acks of show commands;
//   * files   - written by the show's save worker: project path, last save,
//               autosave / restore state, recent projects, missing clips;
//   * text    - one text box per producer, written only by that producer: the
//               path or name a Project / SongEdit command needs (commands carry
//               no strings). Write the box first, then send the command.
// Readers copy a section with seq_read and work on the copy.
#pragma once

#include <cstddef>
#include <cstdint>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/state.h"   // kNameLen

namespace himmel::protocol {

inline constexpr const wchar_t* kShowShmName = L"Local\\HimmelShow";
inline constexpr uint32_t kShowMagic = 0x57534D48;  // "HMSW"
inline constexpr uint32_t kShowVersion = 1;

inline constexpr int kScenePads = 16;        // scenes per bank / per song (D69)
inline constexpr int kSceneNameLen = 24;
inline constexpr int kPathLen = 520;         // UTF-8
inline constexpr int kRecent = 10;           // "Open recent": the last 10 (projects.md §3)
inline constexpr int kMissingShown = 8;

// SceneRecall.i besides 0..15
inline constexpr int32_t kSceneNext = -2;    // GO / ENTER: the next scene of the song (or bank)
inline constexpr int32_t kScenePrev = -3;

enum class DiceVerb : uint8_t { Roll = 0, Mutate = 1, Back = 2, Forward = 3, Rotate = 4, Count };

// The 8 locks (presets-and-shuffle.md §2). A set bit = held (the dice keep it).
enum LockBit : uint32_t {
    kLockClip = 1u << 0,
    kLockFx = 1u << 1,
    kLockColour = 1u << 2,
    kLockMotion = 1u << 3,
    kLockLayout = 1u << 4,
    kLockSource = 1u << 5,
    kLockReact = 1u << 6,   // reactions arrive in M8; the bit is kept already
    kLockMood = 1u << 7,    // moods arrive later; the bit is kept already
    kLockAll = 0xFFu
};
enum class LockOp : uint8_t { Set = 0, Toggle = 1, Clear = 2, Count };

enum class ProjectAction : uint8_t {
    New = 0,             // i = screens of the template (1 or 3, 0 = 3); the new project is "Untitled" until saved as
    Open = 1,            // slot = recent index 0..9; slot = kNone: i = text id, the path (folder or .himmelshow) is in the box
    Save = 2,            // the project folder (an untitled project needs SaveAs)
    SaveAs = 3,          // i = text id: a new folder path or a name (then it goes into the projects folder)
    Export = 4,          // i = 0: next to the project folder; else i = text id: the .himmelshow path
    Revert = 5,          // back to the last save
    RestoreAutosave = 6, // load the newest autosave (journal) of the current project
    Relink = 7,          // i = text id: a folder to search for the missing clips (by name)
    Count
};

enum class SelectWhat : uint8_t {
    Bank = 0,            // i = bank 0..15 (Relative: delta)
    Song = 1,            // i = song in the setlist (Relative: delta)
    UseSetlist = 2,      // i = Toggle value: pages show the song's scenes (on) or the bank (off)
    Count
};

enum class PadKind : uint8_t { Empty = 0, Clip = 1, Cue = 2, Effect = 3, Scene = 4, Action = 5, Count };
enum class PadTarget : uint8_t { Selected = 0, Layer1 = 1, Layer2 = 2, Layer3 = 3, Layer4 = 4, All = 5, Free = 6, Hit = 7, Count };
enum class PadAction : int32_t { Black = 0, WhiteFlash = 1, Freeze = 2, NextScene = 3, DiceLayer = 4, Count };

enum class SongAction : uint8_t {
    New = 0,             // i = text id of the name (0 = "Song n"); added at the end of the setlist
    Delete = 1,          // i = song
    AddScene = 2,        // i = song; deck = source: the deck's look becomes a new scene at the end of the song
    RemoveScene = 3,     // i = song, slot = position 0..15
    Bpm = 4,             // i = song, f[0] = 20..300 (0 = none)
    Rename = 5,          // slot = song, i = text id of the name
    Move = 6,            // i = song, slot = new place in the setlist
    RenameScene = 7,     // slot = pad on the current page, i = text id of the name
    Count
};

// Where the last saved / opened project came from (FilesStatus.restored).
enum class Restored : uint8_t { None = 0, Autosave = 1, InterruptedSave = 2, Count };

// ---- status (render process) -----------------------------------------------
struct ScenePadInfo {
    char name[kSceneNameLen];
    uint8_t used;          // 1 = a scene is on this pad
    uint8_t variations;    // count of variations (0 = only the main one)
    uint8_t on_deck;       // bit 0 = on deck A, bit 1 = on deck B
    uint8_t reserved;
    uint32_t thumb_rgb;    // average colour of the scene's first clip (placeholder)
};

struct DeckSceneInfo {
    int16_t scene;         // scene id in the show (-1 = none / edited from empty)
    int8_t pad;            // pad on the current page (-1 = not on this page)
    char variation;        // 'a' main, 'b', 'c' ...; 0 = none
    uint8_t edited;        // 1 = changed since it was recalled (REC would save a variation)
    uint8_t morphing;      // 1 while a morph / reset glide runs on this deck
    uint8_t reserved[2];
    float morph_progress;  // 0..1
    uint16_t dice_pos;     // place in the dice history (0 = none)
    uint16_t dice_count;
};

struct ShowStatus {
    uint64_t time_us;
    char project[kNameLen];        // project name ("Untitled" before the first save as)
    uint8_t dirty;                 // 1 = unsaved changes (the dot in the title bar)
    uint8_t busy;                  // 1 = the worker is saving / opening
    uint8_t use_setlist;           // 1 = pages show the current song
    uint8_t bank;                  // current bank 0..15
    int16_t song;                  // current song in the setlist (-1 = none)
    uint16_t song_count;
    char song_name[kSceneNameLen];
    float song_bpm;
    ScenePadInfo pad[kScenePads];  // the current page
    DeckSceneInfo deck[kDecks];
    uint32_t locks;                // LockBit mask
    uint16_t undo_depth, redo_depth;
    float macro[4];                // the PROGRAM scene's macro values 0..1
    char macro_name[4][12];
    uint32_t last_recall_us;       // cost of the last recall inside the frame (µs)
    uint32_t scene_count;          // scenes in the show (all banks, songs, variations)
    // Show commands are acked here (the deck model's LiveState acks the others).
    uint32_t acked_seq[kProducers];
    uint32_t rejected[kProducers];
    uint8_t last_reject[kProducers];
    char message[64];              // last event ("3c saved", "Saved", "Autosave restored 22:41")
    uint32_t message_seq;          // bumps with every new message
};

// ---- files (save worker) -------------------------------------------------------
struct RecentProject {
    char name[kNameLen];
    char path[kPathLen];
    uint64_t saved_unix;   // seconds since 1970 (UTC)
    uint8_t outputs;
    uint8_t reserved[7];
};

struct FilesStatus {
    char path[kPathLen];           // the project folder ("" = untitled)
    uint64_t saved_unix;           // last successful save
    uint64_t autosave_unix;        // newest journal record / autosave
    uint8_t restored;              // Restored: how the open project came back
    uint8_t save_ok;               // 1 = the last save worked
    uint8_t recent_count;
    uint8_t missing_count;         // clips the project names that the catalog does not have
    uint32_t saves;                // saves done since start
    uint32_t journal_records;      // live-state records in the current journal
    uint32_t relink_found;         // clips found by the last relink
    RecentProject recent[kRecent];
    char missing[kMissingShown][kNameLen];
    char error[128];               // last error ("" = none)
};

// ---- text boxes (one per producer) -----------------------------------------
// Handshake: the producer picks a new non-zero id (its next command seq is a good one),
// writes id + text with write_text(), then sends the command with i = that id. The
// runtime refuses the command (BadValue) when the box does not hold that id, so a stale
// or half-written text is never used.
struct TextBox {
    std::atomic<uint32_t> seq;     // seqlock over id + text
    uint32_t id;                   // the id of the text below (0 = none)
    char text[kPathLen];           // UTF-8, NUL-terminated
};
static_assert(offsetof(TextBox, text) == offsetof(TextBox, id) + sizeof(uint32_t), "id and text are one seqlock range");

inline void write_text(TextBox& b, uint32_t id, const char* s) {   // the box's producer only
    uint8_t buf[sizeof(uint32_t) + kPathLen] = {};
    std::memcpy(buf, &id, sizeof(id));
    for (int k = 0; s && s[k] && k < kPathLen - 1; ++k) buf[sizeof(id) + k] = static_cast<uint8_t>(s[k]);
    seq_write_bytes(b.seq, reinterpret_cast<uint8_t*>(&b.id), buf, sizeof(buf));
}
// False when the writer stayed busy; `out` gets the text, NUL-terminated.
inline bool read_text(const TextBox& b, uint32_t& id, char (&out)[kPathLen]) {
    uint8_t buf[sizeof(uint32_t) + kPathLen];
    if (!seq_read_bytes(b.seq, reinterpret_cast<const uint8_t*>(&b.id), buf, sizeof(buf))) return false;
    std::memcpy(&id, buf, sizeof(id));
    std::memcpy(out, buf + sizeof(id), kPathLen);
    out[kPathLen - 1] = 0;
    return true;
}

// Slot 4 (Show) goes through the deck model's validation and rate limit like every producer.
// The runtime sends at most this many commands per frame (one deck's full look = 352 worst
// case, so a recall always lands in one frame); the rest waits in its outbox for the next
// frame, and it never sends more than kMaxCommandsPerSecond in a second.
inline constexpr int kShowCommandsPerFrame = 384;
inline constexpr int kLookCommandsMax = kLayersPerDeck * (28 + 1 + 4 + kFxSlots * (3 + kFxParams)) + kFxSlots * (3 + kFxParams);
static_assert(kLookCommandsMax <= kShowCommandsPerFrame, "a full one-deck recall must fit one frame");

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ShowBlock {
    BlockHeader header;            // kShowMagic, kShowVersion, sizeof(ShowBlock)
    alignas(64) std::atomic<uint32_t> status_seq;
    ShowStatus status;
    alignas(64) std::atomic<uint32_t> files_seq;
    FilesStatus files;
    alignas(64) TextBox text[kProducers];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Command types 25..39 belong to SHOW (LEAD, wave 2 number ranges): the render process hands
// all of them to the show runtime, which refuses the ones it does not know (UnknownType).
inline constexpr uint16_t kShowTypeFirst = 25;
inline constexpr uint16_t kShowTypeLast = 39;
inline bool is_show_command(CommandType t) {
    const auto v = static_cast<uint16_t>(t);
    return v >= kShowTypeFirst && v <= kShowTypeLast;
}

}  // namespace himmel::protocol
