// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// show_cmd.exe - show commands (M3) from the command line (producer slot 3
// "Tool", like himmel_cmd: do not run both at the same time) and the show block.
//
//   show_cmd status                          project, page, decks, undo, files, recents
//   show_cmd recall D PAD [VAR] [MORPH_S]    D = a|b, PAD = 1..16 | next | prev, VAR = a..h | -
//   show_cmd save D [PAD|next] [var]         REC (var = a new variation of the deck's scene)
//   show_cmd undo | redo | reset D [L] [S]   reset: L = 1..4 or all, S = glide seconds
//   show_cmd dice D roll|mutate|back|fwd|rotate [L|all] [SEED|-1]
//   show_cmd locks set|toggle|clear clip,fx,colour,motion,layout,source,react,mood
//   show_cmd project new [1|3] | open N|PATH | save | saveas NAME|PATH | export [PATH] | revert | restore | relink FOLDER
//   show_cmd bank N | song N | setlist on|off
//   show_cmd macro N V                       knob 5..8 = macro 1..4, V = 0..1 on PROGRAM
//   show_cmd kit D PAD clip|effect|scene|action|empty ID [selected|layer1..4|all|free|hit]
//   show_cmd songnew NAME | songadd SONG D | songbpm SONG BPM
// Exit: 0 = applied, 1 = usage, 2 = no engine, 3 = refused.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/shm_block.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"

using namespace himmel::protocol;
using himmel::engine::ShmBlock;

namespace {

constexpr int kSlot = static_cast<int>(Producer::Tool);

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Link {
    ShmBlock cmd_shm, show_shm, state_shm;
    CommandBlock* cmd = nullptr;
    ShowBlock* show = nullptr;
    StateBlock* state = nullptr;
    uint32_t seq = 0;

    bool open() {
        if (!show_shm.open_existing(kShowShmName, sizeof(ShowBlock))) return false;
        show = static_cast<ShowBlock*>(show_shm.data());
        if (!himmel::engine::header_ok(show->header, kShowMagic, kShowVersion, sizeof(ShowBlock))) return false;
        if (state_shm.open_existing(kStateShmName, sizeof(StateBlock))) state = static_cast<StateBlock*>(state_shm.data());
        if (!cmd_shm.open_existing(kCommandShmName, sizeof(CommandBlock))) return false;
        cmd = static_cast<CommandBlock*>(cmd_shm.data());
        if (!himmel::engine::header_ok(cmd->header, kCommandMagic, kCommandVersion, sizeof(CommandBlock))) return false;
        cmd->producers[kSlot].pid.store(GetCurrentProcessId());
        cmd->producers[kSlot].heartbeat_us.store(now_us());
        ShowStatus s{};
        if (seq_read(show->status_seq, show->status, s)) seq = s.acked_seq[kSlot];
        if (state) {
            LiveState l{};
            if (seq_read(state->live_seq, state->live, l) && l.acked_seq[kSlot] > seq) seq = l.acked_seq[kSlot];
        }
        return true;
    }
    // The text handshake (show-block.md §2): the id is the next command seq.
    int text(const std::string& t) {
        const uint32_t id = seq + 1;
        write_text(show->text[kSlot], id, t.c_str());
        return static_cast<int>(id);
    }
    int send(Command c) {
        c.seq = ++seq;
        ShowStatus before{};
        seq_read(show->status_seq, show->status, before);
        cmd->producers[kSlot].heartbeat_us.store(now_us());
        if (!cmd->rings[kSlot].push(c)) return std::printf("ring full\n"), 3;
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ShowStatus s{};
            if (!seq_read(show->status_seq, show->status, s)) continue;
            if (s.acked_seq[kSlot] == c.seq) {
                std::printf("%s: applied%s%s\n", himmel::engine::command_name(c.type), s.message[0] ? " - " : "", s.message);
                return 0;
            }
            if (s.rejected[kSlot] != before.rejected[kSlot]) {
                std::printf("%s: REFUSED (%s)\n", himmel::engine::command_name(c.type),
                            himmel::engine::reject_name(static_cast<Reject>(s.last_reject[kSlot])));
                return 3;
            }
        }
        std::printf("%s: no answer from the engine within 1 s\n", himmel::engine::command_name(c.type));
        return 2;
    }
};

