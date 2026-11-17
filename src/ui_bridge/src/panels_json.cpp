// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ui/panels_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

namespace himmel::ui {

using namespace himmel::protocol;
using nlohmann::json;

namespace {

std::string fixed(const char* p, size_t cap) {
    size_t n = 0;
    while (n < cap && p[n]) ++n;
    return std::string(p, n);
}
template <size_t N>
std::string fixed(const char (&a)[N]) { return fixed(a, N); }

double r3(double v) { return std::isfinite(v) ? std::round(v * 1000.0) / 1000.0 : 0.0; }
double r1(double v) { return std::isfinite(v) ? std::round(v * 10.0) / 10.0 : 0.0; }

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

template <size_t N>
json names_of(const char* const (&a)[N]) {
    json j = json::array();
    for (const char* s : a) j.push_back(s);
    return j;
}
template <size_t N>
const char* name_or(const char* const (&a)[N], int v) { return (v >= 0 && v < static_cast<int>(N)) ? a[v] : "?"; }

// ---- enum names (the page uses the same words) ----
const char* const kDiceNames[] = {"Roll", "Mutate", "Back", "Forward", "Rotate"};
const char* const kLockOpNames[] = {"Set", "Toggle", "Clear"};
const char* const kLockNames[] = {"Clip", "Fx", "Colour", "Motion", "Layout", "Source", "React", "Mood"};
const char* const kProjectNames[] = {"New", "Open", "Save", "SaveAs", "Export", "Revert", "RestoreAutosave", "Relink"};
const char* const kSelectNames[] = {"Bank", "Song", "UseSetlist"};
const char* const kPadKindNames[] = {"Empty", "Clip", "Cue", "Effect", "Scene", "Action"};
const char* const kPadTargetNames[] = {"Selected", "Layer1", "Layer2", "Layer3", "Layer4", "All", "Free", "Hit"};
const char* const kPadActionNames[] = {"Black", "WhiteFlash", "Freeze", "NextScene", "DiceLayer"};
const char* const kSongNames[] = {"New", "Delete", "AddScene", "RemoveScene", "Bpm", "Rename", "Move", "RenameScene"};
const char* const kRestoredNames[] = {"None", "Autosave", "InterruptedSave"};
static_assert(std::size(kDiceNames) == static_cast<size_t>(DiceVerb::Count));
static_assert(std::size(kLockOpNames) == static_cast<size_t>(LockOp::Count));
static_assert(std::size(kProjectNames) == static_cast<size_t>(ProjectAction::Count));
static_assert(std::size(kSelectNames) == static_cast<size_t>(SelectWhat::Count));
static_assert(std::size(kPadKindNames) == static_cast<size_t>(PadKind::Count));
static_assert(std::size(kPadTargetNames) == static_cast<size_t>(PadTarget::Count));
static_assert(std::size(kPadActionNames) == static_cast<size_t>(PadAction::Count));
static_assert(std::size(kSongNames) == static_cast<size_t>(SongAction::Count));
static_assert(std::size(kRestoredNames) == static_cast<size_t>(Restored::Count));

const char* const kOutStateNames[] = {"Off", "Ok", "Lost", "Window", "Other"};
const char* const kPresetNames[] = {"P720", "P1080", "P1440", "P2160", "Custom"};
const char* const kRefreshNames[] = {"Hz60", "Hz50", "Hz59_94", "Hz30", "Hz25"};
const char* const kCanvasNames[] = {"Span", "Mirror"};
const char* const kPlaceNames[] = {"FillAll", "Out", "Fit", "Free"};
const char* const kOutParamNames[] = {"Resolution", "Refresh", "BlendWidth", "BlendGamma", "Mode", "Adopt", "SimLost"};
const char* const kIdentifyNames[] = {"Off", "5s", "Hold"};
static_assert(std::size(kOutStateNames) == static_cast<size_t>(OutputState::Count));
static_assert(std::size(kPresetNames) == static_cast<size_t>(ResolutionPreset::Count));
static_assert(std::size(kRefreshNames) == static_cast<size_t>(RefreshRate::Count));
static_assert(std::size(kCanvasNames) == static_cast<size_t>(CanvasMode::Count));
static_assert(std::size(kPlaceNames) == static_cast<size_t>(Place::Count));
static_assert(std::size(kOutParamNames) == static_cast<size_t>(OutputParam::Count));

const char* const kSigNames[] = {"level", "bass", "mid", "high", "kick", "snare", "hat", "onset", "energy", "build", "drop"};
static_assert(std::size(kSigNames) == static_cast<size_t>(audio::Sig::Count));
const char* const kInputStateNames[] = {"None", "Opening", "Running", "Lost"};
const char* const kInputModeNames[] = {"Capture", "Loopback"};
const char* const kClockKindNames[] = {"None", "Midi", "ProDjLink", "Link"};
const char* const kClockRunNames[] = {"Absent", "Stopped", "Running"};
const char* const kVerdictNames[] = {"Applied", "Refused", "Blocked", "Garbage", "RateLimited"};
static_assert(std::size(kVerdictNames) == static_cast<size_t>(osc::Verdict::Count));
const char* const kIngestModeNames[] = {"Idle", "Full", "Background", "High", "Hold"};
const char* const kIngestCmdNames[] = {"Import", "Rescan", "Remove", "Relink", "Reanalyse", "Retry", "Mode", "Unwatch",
                                       "ClearQueue", "Library"};   // v3: 68, 69
static_assert(std::size(kIngestCmdNames) == library::kIngestLast - library::kIngestFirst + 1);
const char* const kIngestReqNames[] = {"Auto", "Full", "Background", "Hold"};
const char* const kIngestStatusNames[] = {"Ok", "Accepted", "UnknownType", "BadId", "BadPath", "Mismatch",
                                          "Duplicate", "Failed", "Interrupted", "BadValue", "NoSpace", "Busy"};   // v3: 10, 11
const char* const kMoveStateNames[] = {"None", "Copying", "Verifying", "Switching", "Done", "Failed"};

json locks_json(uint32_t mask) {
    json a = json::array();
    for (int k = 0; k < 8; ++k)
        if (mask & (1u << k)) a.push_back(kLockNames[k]);
    return a;
}

std::string ip_text(uint32_t ip) {
    return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

}  // namespace

// =============================================================================== blocks
json show_to_json(const ShowStatus& s, const FilesStatus& f, int producer) {
    json j;
    j["present"] = true;
    j["project"] = fixed(s.project);
    j["dirty"] = s.dirty != 0;
    j["busy"] = s.busy != 0;
    j["use_setlist"] = s.use_setlist != 0;
    j["bank"] = s.bank;
    j["song"] = s.song;
    j["song_count"] = s.song_count;
    j["song_name"] = fixed(s.song_name);
    j["song_bpm"] = r1(s.song_bpm);
    json pads = json::array();
    for (const auto& p : s.pad) {
        json pj;
        pj["used"] = p.used != 0;
        if (p.used) {
            pj["name"] = fixed(p.name);
            pj["variations"] = p.variations;
            pj["on"] = json::array({(p.on_deck & 1) != 0, (p.on_deck & 2) != 0});
            pj["rgb"] = p.thumb_rgb & 0xFFFFFFu;
        }
        pads.push_back(std::move(pj));
    }
    j["pads"] = std::move(pads);
    json decks = json::array();
    for (const auto& d : s.deck) {
        json dj;
        dj["scene"] = d.scene;
        dj["pad"] = d.pad;
        dj["variation"] = d.variation ? std::string(1, d.variation) : std::string();
        dj["edited"] = d.edited != 0;
        dj["morphing"] = d.morphing != 0;
        dj["morph"] = r3(d.morph_progress);
        dj["dice_pos"] = d.dice_pos;
        dj["dice_count"] = d.dice_count;
        decks.push_back(std::move(dj));
    }
    j["decks"] = std::move(decks);
    j["locks"] = locks_json(s.locks);
    j["undo"] = s.undo_depth;
    j["redo"] = s.redo_depth;
    json macros = json::array();
    for (int k = 0; k < 4; ++k) macros.push_back({{"name", fixed(s.macro_name[k])}, {"value", r3(s.macro[k])}});
    j["macros"] = std::move(macros);
    j["last_recall_us"] = s.last_recall_us;
    j["scene_count"] = s.scene_count;
    if (producer >= 0 && producer < kProducers) {
        j["acked"] = s.acked_seq[producer];
        j["rejected"] = s.rejected[producer];
        j["last_reject"] = reject_name(s.last_reject[producer]);
    }
    j["message"] = fixed(s.message);
    j["message_seq"] = s.message_seq;

    json fj;
    fj["path"] = fixed(f.path);
    fj["saved_unix"] = f.saved_unix;
    fj["autosave_unix"] = f.autosave_unix;
    fj["restored"] = name_or(kRestoredNames, f.restored);
    fj["save_ok"] = f.save_ok != 0;
    fj["saves"] = f.saves;
    fj["journal_records"] = f.journal_records;
    fj["relink_found"] = f.relink_found;
    fj["error"] = fixed(f.error);
    json recent = json::array();
    for (int k = 0; k < std::min<int>(f.recent_count, kRecent); ++k)
        recent.push_back({{"name", fixed(f.recent[k].name)},
                          {"path", fixed(f.recent[k].path)},
                          {"saved_unix", f.recent[k].saved_unix},
                          {"outputs", f.recent[k].outputs}});
    fj["recent"] = std::move(recent);
    fj["missing_count"] = f.missing_count;
    json missing = json::array();
    for (int k = 0; k < std::min<int>(f.missing_count, kMissingShown); ++k) missing.push_back(fixed(f.missing[k]));
    fj["missing"] = std::move(missing);
    j["files"] = std::move(fj);
    return j;
}

json outputs_to_json(const OutputsSection& s) {
    json j;
    j["present"] = true;
    j["count"] = s.count;
    j["mode"] = name_or(kCanvasNames, s.mode);
    j["refresh"] = name_or(kRefreshNames, s.refresh);
    j["fullscreen"] = s.fullscreen != 0;
    j["identify"] = s.identify != 0;
    j["identify_left_ms"] = s.identify_ms_left / 250 * 250;   // coarse, so the hub sends ~4 Hz, not every frame
    j["canvas"] = {s.canvas_w, s.canvas_h};
    j["blend_px"] = s.blend_px;
    j["blend_gamma"] = r1(s.blend_gamma);
    j["hz_target"] = r1(s.hz_target);
    j["hz_measured"] = r1(s.hz_measured);
    j["displays"] = s.displays;
    j["config_by_command"] = s.config_by_command != 0;
    json outs = json::array();
    const uint32_t n = std::min<uint32_t>(s.count, kMaxOutputs);
    for (uint32_t k = 0; k < n; ++k) {
        const OutputInfo& o = s.out[k];
        json oj;
        oj["state"] = name_or(kOutStateNames, o.state);
        oj["name"] = fixed(o.name);
        oj["display"] = {{"maker", fixed(o.id.maker)},
                         {"product", o.id.product},
                         {"serial", o.id.serial},
                         {"serial_text", fixed(o.id.serial_text)},
                         {"connector", o.id.connector}};
        oj["black"] = o.black != 0;
        oj["preset"] = name_or(kPresetNames, o.preset);
        oj["identify"] = o.identify != 0;
        oj["size"] = {o.width, o.height};
        oj["canvas_x"] = o.canvas_x;
        oj["swap"] = {o.swap_w, o.swap_h};
        oj["hz"] = r1(o.hz);
        oj["rect"] = {o.rect[0], o.rect[1], o.rect[2], o.rect[3]};
        oj["lost_count"] = o.lost_count;
        oj["returned_ms"] = o.returned_ms;
        oj["blend"] = {r1(o.blend_left), r1(o.blend_right)};
        outs.push_back(std::move(oj));
    }
    j["outputs"] = std::move(outs);
    json places = json::array();
    for (const auto& p : s.layer)
        places.push_back({{"place", name_or(kPlaceNames, p.place)},
                          {"out", p.out},
                          {"rect", {r3(p.rect[0]), r3(p.rect[1]), r3(p.rect[2]), r3(p.rect[3])}}});
    j["places"] = std::move(places);   // 0..7 = deck * 4 + layer, 8 = HIT
    return j;
}

json audio_to_json(const audio::Signals& sig, const audio::InputStatus& in,
                   const audio::ClockSource (&clock)[audio::kClockSources], const audio::AudioConfig& cfg,
                   uint32_t config_gen_applied, uint64_t now_us) {
    json j;
    j["present"] = true;
    json v;
    for (size_t k = 0; k < std::size(kSigNames); ++k) v[kSigNames[k]] = r3(sig.v[k]);
    j["sig"] = std::move(v);
    json sp = json::array();
    for (float b : sig.spectrum) sp.push_back(r3(b));
    j["spectrum"] = std::move(sp);
    j["hits"] = {sig.kicks, sig.snares, sig.hats};
    j["rms_db"] = r1(sig.rms_db);
    j["peak_db"] = r1(sig.peak_db);
    j["gain_db"] = r1(sig.gain_db);
    j["gated"] = sig.gated != 0;
    j["clipping"] = sig.clipping != 0;
    j["hop"] = sig.hop;
    j["input"] = {{"state", name_or(kInputStateNames, in.state)},
                  {"mode", name_or(kInputModeNames, in.mode)},
                  {"device", fixed(in.device)},
                  {"channel_first", in.channel_first},
                  {"channel_count", in.channel_count},
                  {"sample_rate", in.sample_rate},
                  {"device_channels", in.device_channels},
                  {"opens", in.opens},
                  {"glitches", in.glitches}};
    json clk = json::array();
    for (const auto& c : clock) {
        if (c.kind == static_cast<uint8_t>(audio::ClockKind::None)) continue;
        double beat = c.beat;
        if (c.run == static_cast<uint8_t>(audio::ClockRun::Running) && c.bpm > 0 && c.time_us && now_us > c.time_us)
            beat += static_cast<double>(now_us - c.time_us) / 1e6 * c.bpm / 60.0;
        clk.push_back({{"kind", name_or(kClockKindNames, c.kind)},
                       {"run", name_or(kClockRunNames, c.run)},
                       {"bpm", r1(c.bpm)},
                       {"confidence", r3(c.confidence)},
                       {"phase_valid", c.phase_valid != 0},
                       {"beat", std::round(beat * 100.0) / 100.0},
                       {"port", fixed(c.port)}});
    }
    j["clock"] = std::move(clk);
    j["config"] = {{"gen", cfg.gen},
                   {"applied", config_gen_applied},
                   {"mode", name_or(kInputModeNames, cfg.mode)},
                   {"device", fixed(cfg.device)},
                   {"channel_first", cfg.channel_first},
                   {"channel_count", cfg.channel_count},
                   {"auto_gain", cfg.auto_gain != 0},
                   {"gain_db", r1(cfg.gain_db)},
                   {"gate_db", r1(cfg.gate_db)},
                   {"onset_sensitivity", r3(cfg.onset_sensitivity)},
                   {"midi_clock_port", fixed(cfg.midi_clock_port)}};
    return j;
}

json osc_to_json(const osc::OscStatus& s) {
    json j;
    j["present"] = true;
    j["port"] = s.port;
    j["allow"] = fixed(s.allow);
    j["counts"] = {{"received", s.received}, {"applied", s.applied},           {"refused", s.refused},
                   {"blocked", s.blocked},   {"garbage", s.garbage},           {"rate_limited", s.rate_limited},
                   {"ring_full", s.ring_full}};
    j["total"] = s.total;
    json last = json::array();   // newest first
    const uint32_t n = std::min<uint32_t>(s.total, osc::kLastMessages);
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t idx = (s.next + osc::kLastMessages - 1 - k) % osc::kLastMessages;
        const auto& m = s.last[idx];
        last.push_back({{"unix_ms", m.unix_ms},
                        {"from", ip_text(m.from_ip) + ":" + std::to_string(m.from_port)},
                        {"verdict", name_or(kVerdictNames, m.verdict)},
                        {"reason", m.reason},
                        {"text", fixed(m.text)}});
    }
    j["last"] = std::move(last);
    return j;
}

void library_v3_to_json(json& j, const LibraryV3& v, const library::IngestProgress& p, const library::IngestPlan& plan,
                        const library::LibraryRoot& root, int my_producer) {
    j["stored_mode"] = name_or(kIngestReqNames, static_cast<int>(v.stored_mode));
    j["disk_low"] = v.disk_low != 0;
    j["disk"] = {{"free", v.disk_free}, {"total", v.disk_total}, {"reserve", v.disk_reserve},
                 {"library_bytes", v.library_bytes}, {"queue_bytes", v.queue_bytes}};
    j["progress"] = {{"done", p.done}, {"total", p.total}, {"clip", p.clip}, {"file", fixed(p.file)},
                     {"file_progress", std::round(std::clamp(p.file_progress, 0.0f, 1.0f) * 1000.0) / 1000.0},
                     {"eta_s", p.eta_s}, {"bytes_written", p.bytes_written}, {"file_bytes", p.file_bytes},
                     {"error_clip", p.error_clip}, {"error", fixed(p.error)}};
    j["plan"] = {{"seq", plan.seq}, {"mine", plan.producer == my_producer}, {"dry_run", plan.dry_run != 0},
                 {"fits", plan.fits != 0}, {"refused", plan.refused != 0}, {"clips", plan.clips}, {"known", plan.known},
                 {"fit_clips", plan.fit_clips}, {"need_bytes", plan.need_bytes}, {"queue_bytes", plan.queue_bytes},
                 {"free_bytes", plan.free_bytes}, {"reserve_bytes", plan.reserve_bytes}, {"message", fixed(plan.message)}};
    j["root_info"] = {{"path", fixed(root.path)}, {"move_state", name_or(kMoveStateNames, static_cast<int>(root.move_state))},
                      {"move_progress", std::round(std::clamp(root.move_progress, 0.0f, 1.0f) * 1000.0) / 1000.0},
                      {"move_done_bytes", root.move_done_bytes}, {"move_total_bytes", root.move_total_bytes},
                      {"move_to", fixed(root.move_to)}, {"move_message", fixed(root.move_message)}};
}

json library_block_to_json(const LibraryCounts& c, const library::IngestJob& job) {
    json j;
    j["present"] = true;
    j["mode"] = name_or(kIngestModeNames, static_cast<int>(c.mode));
    j["paused"] = c.paused != 0;
    j["queued"] = c.queued;
    j["working"] = c.working;
    j["ready_total"] = c.ready_total;
    j["failed_total"] = c.failed_total;
    j["pauses"] = c.pauses;
    j["ready_seq"] = c.ready_seq;
    j["catalog_full"] = c.catalog_full;
    j["job"] = {{"name", fixed(job.name)},
                {"stage", fixed(job.stage)},
                {"progress", std::round(std::clamp(job.progress, 0.0f, 1.0f) * 100.0) / 100.0},
                {"last_error", fixed(job.last_error)}};
    return j;
}

bool audio_config_from_json(const json& j, audio::AudioConfig& cfg, std::string& error) {
    if (!j.is_object()) {
        error = "the audio config must be a JSON object";
        return false;
    }
    audio::AudioConfig c = cfg;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        const json& v = it.value();
        auto num = [&](double lo, double hi, double& out) {
            if (!v.is_number() || !std::isfinite(v.get<double>())) {
                error = "\"" + k + "\" must be a number";
                return false;
            }
            out = std::clamp(v.get<double>(), lo, hi);
            return true;
        };
        double d = 0;
        if (k == "device" || k == "midi_clock_port") {
            if (!v.is_string()) {
                error = "\"" + k + "\" must be text";
                return false;
            }
            const std::string s = v.get<std::string>();
            char* dst = k == "device" ? c.device : c.midi_clock_port;
            const size_t cap = k == "device" ? sizeof c.device : sizeof c.midi_clock_port;
            if (s.size() >= cap) {
                error = "\"" + k + "\" is too long";
                return false;
            }
            std::memset(dst, 0, cap);
            std::memcpy(dst, s.data(), s.size());
        } else if (k == "mode") {
            int m = -1;
            if (v.is_string()) {
                for (int q = 0; q < 2; ++q)
                    if (ieq(v.get<std::string>(), kInputModeNames[q])) m = q;
            } else if (v.is_number_integer()) {
                m = v.get<int>();
            }
            if (m < 0 || m > 1) {
                error = "\"mode\" must be Capture or Loopback";
                return false;
            }
            c.mode = static_cast<uint8_t>(m);
        } else if (k == "channel_first") {
            if (!num(0, 63, d)) return false;
            c.channel_first = static_cast<uint8_t>(d);
        } else if (k == "channel_count") {
            if (!num(1, 2, d)) return false;
            c.channel_count = static_cast<uint8_t>(d);
        } else if (k == "auto_gain") {
            if (!v.is_boolean()) {
                error = "\"auto_gain\" must be true or false";
                return false;
            }
            c.auto_gain = v.get<bool>() ? 1 : 0;
        } else if (k == "gain_db") {
            if (!num(-24, 48, d)) return false;
            c.gain_db = static_cast<float>(d);
        } else if (k == "gate_db") {
            if (!num(-96, -20, d)) return false;
            c.gate_db = static_cast<float>(d);
        } else if (k == "onset_sensitivity") {
            if (!num(0, 1, d)) return false;
            c.onset_sensitivity = static_cast<float>(d);
        } else {
            error = "unknown audio config key \"" + k + "\"";
            return false;
        }
    }
    cfg = c;
    return true;
}

