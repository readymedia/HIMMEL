// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/files.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <share.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>

#include "himmel/show/look_ops.h"

namespace himmel::show::files {

using json = nlohmann::json;
using namespace himmel::protocol;

// ============================================================================
// misc
// ============================================================================
uint32_t crc32(const void* data, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    crc = ~crc;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint64_t unix_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
uint64_t unix_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string safe_name(const std::string& name) {
    std::string out;
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 32 || std::strchr("<>:\"/\\|?*", ch)) continue;
        out.push_back(ch);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Untitled") : out;
}

namespace {
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}
}  // namespace

fs::path default_projects_root() {
    fs::path d = known_folder(FOLDERID_Documents);
    if (d.empty()) d = fs::temp_directory_path();
    return d / "Himmel" / "Projects";
}
fs::path default_app_dir() {
    fs::path d = known_folder(FOLDERID_LocalAppData);
    if (d.empty()) d = fs::temp_directory_path();
    return d / "Himmel";
}

bool read_file(const fs::path& file, std::string& out) {
    // Shared read (a scanner or the indexer may have the file open for a moment): retry briefly.
    FILE* f = nullptr;
    for (int attempt = 0; attempt < 20 && !f; ++attempt) {
        f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
        std::error_code ec;
        if (!f && !fs::exists(file, ec)) return false;  // gone: no retry
        if (!f) Sleep(10);
    }
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool write_atomic(const fs::path& file, const std::string& bytes, std::string* err) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.wstring() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot write " + utf8(tmp) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD done = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err) *err = "write failed: " + utf8(tmp);
        return false;
    }
    // A virus scanner or the indexer may hold the old file for a moment: retry briefly (worker thread).
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(10);
    }
    if (err) *err = "cannot replace " + utf8(file) + " (error " + std::to_string(GetLastError()) + ")";
    DeleteFileW(tmp.c_str());
    return false;
}

// ============================================================================
// JSON: names for enums
// ============================================================================
namespace {

const char* kBlendNames[] = {"normal", "add", "screen", "multiply", "difference", "lighten"};
const char* kModeNames[] = {"loop", "pingpong", "once_hold", "once_eject", "once_next", "random", "chop", "step", "scratch"};
const char* kPadKindNames[] = {"empty", "clip", "cue", "effect", "scene", "action"};
const char* kTargetNames[] = {"selected", "layer1", "layer2", "layer3", "layer4", "all", "free", "hit"};
const char* kTransitionNames[] = {"cut", "morph", "take"};
const char* kPresetNames[] = {"720p", "1080p", "1440p", "4k", "custom"};
const char* kLockNames[] = {"clip", "fx", "colour", "motion", "layout", "source", "react", "mood"};
// Audio reactions (D131): the same order as the protocol enums (commands.h).
const char* kReactSourceNames[] = {"default", "custom", "off"};
const char* kSignalNames[] = {"none", "level", "bass", "mid", "high", "kick", "snare",
                              "hat", "onset", "beat", "bar", "energy", "build", "drop"};
const char* kReactModeNames[] = {"add", "follow", "trigger"};
const char* kCurveNames[] = {"linear", "soft", "hard", "steps", "exp"};
const char* kQuantiseNames[] = {"off", "1/16", "1/8", "1/4", "1/2", "bar"};
static_assert(std::size(kReactSourceNames) == static_cast<size_t>(ReactSource::Count));
static_assert(std::size(kSignalNames) == static_cast<size_t>(ReactSignal::Count));
static_assert(std::size(kReactModeNames) == static_cast<size_t>(ReactMode::Count));
static_assert(std::size(kCurveNames) == static_cast<size_t>(ReactCurve::Count));
static_assert(std::size(kQuantiseNames) == static_cast<size_t>(ReactQuantise::Count));

template <size_t N>
int index_of(const char* (&names)[N], const json& v, int fallback) {
    if (v.is_number_integer()) {
        const int i = v.get<int>();
        return i >= 0 && i < static_cast<int>(N) ? i : fallback;
    }
    if (!v.is_string()) return fallback;
    const std::string s = v.get<std::string>();
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<int>(i);
    return fallback;
}
template <size_t N>
const char* name_of(const char* (&names)[N], int i) {
    return i >= 0 && i < static_cast<int>(N) ? names[i] : names[0];
}

float jf(const json& o, const char* k, float d) {
    auto it = o.find(k);
    return it != o.end() && it->is_number() ? it->get<float>() : d;
}
int ji(const json& o, const char* k, int d) {
    auto it = o.find(k);
    return it != o.end() && it->is_number() ? it->get<int>() : d;
}
std::string js(const json& o, const char* k, const char* d = "") {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string(d);
}
float r4(float v) { return std::round(v * 10000.0f) / 10000.0f; }  // stable, diff-friendly numbers

// ---- knob reactions (D131): only Custom and Off knobs are written ----
json react_to_json(const ReactRow& row, int knobs) {
    json a = json::array();
    for (int p = 0; p < knobs && p < kReactParams; ++p) {
        const ReactKnob& k = row.k[p];
        if (k.source == static_cast<uint8_t>(ReactSource::Off)) {
            a.push_back({{"knob", p}, {"source", "off"}});
            continue;
        }
        if (k.source != static_cast<uint8_t>(ReactSource::Custom)) continue;
        json o = {{"knob", p},
                  {"source", "custom"},
                  {"signal", name_of(kSignalNames, k.signal)},
                  {"mode", name_of(kReactModeNames, k.mode)},
                  {"curve", name_of(kCurveNames, k.curve)},
                  {"amount", r4(k.amount)},
                  {"attack_ms", r4(k.attack_ms)},
                  {"release_ms", r4(k.release_ms)},
                  {"threshold", r4(k.threshold)}};
        if (k.quantise) o["quantise"] = name_of(kQuantiseNames, k.quantise);
        if (k.hold_q) o["hold_beats"] = k.hold_q * 0.25f;
        if (k.ranged) o["range"] = json::array({r4(k.range_lo), r4(k.range_hi)});
        a.push_back(o);
    }
    return a;
}

// Unknown or broken entries are skipped (that knob stays Default); numbers are clamped like FxReact does.
void react_from_json(const json& a, ReactRow& row, int knobs) {
    std::memset(&row, 0, sizeof(row));
    if (!a.is_array()) return;
    for (const json& o : a) {
        if (!o.is_object()) continue;
        const int p = ji(o, "knob", -1);
        if (p < 0 || p >= knobs || p >= kReactParams) continue;
        const int src = index_of(kReactSourceNames, o.value("source", json()), -1);
        ReactKnob k{};
        if (src == static_cast<int>(ReactSource::Off)) {
            k.source = static_cast<uint8_t>(ReactSource::Off);
        } else if (src == static_cast<int>(ReactSource::Custom)) {
            k.source = static_cast<uint8_t>(ReactSource::Custom);
            k.signal = static_cast<uint8_t>(index_of(kSignalNames, o.value("signal", json()), 0));
            k.mode = static_cast<uint8_t>(index_of(kReactModeNames, o.value("mode", json()), 0));
            k.curve = static_cast<uint8_t>(index_of(kCurveNames, o.value("curve", json()), 0));
            k.quantise = static_cast<uint8_t>(index_of(kQuantiseNames, o.value("quantise", json()), 0));
            k.hold_q = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(jf(o, "hold_beats", 0.0f) * 4.0f)), 0, kReactHoldMax));
            k.amount = std::clamp(jf(o, "amount", 0.0f), -1.0f, 1.0f);
            k.attack_ms = std::clamp(jf(o, "attack_ms", 0.0f), 0.0f, kReactMaxMs);
            k.release_ms = std::clamp(jf(o, "release_ms", 0.0f), 0.0f, kReactMaxMs);
            k.threshold = std::clamp(jf(o, "threshold", 0.0f), 0.0f, 1.0f);
            if (auto r = o.find("range"); r != o.end() && r->is_array() && r->size() == 2 && (*r)[0].is_number() &&
                                          (*r)[1].is_number()) {
                k.ranged = 1;
                k.range_lo = std::clamp((*r)[0].get<float>(), 0.0f, 1.0f);
                k.range_hi = std::clamp((*r)[1].get<float>(), 0.0f, 1.0f);
            }
        } else {
            continue;  // "default" or unknown: the effect's own reaction
        }
        row.k[p] = k;
    }
}

