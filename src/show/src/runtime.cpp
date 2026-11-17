// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "himmel/protocol/shm.h"
#include "himmel/show/dice.h"
#include "himmel/show/files.h"

namespace himmel::show {

using namespace himmel::protocol;
namespace fs = std::filesystem;
namespace F = himmel::show::files;

namespace {

constexpr int kShowSlot = static_cast<int>(Producer::Show);

uint64_t steady_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool finite(float v) { return std::isfinite(v); }
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool same_edit(const LiveSnapshot& a, const LiveSnapshot& b) {  // what undo covers: decks + master FX
    for (int d = 0; d < kDecks; ++d)
        if (!same_look(a.deck[d], b.deck[d])) return false;
    return std::memcmp(a.master_fx, b.master_fx, sizeof(a.master_fx)) == 0;
}
bool same_mixer(const LiveSnapshot& a, const LiveSnapshot& b) {
    return std::fabs(a.crossfader - b.crossfader) < 1e-4f && a.program_deck == b.program_deck && a.black == b.black;
}

bool model_empty(const LiveState& live) {
    for (const auto& d : live.deck) {
        for (const auto& l : d.layer) {
            if (l.clip >= 0) return false;
            for (const auto& f : l.fx)
                if (f.effect >= 0) return false;
        }
        for (const auto& f : d.fx)
            if (f.effect >= 0) return false;
    }
    for (const auto& f : live.master_fx)
        if (f.effect >= 0) return false;
    return true;
}

}  // namespace

// ============================================================================
// the worker
// ============================================================================
enum class Apply : uint8_t { None = 0, IfEmpty = 1, Preview = 2, Full = 3 };

struct ShowRuntime::Worker {
    RuntimeConfig cfg;
    std::unique_ptr<Catalog> cat;
    ShowBlock* block = nullptr;
    std::unique_ptr<SpscRing<Request, 64>> req = std::make_unique<SpscRing<Request, 64>>();
    std::unique_ptr<SpscRing<Event, 64>> ev = std::make_unique<SpscRing<Event, 64>>();
    std::unique_ptr<SpscRing<LiveRec, 32>> live = std::make_unique<SpscRing<LiveRec, 32>>();
    // frame -> worker
    ShowData* stage = nullptr;
    LiveSnapshot stage_live{};
    std::atomic<int> stage_full{0};
    // worker -> frame
    ShowData* load = nullptr;
    LiveSnapshot load_live{};
    bool load_have_live = false;
    Apply load_apply = Apply::None;
    bool load_clean = true;
    std::atomic<int> load_ready{0};
    uint32_t handed = 0;             // loads handed over (worker thread only; the frame counts its own)
    std::atomic<bool> ready{false};  // the start-up open is done: the frame may autosave / journal
    std::atomic<bool> quit{false};
    std::atomic<int> busy{0};
    std::atomic<uint32_t> sent{0};   // requests pushed (frame thread)
    std::atomic<uint32_t> done{0};   // requests handled (worker thread)
    std::atomic<uint32_t> recs{0};   // journal records popped (worker thread)
    uint32_t recs_wanted = 0;        // journal records pushed (frame thread)
    bool push(const Request& r) {    // frame thread
        if (!req->push(r)) return false;
        sent.fetch_add(1);
        return true;
    }
    // worker only
    std::unique_ptr<ShowData> wshow = std::make_unique<ShowData>();
    LiveSnapshot wlive{};
    bool have_wshow = false;
    fs::path folder;                 // the project folder ("" = untitled)
    F::Journal journal;
    uint64_t last_snapshot_ms = 0;
    std::vector<F::Recent> recents;
    std::unique_ptr<FilesStatus> fstat = std::make_unique<FilesStatus>();
    std::thread th;

    fs::path projects_root() const { return cfg.projects_root.empty() ? F::default_projects_root() : cfg.projects_root; }
    fs::path app_dir() const { return cfg.app_dir.empty() ? F::default_app_dir() : cfg.app_dir; }
    fs::path dir() const { return folder.empty() ? app_dir() / "Untitled.himmel" : folder; }
    fs::path autosave() const { return dir() / "autosave"; }

    void event(Ev kind, uint32_t sg, uint32_t lg, const char* fmt, ...) {
        Event e;
        e.kind = kind;
        e.show_gen = sg;
        e.live_gen = lg;
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(e.text, sizeof(e.text), fmt, ap);
        va_end(ap);
        ev->push(e);
    }
    void error(const std::string& e) {
        copy_str(fstat->error, sizeof(fstat->error), e.c_str());
        publish();
    }
    void publish() {
        fstat->recent_count = static_cast<uint8_t>(std::min<size_t>(recents.size(), kRecent));
        for (int i = 0; i < kRecent; ++i) {
            RecentProject& r = fstat->recent[i];
            std::memset(&r, 0, sizeof(r));
            if (i >= fstat->recent_count) continue;
            copy_str(r.name, kNameLen, recents[i].name.c_str());
            copy_str(r.path, kPathLen, recents[i].path.c_str());
            r.saved_unix = recents[i].saved_unix;
            r.outputs = static_cast<uint8_t>(recents[i].outputs);
        }
        copy_str(fstat->path, kPathLen, folder.empty() ? "" : F::utf8(folder).c_str());
        fstat->journal_records = journal.records();
        if (block) seq_write(block->files_seq, block->files, *fstat);
    }
    void remember(const ShowData& s, uint64_t saved) {
        if (folder.empty()) return;
        F::Recent r;
        r.name = s.name;
        r.path = F::utf8(folder);
        r.saved_unix = saved;
        r.outputs = s.outputs.count;
        F::touch_recent(recents, r);
        F::save_recents(app_dir() / "recent.json", recents);
    }
    // Start a fresh journal + autosave document from wshow / wlive (after open / save as / snapshot).
    // `ms` = the time stamp of what it holds (the save time after a save, so it never looks newer).
    void restart_autosave(uint64_t ms = 0) {
        if (!ms) ms = F::unix_ms_now();
        std::string err;
        journal.close();
        if (!journal.open_new(autosave() / "journal.bin", *cat, &err)) error(err);
        journal.append(wlive, ms);
        if (have_wshow) F::write_atomic(autosave() / "show.json", F::show_document(*wshow, wlive, *cat, ms), &err);
        fstat->autosave_unix = ms / 1000;
    }
    void snapshot(uint64_t now_ms) {
        last_snapshot_ms = now_ms;
        if (!have_wshow) return;
        const std::time_t t = static_cast<std::time_t>(now_ms / 1000);
        std::tm tm{};
        localtime_s(&tm, &t);
        char name[64];
        std::strftime(name, sizeof(name), "snap-%Y%m%d-%H%M%S.json", &tm);
        std::string err;
        F::write_atomic(autosave() / name, F::show_document(*wshow, wlive, *cat, now_ms), &err);
        std::vector<fs::path> snaps;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(autosave(), ec)) {
            const std::string n = F::utf8(e.path().filename());
            if (n.rfind("snap-", 0) == 0 && e.path().extension() == ".json") snaps.push_back(e.path());
        }
        std::sort(snaps.begin(), snaps.end());
        while (snaps.size() > cfg.keep_snapshots) fs::remove(snaps.front(), ec), snaps.erase(snaps.begin());
        restart_autosave();  // the snapshot holds the live state: the journal starts again
    }

    // Wait until the frame took the previous load (worker thread only).
    bool wait_load_free() {
        for (int i = 0; i < 5000 && load_ready.load(std::memory_order_acquire); ++i) {
            if (quit.load()) return false;
            Sleep(1);
        }
        return !load_ready.load(std::memory_order_acquire);
    }
    void hand_over(const LiveSnapshot& lv, bool have_live, Apply apply, bool clean) {
        load_live = lv;
        load_have_live = have_live;
        load_apply = apply;
        load_clean = clean;
        ++handed;
        load_ready.store(1, std::memory_order_release);
    }