std::string when(uint64_t unix_s) {
    if (!unix_s) return "never";
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

void print_status(const Link& l) {
    ShowStatus s{};
    FilesStatus f{};
    seq_read(l.show->status_seq, l.show->status, s);
    seq_read(l.show->files_seq, l.show->files, f);
    std::printf("project  %s%s%s  (%s)\n", s.project, s.dirty ? " *" : "", s.busy ? "  [busy]" : "", f.path[0] ? f.path : "untitled");
    std::printf("saved    %s   autosave %s   restored %u   journal %u records   saves %u\n", when(f.saved_unix).c_str(),
                when(f.autosave_unix).c_str(), f.restored, f.journal_records, f.saves);
    if (f.error[0]) std::printf("error    %s\n", f.error);
    if (f.missing_count) {
        std::printf("missing  %u clip(s):", f.missing_count);
        for (int i = 0; i < kMissingShown && f.missing[i][0]; ++i) std::printf(" %s", f.missing[i]);
        std::printf("\n");
    }
    if (s.use_setlist) std::printf("page     song %d/%u \"%s\" %.1f BPM\n", s.song + 1, s.song_count, s.song_name, s.song_bpm);
    else std::printf("page     bank %u   (%u songs)\n", s.bank + 1, s.song_count);
    for (int p = 0; p < kScenePads; ++p) {
        const ScenePadInfo& pi = s.pad[p];
        if (!pi.used) continue;
        std::printf("  pad %2d  %-24s +%u var  %s%s\n", p + 1, pi.name, pi.variations, pi.on_deck & 1 ? "[A]" : "", pi.on_deck & 2 ? "[B]" : "");
    }
    for (int d = 0; d < kDecks; ++d) {
        const DeckSceneInfo& di = s.deck[d];
        std::printf("deck %c   scene %d pad %d%c%s%s  dice %u/%u\n", 'A' + d, di.scene, di.pad + 1, di.variation ? di.variation : ' ',
                    di.edited ? " (edited)" : "", di.morphing ? " (morphing)" : "", di.dice_pos, di.dice_count);
    }
    std::printf("locks    0x%02X   undo %u  redo %u   macros", s.locks, s.undo_depth, s.redo_depth);
    for (int k = 0; k < 4; ++k) std::printf("  %s %.2f", s.macro_name[k], s.macro[k]);
    std::printf("\nlast     \"%s\"   recall %u us   %u scenes\n", s.message, s.last_recall_us, s.scene_count);
    for (int i = 0; i < f.recent_count; ++i) std::printf("recent %d %s  (%s)\n", i, f.recent[i].path, when(f.recent[i].saved_unix).c_str());
}

int deck_of(const char* s) { return (s[0] == 'b' || s[0] == 'B' || s[0] == '1') ? 1 : 0; }

Command make_show(CommandType t, int deck = kNone, int layer = kNone, int slot = kNone, int param = kNone, int i = 0, float f = 0.f,
                  uint16_t flags = 0) {
    return make_fx(t, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), static_cast<uint8_t>(slot), static_cast<uint8_t>(param), i,
                   f, flags);
}

uint32_t lock_mask(const std::string& s) {
    static const char* names[] = {"clip", "fx", "colour", "motion", "layout", "source", "react", "mood"};
    uint32_t m = 0;
    for (int i = 0; i < 8; ++i)
        if (s.find(names[i]) != std::string::npos) m |= 1u << i;
    if (s == "all") m = kLockAll;
    return m;
}