json parse_audio_list(const std::string& text) {
    json j = {{"capture", json::array()}, {"render", json::array()}, {"midi", json::array()}};
    std::istringstream in(text);
    std::string line;
    const char* section = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] != ' ') {
            section = line.rfind("Capture", 0) == 0 ? "capture"
                    : line.rfind("Render", 0) == 0  ? "render"
                    : line.rfind("MIDI", 0) == 0    ? "midi"
                                                    : nullptr;
            continue;
        }
        if (!section) continue;
        const bool def = line.size() > 2 && line[2] == '*';
        std::string s = line.substr(std::min<size_t>(line.size(), 4));
        if (std::string_view(section) == "midi") {
            j["midi"].push_back(s);
            continue;
        }
        // "Name (Maker)  (2 ch, 48000 Hz)"
        json e = {{"name", s}, {"channels", 0}, {"rate", 0}, {"default", def}};
        const size_t p = s.rfind("  (");
        if (p != std::string::npos) {
            e["name"] = s.substr(0, p);
            const std::string tail = s.substr(p + 3);
            e["channels"] = std::atoi(tail.c_str());
            const size_t c = tail.find(", ");
            if (c != std::string::npos) e["rate"] = std::atoi(tail.c_str() + c + 2);
        }
        j[section].push_back(std::move(e));
    }
    return j;
}