    enum class Mode { Start, User, Revert, Restore };
    bool open_folder(fs::path path, Mode mode) {
        std::error_code ec;
        std::string err;
        if (F::utf8(path.extension()) == F::kExportExt) {  // unpack a .himmelshow into the projects folder
            const std::string base = F::safe_name(F::utf8(path.stem()));
            fs::path dest = projects_root() / F::from_utf8(base + F::kProjectExt);
            for (int n = 2; fs::exists(dest, ec) && n < 100; ++n)
                dest = projects_root() / F::from_utf8(base + " (" + std::to_string(n) + ")" + F::kProjectExt);
            if (!F::unzip_to(path, dest, &err)) {
                error(err);
                event(Ev::Failed, 0, 0, "Open failed");
                return false;
            }
            path = dest;
        }
        if (!wait_load_free()) return false;
        ShowData& s = *load;
        LiveSnapshot lv{};
        F::LoadInfo info = F::load_project(path, s, lv, *cat);
        // The autosave: autosave/show.json + the newest journal record (offer files first, see below).
        const fs::path as = path / "autosave";
        auto adoc = std::make_unique<ShowData>();
        LiveSnapshot alive{}, jlive{};
        uint64_t jms = 0;
        F::LoadInfo ainfo;
        bool have_journal = false;
        for (const char* pre : {"offer-", ""}) {
            std::string text;
            if (!F::read_file(as / F::from_utf8(std::string(pre) + "show.json"), text)) continue;
            ainfo = F::load_document(text, *adoc, alive, *cat);
            have_journal = F::Journal::read_last(as / F::from_utf8(std::string(pre) + "journal.bin"), *cat, jlive, jms);
            if (ainfo.ok) break;
        }
        const uint64_t auto_ms = std::max<uint64_t>(ainfo.ok ? ainfo.saved_ms : 0, jms);
        const bool broken = !info.ok || info.interrupted;
        const bool use_auto = ainfo.ok && (mode == Mode::Restore || broken || (mode == Mode::Start && auto_ms > info.saved_ms));
        Restored restored = Restored::None;
        if (use_auto) {
            s = *adoc;
            lv = (have_journal && jms >= ainfo.saved_ms) ? jlive : alive;
            info.have_live = true;
            info.missing = ainfo.missing;
            info.missing_names = ainfo.missing_names;
            restored = broken ? Restored::InterruptedSave : Restored::Autosave;
        } else if (!info.ok) {
            error(info.error.empty() ? std::string("cannot open ") + F::utf8(path) : info.error);
            event(Ev::Failed, 0, 0, "Open failed");
            return false;
        } else if (mode == Mode::Restore) {
            event(Ev::Message, 0, 0, "No autosave to restore");
            return false;
        } else if (ainfo.ok && auto_ms > info.saved_ms) {
            // A newer autosave that we do not use now: keep it as an offer (RestoreAutosave).
            fs::rename(as / "show.json", as / "offer-show.json", ec);
            fs::rename(as / "journal.bin", as / "offer-journal.bin", ec);
            fstat->autosave_unix = auto_ms / 1000;
        }
        if (use_auto) {
            fs::remove(as / "offer-show.json", ec);
            fs::remove(as / "offer-journal.bin", ec);
        }
        if (!s.name[0]) copy_str(s.name, kNameLen, F::utf8(path.stem()).c_str());
        folder = path;
        *wshow = s;
        have_wshow = true;
        wlive = lv;
        fstat->restored = static_cast<uint8_t>(restored);
        fstat->saved_unix = info.saved_ms / 1000;
        fstat->missing_count = static_cast<uint8_t>(std::min(info.missing, 255));
        for (int i = 0; i < kMissingShown; ++i)
            copy_str(fstat->missing[i], kNameLen, i < static_cast<int>(info.missing_names.size()) ? info.missing_names[i].c_str() : "");
        fstat->error[0] = 0;
        remember(s, info.saved_ms / 1000);
        restart_autosave(use_auto ? 0 : info.saved_ms);
        last_snapshot_ms = F::unix_ms_now();
        publish();
        const Apply apply = mode == Mode::Start ? Apply::IfEmpty : (mode == Mode::User ? Apply::Preview : Apply::Full);
        hand_over(lv, info.have_live, apply, restored == Restored::None);
        if (restored != Restored::None) {
            const std::time_t t = static_cast<std::time_t>(auto_ms / 1000);
            std::tm tm{};
            localtime_s(&tm, &t);
            char hm[16];
            std::strftime(hm, sizeof(hm), "%H:%M", &tm);
            event(Ev::Message, 0, 0, restored == Restored::InterruptedSave ? "Save was cut off: autosave %s back" : "Autosave %s restored", hm);
        } else {
            event(Ev::Message, 0, 0, "Opened %s", s.name);
        }
        return true;
    }

    bool start_untitled() {
        // An untitled show from the last run lives in <app>/Untitled.himmel/autosave.
        folder.clear();
        std::string text;
        if (!F::read_file(autosave() / "show.json", text) || !wait_load_free()) return false;
        LiveSnapshot lv{}, jlive{};
        F::LoadInfo info = F::load_document(text, *load, lv, *cat);
        if (!info.ok) return false;
        uint64_t jms = 0;
        if (F::Journal::read_last(autosave() / "journal.bin", *cat, jlive, jms) && jms >= info.saved_ms) lv = jlive;
        *wshow = *load;
        have_wshow = true;
        wlive = lv;
        fstat->restored = static_cast<uint8_t>(Restored::Autosave);
        restart_autosave();
        publish();
        hand_over(lv, true, Apply::IfEmpty, false);
        event(Ev::Message, 0, 0, "Untitled show restored");
        return true;
    }

    fs::path target_of(const std::string& text) const {
        if (text.empty()) return {};
        fs::path p = F::from_utf8(text);
        const bool is_path = text.find_first_of("\\/:") != std::string::npos;
        if (!is_path) p = projects_root() / F::from_utf8(F::safe_name(text));
        if (F::utf8(p.extension()) != F::kProjectExt) p += F::kProjectExt;
        return p;
    }

    bool save_to(const fs::path& path, const Request& r) {
        std::string err;
        const uint64_t now = F::unix_ms_now();
        if (!F::save_project(path, *wshow, wlive, *cat, now, &err)) {
            fstat->save_ok = 0;
            error(err);
            event(Ev::Failed, 0, 0, "Save failed");
            return false;
        }
        const bool moved = path != folder;
        folder = path;
        fstat->saved_unix = now / 1000;
        fstat->save_ok = 1;
        fstat->restored = 0;
        ++fstat->saves;
        fstat->error[0] = 0;
        remember(*wshow, now / 1000);
        if (moved) restart_autosave(now);
        publish();
        event(Ev::Saved, r.show_gen, r.live_gen, "%s", wshow->name);
        return true;
    }

    // The frame copy for this request. False when it was made before the frame took the
    // last load (it holds the show that was replaced): then it is dropped.
    bool take_stage(const Request& r) {
        if (!stage_full.load(std::memory_order_acquire)) return false;
        const bool fresh = r.epoch == handed;
        if (fresh) {
            *wshow = *stage;
            wlive = stage_live;
            have_wshow = true;
        }
        stage_full.store(0, std::memory_order_release);
        return fresh;
    }

