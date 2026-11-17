// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "panels_link.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>

#include "himmel/library/library_db.h"
#include "himmel/weapon/shared.h"
#include "himmel/ui/panels_json.h"
#include "himmel/ui/shm_win.h"

namespace himmel::ui {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace himmel::protocol;

namespace {

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// One optional block: its view, its status word and a heartbeat watch.
struct Link {
    const wchar_t* name;
    size_t size;
    uint32_t magic, version;
    bool writable;
    ShmView view;
    ShmStatus st = ShmStatus::Absent;
    uint64_t last_beat = 0;
    uint64_t beat_seen_us = 0;   // bridge clock when the beat last moved
    bool seen = false;

    void recheck() {
        if (st == ShmStatus::Ok && view.check(size, magic, version) != ShmStatus::Ok) {
            view.close();
            st = ShmStatus::Version;
        }
    }
    void retry() {
        if (st == ShmStatus::Ok) return;
        st = view.open_existing(name, size, magic, version, writable);
        if (st != ShmStatus::Ok) view.close();
        seen = false;
    }
    // The writer is alive when its heartbeat moved in the last 1.5 s.
    bool alive(uint64_t beat, uint64_t now) {
        if (beat != last_beat) {
            last_beat = beat;
            beat_seen_us = now;
            seen = true;
        }
        return seen && now - beat_seen_us < 1500000;
    }
    std::string absent_json(const char* panel) const {
        const char* word = st == ShmStatus::Version ? "version" : st == ShmStatus::Starting ? "starting" : "absent";
        return dump(json{{"present", false}, {"state", word}, {"panel", panel}});
    }
};

struct Panels {
    std::shared_mutex views;   // unique: poller (re)opening; shared: readers / the bridge's writers
    bool watch = false;
    fs::path library_root, exe_dir;
    Link show{kShowShmName, sizeof(ShowBlock), kShowMagic, kShowVersion, true, {}};
    Link outputs{kOutputsShmName, sizeof(OutputsBlock), kOutputsMagic, kOutputsVersion, false, {}};
    Link audio{audio::kBlockName, sizeof(audio::AudioBlock), audio::kMagic, audio::kVersion, true, {}};
    Link osc{osc::kOscShmName, sizeof(osc::OscBlock), osc::kOscMagic, osc::kOscVersion, false, {}};
    Link library{library::kLibraryShmName, sizeof(library::LibraryBlock), library::kLibraryMagic,
                 library::kLibraryVersion, true, {}};   // v2: the bridge writes its ingest command channel (Gui = 1)
    uint32_t ingest_seq = 0;        // the bridge's running ingest seq (under write_m)
    json ingest_replies = json::array();   // the last replies, newest first (under write_m)
    std::mutex write_m;        // the bridge's own writes (text box, audio config): one writer
    uint64_t weapon_beat[weapon::kMaxSlots + 1] = {};   // last heartbeat per weapon slot (alive = it moved in 1 s)