// =============================================================================== commands
namespace {

struct Ctx {
    const json& j;
    ParseResult& r;
    const char* name;
    bool fail(const std::string& m) {
        r.ok = false;
        r.error = m;
        return true;   // "handled" (it was one of ours)
    }
    const json* field(const char* k) const {
        const auto it = j.find(k);
        return (it == j.end() || it->is_null()) ? nullptr : &*it;
    }
    const json* value() const {
        if (const json* v = field("value")) return v;
        return field("i");
    }
    bool flag(const char* k, bool& out) {
        out = false;
        const json* v = field(k);
        if (!v) return true;
        if (!v->is_boolean()) {
            fail(std::string("\"") + k + "\" must be true or false");
            return false;
        }
        out = v->get<bool>();
        return true;
    }
};

bool as_int(const json& v, int64_t& out) {
    if (v.is_boolean()) {
        out = v.get<bool>() ? 1 : 0;
        return true;
    }
    if (v.is_number_integer()) {
        out = v.get<int64_t>();
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::floor(d) != d || std::fabs(d) > 3e9) return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

// An enum given as a name or a number in [0, n).
template <size_t N>
bool enum_val(const json* v, const char* const (&names)[N], int64_t& out) {
    if (!v) return false;
    if (v->is_string()) {
        for (size_t k = 0; k < N; ++k)
            if (ieq(v->get<std::string>(), names[k])) {
                out = static_cast<int64_t>(k);
                return true;
            }
        return false;
    }
    return as_int(*v, out) && out >= 0 && out < static_cast<int64_t>(N);
}
template <size_t N>
std::string list_of(const char* const (&names)[N]) {
    std::string s;
    for (size_t k = 0; k < N; ++k) s += (k ? ", " : "") + std::string(names[k]);
    return s;
}

}  // namespace

bool panel_command_from_json(const json& j, ParseResult& r) {
    if (!j.is_object()) return false;
    const auto it_type = j.find("type");
    if (it_type == j.end() || !it_type->is_string()) return false;
    const std::string t = it_type->get<std::string>();
    struct TypeName {
        CommandType type;
        const char* name;
    };
    static const TypeName kTypes[] = {
        {CommandType::SceneRecall, "SceneRecall"}, {CommandType::SceneSave, "SceneSave"},
        {CommandType::Undo, "Undo"},               {CommandType::SceneReset, "SceneReset"},
        {CommandType::Dice, "Dice"},               {CommandType::Locks, "Locks"},
        {CommandType::Project, "Project"},         {CommandType::ShowSelect, "ShowSelect"},
        {CommandType::KitSet, "KitSet"},           {CommandType::MacroSet, "MacroSet"},
        {CommandType::MacroValue, "MacroValue"},   {CommandType::SongEdit, "SongEdit"},
        {CommandType::OutputBlack, "OutputBlack"}, {CommandType::Identify, "Identify"},
        {CommandType::LayerPlace, "LayerPlace"},   {CommandType::OutputConfig, "OutputConfig"},
    };
    const TypeName* tn = nullptr;
    for (const auto& k : kTypes)
        if (ieq(t, k.name)) tn = &k;
    if (!tn) return false;

    r = ParseResult{};
    r.ok = true;
    Command& c = r.cmd;
    c = make(tn->type);
    Ctx x{j, r, tn->name};
    const std::string N = tn->name;

    // ---- shared field readers ----
    auto deck = [&](bool required, bool hit_ok) -> bool {
        const json* d = x.field("deck");
        if (!d) {
            if (required) x.fail(N + " needs \"deck\" (A or B)");
            return !required;
        }
        int64_t v = 0;
        if (d->is_string() && hit_ok && ieq(d->get<std::string>(), "hit")) c.deck = kHitDeck;
        else if (d->is_string() && ieq(d->get<std::string>(), "A")) c.deck = 0;
        else if (d->is_string() && ieq(d->get<std::string>(), "B")) c.deck = 1;
        else if (as_int(*d, v) && v >= 0 && v < kDecks) c.deck = static_cast<uint8_t>(v);
        else {
            x.fail(std::string("\"deck\" must be A or B") + (hit_ok ? " or \"hit\"" : ""));
            return false;
        }
        return true;
    };
    auto small = [&](const char* key, int lo, int hi, uint8_t& out, bool required) -> bool {
        const json* v = x.field(key);
        if (!v) {
            if (required) x.fail(N + " needs \"" + key + "\"");
            return !required;   // stays kNone
        }
        int64_t q = 0;
        if (!as_int(*v, q) || q < lo || q > hi) {
            x.fail("\"" + std::string(key) + "\" must be " + std::to_string(lo) + ".." + std::to_string(hi));
            return false;
        }
        out = static_cast<uint8_t>(q);
        return true;
    };
    auto floats = [&](int need, int max) -> bool {
        const json* f = x.field("f");
        if (!f) {
            if (need) x.fail(N + " needs \"f\"");
            return need == 0;
        }
        std::vector<double> v;
        if (f->is_number()) v.push_back(f->get<double>());
        else if (f->is_array())
            for (const auto& e : *f) {
                if (!e.is_number()) {
                    x.fail("\"f\" must be numbers");
                    return false;
                }
                v.push_back(e.get<double>());
            }
        else {
            x.fail("\"f\" must be a number or an array of numbers");
            return false;
        }
        if (static_cast<int>(v.size()) < need || static_cast<int>(v.size()) > max) {
            x.fail("\"f\" needs " + std::to_string(need) + ".." + std::to_string(max) + " numbers");
            return false;
        }
        for (size_t k = 0; k < v.size(); ++k) {
            if (!std::isfinite(v[k])) {
                x.fail("\"f\" must be finite");
                return false;
            }
            c.f[k] = static_cast<float>(v[k]);
        }
        return true;
    };
    auto int_val = [&](int64_t lo, int64_t hi, bool required, int64_t dflt) -> bool {
        const json* v = x.value();
        int64_t q = dflt;
        if (!v) {
            if (required) {
                x.fail(N + " needs \"value\"");
                return false;
            }
        } else if (!as_int(*v, q) || q < lo || q > hi) {
            x.fail("\"value\" must be " + std::to_string(lo) + ".." + std::to_string(hi));
            return false;
        }
        c.i = static_cast<int32_t>(q);
        return true;
    };
    auto relative = [&](bool allowed) -> bool {
        bool rel = false;
        if (!x.flag("relative", rel)) return false;
        if (rel && !allowed) {
            x.fail(N + " cannot be relative");
            return false;
        }
        if (rel) c.flags |= kRelative;
        return true;
    };
    auto text = [&](bool allowed) -> bool {
        const json* s = x.field("text");
        if (!s) return true;
        if (!allowed) {
            x.fail(N + " takes no \"text\" here");
            return false;
        }
        if (!s->is_string()) {
            x.fail("\"text\" must be a string");
            return false;
        }
        r.text = s->get<std::string>();
        if (r.text.empty()) {
            x.fail("\"text\" is empty");
            return false;
        }
        if (r.text.size() >= static_cast<size_t>(kPathLen)) {
            x.fail("\"text\" is too long (max " + std::to_string(kPathLen - 1) + " bytes)");
            return false;
        }
        for (char ch : r.text)
            if (static_cast<unsigned char>(ch) < 32) {
                x.fail("\"text\" has a control character");
                return false;
            }
        r.needs_text = true;
        return true;
    };
    auto param_enum = [&](const auto& names, bool required) -> bool {
        const json* p = x.field("param");
        if (!p) {
            if (required) x.fail(N + " needs \"param\" (" + list_of(names) + ")");
            return !required;
        }
        int64_t v = 0;
        if (!enum_val(p, names, v)) {
            x.fail("unknown " + N + " param (" + list_of(names) + ")");
            return false;
        }
        c.param = static_cast<uint8_t>(v);
        return true;
    };
    auto value_enum = [&](const auto& names, bool required, int64_t dflt) -> bool {
        const json* v = x.value();
        if (!v) {
            if (required) x.fail(N + " needs \"value\" (" + list_of(names) + ")");
            c.i = static_cast<int32_t>(dflt);
            return !required;
        }
        int64_t q = 0;
        if (!enum_val(v, names, q)) {
            x.fail("\"value\" must be one of " + list_of(names));
            return false;
        }
        c.i = static_cast<int32_t>(q);
        return true;
    };
    auto toggle_val = [&]() -> bool {
        const json* v = x.value();
        int64_t q = 0;
        if (v && v->is_string() && ieq(v->get<std::string>(), "toggle")) q = 2;
        else if (!v || !as_int(*v, q) || q < 0 || q > 2) {
            x.fail("\"value\" must be 0, 1 or \"toggle\"");
            return false;
        }
        c.i = static_cast<int32_t>(q);
        return true;
    };

    switch (tn->type) {
        case CommandType::SceneRecall: {
            if (!deck(true, false)) return true;
            const json* v = x.value();
            int64_t q = 0;
            if (v && v->is_string() && ieq(v->get<std::string>(), "next")) q = kSceneNext;
            else if (v && v->is_string() && ieq(v->get<std::string>(), "prev")) q = kScenePrev;
            else if (!v || !as_int(*v, q) || q < 0 || q >= kScenePads) {
                x.fail("SceneRecall \"value\" must be a pad 0..15, \"next\" or \"prev\"");
                return true;
            }
            c.i = static_cast<int32_t>(q);
            if (!small("slot", 0, 7, c.slot, false)) return true;
            c.f[0] = -1.0f;   // default: the scene's own transition
            if (!floats(0, 1)) return true;
            break;
        }
        case CommandType::SceneSave: {
            if (!deck(true, false) || !int_val(-1, kScenePads - 1, false, -1)) return true;
            bool var = false;
            if (!x.flag("variation", var)) return true;
            if (var) c.flags |= kVariation;
            break;
        }
        case CommandType::Undo: {
            const json* v = x.value();
            int64_t q = 0;
            if (v && v->is_string() && ieq(v->get<std::string>(), "undo")) q = 0;
            else if (v && v->is_string() && ieq(v->get<std::string>(), "redo")) q = 1;
            else if (v && (!as_int(*v, q) || q < 0 || q > 1)) {
                x.fail("Undo \"value\" must be \"undo\" or \"redo\"");
                return true;
            }
            c.i = static_cast<int32_t>(q);
            break;
        }
        case CommandType::SceneReset:
            if (!deck(true, false) || !small("layer", 0, kLayersPerDeck - 1, c.layer, false) || !floats(0, 1)) return true;
            if (c.f[0] < 0) return x.fail("\"f\" (glide seconds) must be >= 0");
            break;
        case CommandType::Dice:
            if (!param_enum(kDiceNames, true) || !deck(true, false) ||
                !small("layer", 0, kLayersPerDeck - 1, c.layer, false) || !small("slot", 0, kFxSlots - 1, c.slot, false) ||
                !int_val(-1000000, 1000000000, false, 0) || !floats(0, 1))
                return true;
            break;
        case CommandType::Locks: {
            if (!param_enum(kLockOpNames, true)) return true;
            const json* v = x.value();
            int64_t mask = 0;
            if (v && v->is_array()) {
                for (const auto& e : *v) {
                    int64_t b = 0;
                    if (e.is_string() && ieq(e.get<std::string>(), "all")) {
                        mask = kLockAll;
                        continue;
                    }
                    if (!enum_val(&e, kLockNames, b)) return x.fail("unknown lock (" + list_of(kLockNames) + ")");
                    mask |= int64_t{1} << b;
                }
            } else if (v && v->is_string()) {
                int64_t b = 0;
                if (ieq(v->get<std::string>(), "all")) mask = kLockAll;
                else if (enum_val(v, kLockNames, b)) mask = int64_t{1} << b;
                else return x.fail("unknown lock (" + list_of(kLockNames) + ")");
            } else if (!v || !as_int(*v, mask) || mask < 0 || mask > 0xFF) {
                return x.fail("Locks \"value\" must be a lock name, a list of them, or a mask 0..255");
            }
            c.i = static_cast<int32_t>(mask);
            break;
        }
        case CommandType::Project: {
            if (!param_enum(kProjectNames, true)) return true;
            const auto a = static_cast<ProjectAction>(c.param);
            const bool takes_text = a == ProjectAction::Open || a == ProjectAction::SaveAs ||
                                    a == ProjectAction::Export || a == ProjectAction::Relink;
            if (!text(takes_text)) return true;
            if (a == ProjectAction::Open) {
                if (!small("slot", 0, kRecent - 1, c.slot, false)) return true;
                if (c.slot == kNone && !r.needs_text) return x.fail("Project Open needs \"slot\" (recent 0..9) or \"text\" (a path)");
                if (c.slot != kNone && r.needs_text) return x.fail("Project Open takes \"slot\" or \"text\", not both");
            } else if ((a == ProjectAction::SaveAs || a == ProjectAction::Relink) && !r.needs_text) {
                return x.fail("Project " + std::string(kProjectNames[c.param]) + " needs \"text\"");
            } else if (a == ProjectAction::New) {
                if (!int_val(0, 3, false, 0)) return true;
                if (c.i == 2) return x.fail("Project New \"value\" = screens: 1 or 3 (0 = 3)");
            }
            break;
        }
        case CommandType::ShowSelect: {
            if (!param_enum(kSelectNames, true) || !relative(true)) return true;
            if (static_cast<SelectWhat>(c.param) == SelectWhat::UseSetlist) {
                if (c.flags & kRelative) return x.fail("UseSetlist cannot be relative");
                if (!toggle_val()) return true;
            } else if (c.flags & kRelative) {
                if (!int_val(-64, 64, true, 0)) return true;
            } else if (!int_val(0, static_cast<SelectWhat>(c.param) == SelectWhat::Bank ? 15 : 65535, true, 0)) {
                return true;
            }
            break;
        }
        case CommandType::KitSet: {
            if (!deck(true, false) || !small("slot", 0, kScenePads - 1, c.slot, true) || !param_enum(kPadKindNames, true))
                return true;
            const json* l = x.field("layer");
            int64_t q = 0;
            if (l && !enum_val(l, kPadTargetNames, q)) return x.fail("\"layer\" (pad target) must be one of " + list_of(kPadTargetNames));
            c.layer = static_cast<uint8_t>(l ? q : 0);
            if (static_cast<PadKind>(c.param) == PadKind::Action) {
                if (!value_enum(kPadActionNames, true, 0)) return true;
            } else if (!int_val(-1, 1 << 20, static_cast<PadKind>(c.param) != PadKind::Empty, 0)) {
                return true;
            }
            break;
        }
        case CommandType::MacroSet: {
            if (!deck(true, false) || !small("slot", 0, 3, c.slot, true) ||
                !small("param", 0, 7, c.param, false) || !small("layer", 0, kLayersPerDeck - 1, c.layer, false))
                return true;
            bool clr = false;
            if (!x.flag("clear", clr)) return true;
            if (clr) c.flags |= kClear;
            if (c.param == kNone) {   // rename the macro
                if (!text(true)) return true;
                if (!r.needs_text) return x.fail("MacroSet without \"param\" renames the macro: it needs \"text\"");
            } else {
                if (!text(false) || !int_val(0, kFxSlots * 16 - 1, !clr, 0) || !floats(0, 2)) return true;
                if (!x.field("f")) c.f[1] = 1.0f;   // default range 0..1
            }
            break;
        }
        case CommandType::MacroValue:
            if (!small("slot", 0, 3, c.slot, true) || !relative(true) || !floats(1, 1)) return true;
            if (!(c.flags & kRelative)) c.f[0] = std::clamp(c.f[0], 0.0f, 1.0f);
            else c.f[0] = std::clamp(c.f[0], -1.0f, 1.0f);
            break;
        case CommandType::SongEdit: {
            if (!param_enum(kSongNames, true)) return true;
            const auto a = static_cast<SongAction>(c.param);
            const bool takes_text = a == SongAction::New || a == SongAction::Rename || a == SongAction::RenameScene;
            if (!text(takes_text)) return true;
            if ((a == SongAction::Rename || a == SongAction::RenameScene) && !r.needs_text)
                return x.fail("SongEdit " + std::string(kSongNames[c.param]) + " needs \"text\"");
            if (!deck(a == SongAction::AddScene, false)) return true;
            if (!small("slot", 0, 255, c.slot, a == SongAction::RemoveScene || a == SongAction::Rename ||
                                                    a == SongAction::Move || a == SongAction::RenameScene))
                return true;
            if (!r.needs_text && a != SongAction::New && !int_val(0, 65535, true, 0)) return true;
            if (!floats(a == SongAction::Bpm ? 1 : 0, 1)) return true;
            if (a == SongAction::Bpm && c.f[0] != 0 && (c.f[0] < 20 || c.f[0] > 300))
                return x.fail("song BPM must be 20..300 (0 = none)");
            break;
        }
        case CommandType::OutputBlack:
            if (!small("slot", 0, kMaxOutputs - 1, c.slot, false) || !toggle_val()) return true;
            break;
        case CommandType::Identify:
            if (!value_enum(kIdentifyNames, true, 0)) return true;
            break;
        case CommandType::LayerPlace: {
            if (!deck(true, true)) return true;
            if (c.deck == kHitDeck) {
                c.layer = 0;
                if (const json* l = x.field("layer")) {
                    int64_t q = 0;
                    if (!as_int(*l, q) || q != 0) return x.fail("the HIT layer is layer 0");
                }
            } else if (!small("layer", 0, kLayersPerDeck - 1, c.layer, true)) {
                return true;
            }
            if (!value_enum(kPlaceNames, true, 0)) return true;
            const auto p = static_cast<Place>(c.i);
            if (p == Place::Out) {
                if (!small("param", 0, kMaxOutputs - 1, c.param, true)) return true;
            } else if (x.field("param")) {
                return x.fail("\"param\" (output) is only for Place Out");
            }
            if (p == Place::Free) {
                if (!floats(4, 4)) return true;
                if (c.f[2] <= 0 || c.f[3] <= 0) return x.fail("Place Free needs w, h > 0");
            } else if (!floats(0, 4)) {
                return true;
            }
            break;
        }
        case CommandType::OutputConfig: {
            if (!param_enum(kOutParamNames, true) || !small("slot", 0, kMaxOutputs - 1, c.slot, false)) return true;
            switch (static_cast<OutputParam>(c.param)) {
                case OutputParam::Resolution:
                    if (!value_enum(kPresetNames, true, 0)) return true;
                    break;
                case OutputParam::Refresh:
                    if (!value_enum(kRefreshNames, true, 0)) return true;
                    break;
                case OutputParam::Mode:
                    if (!value_enum(kCanvasNames, true, 0)) return true;
                    break;
                case OutputParam::BlendWidth:
                    if (!int_val(0, 1024, true, 0)) return true;
                    break;
                case OutputParam::BlendGamma:
                    if (!floats(1, 1)) return true;
                    c.f[0] = std::clamp(c.f[0], 1.0f, 3.0f);
                    break;
                case OutputParam::Adopt:
                case OutputParam::SimLost:
                    if (c.slot == kNone) return x.fail(N + " " + kOutParamNames[c.param] + " needs \"slot\" (an output)");
                    if (!int_val(0, 1, true, 0)) return true;
                    break;
                default: break;
            }
            break;
        }
        default: return false;
    }
    return true;
}

json panels_names() {
    json j;
    j["dice"] = names_of(kDiceNames);
    j["lock_ops"] = names_of(kLockOpNames);
    j["locks"] = names_of(kLockNames);
    j["project"] = names_of(kProjectNames);
    j["select"] = names_of(kSelectNames);
    j["pad_kinds"] = names_of(kPadKindNames);
    j["pad_targets"] = names_of(kPadTargetNames);
    j["pad_actions"] = names_of(kPadActionNames);
    j["song"] = names_of(kSongNames);
    j["output_states"] = names_of(kOutStateNames);
    j["presets"] = names_of(kPresetNames);
    j["refresh"] = names_of(kRefreshNames);
    j["canvas_modes"] = names_of(kCanvasNames);
    j["places"] = names_of(kPlaceNames);
    j["output_params"] = names_of(kOutParamNames);
    j["identify"] = names_of(kIdentifyNames);
    j["signals"] = names_of(kSigNames);
    j["input_modes"] = names_of(kInputModeNames);
    j["verdicts"] = names_of(kVerdictNames);
    j["ingest"] = names_of(kIngestCmdNames);        // GUI w7: the page shows only the ingest buttons this bridge can send
    j["ingest_modes"] = names_of(kIngestReqNames);
    j["scene_pads"] = kScenePads;
    j["max_outputs"] = kMaxOutputs;
    return j;
}

std::vector<float> downsample_curve(const std::vector<float>& curve, int n) {
    std::vector<float> out;
    if (curve.empty() || n <= 0) return out;
    const size_t m = curve.size();
    const size_t k = std::min<size_t>(static_cast<size_t>(n), m);
    out.assign(k, 0.0f);
    for (size_t b = 0; b < k; ++b) {
        const size_t lo = b * m / k, hi = std::max(lo + 1, (b + 1) * m / k);
        float v = 0.0f;
        for (size_t x = lo; x < hi && x < m; ++x)
            if (std::isfinite(curve[x])) v = std::max(v, curve[x]);
        out[b] = v;
    }
    const float top = *std::max_element(out.begin(), out.end());
    for (auto& v : out) v = top > 0.0f ? std::round(v / top * 1000.0f) / 1000.0f : 0.0f;
    return out;
}

json show_doc_for_page(const json& doc) {
    if (!doc.is_object() || doc.value("format", std::string()) != "himmel-show/1")
        return {{"ok", false}, {"error", "not a himmel-show/1 document"}};
    json j = json::object();
    for (const char* k : {"name", "use_setlist", "bank", "song", "locks", "default_take_ms", "saved_ms"})
        if (auto it = doc.find(k); it != doc.end()) j[k] = *it;
    json banks = json::array(), setlist = json::array();
    if (auto b = doc.find("banks"); b != doc.end() && b->is_array())
        for (const auto& x : *b)
            if (x.is_object()) banks.push_back({{"index", x.value("index", 0)}, {"pads", x.value("pads", json::array())}});
    if (auto s = doc.find("setlist"); s != doc.end() && s->is_array())
        for (const auto& x : *s)
            if (x.is_object()) {
                json so = {{"name", x.value("name", std::string())}, {"scenes", x.value("scenes", json::array())}};
                if (x.contains("bpm")) so["bpm"] = x["bpm"];
                setlist.push_back(std::move(so));
            }
    j["banks"] = std::move(banks);
    j["setlist"] = std::move(setlist);
    j["ok"] = true;
    return j;
}

// ---- MEDIA ingest commands (library-db.md §10, CommandType 60..69 in the library block) ----
bool ingest_command_from_json(const json& j, library::IngestCommand& c, std::string& error) {
    using library::IngestCommandType;
    if (!j.is_object()) { error = "an ingest command must be a JSON object"; return false; }
    const auto t = j.find("type");
    int idx = -1;
    if (t != j.end() && t->is_string())
        for (size_t k = 0; k < std::size(kIngestCmdNames); ++k)
            if (ieq(t->get<std::string>(), kIngestCmdNames[k])) idx = static_cast<int>(k);
    if (idx < 0) { error = "\"type\" must be one of " + list_of(kIngestCmdNames); return false; }
    const auto type = static_cast<IngestCommandType>(library::kIngestFirst + idx);
    int64_t clip = 0;
    if (auto it = j.find("clip"); it != j.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<int64_t>() < 0) { error = "\"clip\" must be a library id (>= 0, 0 = all)"; return false; }
        clip = it->get<int64_t>();
    }
    std::string path;
    if (auto it = j.find("path"); it != j.end() && !it->is_null()) {
        if (!it->is_string()) { error = "\"path\" must be a string"; return false; }
        path = it->get<std::string>();
    }
    auto flag = [&](const char* k) { auto it = j.find(k); return it != j.end() && it->is_boolean() && it->get<bool>(); };
    uint16_t flags = 0;
    if (flag("watch")) flags |= library::kIngestWatch;
    if (flag("force")) flags |= library::kIngestForce;
    if (flag("thumbs")) flags |= library::kIngestThumbs;
    int32_t iv = 0;
    // v3 (wave 7): Import options, Library flags.
    if (flag("hapq")) flags |= library::kIngestHapQ;
    if (flag("skip_known")) flags |= library::kIngestSkipKnown;
    if (flag("list")) flags |= library::kIngestList;
    if (flag("dry_run")) flags |= library::kIngestDryRun;
    if (flag("fit")) flags |= library::kIngestFit;
    if (flag("move")) flags |= library::kIngestMove;
    uint32_t conform = 0;
    if (type == IngestCommandType::Import) {
        if (auto it = j.find("height"); it != j.end() && !it->is_null()) {
            if (!it->is_number_integer() || !(it->get<int64_t>() == 0 || (it->get<int64_t>() >= 144 && it->get<int64_t>() <= 4320))) {
                error = "\"height\" must be 0 (keep) or 144..4320";
                return false;
            }
            iv = static_cast<int32_t>(it->get<int64_t>());
        }
        if (auto it = j.find("conform_hz"); it != j.end() && !it->is_null()) {
            if (!it->is_number_integer() || !(it->get<int64_t>() == 0 || it->get<int64_t>() == 50 || it->get<int64_t>() == 60)) {
                error = "\"conform_hz\" must be 0, 50 or 60";
                return false;
            }
            conform = static_cast<uint32_t>(it->get<int64_t>());
        }
    }
    const bool needs_path = type == IngestCommandType::Import || type == IngestCommandType::Unwatch ||
                            type == IngestCommandType::Relink || type == IngestCommandType::Library;
    const bool needs_clip = type == IngestCommandType::Remove;
    if (needs_path && path.empty()) { error = std::string(kIngestCmdNames[idx]) + " needs \"path\""; return false; }
    if (needs_clip && clip <= 0) { error = "Remove needs \"clip\" (a library id)"; return false; }
    // Absolute Windows paths only (C:\... or \\server\...): the worker refuses others as BadPath anyway.
    if (!path.empty() && !((path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
                           path.rfind("\\\\", 0) == 0)) {
        error = "\"path\" must be absolute (C:\\... or \\\\server\\...)";
        return false;
    }
    if (type == IngestCommandType::Mode) {
        auto it = j.find("mode");
        int m = -1;
        if (it != j.end() && it->is_string())
            for (size_t k = 0; k < std::size(kIngestReqNames); ++k)
                if (ieq(it->get<std::string>(), kIngestReqNames[k])) m = static_cast<int>(k);
        if (m < 0) { error = "Mode needs \"mode\": one of " + list_of(kIngestReqNames); return false; }
        iv = m;
    }
    if (!library::make_ingest(c, type, clip, path.empty() ? nullptr : path.c_str(), flags, iv)) {
        error = "the path is too long";
        return false;
    }
    c.conform_hz = conform;
    return true;
}

json ingest_reply_to_json(const library::IngestReply& r) {
    size_t n = 0;
    while (n < sizeof(r.message) && r.message[n]) ++n;
    const int t = static_cast<int>(r.type) - library::kIngestFirst;
    return {{"seq", r.seq},
            {"type", t >= 0 && t < static_cast<int>(std::size(kIngestCmdNames)) ? kIngestCmdNames[t] : "?"},
            {"status", name_or(kIngestStatusNames, r.status)},
            {"ok", r.status <= 1},
            {"count", r.count},
            {"clip", r.clip},
            {"message", std::string(r.message, n)}};
}

std::string import_kind_of(const std::string& file_name) {
    const size_t dot = file_name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= file_name.size()) return {};
    std::string e = file_name.substr(dot + 1);
    for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const char* const video[] = {"mov", "mp4", "m4v", "mkv", "avi", "webm", "mxf", "mpg", "mpeg", "wmv"};
    static const char* const still[] = {"png", "jpg", "jpeg", "webp", "tif", "tiff", "bmp", "tga"};
    for (const char* x : video)
        if (e == x) return "video";
    for (const char* x : still)
        if (e == x) return "still";
    if (e == "gif" || e == "apng") return "animation";
    if (e == "svg") return "vector";
    return {};
}

std::string parent_dir_of(const std::string& path) {
    const auto sep = [](char ch) { return ch == '\\' || ch == '/'; };
    std::string p = path;
    while (p.size() > 3 && sep(p.back())) p.pop_back();
    if (p.size() >= 2 && p.size() <= 3 && p[1] == ':') return {};   // a drive root -> the drive list
    if (p.size() >= 2 && sep(p[0]) && sep(p[1])) {                  // \\server\share[\...]
        int parts = 0;
        for (size_t k = 2; k < p.size(); ++k)
            if (sep(p[k]) && !sep(p[k - 1])) ++parts;
        if (parts <= 1) return {};
    }
    size_t k = p.size();
    while (k > 0 && !sep(p[k - 1])) --k;
    if (k == 0) return {};
    if (k == 3 && p[1] == ':') return p.substr(0, 3);   // C:\x -> C:\ (keep the separator)
    return p.substr(0, k - 1);
}

int eta_seconds(int left, double per_item_s) {
    if (left <= 0) return 0;
    if (!(per_item_s > 0) || per_item_s > 86400) return -1;
    const double s = left * per_item_s;
    return s >= 86400 ? 86400 : static_cast<int>(s + 0.5);
}

}  // namespace himmel::ui