// ---- FX slot ----
json fx_to_json(const FxSlotState& s, const Catalog& cat, const ReactRow* react = nullptr) {
    if (s.effect < 0 || s.effect >= static_cast<int>(cat.effect_count)) return nullptr;
    const EffectInfo& e = cat.effect[s.effect];
    json params = json::array();
    for (int p = 0; p < e.param_count && p < kFxParams; ++p) params.push_back(r4(s.param[p]));
    json o = {{"effect", e.name}, {"params", params}, {"mix", r4(s.mix)}};
    if (s.bypass) o["bypass"] = true;
    if (react) {
        json r = react_to_json(*react, e.param_count);
        if (!r.empty()) o["react"] = std::move(r);
    }
    return o;
}

void fx_from_json(const json& o, FxSlotState& s, const Catalog& cat, std::vector<std::string>* unknown,
                  ReactRow* react = nullptr) {
    std::memset(&s, 0, sizeof(s));
    s.effect = -1;
    s.mix = 1.0f;
    if (react) std::memset(react, 0, sizeof(*react));
    if (!o.is_object()) return;
    const std::string name = js(o, "effect");
    const int id = find_effect(cat, name.c_str());
    if (id < 0) {
        if (unknown && !name.empty()) unknown->push_back(name);
        return;
    }
    s.effect = static_cast<int16_t>(id);
    const EffectInfo& e = cat.effect[id];
    for (int p = 0; p < kFxParams; ++p) s.param[p] = p < e.param_count ? std::clamp(e.param_default[p], 0.0f, 1.0f) : 0.0f;
    if (auto it = o.find("params"); it != o.end() && it->is_array())
        for (int p = 0; p < static_cast<int>(it->size()) && p < e.param_count && p < kFxParams; ++p)
            if ((*it)[p].is_number()) s.param[p] = std::clamp((*it)[p].get<float>(), 0.0f, 1.0f);
    s.mix = std::clamp(jf(o, "mix", 1.0f), 0.0f, 1.0f);
    s.bypass = o.value("bypass", false) ? 1 : 0;
    if (react) react_from_json(o.value("react", json()), *react, e.param_count);  // no "react" = Default (old files)
}

json fx_list(const FxSlotState* s, const Catalog& cat, const ReactRow* react = nullptr) {
    json a = json::array();
    for (int i = 0; i < kFxSlots; ++i) a.push_back(fx_to_json(s[i], cat, react ? &react[i] : nullptr));
    return a;
}
void fx_list_from(const json& a, FxSlotState* s, const Catalog& cat, std::vector<std::string>* unknown,
                  ReactRow* react = nullptr) {
    for (int i = 0; i < kFxSlots; ++i)
        fx_from_json(a.is_array() && i < static_cast<int>(a.size()) ? a[i] : json(), s[i], cat, unknown,
                     react ? &react[i] : nullptr);
}

// ---- transport ----
json transport_to_json(const LayerTransport& t) {
    return {{"mode", name_of(kModeNames, t.mode)},
            {"backward", t.backward != 0},
            {"in", r4(t.in)},
            {"out", r4(t.out)},
            {"loop_in", r4(t.loop_in)},
            {"loop_out", r4(t.loop_out)},
            {"inner_loop", t.inner_loop != 0},
            {"time_base", t.time_base},
            {"beats", r4(t.beats)},
            {"interval", r4(t.interval)},
            {"distance", r4(t.distance)},
            {"slices", t.slices},
            {"step_frames", t.step_frames},
            {"step_div", r4(t.step_div)},
            {"glide", r4(t.glide)},
            {"trigger", t.trigger},
            {"start", t.start},
            {"fade_in", r4(t.fade_in)},
            {"fade_out", r4(t.fade_out)},
            {"snap", t.snap == kNone ? -1 : static_cast<int>(t.snap)},
            {"target", t.target},
            {"autopilot", t.autopilot},
            {"autopilot_count", t.autopilot_count},
            {"transition", t.transition},
            {"transition_time", r4(t.transition_time)},
            {"catch_up", t.catch_up != 0},
            {"scratch_snap", t.scratch_snap != 0},
            {"seed", t.seed}};
}