    std::mutex dev_m;
    json devices;
    std::chrono::steady_clock::time_point devices_at{};
};
Panels* g = nullptr;

uint64_t steady_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

void panels_init(bool watch, const fs::path& library_root, const fs::path& exe_dir) {
    g = new Panels();
    g->watch = watch;
    g->library_root = library_root;
    g->exe_dir = exe_dir;
    if (watch) g->show.writable = g->audio.writable = g->library.writable = false;
}

void panels_close() {
    if (!g) return;
    std::unique_lock vl(g->views);
    for (Link* l : {&g->show, &g->outputs, &g->audio, &g->osc, &g->library}) l->view.close();
}

PanelJson panels_poll(uint64_t now_us, bool retry) {
    PanelJson out;
    if (!g) return out;
    {
        std::unique_lock vl(g->views);
        for (Link* l : {&g->show, &g->outputs, &g->audio, &g->osc, &g->library}) {
            l->recheck();
            if (retry) l->retry();
        }
    }
    const uint64_t now = steady_us();
    std::shared_lock vl(g->views);

    // ---- SHOW ----
    if (g->show.st == ShmStatus::Ok) {
        const auto* b = g->show.view.as<const ShowBlock>();
        auto st = std::make_unique<ShowStatus>();
        auto fl = std::make_unique<FilesStatus>();
        if (seq_read(b->status_seq, b->status, *st) && seq_read(b->files_seq, b->files, *fl)) {
            json j = show_to_json(*st, *fl, static_cast<int>(Producer::Gui));
            j["alive"] = g->show.alive(st->time_us, now);   // the runtime writes at least every 250 ms
            j["writable"] = g->show.writable;
            out.show = dump(j);
        }
    }
    if (out.show.empty() && g->show.st != ShmStatus::Ok) out.show = g->show.absent_json("show");

    // ---- OUTPUTS ----
    if (g->outputs.st == ShmStatus::Ok) {
        const auto* b = g->outputs.view.as<const OutputsBlock>();
        auto s = std::make_unique<OutputsSection>();
        if (seq_read(b->seq, b->s, *s)) {
            json j = outputs_to_json(*s);
            j["alive"] = g->outputs.alive(b->status.heartbeat_us.load(std::memory_order_acquire), now);
            out.outputs = dump(j);
        }
    } else {
        out.outputs = g->outputs.absent_json("outputs");
    }

    // ---- AUDIO ----
    if (g->audio.st == ShmStatus::Ok) {
        const auto* b = g->audio.view.as<const audio::AudioBlock>();
        audio::Signals sig{};
        audio::InputStatus in{};
        audio::ClockSource clk[audio::kClockSources]{};
        audio::AudioConfig cfg{};
        const bool ok = seq_read(b->signals_seq, b->signals, sig) && seq_read(b->input_seq, b->input, in) &&
                        seq_read(b->clock_seq, b->clock, clk) && seq_read(b->config_seq, b->config, cfg);
        if (ok) {
            json j = audio_to_json(sig, in, clk, cfg, b->status.config_gen_applied.load(std::memory_order_acquire), now_us);
            j["alive"] = g->audio.alive(b->status.heartbeat_us.load(std::memory_order_acquire), now);
            j["writable"] = g->audio.writable;
            out.audio = dump(j);
        }
    } else {
        out.audio = g->audio.absent_json("audio");
    }

    // ---- OSC ----
    if (g->osc.st == ShmStatus::Ok) {
        const auto* b = g->osc.view.as<const osc::OscBlock>();
        auto s = std::make_unique<osc::OscStatus>();
        if (seq_read(b->status_seq, b->status, *s)) {
            json j = osc_to_json(*s);
            j["alive"] = g->osc.alive(s->heartbeat_us, now);
            out.osc = dump(j);
        }
    } else {
        out.osc = g->osc.absent_json("osc");
    }

    // ---- LIBRARY (ingest worker) ----
    if (g->library.st == ShmStatus::Ok) {
        const auto* b = g->library.view.as<const library::LibraryBlock>();
        LibraryCounts c;
        c.mode = b->mode.load();
        c.paused = b->paused.load();
        c.queued = b->queued.load();
        c.working = b->working.load();
        c.ready_total = b->ready_total.load();
        c.failed_total = b->failed_total.load();
        c.pauses = b->pauses.load();
        c.catalog_full = b->catalog_full.load();
        c.ready_seq = b->ready_seq.load();
        library::IngestJob job{};
        if (!seq_read(b->job_seq, b->job, job)) job = library::IngestJob{};
        json j = library_block_to_json(c, job);
        j["alive"] = g->library.alive(b->heartbeat_us.load(std::memory_order_acquire), now);
        // v2: the ingest replies to this bridge (the GUI slot); drained here, kept for the page.
        {
            std::lock_guard wl(g->write_m);
            if (g->library.writable) {
                auto& ch = const_cast<library::LibraryBlock*>(b)->channels[static_cast<int>(Producer::Gui)];
                library::IngestReply rep{};
                while (ch.replies.pop(rep)) {
                    g->ingest_replies.insert(g->ingest_replies.begin(), ingest_reply_to_json(rep));
                    if (g->ingest_replies.size() > 8) g->ingest_replies.erase(g->ingest_replies.end() - 1);
                }
                j["cmd"] = {{"sent", g->ingest_seq}, {"done", ch.done_seq.load()}, {"rejected", ch.rejected.load()},
                            {"applying", ch.applying_seq.load()}, {"replies", g->ingest_replies}};
            }
            j["writable"] = g->library.writable;
        }
        j["root"] = utf8(g->library_root);
        out.library = dump(j);
    } else {
        json j = json::parse(g->library.absent_json("library"));
        j["root"] = utf8(g->library_root);   // the clip list still works from the DB
        out.library = dump(j);
    }
    // ---- weapon drivers (Local\HimmelWeapon1..16), once a second: name, connected, alive ----
    if (retry) {
        json ws = json::array();
        for (int slot = 1; slot <= weapon::kMaxSlots; ++slot) {
            const std::wstring name = L"Local\\HimmelWeapon" + std::to_wstring(slot);
            ShmView v;
            if (v.open_existing(name.c_str(), sizeof(weapon::SharedBlock), 0, 0, false) == ShmStatus::Absent) continue;
            const auto* b = v.as<const weapon::SharedBlock>();
            if (b->magic != weapon::kShmMagic || b->version != weapon::kShmVersion || b->size != sizeof(weapon::SharedBlock)) {
                ws.push_back({{"slot", slot}, {"state", "version"}});
                continue;
            }
            const uint64_t beat = b->status.heartbeat_us.load(std::memory_order_acquire);
            uint64_t& last = g->weapon_beat[slot];
            const bool alive = beat != 0 && beat != last;
            last = beat;
            auto txt = [](const char* p, size_t n) { size_t k = 0; while (k < n && p[k]) ++k; return std::string(p, k); };
            ws.push_back({{"slot", slot},
                          {"state", "ok"},
                          {"weapon", txt(b->header.weapon, sizeof b->header.weapon)},
                          {"name", txt(b->header.name, sizeof b->header.name)},
                          {"driver", txt(b->header.driver, sizeof b->header.driver)},
                          {"simulated", b->header.simulated != 0},
                          {"connected", b->status.connected.load() != 0},
                          {"alive", alive}});
        }
        out.weapons = dump(ws);
    }
    return out;
}

bool panels_write_show_text(uint32_t id, const std::string& text, std::string& err) {
    if (!g) return err = "not ready", false;
    if (g->watch) return err = "this bridge runs with --watch (read-only)", false;
    std::shared_lock vl(g->views);
    if (g->show.st != ShmStatus::Ok) return err = "the show is not running (Local\\HimmelShow is not there)", false;
    std::lock_guard wl(g->write_m);
    auto* b = g->show.view.as<ShowBlock>();
    write_text(b->text[static_cast<int>(Producer::Gui)], id, text.c_str());
    return true;
}

json panels_audio_config(const json& body) {
    if (!g) return {{"ok", false}, {"error", "not ready"}};
    if (g->watch) return {{"ok", false}, {"error", "this bridge runs with --watch (read-only)"}};
    std::shared_lock vl(g->views);
    if (g->audio.st != ShmStatus::Ok) return {{"ok", false}, {"error", "himmel_audio is not running"}};
    std::lock_guard wl(g->write_m);
    auto* b = g->audio.view.as<audio::AudioBlock>();
    audio::AudioConfig cfg{};
    if (!seq_read(b->config_seq, b->config, cfg)) return {{"ok", false}, {"error", "the config is busy, try again"}};
    if (cfg.gen == 0) {   // nothing requested yet: start from what himmel_audio uses now
        audio::InputStatus in{};
        if (seq_read(b->input_seq, b->input, in)) {
            std::memcpy(cfg.device, in.device, sizeof cfg.device);
            cfg.mode = in.mode;
            cfg.channel_first = in.channel_first;
            cfg.channel_count = in.channel_count ? in.channel_count : 1;
        }
        cfg.auto_gain = 1;
        cfg.gate_db = -60.0f;
        cfg.onset_sensitivity = 0.5f;
    }
    std::string err;
    if (!audio_config_from_json(body, cfg, err)) return {{"ok", false}, {"error", err}};
    ++cfg.gen;
    if (cfg.gen == 0) cfg.gen = 1;
    seq_write(b->config_seq, b->config, cfg);
    return {{"ok", true}, {"gen", cfg.gen}};
}

json panels_audio_devices() {
    if (!g) return {{"ok", false}};
    std::lock_guard dl(g->dev_m);
    const auto now = std::chrono::steady_clock::now();
    if (!g->devices.is_null() && now - g->devices_at < std::chrono::seconds(10)) return g->devices;
    // Run `himmel_audio --list` (it only lists, it does not open a device) with a 5 s limit.
    const fs::path exe = g->exe_dir / "himmel_audio.exe";
    json result = {{"ok", false}, {"error", "himmel_audio.exe not found next to himmel_ui.exe"}};
    std::error_code ec;
    if (fs::exists(exe, ec)) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (CreatePipe(&rd, &wr, &sa, 0)) {
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = wr;
            si.hStdError = wr;
            si.hStdInput = nullptr;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + exe.wstring() + L"\" --list";
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(wr);
                wr = nullptr;
                std::string text;
                char buf[4096];
                const auto t0 = std::chrono::steady_clock::now();
                for (;;) {
                    DWORD avail = 0;
                    if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // closed: done
                    if (avail) {
                        DWORD got = 0;
                        if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
                        text.append(buf, got);
                        if (text.size() > 256 * 1024) break;
                    } else if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
                        DWORD more = 0;
                        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &more, nullptr) || !more) break;
                    }
                    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
                        TerminateProcess(pi.hProcess, 1);
                        break;
                    }
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                result = parse_audio_list(text);
                result["ok"] = true;
            }
            if (wr) CloseHandle(wr);
            CloseHandle(rd);
        }
    }
    g->devices = result;
    g->devices_at = now;
    return result;
}