int usage() {
    std::printf("usage: see the top of src/show/tools/show_cmd_main.cpp\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Link l;
    if (!l.open()) {
        std::printf("no engine (Local\\HimmelShow / Local\\HimmelCommands not found)\n");
        return 2;
    }
    const std::string v = argc > 1 ? argv[1] : "status";
    auto arg = [&](int i, const char* d = "") { return std::string(i < argc ? argv[i] : d); };
    if (v == "status") return print_status(l), 0;
    if (v == "recall") {
        const std::string p = arg(3, "1");
        const int pad = p == "next" ? kSceneNext : (p == "prev" ? kScenePrev : std::atoi(p.c_str()) - 1);
        const std::string var = arg(4, "-");
        const int slot = var == "-" ? kNone : var[0] - 'a';
        return l.send(make_show(CommandType::SceneRecall, deck_of(arg(2, "a").c_str()), kNone, slot, kNone, pad,
                                static_cast<float>(std::atof(arg(5, "0").c_str()))));
    }
    if (v == "save") {
        const std::string p = arg(3, "next");
        const int pad = p == "next" ? -1 : std::atoi(p.c_str()) - 1;
        return l.send(make_show(CommandType::SceneSave, deck_of(arg(2, "a").c_str()), kNone, kNone, kNone, pad, 0.f,
                                arg(4) == "var" ? kVariation : 0));
    }
    if (v == "undo" || v == "redo") return l.send(make_show(CommandType::Undo, kNone, kNone, kNone, kNone, v == "redo"));
    if (v == "reset") {
        const std::string L = arg(3, "all");
        return l.send(make_show(CommandType::SceneReset, deck_of(arg(2, "a").c_str()), L == "all" ? kNone : std::atoi(L.c_str()) - 1,
                                kNone, kNone, 0, static_cast<float>(std::atof(arg(4, "0").c_str()))));
    }
    if (v == "dice") {
        const std::string w = arg(3, "roll");
        const int verb = w == "mutate" ? 1 : w == "back" ? 2 : w == "fwd" ? 3 : w == "rotate" ? 4 : 0;
        const std::string L = arg(4, "all");
        return l.send(make_show(CommandType::Dice, deck_of(arg(2, "a").c_str()), L == "all" ? kNone : std::atoi(L.c_str()) - 1, kNone,
                                verb, std::atoi(arg(5, "0").c_str())));
    }
    if (v == "locks") {
        const std::string op = arg(2, "set");
        return l.send(make_show(CommandType::Locks, kNone, kNone, kNone, op == "toggle" ? 1 : op == "clear" ? 2 : 0, static_cast<int>(lock_mask(arg(3)))));
    }
    if (v == "project") {
        const std::string a = arg(2);
        static const char* names[] = {"new", "open", "save", "saveas", "export", "revert", "restore", "relink"};
        int act = -1;
        for (int i = 0; i < 8; ++i)
            if (a == names[i]) act = i;
        if (act < 0) return usage();
        int i = 0, slot = kNone;
        const std::string t = arg(3);
        if (act == 0) i = t.empty() ? 3 : std::atoi(t.c_str());
        if (act == 1) {
            const bool num = !t.empty() && t.find_first_not_of("0123456789") == std::string::npos;
            if (num) slot = std::atoi(t.c_str());
            else i = l.text(t);
        }
        if (act == 3 || act == 7 || (act == 4 && !t.empty())) i = l.text(t);
        const int r = l.send(make_show(CommandType::Project, kNone, kNone, slot, act, i));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        print_status(l);
        return r;
    }
    if (v == "bank" || v == "song") return l.send(make_show(CommandType::ShowSelect, kNone, kNone, kNone, v == "song", std::atoi(arg(2, "1").c_str()) - 1));
    if (v == "setlist") return l.send(make_show(CommandType::ShowSelect, kNone, kNone, kNone, 2, arg(2, "on") == "on"));
    if (v == "macro")
        return l.send(make_show(CommandType::MacroValue, kNone, kNone, std::atoi(arg(2, "1").c_str()) - 1, kNone, 0,
                                static_cast<float>(std::atof(arg(3, "0.5").c_str()))));
    if (v == "kit") {
        static const char* kinds[] = {"empty", "clip", "cue", "effect", "scene", "action"};
        static const char* targets[] = {"selected", "layer1", "layer2", "layer3", "layer4", "all", "free", "hit"};
        int kind = 0, target = 0;
        for (int i = 0; i < 6; ++i)
            if (arg(4) == kinds[i]) kind = i;
        for (int i = 0; i < 8; ++i)
            if (arg(6, "selected") == targets[i]) target = i;
        return l.send(make_show(CommandType::KitSet, deck_of(arg(2, "a").c_str()), target, std::atoi(arg(3, "1").c_str()) - 1, kind,
                                std::atoi(arg(5, "0").c_str())));
    }
    if (v == "songnew") {
        const int id = arg(2).empty() ? 0 : l.text(arg(2));
        return l.send(make_show(CommandType::SongEdit, kNone, kNone, kNone, 0, id));
    }
    if (v == "songadd")
        return l.send(make_show(CommandType::SongEdit, deck_of(arg(3, "a").c_str()), kNone, kNone, 2, std::atoi(arg(2, "1").c_str()) - 1));
    if (v == "songbpm")
        return l.send(make_show(CommandType::SongEdit, kNone, kNone, kNone, 4, std::atoi(arg(2, "1").c_str()) - 1,
                                static_cast<float>(std::atof(arg(3, "120").c_str()))));
    return usage();
}