    void handle(Request& r) {
        std::string err;
        std::error_code ec;
        const std::string text = r.text;
        switch (r.kind) {
            case Req::Startup: {
                recents = F::load_recents(app_dir() / "recent.json");
                bool opened = false;
                if (!cfg.open.empty()) opened = open_folder(cfg.open, Mode::Start);
                else if (cfg.open_last && !recents.empty() && fs::exists(F::from_utf8(recents[0].path) / "project.json", ec))
                    opened = open_folder(F::from_utf8(recents[0].path), Mode::Start);
                if (!opened && !start_untitled()) {
                    folder.clear();
                    restart_autosave();
                }
                last_snapshot_ms = F::unix_ms_now();
                publish();
                ready.store(true);
                break;
            }
            case Req::Autosave:
                if (!take_stage(r)) break;
                if (F::write_atomic(autosave() / "show.json", F::show_document(*wshow, wlive, *cat, F::unix_ms_now()), &err))
                    fstat->autosave_unix = F::unix_now();
                else
                    error(err);
                break;
            case Req::Save:
                if (!take_stage(r)) {
                    event(Ev::Failed, 0, 0, "Save skipped: the project changed");
                    break;
                }
                if (folder.empty()) {  // an untitled show is saved under its name in the projects folder
                    fs::path p = target_of(wshow->name);
                    for (int n = 2; fs::exists(p, ec) && n < 100; ++n)
                        p = target_of(std::string(wshow->name) + " " + std::to_string(n));
                    save_to(p, r);
                } else {
                    save_to(folder, r);
                }
                break;
            case Req::SaveAs: {
                if (!take_stage(r)) {
                    event(Ev::Failed, 0, 0, "Save as skipped: the project changed");
                    break;
                }
                const fs::path p = target_of(text);
                if (p.empty()) {
                    error("Save as: no name or path in the text box");
                    event(Ev::Failed, 0, 0, "Save as: no name");
                    break;
                }
                std::string nm = F::utf8(p.stem());
                copy_str(wshow->name, kNameLen, nm.c_str());
                save_to(p, r);
                break;
            }
            case Req::Export: {
                if (!take_stage(r)) {
                    event(Ev::Failed, 0, 0, "Export skipped: the project changed");
                    break;
                }
                const std::string base = F::safe_name(wshow->name);
                fs::path out = text.empty() ? (folder.empty() ? projects_root() : folder.parent_path()) / F::from_utf8(base + F::kExportExt)
                                            : F::from_utf8(text);
                if (F::utf8(out.extension()) != F::kExportExt) out += F::kExportExt;
                const fs::path tmp = app_dir() / "export" / F::from_utf8(base + F::kProjectExt);
                fs::remove_all(tmp, ec);
                if (F::save_project(tmp, *wshow, wlive, *cat, F::unix_ms_now(), &err) && F::zip_folder(tmp, out, &err))
                    event(Ev::Message, 0, 0, "Exported %s", F::utf8(out.filename()).c_str());
                else
                    error(err), event(Ev::Failed, 0, 0, "Export failed");
                fs::remove_all(tmp, ec);
                break;
            }
            case Req::Open: {
                fs::path p;
                if (r.i >= 0) {
                    if (r.i < static_cast<int>(recents.size())) p = F::from_utf8(recents[r.i].path);
                } else {
                    p = F::from_utf8(text);
                    if (!text.empty() && F::utf8(p.filename()) == "project.json") p = p.parent_path();
                }
                if (p.empty()) {
                    error("Open: no such recent project / no path");
                    event(Ev::Failed, 0, 0, "Open: nothing to open");
                } else {
                    open_folder(p, Mode::User);
                }
                break;
            }
            case Req::New: {
                if (!wait_load_free()) break;
                clear_show(*load, r.i == 1 ? 1 : 3);
                load->created_unix = F::unix_now();
                folder.clear();
                *wshow = *load;
                have_wshow = true;
                fstat->restored = 0;
                fstat->saved_unix = 0;
                fstat->missing_count = 0;
                restart_autosave();
                publish();
                hand_over(wlive, false, Apply::None, false);
                event(Ev::Message, 0, 0, "New project");
                break;
            }
            case Req::Revert:
                if (folder.empty()) event(Ev::Failed, 0, 0, "Revert: never saved");
                else open_folder(folder, Mode::Revert);
                break;
            case Req::RestoreAutosave:
                if (folder.empty()) start_untitled();
                else open_folder(folder, Mode::Restore);
                break;
            case Req::Relink: {
                std::vector<std::string> names;
                for (int i = 0; i < kMissingShown && i < fstat->missing_count; ++i)
                    if (fstat->missing[i][0]) names.emplace_back(fstat->missing[i]);
                const auto found = text.empty() ? decltype(F::find_files({}, {})){} : F::find_files(F::from_utf8(text), names);
                fstat->relink_found = static_cast<uint32_t>(found.size());
                publish();
                // GUESS: adding the found files to the engine's catalog is ENGINE / MEDIA work (M7);
                // until then the operator copies them into media/ and restarts.
                event(Ev::Message, 0, 0, "Found %u of %u missing clips", static_cast<unsigned>(found.size()),
                      static_cast<unsigned>(names.size()));
                break;
            }
            default: break;
        }
    }

    void run() {
        uint64_t last_flush = 0;
        while (!quit.load()) {
            bool did = false;
            Request r;
            while (req->pop(r)) {
                busy.store(1);
                handle(r);
                done.fetch_add(1);
                did = true;
            }
            LiveRec rec;
            uint32_t n = 0;
            uint32_t popped = 0;
            while (live->pop(rec)) {
                ++popped;
                if (rec.epoch != handed) continue;  // the live state of a show that was replaced
                wlive = rec.snap;
                if (journal.is_open()) journal.append(rec.snap, rec.unix_ms);
                ++n;
            }
            recs.fetch_add(popped);
            if (n) {
                fstat->autosave_unix = rec.unix_ms / 1000;
                fstat->journal_records = journal.records();
                if (block) seq_write(block->files_seq, block->files, *fstat);
                did = true;
            }
            const uint64_t now_ms = F::unix_ms_now();
            if (have_wshow && now_ms - last_snapshot_ms >= static_cast<uint64_t>(cfg.snapshot_every_s) * 1000) snapshot(now_ms);
            if (now_ms - last_flush >= 1000) journal.flush(), last_flush = now_ms;
            busy.store(0);
            if (!did) Sleep(5);
        }
        // Last records, then close.
        LiveRec rec;
        while (live->pop(rec))
            if (journal.is_open() && rec.epoch == handed) journal.append(rec.snap, rec.unix_ms);
        journal.close();
    }
};

// ============================================================================
// set-up
// ============================================================================
ShowRuntime::ShowRuntime() {
    for (auto& b : bufs_) b = std::make_unique<ShowData>();
    clear_show(*bufs_[0]);
    front_ = bufs_[0].get();
    undo_ = std::make_unique<LiveSnapshot[]>(kUndoDepth);
    redo_ = std::make_unique<LiveSnapshot[]>(kUndoDepth);
    hist_ = std::make_unique<Look[]>(kDecks * kDiceHistory);
    scratch_ = std::make_unique<CommandList>();
    outbox_ = std::make_unique<CommandList>();
    pending_ = std::make_unique<Request[]>(kPendingData);
    for (auto& l : recalled_) clear_look(l);
}

ShowRuntime::~ShowRuntime() { stop(); }

bool ShowRuntime::start(const RuntimeConfig& cfg, const Catalog& cat, ShowBlock* block) {
    cfg_ = cfg;
    block_ = block;
    started_ = true;
    if (!cfg.disk) return true;
    w_ = std::make_unique<Worker>();
    w_->cfg = cfg;
    w_->cat = std::make_unique<Catalog>(cat);
    w_->block = block;
    w_->stage = bufs_[1].get();
    w_->load = bufs_[2].get();
    Request r;
    r.kind = Req::Startup;
    w_->push(r);
    w_->th = std::thread([this] { w_->run(); });
    return true;
}

void ShowRuntime::stop() {
    if (w_ && w_->th.joinable()) {
        w_->quit.store(true);
        w_->th.join();
    }
}

bool ShowRuntime::worker_idle() const {
    if (!w_) return true;
    // Counters, not ring sizes: a request the worker has popped but not finished still counts.
    return w_->done.load() == w_->sent.load() && pending_n_ == 0 && !w_->stage_full.load() && !w_->load_ready.load() &&
           w_->live->size() == 0 && w_->recs_wanted == w_->recs.load();
}

FilesStatus ShowRuntime::files() const {
    FilesStatus f{};
    if (block_) seq_read(block_->files_seq, block_->files, f);
    return f;
}

bool ShowRuntime::dirty() const { return show_gen_ != saved_show_gen_ || live_gen_ != saved_live_gen_; }

uint32_t ShowRuntime::set_text(int producer, const char* text) {
    if (producer < 0 || producer >= kProducers) return 0;
    const uint32_t id = 0x40000000u + ++text_ids_;
    write_text(block_ ? block_->text[producer] : local_text_[producer], id, text);
    return id;
}

bool ShowRuntime::take_text(int producer, int32_t id, char (&out)[kPathLen]) const {
    out[0] = 0;
    if (id == 0) return false;
    uint32_t have = 0;
    if (!read_text(block_ ? block_->text[producer] : local_text_[producer], have, out)) return false;
    return have == static_cast<uint32_t>(id);
}

void ShowRuntime::message(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(status_.message, sizeof(status_.message), fmt, ap);
    va_end(ap);
    ++status_.message_seq;
    status_dirty_ = true;
}

// ============================================================================
// commands -> the model
// ============================================================================
void ShowRuntime::send(engine::DeckModel& m, uint64_t now_us) {
    // Into the outbox, then out as far as this frame's cap and the second's budget allow.
    if (outbox_head_ == outbox_->n) drop_outbox();
    for (int k = 0; k < scratch_->n; ++k) outbox_->push(scratch_->c[k]);
    scratch_->clear();
    flush(m, now_us);
}

void ShowRuntime::flush(engine::DeckModel& m, uint64_t now_us) {
    if (now_us - budget_window_us_ >= 1000000 || now_us < budget_window_us_) budget_window_us_ = now_us, budget_used_ = 0;
    while (outbox_head_ < outbox_->n && frame_sent_ < kShowCommandsPerFrame && budget_used_ < kMaxCommandsPerSecond) {
        Command c = outbox_->c[outbox_head_++];
        c.seq = ++seq_;
        m.submit(kShowSlot, c, now_us);  // validated (and rate limited) like every other producer
        ++frame_sent_;
        ++budget_used_;
    }
    if (outbox_head_ == outbox_->n) drop_outbox();
}