json panels_library_clips() {
    json out = {{"ok", false}, {"root", g ? utf8(g->library_root) : std::string()}, {"clips", json::array()}};
    if (!g) return out;
    if (g->library_root.empty()) {
        out["error"] = "no library in this run (--no-library / a test run)";
        return out;
    }
    const fs::path dbf = g->library_root / library::kDbFile;
    std::error_code ec;
    if (!fs::exists(dbf, ec)) {
        out["error"] = "no library yet (" + utf8(dbf) + ")";
        return out;
    }
    library::LibraryDb db;
    std::string err;
    if (!db.open(dbf, true, &err)) {
        out["error"] = "cannot open the library: " + err;
        return out;
    }
    auto rows = db.list(-1, 0, 2000);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    for (const auto& r : rows) {
        const auto& a = r.analysis;
        json c;
        c["id"] = r.id;
        c["hash"] = r.hash;
        c["name"] = r.name;
        c["kind"] = library::kind_name(r.kind);
        c["state"] = library::state_name(r.state);
        c["error"] = r.error;
        c["source"] = r.source;
        c["codec"] = r.codec;
        c["size"] = {r.width, r.height};
        c["fps"] = std::round(r.fps * 100.0) / 100.0;
        c["seconds"] = std::round(r.seconds * 10.0) / 10.0;
        c["alpha"] = r.probe.alpha;
        c["judder"] = r.judder;
        c["thumb"] = !r.thumb_path.empty();
        c["stars"] = r.stars;
        c["favourite"] = r.favourite;
        c["tags"] = r.user_tags;
        if (!a.motion_curve.empty()) c["curve"] = downsample_curve(a.motion_curve, 96);
        if (!a.cuts.empty() && r.seconds > 0 && r.fps > 0) {
            const double frames = r.seconds * r.fps;
            json cuts = json::array();
            for (const auto f : a.cuts)
                if (f >= 0 && f < frames) cuts.push_back(std::round(f / frames * 10000.0) / 10000.0);
            c["cuts"] = std::move(cuts);
        }
        if (a.version > 0) {
            char rgb[8];
            std::snprintf(rgb, sizeof rgb, "#%06x", a.avg_rgb & 0xFFFFFFu);
            json pal = json::array();
            for (const auto& p : a.palette) {
                char h[8];
                std::snprintf(h, sizeof h, "#%06x", p.rgb & 0xFFFFFFu);
                pal.push_back({h, std::round(p.weight * 100.0) / 100.0});
            }
            auto r2 = [](double v) { return std::round(v * 100.0) / 100.0; };
            c["analysis"] = {{"avg_rgb", rgb},
                             {"palette", pal},
                             {"brightness", r2(a.brightness)},
                             {"contrast", r2(a.contrast)},
                             {"saturation", r2(a.saturation)},
                             {"warmth", r2(a.warmth)},
                             {"motion", r2(a.motion)},
                             {"energy", a.energy},
                             {"mood", {{"chill", r2(a.mood_chill)}, {"groove", r2(a.mood_groove)},
                                       {"techno", r2(a.mood_techno)}, {"crazy", r2(a.mood_crazy)}}},
                             {"loop_style", a.loop_style},
                             {"loop_score", r2(a.loop_score)},
                             {"natural_beats", a.natural_beats},
                             {"cuts_per_min", r2(a.cuts_per_min)},
                             {"flicker", a.flicker}};
        }
        out["clips"].push_back(std::move(c));
    }
    out["ok"] = true;
    return out;
}