void transport_from_json(const json& o, LayerTransport& t) {  // t holds the defaults
    if (!o.is_object()) return;
    auto en = [&](const char* k, uint8_t& f, int count) {
        const int v = ji(o, k, f);
        if (v >= 0 && v < count) f = static_cast<uint8_t>(v);
    };
    auto fl = [&](const char* k, float& f, float lo, float hi) { f = std::clamp(jf(o, k, f), lo, hi); };
    auto bo = [&](const char* k, uint8_t& f) {
        auto it = o.find(k);
        if (it != o.end() && it->is_boolean()) f = it->get<bool>() ? 1 : 0;
    };
    t.mode = static_cast<uint8_t>(index_of(kModeNames, o.value("mode", json()), t.mode));
    bo("backward", t.backward);
    fl("in", t.in, 0.f, 1.f);
    fl("out", t.out, 0.f, 1.f);
    fl("loop_in", t.loop_in, 0.f, 1.f);
    fl("loop_out", t.loop_out, 0.f, 1.f);
    bo("inner_loop", t.inner_loop);
    en("time_base", t.time_base, static_cast<int>(TimeBase::Count));
    fl("beats", t.beats, 0.25f, 64.f);
    fl("interval", t.interval, 1.f / 16.f, 64.f);
    fl("distance", t.distance, 0.f, 1.f);
    t.slices = static_cast<uint8_t>(std::clamp(ji(o, "slices", t.slices), 2, 16));
    t.step_frames = static_cast<uint8_t>(std::clamp(ji(o, "step_frames", t.step_frames), 1, 64));
    fl("step_div", t.step_div, 1.f / 16.f, 16.f);
    fl("glide", t.glide, 0.f, 16.f);
    en("trigger", t.trigger, static_cast<int>(TriggerStyle::Count));
    en("start", t.start, static_cast<int>(StartPos::Count));
    fl("fade_in", t.fade_in, 0.f, 10.f);
    fl("fade_out", t.fade_out, 0.f, 10.f);
    const int snap = ji(o, "snap", t.snap == kNone ? -1 : t.snap);
    t.snap = snap < 0 || snap >= static_cast<int>(GridDiv::Count) ? kNone : static_cast<uint8_t>(snap);
    en("target", t.target, static_cast<int>(ClipTarget::Count));
    en("autopilot", t.autopilot, static_cast<int>(AutopilotMode::Count));
    t.autopilot_count = static_cast<uint8_t>(std::clamp(ji(o, "autopilot_count", t.autopilot_count), 1, 64));
    en("transition", t.transition, static_cast<int>(TransitionType::Count));
    fl("transition_time", t.transition_time, 0.f, 10.f);
    bo("catch_up", t.catch_up);
    bo("scratch_snap", t.scratch_snap);
    if (auto it = o.find("seed"); it != o.end() && it->is_number_unsigned()) t.seed = it->get<uint32_t>();
}

// ---- look ----
json clip_to_json(const Look& l, int i, const ClipIndex& clips) {
    if (!ref_used(l.clip_lib[i], l.clip_hash[i], l.clip_name[i])) return nullptr;
    json c = {{"name", l.clip_name[i]}};
    if (l.clip_lib[i] > 0) c["library_id"] = l.clip_lib[i];
    if (l.clip_hash[i][0]) c["hash"] = l.clip_hash[i];
    const int id = l.layer[i].clip;
    if (clips.valid(id)) {  // helps relink: size and length of the clip (projects.md §1)
        const ClipIdent& e = clips.at(id);
        c["w"] = e.width;
        c["h"] = e.height;
        c["seconds"] = r4(e.seconds);
    }
    return c;
}

// A clip reference from JSON (absent fields = unknown: a version 1 file has the name only).
void ref_from_json(const json& c, ClipRefView ref) {
    *ref.library_id = 0;
    ref.hash[0] = 0;
    ref.name[0] = 0;
    if (!c.is_object()) return;
    copy_str(ref.name, kNameLen, js(c, "name").c_str());
    if (auto it = c.find("library_id"); it != c.end() && it->is_number_integer() && it->get<int64_t>() > 0)
        *ref.library_id = it->get<int64_t>();
    const std::string h = js(c, "hash");
    if (is_hash(h.c_str())) copy_str(ref.hash, kHashLen, h.c_str());
}

json look_to_json(const Look& l, const Catalog& cat, const ClipIndex& clips) {
    json layers = json::array();
    for (int i = 0; i < kLayers; ++i) {
        const LayerState& s = l.layer[i];
        json o = {{"clip", clip_to_json(l, i, clips)},
                  {"opacity", r4(s.opacity)},
                  {"blend", name_of(kBlendNames, s.blend)},
                  {"speed", r4(s.speed)},
                  {"fx", fx_list(s.fx, cat, l.layer_react[i])},
                  {"transport", transport_to_json(l.transport[i])}};
        if (s.mute) o["mute"] = true;
        layers.push_back(o);
    }
    return {{"layers", layers}, {"fx", fx_list(l.fx, cat, l.fx_react)}};
}

void look_from_json(const json& o, Look& l, const Catalog& cat, const ClipIndex& clips, std::vector<std::string>* missing,
                    std::vector<std::string>* unknown_fx) {
    clear_look(l);
    if (!o.is_object()) return;
    const json layers = o.value("layers", json::array());
    for (int i = 0; i < kLayers && i < static_cast<int>(layers.size()); ++i) {
        const json& L = layers[i];
        if (!L.is_object()) continue;
        LayerState& s = l.layer[i];
        if (auto c = L.find("clip"); c != L.end()) ref_from_json(*c, layer_ref(l, i));
        s.opacity = std::clamp(jf(L, "opacity", 1.0f), 0.0f, 1.0f);
        s.blend = static_cast<uint8_t>(index_of(kBlendNames, L.value("blend", json()), 0));
        s.mute = L.value("mute", false) ? 1 : 0;
        s.speed = std::clamp(jf(L, "speed", 1.0f), -4.0f, 4.0f);
        fx_list_from(L.value("fx", json()), s.fx, cat, unknown_fx, l.layer_react[i]);
        transport_from_json(L.value("transport", json()), l.transport[i]);
    }
    fx_list_from(o.value("fx", json()), l.fx, cat, unknown_fx, l.fx_react);
    bind_clips(l, clips);
    for (int i = 0; i < kLayers; ++i)
        if (l.layer[i].clip < 0 && ref_used(l.clip_lib[i], l.clip_hash[i], l.clip_name[i]) && missing)
            missing->push_back(l.clip_name[i][0] ? l.clip_name[i] : l.clip_hash[i]);
}

// ---- scene ----
json pad_to_json(const KitPad& p) {
    if (p.kind == static_cast<uint8_t>(PadKind::Empty)) return nullptr;
    json o = {{"kind", name_of(kPadKindNames, p.kind)}, {"target", name_of(kTargetNames, p.target)}};
    if (p.name[0]) o["name"] = p.name;
    if (p.kind == static_cast<uint8_t>(PadKind::Clip) || p.kind == static_cast<uint8_t>(PadKind::Cue)) {
        if (p.lib > 0) o["library_id"] = p.lib;
        if (p.hash[0]) o["hash"] = p.hash;
    }
    if (p.kind == static_cast<uint8_t>(PadKind::Scene) || p.kind == static_cast<uint8_t>(PadKind::Action) ||
        p.kind == static_cast<uint8_t>(PadKind::Cue))
        o["id"] = p.id;
    if (p.slot != kNone) o["slot"] = p.slot;
    return o;
}