void ShowRuntime::apply_look(engine::DeckModel& m, int deck, const Look& target, uint64_t now_us, uint32_t mask, bool deck_fx,
                             uint32_t parts) {
    Look cur;
    look_from_live(m.live(), m.transport(), m.catalog(), deck, cur);
    scratch_->clear();
    diff_look(cur, target, deck, m.catalog(), *scratch_, parts, mask, deck_fx);
    send(m, now_us);
}

void ShowRuntime::apply_snapshot(engine::DeckModel& m, const LiveSnapshot& s, uint64_t now_us, bool mixer) {
    for (int d = 0; d < kDecks; ++d) apply_look(m, d, s.deck[d], now_us);
    scratch_->clear();
    FxSlotState cur[kFxSlots];
    std::memcpy(cur, m.live().master_fx, sizeof(cur));
    diff_master(cur, s.master_fx, m.catalog(), *scratch_);
    if (mixer) {
        if (std::fabs(m.live().crossfader - s.crossfader) > 1e-4f) scratch_->push(make(CommandType::Crossfader, kNone, kNone, 0, s.crossfader));
        if (m.live().black != s.black) scratch_->push(make(CommandType::Black, kNone, kNone, s.black ? 1 : 0));
    }
    send(m, now_us);
}

void ShowRuntime::capture(engine::DeckModel& m, uint64_t now_us) {
    snapshot_from_live(m.live(), m.transport(), m.catalog(), cur_);
    if (!have_last_) {
        last_ = cur_;
        have_last_ = true;
        return;
    }
    const bool edit = !same_edit(cur_, last_);
    if (edit || !same_mixer(cur_, last_)) ++live_gen_, status_dirty_ = true;
    if (edit) {
        const bool any_morph = morph_[0].active || morph_[1].active;
        if (!any_morph && now_us - last_change_us_ > static_cast<uint64_t>(cfg_.undo_coalesce_ms) * 1000) {
            push_undo(last_);
            redo_n_ = 0;
        }
        last_change_us_ = now_us;
        for (int d = 0; d < kDecks; ++d)
            if (!same_look(cur_.deck[d], last_.deck[d]) && deck_scene_[d] >= 0) status_dirty_ = true;
    }
    last_ = cur_;
}

void ShowRuntime::sync(engine::DeckModel& m) {
    snapshot_from_live(m.live(), m.transport(), m.catalog(), last_);
    have_last_ = true;
    cur_ = last_;
    ++live_gen_;
    status_dirty_ = true;
}

void ShowRuntime::push_undo(const LiveSnapshot& s) {
    undo_[undo_head_] = s;
    undo_head_ = (undo_head_ + 1) % kUndoDepth;
    if (undo_n_ < kUndoDepth) ++undo_n_;
    status_dirty_ = true;
}

void ShowRuntime::start_morph(int deck, const Look& from, const Look& to, float seconds, uint64_t now_us, uint32_t mask,
                              bool deck_fx) {
    Morph& mo = morph_[deck];
    morph_from_[deck] = from;
    morph_to_[deck] = to;
    mo.active = true;
    mo.layer_mask = mask;
    mo.deck_fx = deck_fx;
    mo.t0 = now_us;
    mo.dur_us = static_cast<uint64_t>(std::max(0.01f, seconds) * 1e6f);
    mo.clips_sent = 0;
    mo.progress = 0.0f;
    status_dirty_ = true;
}

void ShowRuntime::tick_morphs(engine::DeckModel& m, uint64_t now_us) {
    if (outbox_head_ < outbox_->n) return;  // the last step is still going out: no new one on top of it
    for (int d = 0; d < kDecks; ++d) {
        Morph& mo = morph_[d];
        if (!mo.active) continue;
        const float p = clamp01(static_cast<float>(static_cast<double>(now_us - mo.t0) / static_cast<double>(mo.dur_us)));
        mo.progress = p;
        Look at;
        lerp_look(morph_from_[d], morph_to_[d], p, at);
        // Everything but the clips each frame; the clip switch once (lerp_look switches it at the middle).
        apply_look(m, d, at, now_us, mo.layer_mask, mo.deck_fx, kDiffMix | kDiffTransport | kDiffFx);
        if (p >= 0.5f) {
            const uint32_t todo = mo.layer_mask & ~mo.clips_sent;
            if (todo) apply_look(m, d, at, now_us, todo, false, kDiffClips);
            mo.clips_sent |= todo;
        }
        if (p >= 1.0f) {
            mo.active = false;
            status_dirty_ = true;
        }
    }
}

int ShowRuntime::pad_of(int scene) const {
    const int main = main_of(*front_, scene);
    if (main < 0) return -1;
    for (int p = 0; p < kPads; ++p)
        if (page_scene(*front_, p) == main) return p;
    return -1;
}

int ShowRuntime::page_next(int from, int step) const {
    for (int p = from + step; p >= 0 && p < kPads; p += step)
        if (page_scene(*front_, p) >= 0) return p;
    return -1;
}

Scene* ShowRuntime::deck_scene_ptr(int deck) {
    const int id = deck >= 0 && deck < kDecks ? deck_scene_[deck] : -1;
    return id >= 0 && front_->scene[id].used ? &front_->scene[id] : nullptr;
}

void ShowRuntime::recall(engine::DeckModel& m, int deck, int scene, float morph_s, uint64_t now_us) {
    Scene& s = front_->scene[scene];
    Look target = s.look;
    bind_clips(target, m.catalog());  // names -> ids for this catalog (a relinked clip comes back)
    drop_outbox();  // a new look replaces whatever was still on its way
    capture(m, now_us);
    push_undo(last_);
    redo_n_ = 0;
    if (morph_s < 0.0f)
        morph_s = s.transition.kind == static_cast<uint8_t>(TransitionKind::Morph) ? s.transition.seconds : 0.0f;
    const uint64_t t0 = steady_us();
    if (morph_s > 0.0f) {
        Look from;
        look_from_live(m.live(), m.transport(), m.catalog(), deck, from);
        start_morph(deck, from, target, morph_s, now_us, 0xF, true);
        tick_morphs(m, now_us);
        last_recall_cmds_ = 0;
    } else {
        morph_[deck].active = false;
        Look cur;
        look_from_live(m.live(), m.transport(), m.catalog(), deck, cur);
        scratch_->clear();
        diff_look(cur, target, deck, m.catalog(), *scratch_);
        last_recall_cmds_ = scratch_->n;
        send(m, now_us);
    }
    last_recall_us_ = static_cast<uint32_t>(steady_us() - t0);
    sync(m);
    deck_scene_[deck] = static_cast<int16_t>(scene);
    recalled_[deck] = target;
    const int pad = pad_of(scene);
    if (pad >= 0) last_pad_ = pad;
    hist_n_[deck] = hist_pos_[deck] = 0;  // a recalled scene starts a new dice history
    message("%s %c on %c", s.name[0] ? s.name : "Scene", 'a' + s.variation, 'A' + deck);
}

void ShowRuntime::apply_macro(engine::DeckModel& m, int macro, uint64_t now_us) {
    const int pd = m.live().program_deck;
    Scene* s = deck_scene_ptr(pd);
    const float v = s ? s->macro[macro].value : macro_fallback_[macro];
    scratch_->clear();
    bool bound = false;
    if (s)
        for (const auto& b : s->macro[macro].bind) {
            if (!b.used) continue;
            bound = true;
            const uint8_t layer = b.layer == kNone ? kNone : b.layer;
            scratch_->push(make_fx(CommandType::FxParam, static_cast<uint8_t>(pd), layer, b.slot, b.param, 0, b.min + (b.max - b.min) * v));
        }
    if (!bound) {
        // GUESS: unbound defaults until "Fill macros" exists: INTENSITY = every layer effect's mix,
        // COLOUR / SPACE = the colour / motion params of the deck's effects, MASTER FX = the master mix.
        const LiveState& live = m.live();
        const Catalog& cat = m.catalog();
        auto each = [&](uint8_t layer, int slot, const FxSlotState& f) {
            if (f.effect < 0 || f.effect >= static_cast<int>(cat.effect_count)) return;
            const EffectInfo& e = cat.effect[f.effect];
            if (macro == 0) scratch_->push(make_fx(CommandType::FxMix, static_cast<uint8_t>(pd), layer, static_cast<uint8_t>(slot), kNone, 0, v));
            for (int p = 0; p < e.param_count && macro > 0; ++p)
                if (param_lock(e.param_name[p]) == (macro == 1 ? kLockColour : kLockMotion))
                    scratch_->push(make_fx(CommandType::FxParam, static_cast<uint8_t>(pd), layer, static_cast<uint8_t>(slot), static_cast<uint8_t>(p), 0, v));
        };
        if (macro < 3) {
            for (int l = 0; l < kLayers; ++l)
                for (int f = 0; f < kFxSlots; ++f) each(static_cast<uint8_t>(l), f, live.deck[pd].layer[l].fx[f]);
            if (macro > 0)
                for (int f = 0; f < kFxSlots; ++f) each(kNone, f, live.deck[pd].fx[f]);
        } else {
            for (int f = 0; f < kFxSlots; ++f)
                if (live.master_fx[f].effect >= 0)
                    scratch_->push(make_fx(CommandType::FxMix, kMasterDeck, kNone, static_cast<uint8_t>(f), kNone, 0, v));
        }
    }
    send(m, now_us);
}