namespace {
bool read_text_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
fs::file_time_type mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type::min() : t;
}
}  // namespace

json panels_ingest_command(const json& body) {
    if (!g) return {{"ok", false}, {"error", "not started"}};
    library::IngestCommand c;
    std::string err;
    if (!ingest_command_from_json(body, c, err)) return {{"ok", false}, {"error", err}};
    std::shared_lock vl(g->views);
    if (!g->library.writable) return {{"ok", false}, {"error", "read-only bridge (--watch)"}};
    if (g->library.st != ShmStatus::Ok) return {{"ok", false}, {"error", "the import worker is not running (no library block v2)"}};
    auto* b = g->library.view.as<library::LibraryBlock>();
    // library-db.md §10.1: no live worker (heartbeat older than 2 s) = do not send.
    const uint64_t beat = b->heartbeat_us.load(std::memory_order_acquire);
    if (!g->library.seen || steady_us() - g->library.beat_seen_us > 2000000 || beat == 0)
        return {{"ok", false}, {"error", "the import worker does not answer (no heartbeat for 2 s)"}};
    std::lock_guard wl(g->write_m);
    const uint32_t seq = ++g->ingest_seq;
    if (!library::push_ingest(b->channels[static_cast<int>(Producer::Gui)], c, seq, GetCurrentProcessId(), steady_us()))
        return {{"ok", false}, {"error", "the import worker's queue is full, try again"}};
    return {{"ok", true}, {"seq", seq}};
}