void pad_from_json(const json& o, KitPad& p, const Catalog& cat, const ClipIndex& clips, std::vector<std::string>* missing) {
    std::memset(&p, 0, sizeof(p));
    p.id = -1;
    p.slot = kNone;
    if (!o.is_object()) return;
    p.kind = static_cast<uint8_t>(index_of(kPadKindNames, o.value("kind", json()), 0));
    p.target = static_cast<uint8_t>(index_of(kTargetNames, o.value("target", json()), 0));
    copy_str(p.name, kNameLen, js(o, "name").c_str());
    const int slot = ji(o, "slot", -1);
    p.slot = slot >= 0 && slot < 16 ? static_cast<uint8_t>(slot) : kNone;
    switch (static_cast<PadKind>(p.kind)) {
        case PadKind::Clip:
        case PadKind::Cue:
            ref_from_json(o, pad_ref(p));
            p.id = bind_clip(clips, pad_ref(p));
            if (p.id < 0 && ref_used(p.lib, p.hash, p.name) && missing) missing->push_back(p.name[0] ? p.name : p.hash);
            if (p.kind == static_cast<uint8_t>(PadKind::Cue)) p.slot = static_cast<uint8_t>(std::clamp(ji(o, "id", 0), 0, 15));
            break;
        case PadKind::Effect: p.id = find_effect(cat, p.name); break;
        case PadKind::Scene: p.id = std::clamp(ji(o, "id", 0), 0, kPads - 1); break;
        case PadKind::Action: p.id = std::clamp(ji(o, "id", 0), 0, static_cast<int>(PadAction::Count) - 1); break;
        default: break;
    }
}

json macro_to_json(const Macro& m) {
    json binds = json::array();
    for (const auto& b : m.bind) {
        if (!b.used) continue;
        json o = {{"slot", b.slot}, {"param", b.param}, {"min", r4(b.min)}, {"max", r4(b.max)}};
        o["layer"] = b.layer == kNone ? -1 : static_cast<int>(b.layer);
        binds.push_back(o);
    }
    return {{"name", m.name}, {"value", r4(m.value)}, {"binds", binds}};
}

void macro_from_json(const json& o, Macro& m) {
    if (!o.is_object()) return;
    copy_str(m.name, sizeof(m.name), js(o, "name", m.name).c_str());
    m.value = std::clamp(jf(o, "value", m.value), 0.0f, 1.0f);
    const json binds = o.value("binds", json::array());
    int n = 0;
    for (const auto& b : binds) {
        if (n >= kMacroBinds || !b.is_object()) break;
        MacroBind& mb = m.bind[n++];
        mb.used = 1;
        const int layer = ji(b, "layer", -1);
        mb.layer = layer >= 0 && layer < kLayers ? static_cast<uint8_t>(layer) : kNone;
        mb.slot = static_cast<uint8_t>(std::clamp(ji(b, "slot", 0), 0, kFxSlots - 1));
        mb.param = static_cast<uint8_t>(std::clamp(ji(b, "param", 0), 0, kFxParams - 1));
        mb.min = std::clamp(jf(b, "min", 0.0f), 0.0f, 1.0f);
        mb.max = std::clamp(jf(b, "max", 1.0f), 0.0f, 1.0f);
    }
}

json scene_to_json(const ShowData& show, int id, const Catalog& cat, const ClipIndex& clips, bool with_variations) {
    const Scene& s = show.scene[id];
    json kit = json::array();
    bool any_pad = false;
    for (const auto& p : s.kit) {
        kit.push_back(pad_to_json(p));
        any_pad = any_pad || p.kind != 0;
    }
    json macros = json::array();
    for (const auto& m : s.macro) macros.push_back(macro_to_json(m));
    json o = {{"name", s.name},
              {"transition",
               {{"kind", name_of(kTransitionNames, s.transition.kind)},
                {"seconds", r4(s.transition.seconds)},
                {"on_beat", s.transition.on_beat != 0}}},
              {"look", look_to_json(s.look, cat, clips)},
              {"macros", macros}};
    if (any_pad) o["kit"] = kit;
    if (with_variations) {
        int v[kMaxVariations];
        const int n = variations_of(show, id, v);
        if (n > 1) {
            json vars = json::array();
            for (int k = 1; k < n; ++k) vars.push_back(scene_to_json(show, v[k], cat, clips, false));
            o["variations"] = vars;
        }
    }
    return o;
}

struct LoadCtx {
    const Catalog& cat;
    const ClipIndex& clips;
    std::vector<std::string> missing;
    std::vector<std::string> unknown_fx;
    bool full = false;
};

// Returns the new scene id, -1 when full.
int scene_from_json(const json& o, ShowData& show, Owner owner, int owner_index, int pad, int parent, int variation,
                    LoadCtx& ctx) {
    if (!o.is_object()) return -1;
    const int id = alloc_scene(show);
    if (id < 0) {
        ctx.full = true;
        return -1;
    }
    Scene& s = show.scene[id];
    clear_scene(s);
    s.used = 1;
    s.owner = static_cast<uint8_t>(owner);
    s.owner_index = static_cast<uint8_t>(owner_index);
    s.pad = static_cast<uint8_t>(pad);
    s.parent = static_cast<int16_t>(parent);
    s.variation = static_cast<uint8_t>(variation);
    copy_str(s.name, kSceneNameLen, js(o, "name").c_str());
    if (auto t = o.find("transition"); t != o.end() && t->is_object()) {
        s.transition.kind = static_cast<uint8_t>(index_of(kTransitionNames, t->value("kind", json()), 0));
        s.transition.seconds = std::clamp(jf(*t, "seconds", 2.0f), 0.0f, 60.0f);
        s.transition.on_beat = t->value("on_beat", false) ? 1 : 0;
    }
    look_from_json(o.value("look", json()), s.look, ctx.cat, ctx.clips, &ctx.missing, &ctx.unknown_fx);
    if (auto k = o.find("kit"); k != o.end() && k->is_array())
        for (int p = 0; p < kPads && p < static_cast<int>(k->size()); ++p) pad_from_json((*k)[p], s.kit[p], ctx.cat, ctx.clips, &ctx.missing);
    if (auto m = o.find("macros"); m != o.end() && m->is_array())
        for (int i = 0; i < kMacros && i < static_cast<int>(m->size()); ++i) macro_from_json((*m)[i], s.macro[i]);
    if (parent < 0)
        if (auto v = o.find("variations"); v != o.end() && v->is_array()) {
            int n = 1;
            for (const auto& vo : *v) {
                if (n >= kMaxVariations) break;
                if (scene_from_json(vo, show, owner, owner_index, pad, id, n, ctx) >= 0) ++n;
            }
        }
    return id;
}