bool ShowRuntime::want(Req kind, int producer, int32_t i, const char* text) {
    if (!w_) return false;
    Request r;
    r.kind = kind;
    r.producer = static_cast<uint8_t>(producer);
    r.i = i;
    copy_str(r.text, kPathLen, text);
    const bool needs_data = kind == Req::Save || kind == Req::SaveAs || kind == Req::Export || kind == Req::Autosave;
    if (!needs_data) return w_->push(r);
    if (pending_n_ >= kPendingData) return false;
    pending_[pending_n_++] = r;
    return true;
}

void ShowRuntime::fill_stage(engine::DeckModel& m) {
    // A memcpy of the show (about 1 MB) when the worker asks for data: saves and
    // the coalesced autosave only, never every frame.
    *w_->stage = *front_;
    snapshot_from_live(m.live(), m.transport(), m.catalog(), w_->stage_live);
}

// ============================================================================
// the show commands
// ============================================================================
Reject ShowRuntime::submit(int producer, const Command& c, uint64_t now_us, engine::DeckModel& m) {
    if (producer < 0 || producer >= kProducers) return Reject::BadTarget;
    for (int k = 0; k < 4; ++k)
        if (!finite(c.f[k])) {
            ++status_.rejected[producer];
            status_.last_reject[producer] = static_cast<uint8_t>(Reject::BadValue);
            return Reject::BadValue;
        }
    const bool known = is_show_command(c.type) && static_cast<uint16_t>(c.type) < static_cast<uint16_t>(CommandType::Count);
    const Reject r = known ? do_command(producer, c, now_us, m) : Reject::UnknownType;
    if (r == Reject::None) {
        status_.acked_seq[producer] = c.seq;
    } else {
        ++status_.rejected[producer];
        status_.last_reject[producer] = static_cast<uint8_t>(r);
    }
    status_dirty_ = true;
    return r;
}

