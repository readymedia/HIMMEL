// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ui/json_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::ui {

using namespace himmel::protocol;
using nlohmann::json;

namespace {

enum class DeckRule : uint8_t { None, Deck, DeckOrMaster };
enum class LayerRule : uint8_t { None, Required, Optional };  // Optional: absent = deck level (FX)
enum class Arg : uint8_t { None, Float, Toggle, Blend, Play, ClipId, EffectId, TakeMs };

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
    {CommandType::LayerClip, "LayerClip", DeckRule::Deck, LayerRule::Required, false, false, Arg::ClipId, 0, 0, false},
    {CommandType::LayerOpacity, "LayerOpacity", DeckRule::Deck, LayerRule::Required, false, false, Arg::Float, 0, 1, true},
    {CommandType::LayerBlend, "LayerBlend", DeckRule::Deck, LayerRule::Required, false, false, Arg::Blend, 0, 0, false},
    {CommandType::LayerMute, "LayerMute", DeckRule::Deck, LayerRule::Required, false, false, Arg::Toggle, 0, 0, false},
    {CommandType::LayerPlay, "LayerPlay", DeckRule::Deck, LayerRule::Required, false, false, Arg::Play, 0, 0, false},
    {CommandType::LayerSpeed, "LayerSpeed", DeckRule::Deck, LayerRule::Required, false, false, Arg::Float, -4, 4, true},
    {CommandType::LayerJump, "LayerJump", DeckRule::Deck, LayerRule::Required, false, false, Arg::Float, 0, 1, false},
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
};

const Spec* find_spec(CommandType t) {
    for (const auto& s : kSpecs)
        if (s.type == t) return &s;
    return nullptr;
}

const char* const kBlendNames[] = {"Normal", "Add", "Screen", "Multiply", "Difference", "Lighten"};
static_assert(std::size(kBlendNames) == static_cast<size_t>(BlendMode::Count));
const char* const kPlayNames[] = {"Pause", "Play", "Toggle", "Restart"};
const char* const kRejectNames[] = {"None", "UnknownType", "BadTarget", "BadId", "BadValue", "RateLimited"};
static_assert(std::size(kRejectNames) == static_cast<size_t>(Reject::Count));

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

// Reads an integer field. Accepts JSON integers, and floats that are whole numbers.
bool get_int(const json& v, int64_t& out) {
    if (v.is_number_integer()) {
        out = v.get<int64_t>();
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::floor(d) != d || std::fabs(d) > 1e9) return false;
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

}  // namespace

const char* command_type_name(CommandType t) {
    if (t == CommandType::Hello) return "Hello";
    if (t == CommandType::Nop) return "Nop";
    const Spec* s = find_spec(t);
    return s ? s->name : "Unknown";
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
        } else if (d->is_string() && (iequals(d->get<std::string>(), "A") || iequals(d->get<std::string>(), "B"))) {
            c.deck = iequals(d->get<std::string>(), "A") ? 0 : 1;
        } else if (get_int(*d, v) && v >= 0 && v < kDecks) {
            c.deck = static_cast<uint8_t>(v);
        } else {
            return fail("\"deck\" must be 0 (A) or 1 (B)" +
                        std::string(spec.deck == DeckRule::DeckOrMaster ? " or \"master\"" : ""));
        }
    }
    if (spec.layer != LayerRule::None) {
        const json* l = field("layer");
        if (!l) {
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
    if (relative) c.flags |= kRelative;

    auto need_val = [&]() -> bool { return val != nullptr; };
    switch (spec.arg) {
        case Arg::None: break;
        case Arg::Float: {
            if (!need_val() || !val->is_number()) return fail(std::string(spec.name) + " needs a number \"value\"");
            const double d = val->get<double>();
            if (!std::isfinite(d)) return fail("\"value\" must be finite");
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
                for (size_t k = 0; k < std::size(kBlendNames); ++k)
                    if (iequals(val->get<std::string>(), kBlendNames[k])) v = static_cast<int64_t>(k);
                if (v < 0) return fail("unknown blend mode \"" + val->get<std::string>() + "\"");
            } else if (!get_int(*val, v) || v < 0 || v >= static_cast<int64_t>(BlendMode::Count)) {
                return fail("blend mode out of range");
            }
            c.i = static_cast<int32_t>(v);
            break;
        }
        case Arg::Play: {
            if (!need_val()) return fail("LayerPlay needs \"value\" (Pause, Play, Toggle, Restart)");
            int64_t v = -1;
            if (val->is_string()) {
                for (size_t k = 0; k < std::size(kPlayNames); ++k)
                    if (iequals(val->get<std::string>(), kPlayNames[k])) v = static_cast<int64_t>(k);
                if (v < 0) return fail("unknown play action \"" + val->get<std::string>() + "\"");
            } else if (!get_int(*val, v) || v < 0 || v > 3) {
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
    }

    r.ok = true;
    r.cmd = c;
    return r;
}

json command_to_json(const Command& c) {
    json j;
    j["type"] = command_type_name(c.type);
    if (c.deck == kMasterDeck) j["deck"] = "master";
    else if (c.deck != kNone) j["deck"] = c.deck;
    if (c.layer != kNone) j["layer"] = c.layer;
    if (c.slot != kNone) j["slot"] = c.slot;
    if (c.param != kNone) j["param"] = c.param;
    const Spec* s = find_spec(c.type);
    if (s && s->arg == Arg::Float) j["value"] = c.f[0];
    else if (s && s->arg != Arg::None) j["value"] = c.i;
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
        for (int l = 0; l < kLayersPerDeck; ++l) {
            const LayerState& L = s.deck[d].layer[l];
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
            layers.push_back(std::move(lj));
        }
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
        fx.push_back({{"id", k}, {"name", fixed_str(e.name, kNameLen)}, {"params", pn}, {"defaults", pd}});
    }
    j["clips"] = std::move(clips);
    j["effects"] = std::move(fx);
    return j;
}

}  // namespace himmel::ui