json live_to_json(const LiveSnapshot& l, const Catalog& cat, const ClipIndex& clips) {
    return {{"format", kFormat},
            {"kind", "live"},
            {"decks", json::array({look_to_json(l.deck[0], cat, clips), look_to_json(l.deck[1], cat, clips)})},
            {"master_fx", fx_list(l.master_fx, cat, l.master_react)},
            {"crossfader", r4(l.crossfader)},
            {"program_deck", l.program_deck}};
}

void live_from_json(const json& o, LiveSnapshot& l, LoadCtx& ctx) {
    std::memset(&l, 0, sizeof(l));
    const json decks = o.value("decks", json::array());
    for (int d = 0; d < kDecks; ++d) look_from_json(d < static_cast<int>(decks.size()) ? decks[d] : json(), l.deck[d], ctx.cat, ctx.clips, &ctx.missing, &ctx.unknown_fx);
    fx_list_from(o.value("master_fx", json()), l.master_fx, ctx.cat, &ctx.unknown_fx, l.master_react);
    l.crossfader = std::clamp(jf(o, "crossfader", 0.0f), 0.0f, 1.0f);
    l.program_deck = static_cast<uint8_t>(std::clamp(ji(o, "program_deck", 0), 0, 1));
}

json bank_to_json(const ShowData& show, int b, const Catalog& cat, const ClipIndex& clips) {
    json pads = json::array();
    for (int p = 0; p < kPads; ++p) {
        const int id = show.bank_pad[b][p];
        pads.push_back(id >= 0 && show.scene[id].used ? scene_to_json(show, id, cat, clips, true) : json(nullptr));
    }
    return {{"format", kFormat}, {"kind", "bank"}, {"index", b}, {"pads", pads}};
}

json song_to_json(const ShowData& show, int song, const Catalog& cat, const ClipIndex& clips) {
    const Song& s = show.songs[song];
    json scenes = json::array();
    for (int p = 0; p < kPads; ++p) {
        const int id = s.scene[p];
        scenes.push_back(id >= 0 && show.scene[id].used ? scene_to_json(show, id, cat, clips, true) : json(nullptr));
    }
    while (!scenes.empty() && scenes.back().is_null()) scenes.erase(scenes.size() - 1);
    json o = {{"format", kFormat}, {"kind", "song"}, {"name", s.name}, {"scenes", scenes}};
    if (s.bpm > 0.0f) o["bpm"] = r4(s.bpm);
    return o;
}

int bank_used(const ShowData& show, int b) {
    for (int p = 0; p < kPads; ++p)
        if (show.bank_pad[b][p] >= 0) return 1;
    return 0;
}

char file_name_buf[32];
std::string bank_file(int b) {
    std::snprintf(file_name_buf, sizeof(file_name_buf), "banks/bank-%02d.json", b + 1);
    return file_name_buf;
}
std::string song_file(int n) {
    std::snprintf(file_name_buf, sizeof(file_name_buf), "songs/song-%02d.json", n + 1);
    return file_name_buf;
}

json project_head(const ShowData& show, uint64_t saved_ms) {
    json locks = json::array();
    for (int i = 0; i < 8; ++i)
        if (show.locks & (1u << i)) locks.push_back(kLockNames[i]);
    json out = {{"count", show.outputs.count},
                {"preset", name_of(kPresetNames, show.outputs.preset)},
                {"span", show.outputs.span != 0}};
    if (show.outputs.preset == static_cast<uint8_t>(OutputPreset::Custom)) out["custom"] = {show.outputs.custom_w, show.outputs.custom_h};
    if (show.outputs.refresh_hz > 0.0f) out["refresh_hz"] = show.outputs.refresh_hz;
    return {{"format", kFormat},
            {"kind", "project"},
            {"version", kFormatVersion},
            {"app", "Himmel 0.1"},
            {"name", show.name},
            {"created", show.created_unix},
            {"saved", saved_ms / 1000},
            {"saved_ms", saved_ms},
            {"outputs", out},
            {"start_focus", show.start_focus ? "play" : "prep"},
            {"default_take_ms", show.default_take_ms},
            {"locks", locks},
            {"use_setlist", show.use_setlist != 0},
            {"bank", show.bank},
            {"song", show.song}};
}

void head_from_json(const json& o, ShowData& show, uint64_t& saved) {
    copy_str(show.name, kNameLen, js(o, "name", "Untitled").c_str());
    show.created_unix = o.value("created", static_cast<uint64_t>(0));
    saved = o.value("saved_ms", o.value("saved", static_cast<uint64_t>(0)) * 1000);
    if (auto out = o.find("outputs"); out != o.end() && out->is_object()) {
        show.outputs.count = static_cast<uint8_t>(std::clamp(ji(*out, "count", 3), 1, 8));
        show.outputs.preset = static_cast<uint8_t>(index_of(kPresetNames, out->value("preset", json()), 1));
        show.outputs.span = out->value("span", true) ? 1 : 0;
        if (auto c = out->find("custom"); c != out->end() && c->is_array() && c->size() == 2) {
            show.outputs.custom_w = static_cast<uint16_t>(std::clamp((*c)[0].get<int>(), 0, 16384));
            show.outputs.custom_h = static_cast<uint16_t>(std::clamp((*c)[1].get<int>(), 0, 16384));
        }
        show.outputs.refresh_hz = jf(*out, "refresh_hz", 0.0f);
    }
    show.start_focus = js(o, "start_focus", "prep") == "play" ? 1 : 0;
    show.default_take_ms = static_cast<uint32_t>(std::clamp(ji(o, "default_take_ms", 2000), 0, 60000));
    show.locks = 0;
    for (const auto& l : o.value("locks", json::array())) show.locks |= 1u << index_of(kLockNames, l, 0);
    show.use_setlist = o.value("use_setlist", false) ? 1 : 0;
    show.bank = static_cast<uint8_t>(std::clamp(ji(o, "bank", 0), 0, kMaxBanks - 1));
    show.song = static_cast<int16_t>(ji(o, "song", -1));
}

void finish_load(ShowData& show, LoadCtx& ctx, LoadInfo& info) {
    if (show.song >= show.setlist_count) show.song = show.setlist_count > 0 ? 0 : -1;
    if (show.song < 0 && show.setlist_count > 0) show.song = 0;
    std::sort(ctx.missing.begin(), ctx.missing.end());
    ctx.missing.erase(std::unique(ctx.missing.begin(), ctx.missing.end()), ctx.missing.end());
    info.missing = static_cast<int>(ctx.missing.size());
    info.missing_names = ctx.missing;
    info.ok = true;
    if (ctx.full) info.error = "the show has more than 256 scenes; the rest were not loaded";
}