Reject ShowRuntime::do_command(int producer, const Command& c, uint64_t now_us, engine::DeckModel& m) {
    ShowData& sh = *front_;
    const bool deck_ok = c.deck < kDecks;
    switch (c.type) {
        case CommandType::SceneRecall: {
            if (!deck_ok) return Reject::BadTarget;
            int pad = c.i;
            if (pad == kSceneNext || pad == kScenePrev) {
                const int step = pad == kSceneNext ? 1 : -1;
                int base = last_pad_ >= 0 ? last_pad_ : pad_of(deck_scene_[c.deck]);
                pad = page_next(base, step);
                if (pad < 0 && step > 0 && sh.use_setlist && sh.song + 1 < sh.setlist_count) {
                    ++sh.song;  // the end of a song: on to the next song's first scene
                    changed();
                    pad = page_next(-1, 1);
                } else if (pad < 0 && step > 0 && !sh.use_setlist) {
                    pad = page_next(-1, 1);  // a bank wraps
                }
                if (pad < 0) return Reject::BadId;
            }
            if (pad < 0 || pad >= kPads) return Reject::BadTarget;
            const int main = page_scene(sh, pad);
            if (main < 0) return Reject::BadId;
            int scene = main;
            if (c.slot != kNone) {
                int v[kMaxVariations];
                const int n = variations_of(sh, main, v);
                if (c.slot >= n) return Reject::BadId;
                scene = v[c.slot];
            }
            recall(m, c.deck, scene, c.f[0], now_us);
            return Reject::None;
        }
        case CommandType::SceneSave: {
            if (!deck_ok) return Reject::BadTarget;
            if (c.i < -1 || c.i >= kPads) return Reject::BadTarget;
            Look look;
            look_from_live(m.live(), m.transport(), m.catalog(), c.deck, look);
            const Scene* from = deck_scene_ptr(c.deck);
            const int src = deck_scene_[c.deck];
            int id = -1;
            if ((c.flags & kVariation) && from) {
                const int main = main_of(sh, src);
                int v[kMaxVariations];
                const int n = variations_of(sh, main, v);
                if (n >= kMaxVariations) return Reject::BadValue;
                id = alloc_scene(sh);
                if (id < 0) return Reject::BadValue;
                Scene& s = sh.scene[id];
                s = sh.scene[main];
                s.parent = static_cast<int16_t>(main);
                // The lowest free letter (a deleted variation leaves a gap).
                int letter = 1;
                for (bool taken = true; taken && letter < kMaxVariations; ) {
                    taken = false;
                    for (int k = 0; k < n; ++k) taken = taken || sh.scene[v[k]].variation == letter;
                    if (taken) ++letter;
                }
                s.variation = static_cast<uint8_t>(letter);
                s.look = look;
                std::snprintf(s.name, sizeof(s.name), "%.20s", sh.scene[main].name);
                const int pad = pad_of(main);
                message("%d%c saved", pad >= 0 ? pad + 1 : 0, 'a' + letter);
            } else {
                const bool song = sh.use_setlist && sh.song >= 0 && sh.song < sh.setlist_count;
                int pad = c.i;
                if (pad < 0)
                    for (int p = 0; p < kPads && pad < 0; ++p)
                        if (page_scene(sh, p) < 0) pad = p;
                if (pad < 0) return Reject::BadValue;  // the page is full
                const int existing = page_scene(sh, pad);
                if (existing >= 0) {
                    id = existing;
                    sh.scene[id].look = look;  // REC + an occupied pad replaces its look (kit, macros, variations stay)
                } else {
                    id = alloc_scene(sh);
                    if (id < 0) return Reject::BadValue;
                    Scene& s = sh.scene[id];
                    if (from) {
                        s = *from;
                    } else {
                        clear_scene(s);
                    }
                    s.used = 1;
                    s.parent = -1;
                    s.variation = 0;
                    s.look = look;
                    s.pad = static_cast<uint8_t>(pad);
                    if (song) {
                        const int so = sh.setlist[sh.song];
                        s.owner = static_cast<uint8_t>(Owner::Song);
                        s.owner_index = static_cast<uint8_t>(so);
                        sh.songs[so].scene[pad] = static_cast<int16_t>(id);
                        sh.songs[so].scene_count = static_cast<uint8_t>(std::max<int>(sh.songs[so].scene_count, pad + 1));
                        std::snprintf(s.name, sizeof(s.name), "%.16s %d", sh.songs[so].name[0] ? sh.songs[so].name : "Song", pad + 1);
                    } else {
                        s.owner = static_cast<uint8_t>(Owner::Bank);
                        s.owner_index = sh.bank;
                        sh.bank_pad[sh.bank][pad] = static_cast<int16_t>(id);
                        std::snprintf(s.name, sizeof(s.name), "Scene %02d", sh.bank * kPads + pad + 1);
                    }
                }
                last_pad_ = pad;
                message("%d saved", pad + 1);
            }
            deck_scene_[c.deck] = static_cast<int16_t>(id);
            recalled_[c.deck] = look;
            changed();
            return Reject::None;
        }
        case CommandType::Undo: {
            if (c.i != 0 && c.i != 1) return Reject::BadValue;
            drop_outbox();
            capture(m, now_us);
            morph_[0].active = morph_[1].active = false;
            if (c.i == 0) {
                if (undo_n_ == 0) {
                    message("Nothing to undo");
                    return Reject::None;
                }
                undo_head_ = (undo_head_ + kUndoDepth - 1) % kUndoDepth;
                --undo_n_;
                if (redo_n_ < kUndoDepth) redo_[redo_n_++] = last_;
                apply_snapshot(m, undo_[undo_head_], now_us, false);
                message("Undo (%d left)", undo_n_);
            } else {
                if (redo_n_ == 0) {
                    message("Nothing to redo");
                    return Reject::None;
                }
                const LiveSnapshot target = redo_[--redo_n_];
                push_undo(last_);
                apply_snapshot(m, target, now_us, false);
                message("Redo");
            }
            sync(m);
            last_change_us_ = 0;  // the next edit is its own step
            return Reject::None;
        }
        case CommandType::SceneReset: {
            if (!deck_ok) return Reject::BadTarget;
            if (c.layer != kNone && c.layer >= kLayers) return Reject::BadTarget;
            if (deck_scene_[c.deck] < 0) return Reject::BadTarget;
            drop_outbox();
            capture(m, now_us);
            push_undo(last_);
            redo_n_ = 0;
            const float glide = c.f[0] > 0.0f ? c.f[0] : static_cast<float>(60.0 / std::max(20.0, m.bpm()));  // 1 beat
            Look from;
            look_from_live(m.live(), m.transport(), m.catalog(), c.deck, from);
            const uint32_t mask = c.layer == kNone ? 0xFu : (1u << c.layer);
            start_morph(c.deck, from, recalled_[c.deck], std::min(glide, 60.0f), now_us, mask, c.layer == kNone);
            message(c.layer == kNone ? "Reset scene" : "Reset layer %d", c.layer + 1);
            return Reject::None;
        }
        case CommandType::Dice: {
            if (!deck_ok) return Reject::BadTarget;
            if (c.param >= static_cast<uint8_t>(DiceVerb::Count)) return Reject::BadValue;
            if (c.layer != kNone && c.layer >= kLayers) return Reject::BadTarget;
            if (c.slot != kNone && c.slot >= kFxSlots) return Reject::BadTarget;
            const auto verb = static_cast<DiceVerb>(c.param);
            const int d = c.deck;
            Look* h = &hist_[d * kDiceHistory];
            if (verb == DiceVerb::Rotate) {
                const int step = c.i < 0 ? -1 : 1;
                const int cur = deck_scene_[d];
                int target = -1;
                if (cur >= 0) {
                    int v[kMaxVariations];
                    const int n = variations_of(sh, main_of(sh, cur), v);
                    int at = 0;
                    for (int k = 0; k < n; ++k)
                        if (v[k] == cur) at = k;
                    if (n > 1 && at + step >= 0 && at + step < n) target = v[at + step];
                }
                if (target < 0) {  // no (more) variations: the next / previous scene of the page
                    int base = cur >= 0 ? pad_of(cur) : last_pad_;
                    int pad = page_next(base, step);
                    if (pad < 0) pad = page_next(step > 0 ? -1 : kPads, step);
                    if (pad < 0) return Reject::BadId;
                    target = page_scene(sh, pad);
                    if (step < 0) {  // backwards lands on the last variation
                        int v[kMaxVariations];
                        const int n = variations_of(sh, target, v);
                        target = v[n - 1];
                    }
                }
                recall(m, d, target, 0.0f, now_us);
                return Reject::None;
            }
            Look cur;
            look_from_live(m.live(), m.transport(), m.catalog(), d, cur);
            Look next;
            bool roll = verb == DiceVerb::Roll || verb == DiceVerb::Mutate;
            if (verb == DiceVerb::Back) {
                if (hist_pos_[d] <= 0) return Reject::None;
                next = h[--hist_pos_[d]];
            } else if (verb == DiceVerb::Forward) {
                if (hist_pos_[d] + 1 < hist_n_[d]) next = h[++hist_pos_[d]];
                else roll = true;  // '>' at the end of the history = a new roll
            }
            if (roll) {
                const uint64_t seed = c.i ? static_cast<uint64_t>(static_cast<uint32_t>(c.i)) : mix64(now_us ^ (++dice_counter_ << 32));
                DiceScope scope;
                scope.layer = c.layer == kNone ? -1 : c.layer;
                scope.slot = c.slot == kNone ? -1 : c.slot;
                if (verb == DiceVerb::Mutate) dice_mutate(cur, next, m.catalog(), sh.locks, scope, seed, c.f[0]);
                else dice_roll(cur, next, m.catalog(), sh.locks, scope, seed);
                if (hist_n_[d] == 0) h[hist_n_[d]++] = cur, hist_pos_[d] = 0;  // the look before the first roll
                hist_n_[d] = hist_pos_[d] + 1;                                 // a roll in the middle drops the forward part
                if (hist_n_[d] >= kDiceHistory) {                              // full: forget the oldest
                    std::memmove(static_cast<void*>(h), h + 1, sizeof(Look) * (kDiceHistory - 1));
                    --hist_n_[d];
                }
                h[hist_n_[d]] = next;
                hist_pos_[d] = hist_n_[d]++;
            }
            drop_outbox();
            capture(m, now_us);
            push_undo(last_);
            redo_n_ = 0;
            morph_[d].active = false;
            const uint64_t t0 = steady_us();
            apply_look(m, d, next, now_us);
            last_recall_us_ = static_cast<uint32_t>(steady_us() - t0);
            sync(m);
            message("Dice %u/%u on %c", static_cast<unsigned>(hist_pos_[d]), static_cast<unsigned>(hist_n_[d] - 1), 'A' + d);
            return Reject::None;
        }
        case CommandType::Locks: {
            const uint32_t mask = static_cast<uint32_t>(c.i) & kLockAll;
            switch (static_cast<LockOp>(c.param)) {
                case LockOp::Set: sh.locks = mask; break;
                case LockOp::Toggle: sh.locks ^= mask; break;
                case LockOp::Clear: sh.locks &= ~mask; break;
                default: return Reject::BadValue;
            }
            changed();
            return Reject::None;
        }
        case CommandType::Project: {
            if (c.param >= static_cast<uint8_t>(ProjectAction::Count)) return Reject::BadValue;
            if (!w_) return Reject::BadTarget;  // no disk in this run
            static const Req kMap[] = {Req::New, Req::Open, Req::Save, Req::SaveAs, Req::Export, Req::Revert, Req::RestoreAutosave, Req::Relink};
            const auto a = static_cast<ProjectAction>(c.param);
            char t[kPathLen] = {};
            int32_t arg = c.i;
            // Text handshake (show-block.md §2): i = the id the producer wrote with its text.
            const bool needs_text = a == ProjectAction::SaveAs || a == ProjectAction::Relink ||
                                    (a == ProjectAction::Export && c.i != 0) || (a == ProjectAction::Open && c.slot == kNone);
            if (needs_text && (!take_text(producer, c.i, t) || !t[0])) return Reject::BadValue;
            if (a == ProjectAction::Open) {
                if (c.slot != kNone && c.slot >= kRecent) return Reject::BadId;
                arg = c.slot == kNone ? -1 : c.slot;
            }
            if (!want(kMap[c.param], producer, arg, t)) return Reject::RateLimited;
            return Reject::None;
        }
        case CommandType::ShowSelect: {
            const bool rel = (c.flags & kRelative) != 0;
            switch (static_cast<SelectWhat>(c.param)) {
                case SelectWhat::Bank: {
                    const int b = rel ? sh.bank + c.i : c.i;
                    if (b < 0 || b >= kMaxBanks) return Reject::BadValue;
                    sh.bank = static_cast<uint8_t>(b);
                    break;
                }
                case SelectWhat::Song: {
                    const int s = rel ? sh.song + c.i : c.i;
                    if (s < 0 || s >= sh.setlist_count) return Reject::BadValue;
                    sh.song = static_cast<int16_t>(s);
                    break;
                }
                case SelectWhat::UseSetlist:
                    if (c.i < 0 || c.i > 2) return Reject::BadValue;
                    sh.use_setlist = static_cast<uint8_t>(c.i == 2 ? !sh.use_setlist : c.i);
                    if (sh.use_setlist && sh.song < 0 && sh.setlist_count > 0) sh.song = 0;
                    break;
                default: return Reject::BadValue;
            }
            last_pad_ = -1;
            changed();
            return Reject::None;
        }
        case CommandType::KitSet: {
            if (!deck_ok || c.slot >= kPads) return Reject::BadTarget;
            if (c.param >= static_cast<uint8_t>(PadKind::Count)) return Reject::BadValue;
            if (c.layer != kNone && c.layer >= static_cast<uint8_t>(PadTarget::Count)) return Reject::BadValue;
            Scene* s = deck_scene_ptr(c.deck);
            if (!s) return Reject::BadTarget;  // save the deck as a scene first
            const Catalog& cat = m.catalog();
            KitPad p{};
            p.kind = c.param;
            p.target = c.layer == kNone ? 0 : c.layer;
            p.slot = kNone;
            p.id = c.i;
            switch (static_cast<PadKind>(c.param)) {
                case PadKind::Empty: p.id = -1; break;
                case PadKind::Clip:
                    if (c.i < 0 || c.i >= static_cast<int>(cat.clip_count)) return Reject::BadId;
                    copy_str(p.name, kNameLen, cat.clip[c.i].name);
                    break;
                case PadKind::Cue: {  // i = cue 0..15 of the clip of the target layer (its name is kept)
                    if (c.i < 0 || c.i >= kCuePoints) return Reject::BadId;
                    const int layer = p.target >= 1 && p.target <= 4 ? p.target - 1 : 0;
                    const int clip = s->look.layer[layer].clip;
                    p.slot = static_cast<uint8_t>(c.i);
                    p.id = clip;
                    copy_str(p.name, kNameLen, s->look.clip_name[layer]);
                    break;
                }
                case PadKind::Effect:
                    if (c.i < 0 || c.i >= static_cast<int>(cat.effect_count)) return Reject::BadId;
                    copy_str(p.name, kNameLen, cat.effect[c.i].name);
                    p.slot = static_cast<uint8_t>(kFxSlots - 1);  // GUESS: effect hits use the last FX slot of the target layer
                    break;
                case PadKind::Scene:
                    if (c.i < 0 || c.i >= kPads) return Reject::BadId;
                    break;
                case PadKind::Action:
                    if (c.i < 0 || c.i >= static_cast<int>(PadAction::Count)) return Reject::BadId;
                    break;
                default: return Reject::BadValue;
            }
            s->kit[c.slot] = p;
            changed();
            return Reject::None;
        }
        case CommandType::MacroSet: {
            if (!deck_ok || c.slot >= kMacros) return Reject::BadTarget;
            Scene* s = deck_scene_ptr(c.deck);
            if (!s) return Reject::BadTarget;
            Macro& mac = s->macro[c.slot];
            if (c.param == kNone) {  // rename from the text box
                char t[kPathLen];
                if (!take_text(producer, c.i, t) || !t[0]) return Reject::BadValue;
                copy_str(mac.name, sizeof(mac.name), t);
            } else {
                if (c.param >= kMacroBinds) return Reject::BadTarget;
                MacroBind& b = mac.bind[c.param];
                if (c.flags & kClear) {
                    b = MacroBind{};
                    b.layer = b.slot = b.param = kNone;
                } else {
                    if (c.layer != kNone && c.layer >= kLayers) return Reject::BadTarget;
                    const int fs_ = c.i / 16, fp = c.i % 16;
                    if (c.i < 0 || fs_ >= kFxSlots || fp >= kFxParams) return Reject::BadTarget;
                    b.used = 1;
                    b.layer = c.layer;
                    b.slot = static_cast<uint8_t>(fs_);
                    b.param = static_cast<uint8_t>(fp);
                    b.min = clamp01(c.f[0]);
                    b.max = clamp01(c.f[1]);
                }
            }
            changed();
            return Reject::None;
        }
        case CommandType::MacroValue: {
            if (c.slot >= kMacros) return Reject::BadTarget;
            Scene* s = deck_scene_ptr(m.live().program_deck);
            float& v = s ? s->macro[c.slot].value : macro_fallback_[c.slot];
            v = clamp01((c.flags & kRelative) ? v + c.f[0] : c.f[0]);
            apply_macro(m, c.slot, now_us);
            if (s) changed();
            return Reject::None;
        }
        case CommandType::SongEdit: {
            const auto a = static_cast<SongAction>(c.param);
            if (c.param >= static_cast<uint8_t>(SongAction::Count)) return Reject::BadValue;
            auto song_ok = [&](int s) { return s >= 0 && s < sh.setlist_count; };
            char t[kPathLen] = {};
            switch (a) {
                case SongAction::New: {
                    if (sh.setlist_count >= kMaxSongs) return Reject::BadValue;
                    int id = -1;
                    for (int k = 0; k < kMaxSongs && id < 0; ++k)
                        if (!sh.songs[k].used) id = k;
                    if (id < 0) return Reject::BadValue;
                    Song& so = sh.songs[id];
                    std::memset(&so, 0, sizeof(so));
                    for (auto& x : so.scene) x = -1;
                    so.used = 1;
                    if (c.i != 0 && !take_text(producer, c.i, t)) return Reject::BadValue;  // i = text id, 0 = "Song n"
                    if (t[0]) copy_str(so.name, kSceneNameLen, t);
                    else std::snprintf(so.name, sizeof(so.name), "Song %d", sh.setlist_count + 1);
                    sh.setlist[sh.setlist_count++] = static_cast<int16_t>(id);
                    if (sh.song < 0) sh.song = 0;
                    message("%s added", so.name);
                    break;
                }
                case SongAction::Delete: {
                    if (!song_ok(c.i)) return Reject::BadId;
                    const int id = sh.setlist[c.i];
                    for (int p = 0; p < kPads; ++p) {
                        const int sc = sh.songs[id].scene[p];
                        if (sc < 0) continue;
                        for (auto& x : sh.scene)  // the scene and its variations
                            if (x.used && (&x - sh.scene == sc || x.parent == sc)) x.used = 0;
                    }
                    sh.songs[id].used = 0;
                    for (int k = c.i; k + 1 < sh.setlist_count; ++k) sh.setlist[k] = sh.setlist[k + 1];
                    sh.setlist[--sh.setlist_count] = -1;
                    if (sh.song >= sh.setlist_count) sh.song = static_cast<int16_t>(sh.setlist_count - 1);
                    for (int d = 0; d < kDecks; ++d)
                        if (deck_scene_[d] >= 0 && !sh.scene[deck_scene_[d]].used) deck_scene_[d] = -1;
                    break;
                }
                case SongAction::AddScene: {
                    if (!song_ok(c.i)) return Reject::BadId;
                    if (!deck_ok) return Reject::BadTarget;
                    Song& so = sh.songs[sh.setlist[c.i]];
                    int pos = -1;
                    for (int p = 0; p < kPads && pos < 0; ++p)
                        if (so.scene[p] < 0) pos = p;
                    const int id = alloc_scene(sh);
                    if (pos < 0 || id < 0) return Reject::BadValue;
                    Scene& s = sh.scene[id];
                    clear_scene(s);
                    s.used = 1;
                    s.owner = static_cast<uint8_t>(Owner::Song);
                    s.owner_index = static_cast<uint8_t>(sh.setlist[c.i]);
                    s.pad = static_cast<uint8_t>(pos);
                    look_from_live(m.live(), m.transport(), m.catalog(), c.deck, s.look);
                    std::snprintf(s.name, sizeof(s.name), "%.16s %d", so.name, pos + 1);
                    so.scene[pos] = static_cast<int16_t>(id);
                    so.scene_count = static_cast<uint8_t>(std::max<int>(so.scene_count, pos + 1));
                    break;
                }
                case SongAction::RemoveScene: {
                    if (!song_ok(c.i) || c.slot >= kPads) return Reject::BadTarget;
                    Song& so = sh.songs[sh.setlist[c.i]];
                    const int sc = so.scene[c.slot];
                    if (sc < 0) return Reject::BadId;
                    for (auto& x : sh.scene)
                        if (x.used && (&x - sh.scene == sc || x.parent == sc)) x.used = 0;
                    so.scene[c.slot] = -1;
                    for (int d = 0; d < kDecks; ++d)
                        if (deck_scene_[d] >= 0 && !sh.scene[deck_scene_[d]].used) deck_scene_[d] = -1;
                    break;
                }
                case SongAction::Bpm:
                    if (!song_ok(c.i)) return Reject::BadId;
                    sh.songs[sh.setlist[c.i]].bpm = c.f[0] <= 0.0f ? 0.0f : std::clamp(c.f[0], 20.0f, 300.0f);
                    break;
                case SongAction::Rename:
                    if (c.slot == kNone || !song_ok(c.slot)) return Reject::BadId;
                    if (!take_text(producer, c.i, t) || !t[0]) return Reject::BadValue;
                    copy_str(sh.songs[sh.setlist[c.slot]].name, kSceneNameLen, t);
                    break;
                case SongAction::Move: {
                    if (!song_ok(c.i) || c.slot >= sh.setlist_count) return Reject::BadTarget;
                    const int16_t id = sh.setlist[c.i];
                    const int16_t cur_song = sh.song >= 0 ? sh.setlist[sh.song] : -1;
                    for (int k = c.i; k + 1 < sh.setlist_count; ++k) sh.setlist[k] = sh.setlist[k + 1];
                    for (int k = sh.setlist_count - 1; k > c.slot; --k) sh.setlist[k] = sh.setlist[k - 1];
                    sh.setlist[c.slot] = id;
                    for (int k = 0; k < sh.setlist_count; ++k)
                        if (sh.setlist[k] == cur_song) sh.song = static_cast<int16_t>(k);
                    break;
                }
                case SongAction::RenameScene: {
                    const int sc = c.slot == kNone ? -1 : page_scene(sh, c.slot);
                    if (sc < 0) return Reject::BadId;
                    if (!take_text(producer, c.i, t) || !t[0]) return Reject::BadValue;
                    copy_str(sh.scene[sc].name, kSceneNameLen, t);
                    break;
                }
                default: return Reject::BadValue;
            }
            changed();
            return Reject::None;
        }
        default: return Reject::UnknownType;
    }
}

