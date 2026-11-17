// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// ShowRuntime (M3): the show inside the render process.
//
// Frame side (submit / tick): show commands (types 25..36) from every producer,
// scenes, variations, kits, macros, songs, recall / morph / reset, undo 64,
// dice. It changes the deck model only through ordinary commands on producer
// slot 4 (`Show`), sent straight into DeckModel::submit, so a whole 4-layer
// recall lands in one frame. No heap, no locks, no I/O on this side.
//
// Worker side (its own thread): every file. The frame hands it copies through
// preallocated buffers and SPSC rings: project save / save as / export / open /
// new / revert / relink, the autosave journal (every live change, at most
// 10 per second), autosave/show.json (every show change), a snapshot every
// 5 minutes (the last 10 kept), recent projects. Files: show-files.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

#include "himmel/engine/deck_model.h"
#include "himmel/protocol/show.h"
#include "himmel/show/look_ops.h"
#include "himmel/show/model.h"

namespace himmel::show {

struct RuntimeConfig {
    std::filesystem::path projects_root;   // empty = Documents\Himmel\Projects
    std::filesystem::path app_dir;         // empty = %LOCALAPPDATA%\Himmel (recent.json, Untitled.himmel)
    std::filesystem::path open;            // a project to open at start; empty = the most recent one
    bool open_last = true;                 // open the most recent project when `open` is empty
    bool disk = true;                      // false = no worker, no files (frame-side tests)
    uint32_t snapshot_every_s = 300;       // projects.md §3: every 5 minutes
    uint32_t keep_snapshots = 10;
    uint32_t journal_min_ms = 100;         // at most 10 journal records per second
    uint32_t undo_coalesce_ms = 400;       // GUESS: edits closer than this are one undo step (a knob turn)
};

class ShowRuntime {
public:
    ShowRuntime();
    ~ShowRuntime();
    ShowRuntime(const ShowRuntime&) = delete;
    ShowRuntime& operator=(const ShowRuntime&) = delete;

    // Start-up (allocates, may start the worker, which opens the last project).
    // `block` (Local\HimmelShow) may be null. The catalog is copied for the worker.
    bool start(const RuntimeConfig& cfg, const protocol::Catalog& cat, protocol::ShowBlock* block);
    // Flush the journal, stop the worker. Safe to call twice.
    void stop();

    // ---- frame loop ----
    // One show command from producer slot `producer` (0..7). Acks / refusals go
    // into the show block's status (the deck model acks the other types).
    protocol::Reject submit(int producer, const protocol::Command& c, uint64_t now_us, engine::DeckModel& m);
    // Once per frame after DeckModel::tick: worker results, morphs, undo capture,
    // journal hand-off, status block.
    void tick(engine::DeckModel& m, uint64_t now_us);

    // ---- read side (frame thread / tests) ----
    const ShowData& show() const { return *front_; }
    int deck_scene(int deck) const { return deck_scene_[deck]; }
    int undo_depth() const { return undo_n_; }
    int redo_depth() const { return redo_n_; }
    bool morphing(int deck) const { return morph_[deck].active; }
    uint32_t last_recall_us() const { return last_recall_us_; }
    int last_recall_commands() const { return last_recall_cmds_; }
    const protocol::ShowStatus& status() const { return status_; }
    protocol::FilesStatus files() const;     // the worker's file status (from the block; zero without one)
    bool dirty() const;
    // Write a producer's text box (as that producer would; tests and tools). Returns the id to
    // send in the command's `i`.
    uint32_t set_text(int producer, const char* text);
    // Tests / tools only (never the frame): wait until the worker has nothing to do (and has
    // handed over what it loaded; call tick() meanwhile). Returns false on timeout.
    bool worker_idle() const;

private:
    struct Morph {
        bool active = false;
        uint32_t layer_mask = 0xF;
        bool deck_fx = true;
        uint64_t t0 = 0;
        uint64_t dur_us = 0;
        uint32_t clips_sent = 0;   // bit per layer: the clip switch was sent
        float progress = 0.0f;
    };
    enum class Req : uint8_t { None, Startup, Save, SaveAs, Export, Open, New, Revert, RestoreAutosave, Relink, Autosave };
    struct Request {
        Req kind = Req::None;
        uint8_t producer = 0;
        int32_t i = 0;
        uint32_t show_gen = 0;
        uint32_t live_gen = 0;
        uint32_t epoch = 0;        // loads the frame had taken when it sent this (stale data is dropped)
        char text[protocol::kPathLen] = {};
    };
    enum class Ev : uint8_t { Message, Saved, Failed };
    struct Event {
        Ev kind = Ev::Message;
        uint32_t show_gen = 0, live_gen = 0;
        char text[64] = {};
    };
    struct LiveRec {
        uint64_t unix_ms;
        uint32_t epoch;
        LiveSnapshot snap;
    };
    struct Worker;  // runtime.cpp