void bank_from_json(const json& o, ShowData& show, LoadCtx& ctx) {
    const int b = ji(o, "index", -1);
    if (b < 0 || b >= kMaxBanks) return;
    const json pads = o.value("pads", json::array());
    for (int p = 0; p < kPads && p < static_cast<int>(pads.size()); ++p)
        show.bank_pad[b][p] = static_cast<int16_t>(scene_from_json(pads[p], show, Owner::Bank, b, p, -1, 0, ctx));
}

void song_from_json(const json& o, ShowData& show, LoadCtx& ctx) {
    if (show.setlist_count >= kMaxSongs) return;
    const int id = show.setlist_count;
    Song& s = show.songs[id];
    std::memset(&s, 0, sizeof(s));
    for (auto& x : s.scene) x = -1;
    s.used = 1;
    copy_str(s.name, kSceneNameLen, js(o, "name").c_str());
    s.bpm = std::clamp(jf(o, "bpm", 0.0f), 0.0f, 300.0f);
    const json scenes = o.value("scenes", json::array());
    for (int p = 0; p < kPads && p < static_cast<int>(scenes.size()); ++p) {
        s.scene[p] = static_cast<int16_t>(scene_from_json(scenes[p], show, Owner::Song, id, p, -1, 0, ctx));
        if (s.scene[p] >= 0) s.scene_count = static_cast<uint8_t>(p + 1);
    }
    show.setlist[show.setlist_count++] = static_cast<int16_t>(id);
}

}  // namespace

// ============================================================================
// documents
// ============================================================================
FileSet project_files(const ShowData& show, const LiveSnapshot& live, const Catalog& cat, const ClipIndex& clips,
                      uint64_t saved_ms) {
    FileSet set;
    json head = project_head(show, saved_ms);
    json banks = json::array(), songs = json::array(), manifest = json::object();
    auto add = [&](const std::string& rel, const json& doc) {
        std::string text = doc.dump(1, '\t') + "\n";
        char crc[16];
        std::snprintf(crc, sizeof(crc), "%08x", crc32(text.data(), text.size()));
        manifest[rel] = crc;
        set.files.emplace_back(rel, std::move(text));
    };
    for (int b = 0; b < kMaxBanks; ++b)
        if (bank_used(show, b)) {
            banks.push_back(bank_file(b));
            add(bank_file(b), bank_to_json(show, b, cat, clips));
        }
    for (int n = 0; n < show.setlist_count; ++n) {
        const int id = show.setlist[n];
        if (id < 0 || id >= kMaxSongs || !show.songs[id].used) continue;
        const std::string rel = song_file(static_cast<int>(songs.size()));
        songs.push_back(rel);
        add(rel, song_to_json(show, id, cat, clips));
    }
    add("live.json", live_to_json(live, cat, clips));
    head["banks"] = banks;
    head["setlist"] = songs;
    head["files"] = manifest;
    set.files.emplace_back("project.json", head.dump(1, '\t') + "\n");
    return set;
}

std::string show_document(const ShowData& show, const LiveSnapshot& live, const Catalog& cat, const ClipIndex& clips,
                          uint64_t saved_ms) {
    json doc = project_head(show, saved_ms);
    doc["kind"] = "document";
    json banks = json::array(), songs = json::array();
    for (int b = 0; b < kMaxBanks; ++b)
        if (bank_used(show, b)) banks.push_back(bank_to_json(show, b, cat, clips));
    for (int n = 0; n < show.setlist_count; ++n) {
        const int id = show.setlist[n];
        if (id >= 0 && id < kMaxSongs && show.songs[id].used) songs.push_back(song_to_json(show, id, cat, clips));
    }
    doc["banks"] = banks;
    doc["setlist"] = songs;
    doc["live"] = live_to_json(live, cat, clips);
    return doc.dump() + "\n";
}

LoadInfo load_document(const std::string& text, ShowData& show, LiveSnapshot& live, const Catalog& cat,
                       const ClipIndex& clips) {
    LoadInfo info;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || js(doc, "format") != kFormat) {
        info.error = "not a himmel-show/1 document";
        return info;
    }
    if (ji(doc, "version", 1) > kFormatVersion) {
        info.error = "the show was saved by a newer Himmel";
        return info;
    }
    clear_show(show);
    LoadCtx ctx{cat, clips};
    head_from_json(doc, show, info.saved_ms);
    for (const auto& b : doc.value("banks", json::array()))
        if (b.is_object()) bank_from_json(b, show, ctx);
    for (const auto& s : doc.value("setlist", json::array()))
        if (s.is_object()) song_from_json(s, show, ctx);
    if (auto l = doc.find("live"); l != doc.end() && l->is_object()) {
        live_from_json(*l, live, ctx);
        info.have_live = true;
    }
    finish_load(show, ctx, info);
    return info;
}

LoadInfo load_project(const fs::path& folder, ShowData& show, LiveSnapshot& live, const Catalog& cat,
                      const ClipIndex& clips) {
    LoadInfo info;
    std::string text;
    if (!read_file(folder / "project.json", text)) {
        info.error = "no project.json in " + utf8(folder);
        return info;
    }
    json head = json::parse(text, nullptr, false);
    if (head.is_discarded() || !head.is_object() || js(head, "format") != kFormat || js(head, "kind") != "project") {
        info.error = "project.json is not a himmel-show/1 project";
        info.interrupted = true;
        return info;
    }
    if (ji(head, "version", 1) > kFormatVersion) {
        info.error = "the project was saved by a newer Himmel";
        return info;
    }
    clear_show(show);
    LoadCtx ctx{cat, clips};
    head_from_json(head, show, info.saved_ms);
    const json manifest = head.value("files", json::object());
    auto load = [&](const std::string& rel, json& out) -> bool {
        std::string t;
        if (!read_file(folder / from_utf8(rel), t)) {
            info.interrupted = true;
            return false;
        }
        if (auto m = manifest.find(rel); m != manifest.end() && m->is_string()) {
            char crc[16];
            std::snprintf(crc, sizeof(crc), "%08x", crc32(t.data(), t.size()));
            if (m->get<std::string>() != crc) info.interrupted = true;  // a newer or torn file: the save was cut off
        }
        out = json::parse(t, nullptr, false);
        return !out.is_discarded() && out.is_object();
    };
    for (const auto& b : head.value("banks", json::array())) {
        json doc;
        if (b.is_string() && load(b.get<std::string>(), doc)) bank_from_json(doc, show, ctx);
    }
    for (const auto& s : head.value("setlist", json::array())) {
        json doc;
        if (s.is_string() && load(s.get<std::string>(), doc)) song_from_json(doc, show, ctx);
    }
    json lv;
    if (load("live.json", lv)) {
        live_from_json(lv, live, ctx);
        info.have_live = true;
    }
    finish_load(show, ctx, info);
    if (info.interrupted && info.error.empty()) info.error = "the last save was interrupted";
    return info;
}