// ============================================================================
// per frame
// ============================================================================
void ShowRuntime::take_loaded(engine::DeckModel& m, uint64_t now_us) {
    if (!w_ || !w_->load_ready.load(std::memory_order_acquire)) return;
    drop_outbox();
    std::swap(front_, w_->load);  // the old show goes back to the worker as its next load buffer
    ++epoch_;
    const Apply a = w_->load_apply;
    const LiveSnapshot& lv = w_->load_live;
    if (w_->load_have_live) {
        if ((a == Apply::IfEmpty && model_empty(m.live())) || a == Apply::Full) {
            apply_snapshot(m, lv, now_us, true);
        } else if (a == Apply::Preview) {
            // Switching project mid-show: its look comes into PREVIEW, the room sees nothing (projects.md §3).
            const int preview = 1 - m.live().program_deck;
            apply_look(m, preview, lv.deck[lv.program_deck], now_us);
        }
    }
    deck_scene_[0] = deck_scene_[1] = -1;
    morph_[0].active = morph_[1].active = false;
    hist_n_[0] = hist_n_[1] = hist_pos_[0] = hist_pos_[1] = 0;
    undo_n_ = redo_n_ = 0;
    last_pad_ = -1;
    sync(m);
    ++show_gen_;
    if (w_->load_clean) saved_show_gen_ = show_gen_, saved_live_gen_ = live_gen_;
    autosaved_show_gen_ = show_gen_;
    have_journaled_ = false;
    status_dirty_ = true;
    w_->load_ready.store(0, std::memory_order_release);
}