json panels_show_document() {
    if (!g) return {{"ok", false}, {"error", "not started"}};
    std::string path;
    {
        std::shared_lock vl(g->views);
        if (g->show.st != ShmStatus::Ok) return {{"ok", false}, {"error", "no show block (the renderer is not running)"}};
        const auto* b = g->show.view.as<const ShowBlock>();
        auto fl = std::make_unique<FilesStatus>();
        if (!seq_read(b->files_seq, b->files, *fl)) return {{"ok", false}, {"error", "the show block is busy, try again"}};
        path.assign(fl->path, strnlen(fl->path, sizeof(fl->path)));
    }
    if (path.empty()) return {{"ok", false}, {"error", "no project path yet"}};
    const fs::path folder = fs::path(std::u8string(path.begin(), path.end()));
    const fs::path auto_doc = folder / "autosave" / "show.json", head_file = folder / "project.json";
    std::string text;
    json doc;
    // The autosave is written after every show change (<= 4/s); a plain save rewrites project.json.
    if (mtime(auto_doc) >= mtime(head_file) && read_text_file(auto_doc, text)) doc = json::parse(text, nullptr, false);
    if (!doc.is_object() && read_text_file(head_file, text)) {
        doc = json::parse(text, nullptr, false);
        if (doc.is_object()) {
            json banks = json::array(), songs = json::array();
            auto load = [&](const json& list, json& into) {
                if (!list.is_array()) return;
                for (const auto& rel : list) {
                    std::string t, r = rel.is_string() ? rel.get<std::string>() : std::string();
                    if (r.empty() || r.find("..") != std::string::npos) continue;
                    if (read_text_file(folder / fs::path(std::u8string(r.begin(), r.end())), t)) {
                        json x = json::parse(t, nullptr, false);
                        if (x.is_object()) into.push_back(std::move(x));
                    }
                }
            };
            load(doc.value("banks", json::array()), banks);
            load(doc.value("setlist", json::array()), songs);
            doc["banks"] = std::move(banks);
            doc["setlist"] = std::move(songs);
        }
    }
    if (!doc.is_object()) return {{"ok", false}, {"error", "no show document in " + path}};
    json j = show_doc_for_page(doc);
    j["path"] = path;
    return j;
}

std::string panels_library_thumb(const std::string& file) {
    if (!g || g->library_root.empty()) return {};
    // <16 hex>.jpg or <16 hex>-strip.jpg only.
    auto hex16 = [&](size_t n) {
        if (file.size() < 16) return false;
        for (size_t k = 0; k < 16; ++k) {
            const char ch = file[k];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        }
        return file.size() == n;
    };
    const bool ok = (hex16(20) && file.compare(16, 4, ".jpg") == 0) || (hex16(26) && file.compare(16, 10, "-strip.jpg") == 0);
    if (!ok) return {};
    std::ifstream f(g->library_root / "thumbs" / file, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string panels_library_root() { return g ? utf8(g->library_root) : std::string(); }

}  // namespace himmel::ui