bool save_project(const fs::path& folder, const ShowData& show, const LiveSnapshot& live, const Catalog& cat,
                  const ClipIndex& clips, uint64_t saved_ms, std::string* err) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const FileSet set = project_files(show, live, cat, clips, saved_ms);
    for (const auto& [rel, text] : set.files)
        if (!write_atomic(folder / from_utf8(rel), text, err)) return false;
    // Files of a bank / song that no longer exists: remove (the manifest no longer lists them).
    for (const char* sub : {"banks", "songs"}) {
        const fs::path dir = folder / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string rel = std::string(sub) + "/" + utf8(e.path().filename());
            bool listed = false;
            for (const auto& f : set.files) listed = listed || f.first == rel;
            if (!listed) fs::remove(e.path(), ec);
        }
    }
    return true;
}

// ============================================================================
// journal
// ============================================================================
namespace {
constexpr uint32_t kJournalMagic = 0x4C4E4A48;  // "HJNL"
constexpr uint32_t kRecordMagic = 0x43455248;   // "HREC"
constexpr uint32_t kJournalVersion = 3;  // 3: + clip library id / hash in each look (wave 8)  // 2: LiveSnapshot + knob reactions (D131); a v1 journal is not restored
struct JournalHeader {
    uint32_t magic, version, header_size, snapshot_size;
    uint64_t created_ms;
    uint32_t effect_count, reserved;
    char effect[kMaxEffects][kNameLen];
};
struct RecordHead {
    uint32_t magic, size;
    uint64_t unix_ms;
    uint32_t crc, reserved;
};

void remap_fx(FxSlotState& s, const int* map) {
    if (s.effect < 0) return;
    s.effect = s.effect < kMaxEffects ? static_cast<int16_t>(map[s.effect]) : -1;
    if (s.effect < 0) std::memset(&s, 0, sizeof(s)), s.effect = -1, s.mix = 1.0f;
}
}  // namespace

Journal::~Journal() { close(); }

bool Journal::open_new(const fs::path& file, const Catalog& cat, std::string* err) {
    close();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot open the journal " + utf8(file);
        return false;
    }
    auto hd = std::make_unique<JournalHeader>();
    std::memset(hd.get(), 0, sizeof(JournalHeader));
    hd->magic = kJournalMagic;
    hd->version = kJournalVersion;
    hd->header_size = sizeof(JournalHeader);
    hd->snapshot_size = sizeof(LiveSnapshot);
    hd->created_ms = unix_ms_now();
    hd->effect_count = cat.effect_count;
    for (uint32_t i = 0; i < cat.effect_count && i < static_cast<uint32_t>(kMaxEffects); ++i)
        std::memcpy(hd->effect[i], cat.effect[i].name, kNameLen);
    DWORD done = 0;
    WriteFile(h, hd.get(), sizeof(JournalHeader), &done, nullptr);
    h_ = h;
    records_ = 0;
    return done == sizeof(JournalHeader);
}

bool Journal::append(const LiveSnapshot& s, uint64_t unix_ms) {
    if (!h_) return false;
    RecordHead r{kRecordMagic, static_cast<uint32_t>(sizeof(LiveSnapshot)), unix_ms, crc32(&s, sizeof(s)), 0};
    DWORD a = 0, b = 0;
    const bool ok = WriteFile(h_, &r, sizeof(r), &a, nullptr) && WriteFile(h_, &s, sizeof(s), &b, nullptr) &&
                    a == sizeof(r) && b == sizeof(s);
    if (ok) ++records_;
    if (unix_ms - since_flush_ >= 1000) flush(), since_flush_ = unix_ms;
    return ok;
}

void Journal::flush() {
    if (h_) FlushFileBuffers(h_);
}

void Journal::close() {
    if (h_) {
        FlushFileBuffers(h_);
        CloseHandle(h_);
    }
    h_ = nullptr;
}

bool Journal::read_last(const fs::path& file, const Catalog& cat, const ClipIndex& clips, LiveSnapshot& out, uint64_t& unix_ms,
                        uint32_t* count) {
    std::string data;
    if (!read_file(file, data) || data.size() < sizeof(JournalHeader)) return false;
    auto hd = std::make_unique<JournalHeader>();
    std::memcpy(hd.get(), data.data(), sizeof(JournalHeader));
    if (hd->magic != kJournalMagic || hd->version != kJournalVersion || hd->snapshot_size != sizeof(LiveSnapshot)) return false;
    size_t pos = hd->header_size;
    size_t last = 0;
    uint32_t n = 0;
    uint64_t best = 0;
    while (pos + sizeof(RecordHead) + sizeof(LiveSnapshot) <= data.size()) {
        RecordHead r;
        std::memcpy(&r, data.data() + pos, sizeof(r));
        if (r.magic != kRecordMagic || r.size != sizeof(LiveSnapshot)) break;
        if (crc32(data.data() + pos + sizeof(r), r.size) != r.crc) break;  // torn: stop at the last good one
        if (r.unix_ms >= best) last = pos, best = r.unix_ms;               // the newest (they are in order anyway)
        ++n;
        pos += sizeof(r) + r.size;
    }
    unix_ms = best;
    if (count) *count = n;
    if (!n) return false;
    std::memcpy(&out, data.data() + last + sizeof(RecordHead), sizeof(LiveSnapshot));
    int map[kMaxEffects];
    for (int i = 0; i < kMaxEffects; ++i) {
        char name[kNameLen + 1] = {};
        std::memcpy(name, hd->effect[i], kNameLen);
        map[i] = i < static_cast<int>(hd->effect_count) ? find_effect(cat, name) : -1;
    }
    for (auto& d : out.deck) {
        for (auto& l : d.layer)
            for (auto& f : l.fx) remap_fx(f, map);
        for (auto& f : d.fx) remap_fx(f, map);
        for (auto& nm : d.clip_name) nm[kNameLen - 1] = 0;
        for (auto& h : d.clip_hash) h[kHashLen - 1] = 0;
        bind_clips(d, clips);
    }
    for (auto& f : out.master_fx) remap_fx(f, map);
    return true;
}