void ShowRuntime::write_status(const engine::DeckModel& m, uint64_t now_us) {
    ShowStatus& s = status_;
    const ShowData& sh = *front_;
    s.time_us = now_us;
    copy_str(s.project, kNameLen, sh.name);
    s.dirty = dirty() ? 1 : 0;
    s.busy = w_ && (w_->busy.load() || pending_n_ || w_->req->size()) ? 1 : 0;
    s.use_setlist = sh.use_setlist;
    s.bank = sh.bank;
    s.song = sh.song;
    s.song_count = sh.setlist_count;
    const int so = sh.song >= 0 && sh.song < sh.setlist_count ? sh.setlist[sh.song] : -1;
    copy_str(s.song_name, kSceneNameLen, so >= 0 ? sh.songs[so].name : "");
    s.song_bpm = so >= 0 ? sh.songs[so].bpm : 0.0f;
    const Catalog& cat = m.catalog();
    for (int p = 0; p < kPads; ++p) {
        ScenePadInfo& pi = s.pad[p];
        std::memset(&pi, 0, sizeof(pi));
        const int id = page_scene(sh, p);
        if (id < 0) continue;
        pi.used = 1;
        copy_str(pi.name, kSceneNameLen, sh.scene[id].name);
        int v[kMaxVariations];
        pi.variations = static_cast<uint8_t>(variations_of(sh, id, v) - 1);
        for (int d = 0; d < kDecks; ++d)
            if (deck_scene_[d] >= 0 && main_of(sh, deck_scene_[d]) == id) pi.on_deck |= static_cast<uint8_t>(1u << d);
        const int clip = sh.scene[id].look.layer[0].clip;
        pi.thumb_rgb = clip >= 0 && clip < static_cast<int>(cat.clip_count) ? cat.clip[clip].thumb_rgb : 0;
    }
    for (int d = 0; d < kDecks; ++d) {
        DeckSceneInfo& di = s.deck[d];
        const int id = deck_scene_[d];
        di.scene = static_cast<int16_t>(id);
        di.pad = static_cast<int8_t>(id >= 0 ? pad_of(id) : -1);
        di.variation = id >= 0 ? static_cast<char>('a' + sh.scene[id].variation) : 0;
        di.edited = id >= 0 && have_last_ && !same_look(last_.deck[d], recalled_[d]) ? 1 : 0;
        di.morphing = morph_[d].active ? 1 : 0;
        di.morph_progress = morph_[d].progress;
        di.dice_pos = static_cast<uint16_t>(hist_pos_[d]);
        di.dice_count = static_cast<uint16_t>(hist_n_[d] ? hist_n_[d] - 1 : 0);
    }
    s.locks = sh.locks;
    s.undo_depth = static_cast<uint16_t>(undo_n_);
    s.redo_depth = static_cast<uint16_t>(redo_n_);
    const int pd = m.live().program_deck < kDecks ? m.live().program_deck : 0;
    const Scene* ps = deck_scene_[pd] >= 0 ? &sh.scene[deck_scene_[pd]] : nullptr;
    static const char* kDefault[kMacros] = {"INTENSITY", "COLOUR", "SPACE", "MASTER FX"};
    for (int k = 0; k < kMacros; ++k) {
        s.macro[k] = ps ? ps->macro[k].value : macro_fallback_[k];
        copy_str(s.macro_name[k], sizeof(s.macro_name[k]), ps ? ps->macro[k].name : kDefault[k]);
    }
    s.last_recall_us = last_recall_us_;
    s.scene_count = static_cast<uint32_t>(scene_count(sh));
    if (block_) seq_write(block_->status_seq, block_->status, s);
}

void ShowRuntime::tick(engine::DeckModel& m, uint64_t now_us) {
    if (first_tick_) {
        first_tick_ = false;
        sync(m);
        saved_live_gen_ = live_gen_;
    }
    take_loaded(m, now_us);
    flush(m, now_us);  // what did not fit the last frame
    tick_morphs(m, now_us);
    capture(m, now_us);

    if (w_) {
        // Worker events: messages, saves (clear the dirty dot when nothing changed since the copy).
        Event e;
        while (w_->ev->pop(e)) {
            if (e.kind == Ev::Saved) {
                saved_show_gen_ = e.show_gen;
                saved_live_gen_ = e.live_gen;
                if (std::strcmp(front_->name, e.text) != 0) copy_str(front_->name, kNameLen, e.text), ++show_gen_, ++saved_show_gen_;
                message("Saved %s", e.text);
            } else {
                message("%s", e.text);
            }
        }
        // Show changes -> autosave/show.json (coalesced, at most 4 per second).
        const bool ready = w_->ready.load(std::memory_order_acquire) && !w_->load_ready.load(std::memory_order_acquire);
        if (ready && show_gen_ != autosaved_show_gen_ && now_us - last_autosave_us_ >= 250000) {
            bool queued = false;
            for (int k = 0; k < pending_n_; ++k) queued = queued || pending_[k].kind == Req::Autosave;
            if (!queued && want(Req::Autosave, kShowSlot, 0, "")) {
                autosaved_show_gen_ = show_gen_;
                last_autosave_us_ = now_us;
            }
        }
        // Requests that need a copy of the show: one per free stage buffer.
        if (pending_n_ > 0 && !w_->stage_full.load(std::memory_order_acquire)) {
            Request r = pending_[0];
            for (int k = 1; k < pending_n_; ++k) pending_[k - 1] = pending_[k];
            --pending_n_;
            fill_stage(m);
            r.show_gen = show_gen_;
            r.live_gen = live_gen_;
            r.epoch = epoch_;
            w_->stage_full.store(1, std::memory_order_release);
            w_->push(r);
        }
        // Live changes -> the journal (at most every journal_min_ms).
        if (ready && now_us - last_journal_us_ >= static_cast<uint64_t>(cfg_.journal_min_ms) * 1000 &&
            (!have_journaled_ || !same_edit(cur_, journaled_) || !same_mixer(cur_, journaled_))) {
            LiveRec rec;  // ~4 KB on the stack, copied into the preallocated ring slot
            rec.unix_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            rec.snap = cur_;
            rec.epoch = epoch_;
            if (w_->live->push(rec)) {
                ++w_->recs_wanted;
                journaled_ = cur_;
                have_journaled_ = true;
                last_journal_us_ = now_us;
            }
        }
    }
    if (status_dirty_ || now_us >= status_next_us_) {
        write_status(m, now_us);
        status_dirty_ = false;
        status_next_us_ = now_us + 250000;
    }
    frame_sent_ = 0;  // the next frame (its drain, then its tick) gets a fresh cap
}

}  // namespace himmel::show