    protocol::Reject do_command(int producer, const protocol::Command& c, uint64_t now_us, engine::DeckModel& m);
    void capture(engine::DeckModel& m, uint64_t now_us);
    void boundary() { boundary_ = true; }
    void send(engine::DeckModel& m, uint64_t now_us);  // scratch_ -> model (slot Show)
    void apply_look(engine::DeckModel& m, int deck, const Look& target, uint64_t now_us, uint32_t mask = 0xF,
                    bool deck_fx = true, uint32_t parts = kDiffAll);
    void apply_snapshot(engine::DeckModel& m, const LiveSnapshot& s, uint64_t now_us, bool mixer);
    void recall(engine::DeckModel& m, int deck, int scene, float morph_s, uint64_t now_us);
    void start_morph(int deck, const Look& from, const Look& to, float seconds, uint64_t now_us, uint32_t mask, bool deck_fx);
    void tick_morphs(engine::DeckModel& m, uint64_t now_us);
    void apply_macro(engine::DeckModel& m, int macro, uint64_t now_us);
    int pad_of(int scene) const;
    int page_next(int from_pad, int step) const;
    bool want(Req kind, int producer, int32_t i, const char* text);   // queue a worker request
    void fill_stage(engine::DeckModel& m);
    void sync(engine::DeckModel& m);       // last_ = the model now, no undo step
    void push_undo(const LiveSnapshot& s);
    void take_loaded(engine::DeckModel& m, uint64_t now_us);
    void write_status(const engine::DeckModel& m, uint64_t now_us);
    void message(const char* fmt, ...);
    bool take_text(int producer, int32_t id, char (&out)[protocol::kPathLen]) const;  // false: no such id in the box
    void flush(engine::DeckModel& m, uint64_t now_us);  // outbox -> model, inside the frame cap + rate budget
    void drop_outbox() { outbox_->clear(), outbox_head_ = 0; }
    void changed() { ++show_gen_, status_dirty_ = true; }
    Scene* deck_scene_ptr(int deck);

    RuntimeConfig cfg_;
    protocol::ShowBlock* block_ = nullptr;
    std::unique_ptr<ShowData> bufs_[3];     // front / stage (frame -> worker) / load (worker -> frame)
    ShowData* front_ = nullptr;
    std::unique_ptr<Worker> w_;

    // scenes on the decks
    int deck_scene_[protocol::kDecks] = {-1, -1};
    Look recalled_[protocol::kDecks];       // the look as recalled / saved (reset target, "edited")
    int last_pad_ = -1;
    Morph morph_[protocol::kDecks];
    Look morph_from_[protocol::kDecks], morph_to_[protocol::kDecks];

    // undo
    LiveSnapshot cur_{}, last_{};
    bool have_last_ = false, boundary_ = false;
    uint64_t last_change_us_ = 0;
    std::unique_ptr<LiveSnapshot[]> undo_, redo_;
    int undo_n_ = 0, undo_head_ = 0, redo_n_ = 0;

    // dice history per deck
    std::unique_ptr<Look[]> hist_;           // [deck * kDiceHistory + k]
    int hist_n_[protocol::kDecks] = {}, hist_pos_[protocol::kDecks] = {};
    uint64_t dice_counter_ = 0;

    // generated commands
    std::unique_ptr<CommandList> scratch_;
    uint32_t seq_ = 0;
    uint32_t last_recall_us_ = 0;
    int last_recall_cmds_ = 0;

    // persistence bookkeeping
    uint32_t show_gen_ = 1, live_gen_ = 1, saved_show_gen_ = 0, saved_live_gen_ = 0;
    uint32_t autosaved_show_gen_ = 0;
    uint64_t last_autosave_us_ = 0, last_journal_us_ = 0;
    LiveSnapshot journaled_{};
    bool have_journaled_ = false;
    static constexpr int kPendingData = 8;  // requests waiting for the stage buffer (save, export, autosave)
    std::unique_ptr<Request[]> pending_;
    int pending_n_ = 0;
    bool first_tick_ = true;
    uint32_t epoch_ = 0;                    // loads taken from the worker (Worker::handed counts them on its side)
    bool started_ = false;

    protocol::ShowStatus status_{};
    float macro_fallback_[4] = {0.5f, 0.5f, 0.5f, 0.5f};  // macro values when the PROGRAM deck has no scene
    bool status_dirty_ = true;
    uint64_t status_next_us_ = 0;
    protocol::TextBox local_text_[protocol::kProducers] = {};  // used when there is no shared block
    uint32_t text_ids_ = 0;
    std::unique_ptr<CommandList> outbox_;
    int outbox_head_ = 0;
    int frame_sent_ = 0;
    uint64_t budget_window_us_ = 0;
    uint32_t budget_used_ = 0;
};

}  // namespace himmel::show