// ============================================================================
// recents
// ============================================================================
std::vector<Recent> load_recents(const fs::path& file) {
    std::vector<Recent> out;
    std::string text;
    if (!read_file(file, text)) return out;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) return out;
    for (const auto& r : doc.value("recent", json::array())) {
        if (!r.is_object()) continue;
        Recent x;
        x.name = js(r, "name");
        x.path = js(r, "path");
        x.saved_unix = r.value("saved", static_cast<uint64_t>(0));
        x.outputs = ji(r, "outputs", 0);
        if (!x.path.empty() && out.size() < static_cast<size_t>(kRecent)) out.push_back(x);
    }
    return out;
}

bool save_recents(const fs::path& file, const std::vector<Recent>& r) {
    json a = json::array();
    for (const auto& x : r) a.push_back({{"name", x.name}, {"path", x.path}, {"saved", x.saved_unix}, {"outputs", x.outputs}});
    return write_atomic(file, json{{"format", kFormat}, {"kind", "recent"}, {"recent", a}}.dump(1, '\t') + "\n", nullptr);
}

void touch_recent(std::vector<Recent>& r, const Recent& now) {
    r.erase(std::remove_if(r.begin(), r.end(), [&](const Recent& x) { return _stricmp(x.path.c_str(), now.path.c_str()) == 0; }),
            r.end());
    r.insert(r.begin(), now);
    if (r.size() > static_cast<size_t>(kRecent)) r.resize(kRecent);
}

// ============================================================================
// zip (stored): local headers + central directory, UTF-8 names (flag bit 11)
// ============================================================================
namespace {
void put16(std::string& s, uint16_t v) { s.push_back(static_cast<char>(v & 0xFF)), s.push_back(static_cast<char>(v >> 8)); }
void put32(std::string& s, uint32_t v) { put16(s, static_cast<uint16_t>(v & 0xFFFF)), put16(s, static_cast<uint16_t>(v >> 16)); }
uint16_t get16(const std::string& s, size_t p) { return static_cast<uint16_t>(static_cast<uint8_t>(s[p]) | (static_cast<uint8_t>(s[p + 1]) << 8)); }
uint32_t get32(const std::string& s, size_t p) { return get16(s, p) | (static_cast<uint32_t>(get16(s, p + 2)) << 16); }
}  // namespace

bool zip_folder(const fs::path& folder, const fs::path& zip_file, std::string* err, bool skip_autosave) {
    std::error_code ec;
    std::string out, central;
    uint16_t count = 0;
    const std::string top = utf8(folder.filename());
    for (auto it = fs::recursive_directory_iterator(folder, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), folder, ec);
        std::string name = utf8(rel);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (skip_autosave && (name == "autosave" || name.rfind("autosave/", 0) == 0)) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) continue;
        std::string data;
        if (!read_file(it->path(), data)) continue;
        name = top + "/" + name;
        const uint32_t crc = crc32(data.data(), data.size());
        const auto offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50), put16(out, 20), put16(out, 1u << 11), put16(out, 0), put16(out, 0), put16(out, 0x21);
        put32(out, crc), put32(out, static_cast<uint32_t>(data.size())), put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint16_t>(name.size())), put16(out, 0);
        out += name;
        out += data;
        put32(central, 0x02014b50), put16(central, 20), put16(central, 20), put16(central, 1u << 11), put16(central, 0);
        put16(central, 0), put16(central, 0x21), put32(central, crc), put32(central, static_cast<uint32_t>(data.size()));
        put32(central, static_cast<uint32_t>(data.size())), put16(central, static_cast<uint16_t>(name.size()));
        put16(central, 0), put16(central, 0), put16(central, 0), put16(central, 0), put32(central, 0), put32(central, offset);
        central += name;
        ++count;
    }
    const auto cd_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50), put16(out, 0), put16(out, 0), put16(out, count), put16(out, count);
    put32(out, static_cast<uint32_t>(central.size())), put32(out, cd_offset), put16(out, 0);
    return write_atomic(zip_file, out, err);
}

bool unzip_to(const fs::path& zip_file, const fs::path& folder, std::string* err) {
    std::string z;
    if (!read_file(zip_file, z)) {
        if (err) *err = "cannot read " + utf8(zip_file);
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t p = z.size() >= 22 ? z.size() - 22 : 0; p + 22 <= z.size(); --p) {
        if (get32(z, p) == 0x06054b50) {
            eocd = p;
            break;
        }
        if (p == 0) break;
    }
    if (eocd == std::string::npos) {
        if (err) *err = "not a .himmelshow (zip) file";
        return false;
    }
    const uint16_t count = get16(z, eocd + 10);
    size_t cd = get32(z, eocd + 16);
    for (uint16_t i = 0; i < count; ++i) {
        if (cd + 46 > z.size() || get32(z, cd) != 0x02014b50) break;
        const uint16_t method = get16(z, cd + 10);
        const uint32_t crc = get32(z, cd + 16), csize = get32(z, cd + 20), usize = get32(z, cd + 24);
        const uint16_t nlen = get16(z, cd + 28), xlen = get16(z, cd + 30), clen = get16(z, cd + 32);
        const uint32_t off = get32(z, cd + 42);
        std::string name = z.substr(cd + 46, nlen);
        cd += 46u + nlen + xlen + clen;
        if (method != 0 || csize != usize) {
            if (err) *err = "compressed entries are not supported (" + name + ")";
            return false;
        }
        // Strip the top folder; refuse names that climb out of the target.
        const size_t slash = name.find('/');
        std::string rel = slash == std::string::npos ? name : name.substr(slash + 1);
        if (rel.empty() || rel.back() == '/' || rel.find("..") != std::string::npos || rel[0] == '/' || rel.find(':') != std::string::npos)
            continue;
        if (off + 30 > z.size() || get32(z, off) != 0x04034b50) return false;
        const size_t data = off + 30u + get16(z, off + 26) + get16(z, off + 28);
        if (data + csize > z.size()) return false;
        const std::string bytes = z.substr(data, csize);
        if (crc32(bytes.data(), bytes.size()) != crc) {
            if (err) *err = "damaged file in the archive: " + name;
            return false;
        }
        if (!write_atomic(folder / from_utf8(rel), bytes, err)) return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> find_files(const fs::path& folder, const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string fname = utf8(it->path().filename());
        for (const auto& n : names)
            if (_stricmp(n.c_str(), fname.c_str()) == 0) {
                bool dup = false;
                for (const auto& o : out) dup = dup || _stricmp(o.first.c_str(), n.c_str()) == 0;
                if (!dup) out.emplace_back(n, utf8(it->path()));
            }
    }
    return out;
}

}  // namespace himmel::show::files
