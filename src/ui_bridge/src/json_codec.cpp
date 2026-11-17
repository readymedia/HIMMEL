// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ui/json_codec.h"
#include "himmel/ui/panels_json.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::ui {

using namespace himmel::protocol;
using nlohmann::json;

namespace {

// DeckOrHit: deck 0 / 1 / "A" / "B", or "hit" = the HIT layer (M2; layer must be 0).
enum class DeckRule : uint8_t { None, Deck, DeckOrMaster, DeckOrHit };
enum class LayerRule : uint8_t { None, Required, Optional };  // Optional: absent = deck level (FX)
enum class Arg : uint8_t { None, Float, Toggle, Blend, Play, ClipId, EffectId, TakeMs,
                           LayerSet, Trigger, Cue, BeatLoop, Scratch, ClockArg };

struct Spec {
    CommandType type;
    const char* name;
    DeckRule deck;
    LayerRule layer;
    bool slot;
    bool param;
    Arg arg;
    float lo, hi;     // Float range (absolute)
    bool relative_ok;
};

constexpr Spec kSpecs[] = {
    {CommandType::LayerClip, "LayerClip", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::ClipId, 0, 0, false},
    {CommandType::LayerOpacity, "LayerOpacity", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Float, 0, 1, true},
    {CommandType::LayerBlend, "LayerBlend", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Blend, 0, 0, false},
    {CommandType::LayerMute, "LayerMute", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Toggle, 0, 0, false},
    {CommandType::LayerPlay, "LayerPlay", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Play, 0, 0, false},
    {CommandType::LayerSpeed, "LayerSpeed", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Float, -4, 4, true},
    {CommandType::LayerJump, "LayerJump", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Float, 0, 1, false},
    {CommandType::FxSet, "FxSet", DeckRule::DeckOrMaster, LayerRule::Optional, true, false, Arg::EffectId, 0, 0, false},
    {CommandType::FxParam, "FxParam", DeckRule::DeckOrMaster, LayerRule::Optional, true, true, Arg::Float, 0, 1, true},
    {CommandType::FxBypass, "FxBypass", DeckRule::DeckOrMaster, LayerRule::Optional, true, false, Arg::Toggle, 0, 0, false},
    {CommandType::FxMix, "FxMix", DeckRule::DeckOrMaster, LayerRule::Optional, true, false, Arg::Float, 0, 1, true},
    {CommandType::FxPress, "FxPress", DeckRule::DeckOrMaster, LayerRule::Optional, true, false, Arg::Float, 0, 1, false},
    {CommandType::Crossfader, "Crossfader", DeckRule::None, LayerRule::None, false, false, Arg::Float, 0, 1, true},
    {CommandType::Take, "Take", DeckRule::None, LayerRule::None, false, false, Arg::TakeMs, 0, 0, false},
    {CommandType::Cut, "Cut", DeckRule::None, LayerRule::None, false, false, Arg::None, 0, 0, false},
    {CommandType::Black, "Black", DeckRule::None, LayerRule::None, false, false, Arg::Toggle, 0, 0, false},
    {CommandType::Focus, "Focus", DeckRule::Deck, LayerRule::Required, false, false, Arg::None, 0, 0, false},
    // ---- clip transport (M2, command-protocol.md §4a) ----
    {CommandType::LayerSet, "LayerSet", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::LayerSet, 0, 0, true},
    {CommandType::LayerTrigger, "LayerTrigger", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Trigger, 0, 0, false},
    {CommandType::LayerCue, "LayerCue", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Cue, 0, 0, false},
    {CommandType::LayerBeatLoop, "LayerBeatLoop", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::BeatLoop, 0, 16, false},
    {CommandType::LayerScratch, "LayerScratch", DeckRule::DeckOrHit, LayerRule::Required, false, false, Arg::Scratch, 0, 1, true},
    {CommandType::Clock, "Clock", DeckRule::None, LayerRule::None, false, false, Arg::ClockArg, 0, 0, true},
};

const Spec* find_spec(CommandType t) {
    for (const auto& s : kSpecs)
        if (s.type == t) return &s;
    return nullptr;
}

const char* const kBlendNames[] = {"Normal", "Add", "Screen", "Multiply", "Difference", "Lighten"};
static_assert(std::size(kBlendNames) == static_cast<size_t>(BlendMode::Count));
const char* const kPlayNames[] = {"Pause", "Play", "Toggle", "Restart", "Release", "Stop"};
static_assert(std::size(kPlayNames) == static_cast<size_t>(PlayAction::Count));
const char* const kRejectNames[] = {"None", "UnknownType", "BadTarget", "BadId", "BadValue", "RateLimited"};
static_assert(std::size(kRejectNames) == static_cast<size_t>(Reject::Count));

// ---- clip transport names (M2). The page uses the same words. ----
const char* const kModeNames[] = {"Loop", "PingPong", "OnceHold", "OnceEject", "OnceNext", "Random", "Chop", "Step", "Scratch"};
const char* const kTimeBaseNames[] = {"Free", "Bpm"};
const char* const kTriggerNames[] = {"Normal", "Toggle", "Piano", "Legato"};
const char* const kStartNames[] = {"Restart", "Continue"};
const char* const kTargetNames[] = {"Own", "Free", "Hit"};
const char* const kAutopilotNames[] = {"Off", "Next", "Previous", "Random", "Same"};
const char* const kTransitionNames[] = {"Cut", "Fade", "BlackDip"};
const char* const kGridNames[] = {"Off", "Quarter", "Beat", "Bar", "TwoBars", "FourBars"};
const char* const kCueNames[] = {"Jump", "Set", "Clear"};
const char* const kClockNames[] = {"Tap", "Bpm", "Nudge", "Resync", "Grid"};
const char* const kPhaseNames[] = {"Empty", "Playing", "Paused", "Holding", "Intro", "InnerLoop", "Outro"};
static_assert(std::size(kModeNames) == static_cast<size_t>(PlayMode::Count));
static_assert(std::size(kTimeBaseNames) == static_cast<size_t>(TimeBase::Count));
static_assert(std::size(kTriggerNames) == static_cast<size_t>(TriggerStyle::Count));
static_assert(std::size(kStartNames) == static_cast<size_t>(StartPos::Count));
static_assert(std::size(kTargetNames) == static_cast<size_t>(ClipTarget::Count));
static_assert(std::size(kAutopilotNames) == static_cast<size_t>(AutopilotMode::Count));
static_assert(std::size(kTransitionNames) == static_cast<size_t>(TransitionType::Count));
static_assert(std::size(kGridNames) == static_cast<size_t>(GridDiv::Count));
static_assert(std::size(kCueNames) == static_cast<size_t>(CueAction::Count));
static_assert(std::size(kClockNames) == static_cast<size_t>(ClockParam::Count));
static_assert(std::size(kPhaseNames) == static_cast<size_t>(PlayheadPhase::Count));

enum class PKind : uint8_t { Enum, Bool, Toggle, Int, Float };
struct ParamSpec {
    LayerParam p;
    const char* name;
    PKind kind;
    double lo, hi;                // Int / Float: range (clamped like the engine)
    const char* const* labels;    // Enum: value names
    size_t count;                 // Enum: number of values
    bool minus_one_ok;            // Snap: -1 = follow the global grid
};
#define ENUM_P(P, NAME, LABELS, M1) {LayerParam::P, NAME, PKind::Enum, 0, 0, LABELS, std::size(LABELS), M1}
const ParamSpec kParamSpecs[] = {
    ENUM_P(Mode, "Mode", kModeNames, false),
    {LayerParam::Direction, "Direction", PKind::Bool, 0, 1, nullptr, 0, false},
    {LayerParam::In, "In", PKind::Float, 0, 1, nullptr, 0, false},
    {LayerParam::Out, "Out", PKind::Float, 0, 1, nullptr, 0, false},
    {LayerParam::LoopIn, "LoopIn", PKind::Float, 0, 1, nullptr, 0, false},
    {LayerParam::LoopOut, "LoopOut", PKind::Float, 0, 1, nullptr, 0, false},
    {LayerParam::InnerLoop, "InnerLoop", PKind::Toggle, 0, 2, nullptr, 0, false},
    ENUM_P(TimeBase, "TimeBase", kTimeBaseNames, false),
    {LayerParam::Beats, "Beats", PKind::Float, 0.25, 64, nullptr, 0, false},
    {LayerParam::Interval, "Interval", PKind::Float, 1.0 / 16.0, 64, nullptr, 0, false},
    {LayerParam::Distance, "Distance", PKind::Float, 0, 1, nullptr, 0, false},
    {LayerParam::Slices, "Slices", PKind::Int, 2, 16, nullptr, 0, false},
    {LayerParam::StepFrames, "StepFrames", PKind::Int, 1, 64, nullptr, 0, false},
    {LayerParam::StepDiv, "StepDiv", PKind::Float, 1.0 / 16.0, 16, nullptr, 0, false},
    {LayerParam::Glide, "Glide", PKind::Float, 0, 16, nullptr, 0, false},
    ENUM_P(Trigger, "Trigger", kTriggerNames, false),
    ENUM_P(StartPos, "StartPos", kStartNames, false),
    {LayerParam::FadeIn, "FadeIn", PKind::Float, 0, 10, nullptr, 0, false},
    {LayerParam::FadeOut, "FadeOut", PKind::Float, 0, 10, nullptr, 0, false},
    ENUM_P(Snap, "Snap", kGridNames, true),
    ENUM_P(Target, "Target", kTargetNames, false),
    ENUM_P(Autopilot, "Autopilot", kAutopilotNames, false),
    {LayerParam::AutopilotCount, "AutopilotCount", PKind::Int, 1, 64, nullptr, 0, false},
    ENUM_P(Transition, "Transition", kTransitionNames, false),
    {LayerParam::TransitionTime, "TransitionTime", PKind::Float, 0, 10, nullptr, 0, false},
    {LayerParam::CatchUp, "CatchUp", PKind::Toggle, 0, 2, nullptr, 0, false},
    {LayerParam::ScratchSnap, "ScratchSnap", PKind::Toggle, 0, 2, nullptr, 0, false},
    {LayerParam::ReverseHold, "ReverseHold", PKind::Bool, 0, 1, nullptr, 0, false},
    {LayerParam::Seed, "Seed", PKind::Int, 0, 2147483647.0, nullptr, 0, false},
};
#undef ENUM_P
static_assert(std::size(kParamSpecs) == static_cast<size_t>(LayerParam::Count));

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Index of a name in a list (case-insensitive); -1 when it is not there.
int name_index(const json& v, const char* const* names, size_t n) {
    if (!v.is_string()) return -1;
    const std::string t = v.get<std::string>();
    for (size_t k = 0; k < n; ++k)
        if (iequals(t, names[k])) return static_cast<int>(k);
    return -1;
}

// Reads an integer field. Accepts JSON integers, and floats that are whole numbers.
bool get_int(const json& v, int64_t& out) {
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

double round4(float v) { return std::round(static_cast<double>(v) * 10000.0) / 10000.0; }

std::string fixed_str(const char* p, size_t cap) {
    size_t n = 0;
    while (n < cap && p[n]) ++n;
    return std::string(p, n);
}

json fx_to_json(const FxSlotState& f) {
    json j;
    j["effect"] = f.effect;
    if (f.effect < 0) return j;
    j["bypass"] = f.bypass != 0;
    j["mix"] = round4(f.mix);
    j["press"] = std::round(f.press / 255.0 * 1000.0) / 1000.0;
    json p = json::array();
    for (int k = 0; k < kFxParams; ++k) p.push_back(round4(f.param[k]));
    j["param"] = std::move(p);
    return j;
}

json fx_slots_to_json(const FxSlotState (&fx)[kFxSlots]) {
    json a = json::array();
    for (int k = 0; k < kFxSlots; ++k) a.push_back(fx_to_json(fx[k]));
    return a;
}

json layer_to_json(const LayerState& L) {
    json lj;
    lj["clip"] = L.clip;
    lj["blend"] = L.blend;
    lj["mute"] = L.mute != 0;
    lj["playing"] = L.playing != 0;
    lj["missing"] = L.missing != 0;
    lj["opacity"] = round4(L.opacity);
    lj["speed"] = round4(L.speed);
    lj["position"] = std::round(static_cast<double>(L.position) * 1000.0) / 1000.0;
    lj["fx"] = fx_slots_to_json(L.fx);
    return lj;
}

const char* label_or(const char* const* names, size_t n, int v, const char* other) {
    return (v >= 0 && static_cast<size_t>(v) < n) ? names[v] : other;
}

}  // namespace

const char* command_type_name(CommandType t) {
    if (t == CommandType::Hello) return "Hello";
    if (t == CommandType::Nop) return "Nop";
    const Spec* s = find_spec(t);
    if (s) return s->name;
    switch (t) {   // show / output commands (panels_json.cpp)
        case CommandType::SceneRecall: return "SceneRecall";
        case CommandType::SceneSave: return "SceneSave";
        case CommandType::Undo: return "Undo";
        case CommandType::SceneReset: return "SceneReset";
        case CommandType::Dice: return "Dice";
        case CommandType::Locks: return "Locks";
        case CommandType::Project: return "Project";
        case CommandType::ShowSelect: return "ShowSelect";
        case CommandType::KitSet: return "KitSet";
        case CommandType::MacroSet: return "MacroSet";
        case CommandType::MacroValue: return "MacroValue";
        case CommandType::SongEdit: return "SongEdit";
        case CommandType::OutputBlack: return "OutputBlack";
        case CommandType::Identify: return "Identify";
        case CommandType::LayerPlace: return "LayerPlace";
        case CommandType::OutputConfig: return "OutputConfig";
        default: return "Unknown";
    }
}

bool command_type_from_name(std::string_view name, CommandType& out) {
    for (const auto& s : kSpecs)
        if (iequals(name, s.name)) {
            out = s.type;
            return true;
        }
    return false;
}

const char* reject_name(uint8_t r) { return r < std::size(kRejectNames) ? kRejectNames[r] : "Unknown"; }
const char* blend_name(int b) { return (b >= 0 && b < static_cast<int>(std::size(kBlendNames))) ? kBlendNames[b] : "?"; }

ParseResult command_from_json(const json& j, const CatalogLimits& limits) {
    ParseResult r;
    auto fail = [&](std::string msg) {
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };
    if (!j.is_object()) return fail("a command must be a JSON object");
    const auto it_type = j.find("type");
    if (it_type == j.end() || !it_type->is_string()) return fail("missing \"type\"");
    const std::string tname = it_type->get<std::string>();
    if (iequals(tname, "Hello")) return fail("Hello is sent by the bridge itself");
    // Show (25..36) and output (70..73) commands: their own field-driven parser (panels_json.cpp).
    if (ParseResult pr; panel_command_from_json(j, pr)) return pr;
    CommandType type{};
    if (!command_type_from_name(tname, type)) return fail("unknown command type \"" + tname + "\"");
    const Spec& spec = *find_spec(type);

    Command c = make(type);

    // ---- targets ----
    auto field = [&](const char* key) -> const json* {
        const auto it = j.find(key);
        return (it == j.end() || it->is_null()) ? nullptr : &*it;
    };
    if (spec.deck != DeckRule::None) {
        const json* d = field("deck");
        if (!d) return fail(std::string(spec.name) + " needs \"deck\"");
        int64_t v = 0;
        if (d->is_string() && spec.deck == DeckRule::DeckOrMaster && iequals(d->get<std::string>(), "master")) {
            c.deck = kMasterDeck;
        } else if (d->is_string() && spec.deck == DeckRule::DeckOrHit && iequals(d->get<std::string>(), "hit")) {
            c.deck = kHitDeck;
        } else if (d->is_string() && (iequals(d->get<std::string>(), "A") || iequals(d->get<std::string>(), "B"))) {
            c.deck = iequals(d->get<std::string>(), "A") ? 0 : 1;
        } else if (get_int(*d, v) && v >= 0 && v < kDecks) {
            c.deck = static_cast<uint8_t>(v);
        } else {
            return fail("\"deck\" must be 0 (A) or 1 (B)" +
                        std::string(spec.deck == DeckRule::DeckOrMaster ? " or \"master\""
                                    : spec.deck == DeckRule::DeckOrHit  ? " or \"hit\""
                                                                        : ""));
        }
    }
    if (spec.layer != LayerRule::None) {
        const json* l = field("layer");
        if (c.deck == kHitDeck) {
            int64_t v = 0;
            if (l && (!get_int(*l, v) || v != 0)) return fail("the HIT layer is layer 0");
            c.layer = 0;
        } else if (!l) {
            if (spec.layer == LayerRule::Required) return fail(std::string(spec.name) + " needs \"layer\"");
        } else {
            int64_t v = 0;
            if (c.deck == kMasterDeck) return fail("master FX have no layer");
            if (!get_int(*l, v) || v < 0 || v >= kLayersPerDeck) return fail("\"layer\" must be 0..3");
            c.layer = static_cast<uint8_t>(v);
        }
    }
    if (spec.slot) {
        const json* s = field("slot");
        int64_t v = 0;
        if (!s || !get_int(*s, v) || v < 0 || v >= kFxSlots) return fail("\"slot\" must be 0..3");
        c.slot = static_cast<uint8_t>(v);
    }
    if (spec.param) {
        const json* p = field("param");
        int64_t v = 0;
        if (!p || !get_int(*p, v) || v < 0 || v >= kFxParams) return fail("\"param\" must be 0..7");
        c.param = static_cast<uint8_t>(v);
    }

    // ---- argument ----
    const json* val = field("value");
    bool relative = false;
    if (const json* rel = field("relative")) {
        if (!rel->is_boolean()) return fail("\"relative\" must be true or false");
        relative = rel->get<bool>();
    }
    if (relative && !spec.relative_ok) return fail(std::string(spec.name) + " cannot be relative");

    auto need_val = [&]() -> bool { return val != nullptr; };
    // A finite number from `v`, or an error.
    auto get_float = [&](const json* v, const char* what, double& out) -> bool {
        if (!v || !v->is_number()) {
            fail(std::string(spec.name) + " needs a number \"" + what + "\"");
            return false;
        }
        out = v->get<double>();
        if (!std::isfinite(out)) {
            fail(std::string("\"") + what + "\" must be finite");
            return false;
        }
        return true;
    };
    switch (spec.arg) {
        case Arg::None: break;
        case Arg::Float: {
            double d = 0;
            if (!get_float(val, "value", d)) return r;
            float f = static_cast<float>(d);
            if (relative) {
                const float span = spec.hi - spec.lo;
                f = std::clamp(f, -span, span);
            } else {
                f = std::clamp(f, spec.lo, spec.hi);  // in range = clamped, like the engine
            }
            c.f[0] = f;
            break;
        }
        case Arg::Toggle: {
            if (!need_val()) return fail(std::string(spec.name) + " needs \"value\" (0, 1, 2 = toggle)");
            int64_t v = 0;
            if (val->is_boolean()) v = val->get<bool>() ? 1 : 0;
            else if (val->is_string() && iequals(val->get<std::string>(), "toggle")) v = 2;
            else if (!get_int(*val, v) || v < 0 || v > 2) return fail("\"value\" must be 0, 1 or 2 (toggle)");
            c.i = static_cast<int32_t>(v);
            break;
        }
        case Arg::Blend: {
            if (!need_val()) return fail("LayerBlend needs \"value\"");
            int64_t v = -1;
            if (val->is_string()) {
                v = name_index(*val, kBlendNames, std::size(kBlendNames));
                if (v < 0) return fail("unknown blend mode \"" + val->get<std::string>() + "\"");
            } else if (!get_int(*val, v) || v < 0 || v >= static_cast<int64_t>(BlendMode::Count)) {
                return fail("blend mode out of range");
            }
            c.i = static_cast<int32_t>(v);
            break;
        }
        case Arg::Play: {
            if (!need_val()) return fail("LayerPlay needs \"value\" (Pause, Play, Toggle, Restart, Release, Stop)");
            int64_t v = -1;
            if (val->is_string()) {
                v = name_index(*val, kPlayNames, std::size(kPlayNames));
                if (v < 0) return fail("unknown play action \"" + val->get<std::string>() + "\"");
            } else if (!get_int(*val, v) || v < 0 || v >= static_cast<int64_t>(PlayAction::Count)) {
                return fail("play action out of range");
            }
            c.i = static_cast<int32_t>(v);
            break;
        }
        case Arg::ClipId:
        case Arg::EffectId: {
            const bool clip = spec.arg == Arg::ClipId;
            int64_t v = 0;
            if (!need_val() || !get_int(*val, v)) return fail(std::string(spec.name) + " needs an id \"value\" (-1 = empty)");
            const int count = clip ? limits.clips : limits.effects;
            const int hard = clip ? kMaxClips : kMaxEffects;
            if (v < -1 || v >= hard || (count >= 0 && v >= count))
                return fail(std::string(clip ? "clip" : "effect") + " id " + std::to_string(v) + " is not in the catalog");
            c.i = static_cast<int32_t>(v);
            break;
        }
        case Arg::TakeMs: {
            int64_t v = 0;
            if (val && !get_int(*val, v)) return fail("Take \"value\" must be whole milliseconds");
            c.i = static_cast<int32_t>(std::clamp<int64_t>(v, 0, 60000));
            break;
        }
        case Arg::LayerSet: {
            // {"param": "Mode" | 0..28, "value": number or a value name}
            const json* p = field("param");
            int pi = -1;
            int64_t pv = 0;
            if (p && p->is_string()) {
                for (size_t k = 0; k < std::size(kParamSpecs); ++k)
                    if (iequals(p->get<std::string>(), kParamSpecs[k].name)) pi = static_cast<int>(k);
                if (pi < 0) return fail("unknown LayerSet param \"" + p->get<std::string>() + "\"");
            } else if (p && get_int(*p, pv) && pv >= 0 && pv < static_cast<int64_t>(std::size(kParamSpecs))) {
                pi = static_cast<int>(pv);
            } else {
                return fail("LayerSet needs \"param\" (a LayerParam name or 0..28)");
            }
            const ParamSpec& ps = kParamSpecs[pi];
            c.param = static_cast<uint8_t>(ps.p);
            if (!need_val()) return fail(std::string("LayerSet ") + ps.name + " needs \"value\"");
            if (relative && ps.kind != PKind::Float) return fail(std::string("LayerSet ") + ps.name + " cannot be relative");
            switch (ps.kind) {
                case PKind::Enum: {
                    int64_t v = -1;
                    if (val->is_string()) {
                        v = name_index(*val, ps.labels, ps.count);
                        if (v < 0 && !(ps.minus_one_ok && iequals(val->get<std::string>(), "global")))
                            return fail(std::string("unknown ") + ps.name + " \"" + val->get<std::string>() + "\"");
                    } else if (!get_int(*val, v)) {
                        return fail(std::string(ps.name) + " needs a whole number or a name");
                    }
                    if (v < (ps.minus_one_ok ? -1 : 0) || v >= static_cast<int64_t>(ps.count))
                        return fail(std::string(ps.name) + " value out of range");
                    c.i = static_cast<int32_t>(v);
                    break;
                }
                case PKind::Bool:
                case PKind::Toggle: {
                    int64_t v = 0;
                    const int64_t top = ps.kind == PKind::Toggle ? 2 : 1;
                    if (val->is_boolean()) v = val->get<bool>() ? 1 : 0;
                    else if (ps.kind == PKind::Toggle && val->is_string() && iequals(val->get<std::string>(), "toggle")) v = 2;
                    else if (ps.p == LayerParam::Direction && val->is_string() &&
                             (iequals(val->get<std::string>(), "forward") || iequals(val->get<std::string>(), "backward")))
                        v = iequals(val->get<std::string>(), "backward") ? 1 : 0;
                    else if (!get_int(*val, v) || v < 0 || v > top)
                        return fail(std::string(ps.name) + (top == 2 ? " must be 0, 1 or 2 (toggle)" : " must be 0 or 1"));
                    c.i = static_cast<int32_t>(v);
                    break;
                }
                case PKind::Int: {
                    int64_t v = 0;
                    if (!get_int(*val, v)) return fail(std::string(ps.name) + " needs a whole number");
                    c.i = static_cast<int32_t>(std::clamp<int64_t>(v, static_cast<int64_t>(ps.lo), static_cast<int64_t>(ps.hi)));
                    break;
                }
                case PKind::Float: {
                    double d = 0;
                    if (!get_float(val, "value", d)) return r;
                    const double span = ps.hi - ps.lo;
                    c.f[0] = static_cast<float>(relative ? std::clamp(d, -span, span) : std::clamp(d, ps.lo, ps.hi));
                    break;
                }
            }
            break;
        }
        case Arg::Trigger: {
            // {"value": clip id or -1 (the layer's clip), "velocity": 0..1 (0 = full), "release": bool}
            int64_t v = -1;
            if (val && !get_int(*val, v)) return fail("LayerTrigger \"value\" must be a clip id (-1 = the layer's clip)");
            const int count = limits.clips;
            if (v < -1 || v >= kMaxClips || (count >= 0 && v >= count))
                return fail("clip id " + std::to_string(v) + " is not in the catalog");
            c.i = static_cast<int32_t>(v);
            if (const json* vel = field("velocity")) {
                double d = 0;
                if (!get_float(vel, "velocity", d)) return r;
                c.f[0] = static_cast<float>(std::clamp(d, 0.0, 1.0));
            }
            if (const json* rel = field("release")) {
                if (!rel->is_boolean()) return fail("\"release\" must be true or false");
                if (rel->get<bool>()) c.flags |= kRelease;
            }
            break;
        }
        case Arg::Cue: {
            // {"cue": 0..15, "value": "Jump" | "Set" | "Clear" (or 0..2), "pos": 0..1 (Set; < 0 / absent = the playhead)}
            const json* q = field("cue");
            int64_t qi = 0;
            if (!q || !get_int(*q, qi) || qi < 0 || qi >= kCuePoints) return fail("LayerCue needs \"cue\" 0..15");
            c.slot = static_cast<uint8_t>(qi);
            int64_t a = val ? name_index(*val, kCueNames, std::size(kCueNames)) : -1;
            if (val && a < 0 && !val->is_string() && (!get_int(*val, a) || a < 0 || a >= static_cast<int64_t>(CueAction::Count)))
                return fail("LayerCue action out of range");
            if (a < 0) return fail("LayerCue needs \"value\" (Jump, Set or Clear)");
            c.i = static_cast<int32_t>(a);
            c.f[0] = -1.f;
            if (const json* pos = field("pos")) {
                double d = 0;
                if (!get_float(pos, "pos", d)) return r;
                c.f[0] = d < 0 ? -1.f : static_cast<float>(std::min(d, 1.0));
            }
            break;
        }
        case Arg::BeatLoop: {
            double d = 0;
            if (!get_float(val, "value", d)) return r;
            c.f[0] = d <= 0 ? 0.f : static_cast<float>(std::clamp(d, 1.0 / 16.0, 16.0));
            break;
        }
        case Arg::Scratch: {
            // {"value": 1 hold / move, 0 let go, "pos": 0..1 of the clip (relative: delta in clip seconds)}
            int64_t v = 0;
            if (!val) return fail("LayerScratch needs \"value\" (1 = hold / move, 0 = let go)");
            if (val->is_boolean()) v = val->get<bool>() ? 1 : 0;
            else if (!get_int(*val, v) || v < 0 || v > 1) return fail("LayerScratch \"value\" must be 0 or 1");
            c.i = static_cast<int32_t>(v);
            if (const json* pos = field("pos")) {
                double d = 0;
                if (!get_float(pos, "pos", d)) return r;
                c.f[0] = static_cast<float>(relative ? std::clamp(d, -3600.0, 3600.0) : std::clamp(d, 0.0, 1.0));
            } else if (relative) {
                return fail("a relative LayerScratch needs \"pos\" (seconds)");
            }
            break;
        }
        case Arg::ClockArg: {
            // {"param": "Tap" | "Bpm" | "Nudge" | "Resync" | "Grid", "value": ...}
            const json* p = field("param");
            int64_t pi = p ? name_index(*p, kClockNames, std::size(kClockNames)) : -1;
            if (p && pi < 0 && !p->is_string() && (!get_int(*p, pi) || pi < 0 || pi >= static_cast<int64_t>(ClockParam::Count)))
                pi = -1;
            if (pi < 0) return fail("Clock needs \"param\" (Tap, Bpm, Nudge, Resync or Grid)");
            c.param = static_cast<uint8_t>(pi);
            const auto cp = static_cast<ClockParam>(pi);
            if (relative && cp != ClockParam::Bpm) return fail("only Clock Bpm can be relative");
            if (cp == ClockParam::Bpm) {
                double d = 0;
                if (!get_float(val, "value", d)) return r;
                c.f[0] = static_cast<float>(relative ? std::clamp(d, -280.0, 280.0) : std::clamp(d, 20.0, 300.0));
            } else if (cp == ClockParam::Nudge) {
                double d = 0;
                if (!get_float(val, "value", d)) return r;
                c.f[0] = static_cast<float>(std::clamp(d, -1.0, 1.0));
            } else if (cp == ClockParam::Grid) {
                if (!val) return fail("Clock Grid needs \"value\"");
                int64_t g = name_index(*val, kGridNames, std::size(kGridNames));
                if (g < 0 && (val->is_string() || !get_int(*val, g) || g < 0 || g >= static_cast<int64_t>(GridDiv::Count)))
                    return fail("unknown grid \"" + (val->is_string() ? val->get<std::string>() : val->dump()) + "\"");
                c.i = static_cast<int32_t>(g);
            }
            break;
        }
    }
    if (relative) c.flags |= kRelative;

    r.ok = true;
    r.cmd = c;
    return r;
}

json command_to_json(const Command& c) {
    json j;
    j["type"] = command_type_name(c.type);
    if (c.deck == kMasterDeck) j["deck"] = "master";
    else if (c.deck == kHitDeck) j["deck"] = "hit";
    else if (c.deck != kNone) j["deck"] = c.deck;
    if (c.layer != kNone) j["layer"] = c.layer;
    const Spec* s = find_spec(c.type);
    const Arg a = s ? s->arg : Arg::None;
    if (a == Arg::Cue) {
        if (c.slot != kNone) j["cue"] = c.slot;
        j["value"] = label_or(kCueNames, std::size(kCueNames), c.i, "?");
        if (c.i == static_cast<int32_t>(CueAction::Set)) j["pos"] = c.f[0];
    } else if (a == Arg::LayerSet) {
        const int p = c.param;
        if (p < static_cast<int>(std::size(kParamSpecs))) {
            const ParamSpec& ps = kParamSpecs[p];
            j["param"] = ps.name;
            if (ps.kind == PKind::Float) j["value"] = c.f[0];
            else if (ps.kind == PKind::Enum && c.i >= 0) j["value"] = label_or(ps.labels, ps.count, c.i, "?");
            else j["value"] = c.i;
        } else {
            j["param"] = p;
        }
    } else if (a == Arg::ClockArg) {
        j["param"] = label_or(kClockNames, std::size(kClockNames), c.param, "?");
        if (c.param == static_cast<uint8_t>(ClockParam::Grid)) j["value"] = label_or(kGridNames, std::size(kGridNames), c.i, "?");
        else if (c.param == static_cast<uint8_t>(ClockParam::Bpm) || c.param == static_cast<uint8_t>(ClockParam::Nudge))
            j["value"] = c.f[0];
    } else if (a == Arg::Trigger) {
        j["value"] = c.i;
        j["velocity"] = c.f[0];
        if (c.flags & kRelease) j["release"] = true;
    } else if (a == Arg::Scratch) {
        j["value"] = c.i;
        j["pos"] = c.f[0];
    } else {
        if (c.slot != kNone) j["slot"] = c.slot;
        if (c.param != kNone) j["param"] = c.param;
        if (a == Arg::Float || a == Arg::BeatLoop) j["value"] = c.f[0];
        else if (a != Arg::None) j["value"] = c.i;
    }
    if (c.flags & kRelative) j["relative"] = true;
    if (c.seq) j["seq"] = c.seq;
    return j;
}

json live_to_json(const LiveState& s) {
    json j;
    j["epoch"] = s.epoch;
    j["crossfader"] = round4(s.crossfader);
    j["program_deck"] = s.program_deck;
    j["take_active"] = s.take_active != 0;
    j["take_progress"] = round4(s.take_progress);
    j["take_ms"] = s.take_ms;
    j["black"] = s.black != 0;
    j["focus_deck"] = s.focus_deck == kNone ? -1 : static_cast<int>(s.focus_deck);
    j["focus_layer"] = s.focus_layer == kNone ? -1 : static_cast<int>(s.focus_layer);
    json decks = json::array();
    for (int d = 0; d < kDecks; ++d) {
        json dj;
        json layers = json::array();
        for (int l = 0; l < kLayersPerDeck; ++l) layers.push_back(layer_to_json(s.deck[d].layer[l]));
        dj["layer"] = std::move(layers);
        dj["fx"] = fx_slots_to_json(s.deck[d].fx);
        decks.push_back(std::move(dj));
    }
    j["deck"] = std::move(decks);
    j["master_fx"] = fx_slots_to_json(s.master_fx);
    json prod = json::array();
    for (int p = 0; p < kProducers; ++p) {
        prod.push_back({{"acked", s.acked_seq[p]},
                        {"rejected", s.rejected[p]},
                        {"last_reject", reject_name(s.last_reject[p])},
                        {"alive", s.producer_alive[p] != 0}});
    }
    j["producer"] = std::move(prod);
    return j;
}

json catalog_to_json(const Catalog& c) {
    json j;
    json clips = json::array();
    const uint32_t nc = std::min<uint32_t>(c.clip_count, kMaxClips);
    for (uint32_t k = 0; k < nc; ++k) {
        const ClipInfo& ci = c.clip[k];
        clips.push_back({{"id", k},
                         {"name", fixed_str(ci.name, kNameLen)},
                         {"width", ci.width},
                         {"height", ci.height},
                         {"fps", round4(ci.fps)},
                         {"seconds", round4(ci.seconds)},
                         {"codec", ci.codec},
                         {"thumb_rgb", ci.thumb_rgb}});
    }
    json fx = json::array();
    const uint32_t ne = std::min<uint32_t>(c.effect_count, kMaxEffects);
    for (uint32_t k = 0; k < ne; ++k) {
        const EffectInfo& e = c.effect[k];
        json pn = json::array(), pd = json::array();
        const int np = std::min<int>(e.param_count, kFxParams);
        for (int p = 0; p < np; ++p) {
            pn.push_back(fixed_str(e.param_name[p], kParamNameLen));
            pd.push_back(round4(e.param_default[p]));
        }
        fx.push_back({{"id", k},
                      {"name", fixed_str(e.name, kNameLen)},
                      {"kind", e.reserved[0] == 1 ? "generator" : "effect"},
                      {"params", pn},
                      {"defaults", pd}});
    }
    j["clips"] = std::move(clips);
    j["effects"] = std::move(fx);
    return j;
}

namespace {
json transport_layer_to_json(const LayerTransport& t) {
    json j;
    j["mode"] = label_or(kModeNames, std::size(kModeNames), t.mode, "?");
    j["time_base"] = label_or(kTimeBaseNames, std::size(kTimeBaseNames), t.time_base, "?");
    j["trigger"] = label_or(kTriggerNames, std::size(kTriggerNames), t.trigger, "?");
    j["start"] = label_or(kStartNames, std::size(kStartNames), t.start, "?");
    j["target"] = label_or(kTargetNames, std::size(kTargetNames), t.target, "?");
    j["autopilot"] = label_or(kAutopilotNames, std::size(kAutopilotNames), t.autopilot, "?");
    j["transition"] = label_or(kTransitionNames, std::size(kTransitionNames), t.transition, "?");
    j["snap"] = t.snap == kNone ? "Global" : label_or(kGridNames, std::size(kGridNames), t.snap, "?");
    j["inner_loop"] = t.inner_loop != 0;
    j["catch_up"] = t.catch_up != 0;
    j["scratch_snap"] = t.scratch_snap != 0;
    j["slices"] = t.slices;
    j["step_frames"] = t.step_frames;
    j["autopilot_count"] = t.autopilot_count;
    j["backward"] = t.backward != 0;
    j["reverse_hold"] = t.reverse_hold != 0;
    j["in"] = round4(t.in);
    j["out"] = round4(t.out);
    j["loop_in"] = round4(t.loop_in);
    j["loop_out"] = round4(t.loop_out);
    j["beats"] = round4(t.beats);
    j["interval"] = round4(t.interval);
    j["distance"] = round4(t.distance);
    j["step_div"] = round4(t.step_div);
    j["glide"] = round4(t.glide);
    j["fade_in"] = round4(t.fade_in);
    j["fade_out"] = round4(t.fade_out);
    j["transition_time"] = round4(t.transition_time);
    j["seed"] = t.seed;
    j["phase"] = label_or(kPhaseNames, std::size(kPhaseNames), t.phase, "?");
    j["beat_loop"] = t.beat_loop != 0;
    j["scratching"] = t.scratching != 0;
    j["pending"] = t.pending != 0;
    j["beat_loop_beats"] = round4(t.beat_loop_beats);
    j["speed_now"] = std::round(static_cast<double>(t.speed_now) * 100.0) / 100.0;
    j["gain"] = std::round(static_cast<double>(t.gain) * 100.0) / 100.0;
    j["transition_progress"] = std::round(static_cast<double>(t.transition_progress) * 100.0) / 100.0;
    j["pending_beats"] = std::round(static_cast<double>(t.pending_beats) * 10.0) / 10.0;
    j["velocity"] = std::round(static_cast<double>(t.velocity) * 100.0) / 100.0;
    j["loops"] = t.loops;
    j["frame"] = t.frame;
    json q = json::array();
    for (int k = 0; k < kCuePoints; ++k) q.push_back(t.cue[k] < 0 ? -1.0 : round4(t.cue[k]));
    j["cue"] = std::move(q);
    return j;
}
}  // namespace

json transport_to_json(const TransportSection& t) {
    json j;
    j["clock"] = {{"bpm", std::round(static_cast<double>(t.clock.bpm) * 100.0) / 100.0},
                  {"grid", label_or(kGridNames, std::size(kGridNames), t.clock.grid, "?")},
                  {"source", t.clock.source == 1 ? "tapped" : "internal"},
                  {"taps", t.clock.taps}};
    json decks = json::array();
    for (int d = 0; d < kDecks; ++d) {
        json layers = json::array();
        for (int l = 0; l < kLayersPerDeck; ++l) layers.push_back(transport_layer_to_json(t.layer[d][l]));
        decks.push_back(std::move(layers));
    }
    j["layer"] = std::move(decks);
    j["hit"] = layer_to_json(t.hit);
    j["hit"].erase("fx");
    j["hit_transport"] = transport_layer_to_json(t.hit_transport);
    return j;
}

json clock_to_json(const ClockState& c, uint64_t now_us) {
    // The beat at `now_us`, extrapolated from the engine's last sample.
    double beat = c.beat;
    if (now_us > c.time_us && c.time_us != 0) beat += (static_cast<double>(now_us - c.time_us) / 60e6) * c.bpm;
    return {{"beat", std::round(beat * 1000.0) / 1000.0}};
}

json transport_names() {
    auto list = [](const char* const* n, size_t k) {
        json a = json::array();
        for (size_t i = 0; i < k; ++i) a.push_back(n[i]);
        return a;
    };
    json params = json::array();
    for (const auto& ps : kParamSpecs) {
        json p = {{"name", ps.name}};
        switch (ps.kind) {
            case PKind::Enum: p["kind"] = "enum"; p["values"] = list(ps.labels, ps.count); break;
            case PKind::Bool: p["kind"] = "bool"; break;
            case PKind::Toggle: p["kind"] = "toggle"; break;
            case PKind::Int: p["kind"] = "int"; p["lo"] = ps.lo; p["hi"] = ps.hi; break;
            case PKind::Float: p["kind"] = "float"; p["lo"] = ps.lo; p["hi"] = ps.hi; break;
        }
        params.push_back(std::move(p));
    }
    return {{"params", params},
            {"play", list(kPlayNames, std::size(kPlayNames))},
            {"grid", list(kGridNames, std::size(kGridNames))},
            {"cue", list(kCueNames, std::size(kCueNames))},
            {"clock", list(kClockNames, std::size(kClockNames))},
            {"phase", list(kPhaseNames, std::size(kPhaseNames))},
            {"blend", list(kBlendNames, std::size(kBlendNames))}};
}

}  // namespace himmel::ui
