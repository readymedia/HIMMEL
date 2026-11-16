// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Effect metadata: parse + check the JSON embedded from fx/ (start-up only).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>

#include "himmel/fx/builtin_data.h"
#include "himmel/fx/fx.h"

namespace himmel::fx {

using nlohmann::json;

namespace {

void copy(char* dst, size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }

bool fail(std::string* err, const std::string& what) {
    if (err) *err = what;
    return false;
}

bool curve_from(const std::string& s, Curve& c) {
    if (s == "lin" || s == "linear") c = Curve::Linear;
    else if (s == "soft") c = Curve::Soft;
    else if (s == "hard") c = Curve::Hard;
    else if (s == "steps") c = Curve::Steps;
    else if (s == "exp") c = Curve::Exp;
    else return false;
    return true;
}

bool unit01(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }

constexpr const char* kSignalNames[kSignals] = {"none", "level", "bass",   "mid",    "high", "kick",  "snare",
                                                "hat",  "onset", "beat",   "bar",    "energy", "build", "drop"};

}  // namespace

const char* signal_name(Signal s) {
    const int i = static_cast<int>(s);
    return i >= 0 && i < kSignals ? kSignalNames[i] : "?";
}
bool signal_from_name(const char* name, Signal& out) {
    for (int i = 0; i < kSignals; ++i)
        if (std::strcmp(name, kSignalNames[i]) == 0) {
            out = static_cast<Signal>(i);
            return true;
        }
    return false;
}

int EffectMeta::find_param(const char* pid) const {
    for (int p = 0; p < params; ++p)
        if (std::strcmp(param[p].id, pid) == 0) return p;
    return -1;
}
int EffectMeta::find_preset(const char* pname) const {
    for (int p = 0; p < presets; ++p)
        if (std::strcmp(preset[p].name, pname) == 0) return p;
    return -1;
}

bool Library::parse(const char* text, EffectMeta& m, std::string* err) {
    m = EffectMeta{};
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail(err, "not valid JSON");
    try {
        const std::string id = j.at("id").get<std::string>();
        if (id.empty() || id.size() >= kIdLen) return fail(err, "bad id");
        copy(m.id, sizeof(m.id), id);
        copy(m.name, sizeof(m.name), j.at("name").get<std::string>());
        copy(m.family, sizeof(m.family), j.value("family", std::string("other")));
        const std::string kind = j.value("kind", std::string("effect"));
        if (kind == "effect") m.kind = Kind::Effect;
        else if (kind == "generator") m.kind = Kind::Generator;
        else return fail(err, id + ": kind must be effect or generator");
        m.tier = static_cast<uint8_t>(j.value("tier", 1));
        m.builtin = j.value("shelf", std::string("builtin")) == "builtin";

        const json& ps = j.at("params");
        if (!ps.is_array() || ps.empty() || ps.size() > kParams)
            return fail(err, id + ": params must be 1.." + std::to_string(kParams));
        m.params = static_cast<int>(ps.size());
        for (int p = 0; p < m.params; ++p) {
            const json& q = ps[p];
            ParamMeta& pm = m.param[p];
            copy(pm.id, sizeof(pm.id), q.at("id").get<std::string>());
            copy(pm.name, sizeof(pm.name), q.at("name").get<std::string>());
            copy(pm.unit, sizeof(pm.unit), q.value("unit", std::string()));
            pm.min = q.value("min", 0.0f);
            pm.max = q.value("max", 1.0f);
            pm.def = q.at("default").get<float>();
            if (q.contains("good")) {
                pm.good_lo = q["good"].at(0).get<float>();
                pm.good_hi = q["good"].at(1).get<float>();
            }
            pm.steps = static_cast<uint8_t>(q.value("steps", 0));
            pm.speed = q.value("speed", false);
            pm.exp_curve = q.value("curve", std::string("lin")) == "exp";
        }
        // scopes: {"param id": ["slot", "look", "global", "move"]}; default all
        if (j.contains("scopes")) {
            for (auto it = j["scopes"].begin(); it != j["scopes"].end(); ++it) {
                const int p = m.find_param(it.key().c_str());
                if (p < 0) return fail(err, id + ": scopes names unknown param " + it.key());
                uint8_t bits = 0;
                for (const auto& s : it.value()) {
                    const std::string v = s.get<std::string>();
                    if (v == "slot") bits |= scope_bit(Scope::Slot);
                    else if (v == "look") bits |= scope_bit(Scope::Look);
                    else if (v == "global") bits |= scope_bit(Scope::Global);
                    else if (v == "move") bits |= scope_bit(Scope::Move);
                    else return fail(err, id + ": unknown scope " + v);
                }
                m.param[p].scopes = bits;
            }
        }
        // main: the two main params (knobs 1-2, PMTR 1 / 2, Lightpad X / Y)
        const json& mn = j.at("main");
        if (!mn.is_array() || mn.size() != 2) return fail(err, id + ": main must name 2 params");
        for (int k = 0; k < 2; ++k) {
            const int p = m.find_param(mn[k].get<std::string>().c_str());
            if (p < 0) return fail(err, id + ": main names an unknown param");
            m.main[k] = static_cast<int8_t>(p);
        }
        // reaction (D88: every effect has one)
        if (!j.contains("reaction")) return fail(err, id + ": no default reaction (D88)");
        {
            const json& r = j["reaction"];
            Reaction& re = m.reaction;
            if (!signal_from_name(r.at("signal").get<std::string>().c_str(), re.signal))
                return fail(err, id + ": unknown reaction signal");
            re.param = static_cast<int8_t>(m.find_param(r.at("param").get<std::string>().c_str()));
            if (re.param < 0) return fail(err, id + ": reaction param unknown");
            re.amount = r.at("amount").get<float>();
            re.attack_ms = r.value("attack_ms", 10.0f);
            re.release_ms = r.value("release_ms", 200.0f);
            re.threshold = r.value("threshold", 0.0f);
            const ParamMeta& pm = m.param[re.param];
            re.range_lo = pm.good_lo, re.range_hi = pm.good_hi;
            if (r.contains("range")) re.range_lo = r["range"].at(0).get<float>(), re.range_hi = r["range"].at(1).get<float>();
            const std::string mode = r.value("mode", std::string("add"));
            if (mode == "add") re.mode = ReactMode::Add;
            else if (mode == "follow") re.mode = ReactMode::Follow;
            else if (mode == "trigger") re.mode = ReactMode::Trigger;
            else return fail(err, id + ": unknown reaction mode");
            if (!curve_from(r.value("curve", std::string("linear")), re.curve)) return fail(err, id + ": bad reaction curve");
            re.quantise_beats = r.value("quantise_beats", 0.0f);
            re.hold_beats = r.value("hold_beats", 0.0f);
        }
        // press
        if (!j.contains("press")) return fail(err, id + ": no press target");
        {
            const json& r = j["press"];
            m.press.param = static_cast<int8_t>(m.find_param(r.at("param").get<std::string>().c_str()));
            if (m.press.param < 0) return fail(err, id + ": press param unknown");
            m.press.lo = r.at("lo").get<float>();
            m.press.hi = r.at("hi").get<float>();
            if (!curve_from(r.value("curve", std::string("lin")), m.press.curve)) return fail(err, id + ": bad press curve");
            m.press.release_beats = r.value("release_beats", 0.5f);
        }
        // presets: values by param id; missing ones = the default
        if (j.contains("presets")) {
            const json& pr = j["presets"];
            if (pr.size() > kMaxPresets) return fail(err, id + ": too many presets");
            m.presets = static_cast<int>(pr.size());
            for (int k = 0; k < m.presets; ++k) {
                Preset& p = m.preset[k];
                copy(p.name, sizeof(p.name), pr[k].at("name").get<std::string>());
                for (int q = 0; q < kParams; ++q) p.v[q] = q < m.params ? m.param[q].def : 0.0f;
                for (auto it = pr[k].at("values").begin(); it != pr[k].at("values").end(); ++it) {
                    const int q = m.find_param(it.key().c_str());
                    if (q < 0) return fail(err, id + ": preset " + p.name + " names unknown param " + it.key());
                    p.v[q] = it.value().get<float>();
                }
            }
        }
        // flash (the optional show cap, D65)
        if (j.contains("flash")) {
            const json& f = j["flash"];
            const std::string mode = f.at("mode").get<std::string>();
            m.flash.mode = mode == "rate" ? FlashMode::Rate : mode == "level" ? FlashMode::Level : FlashMode::None;
            if (m.flash.mode == FlashMode::None) return fail(err, id + ": flash mode must be rate or level");
            m.flash.param = static_cast<int8_t>(m.find_param(f.at("param").get<std::string>().c_str()));
            if (m.flash.param < 0) return fail(err, id + ": flash param unknown");
            if (f.contains("duty")) m.flash.duty = static_cast<int8_t>(m.find_param(f["duty"].get<std::string>().c_str()));
            if (m.flash.mode == FlashMode::Rate) {
                const json& b = f.at("beats");
                if (b.size() > kMaxFlashSteps || b.size() != m.param[m.flash.param].steps)
                    return fail(err, id + ": flash beats must match the rate knob's steps");
                m.flash.steps = static_cast<uint8_t>(b.size());
                for (size_t k = 0; k < b.size(); ++k) m.flash.beats[k] = b[k].get<float>();
            }
        }
        if (j.contains("cost")) {
            const std::string c = j["cost"].value("class", std::string("S"));
            m.cost_class = c.empty() ? 'S' : c[0];
        }
        m.passes = j.value("passes", 1);
        const std::string st = j.value("state", std::string("none"));
        m.state = st == "feedback" ? State::Feedback : st == "history" ? State::History : State::None;
        if (j.contains("levels")) {
            m.levels = 0;
            for (const auto& l : j["levels"]) {
                const std::string v = l.get<std::string>();
                if (v == "clip") m.levels |= level_bit(Level::Clip);
                else if (v == "layer") m.levels |= level_bit(Level::Layer);
                else if (v == "deck") m.levels |= level_bit(Level::Deck);
                else if (v == "master") m.levels |= level_bit(Level::Master);
                else if (v == "output") m.levels |= level_bit(Level::Output);
                else return fail(err, id + ": unknown level " + v);
            }
        }
        if (j.contains("help")) {
            const json& h = j["help"];
            for (size_t k = 0; k < 2 && k < h.size(); ++k) copy(m.help[k], kHelpLen, h[k].get<std::string>());
        }
    } catch (const std::exception& e) {
        return fail(err, std::string("missing or wrong field: ") + e.what());
    }
    return validate(m, err);
}

bool Library::validate(const EffectMeta& m, std::string* err) {
    const std::string id = m.id;
    if (m.params < 1 || m.params > kParams) return fail(err, id + ": params out of range");
    for (int p = 0; p < m.params; ++p) {
        const ParamMeta& pm = m.param[p];
        if (!unit01(pm.def) || !unit01(pm.good_lo) || !unit01(pm.good_hi) || pm.good_lo > pm.good_hi)
            return fail(err, id + ": param " + pm.id + ": default / good must be knob positions 0..1, lo <= hi");
        if (pm.steps == 1 || pm.steps > 64) return fail(err, id + ": param " + pm.id + ": steps must be 0 or 2..64");
        if (!(pm.scopes & scope_bit(Scope::Slot))) return fail(err, id + ": param " + pm.id + ": SLOT scope must stay allowed");
        for (int q = 0; q < p; ++q)
            if (std::strcmp(m.param[q].id, pm.id) == 0) return fail(err, id + ": duplicate param id");
    }
    if (m.main[0] == m.main[1]) return fail(err, id + ": the two main params must differ");
    // D88: every effect has a default reaction on a real signal.
    const Reaction& r = m.reaction;
    if (r.signal == Signal::None || r.param < 0 || r.param >= m.params) return fail(err, id + ": needs a default reaction (D88)");
    if (!(std::fabs(r.amount) > 0.0f) || std::fabs(r.amount) > 1.0f) return fail(err, id + ": reaction amount must be -1..1, not 0");
    if (r.attack_ms < 0 || r.attack_ms > 2000 || r.release_ms < 0 || r.release_ms > 2000)
        return fail(err, id + ": attack / release must be 0..2000 ms");
    if (!unit01(r.threshold) || !unit01(r.range_lo) || !unit01(r.range_hi)) return fail(err, id + ": reaction threshold / range 0..1");
    if (r.hold_beats < 0 || r.hold_beats > 4) return fail(err, id + ": hold must be 0..4 beats");
    if (m.press.param < 0 || !unit01(m.press.lo) || !unit01(m.press.hi)) return fail(err, id + ": press needs a param and lo / hi 0..1");
    if (m.press.release_beats < 0 || m.press.release_beats > 8) return fail(err, id + ": press release 0..8 beats");
    if (m.find_preset("subtle") < 0 || m.find_preset("extreme") < 0)
        return fail(err, id + ": needs the presets 'subtle' and 'extreme' (velocity picks them)");
    for (int k = 0; k < m.presets; ++k)
        for (int p = 0; p < m.params; ++p)
            if (!unit01(m.preset[k].v[p])) return fail(err, id + ": preset values must be 0..1");
    if (m.cost_class != 'S' && m.cost_class != 'M' && m.cost_class != 'L') return fail(err, id + ": cost class S / M / L");
    if (m.passes != 1) return fail(err, id + ": only single-pass effects in 0.1");
    if (m.flash.mode == FlashMode::Rate)
        for (int k = 0; k < m.flash.steps; ++k)
            if (!(m.flash.beats[k] > 0)) return fail(err, id + ": flash beats must be > 0");
    if (!m.help[0][0]) return fail(err, id + ": needs a help line");
    return true;
}

bool Library::load_builtin(std::string* err) {
    count_ = 0;
    json cat = json::parse(embedded::kCatalog, nullptr, false);
    if (cat.is_discarded() || !cat.contains("order")) return fail(err, "fx/catalog.json: no order");
    for (const auto& idj : cat["order"]) {
        const std::string id = idj.get<std::string>();
        if (count_ >= kMaxEffects) return fail(err, "more than kMaxEffects effects");
        const embedded::File* f = nullptr;
        for (const auto& file : embedded::kFiles)
            if (id == file.id) f = &file;
        if (!f) return fail(err, "fx/catalog.json names " + id + " but there is no fx/" + id + "/effect.json");
        std::string e;
        if (!parse(f->json, meta_[count_], &e)) return fail(err, "fx/" + id + "/effect.json: " + e);
        if (id != meta_[count_].id) return fail(err, "fx/" + id + ": the id inside effect.json differs");
        if (!f->hlsl[0]) return fail(err, "fx/" + id + "/" + id + ".hlsl is missing");
        hlsl_[count_] = f->hlsl;
        ++count_;
    }
    // Every effect folder must be in the catalog (no orphans).
    for (const auto& file : embedded::kFiles)
        if (find(file.id) < 0) return fail(err, std::string("fx/") + file.id + " is not listed in fx/catalog.json");
    return true;
}

int Library::find(const char* id) const {
    for (int i = 0; i < count_; ++i)
        if (std::strcmp(meta_[i].id, id) == 0) return i;
    return -1;
}

const char* Library::common_hlsl() { return embedded::kCommon; }
const char* Library::main_hlsl() { return embedded::kMain; }

void Library::fill_catalog(protocol::Catalog& c) const {
    c.effect_count = static_cast<uint32_t>(count_);
    for (int e = 0; e < count_; ++e) {
        protocol::EffectInfo& info = c.effect[e];
        std::memset(&info, 0, sizeof(info));
        std::snprintf(info.name, sizeof(info.name), "%s", meta_[e].name);
        info.param_count = static_cast<uint8_t>(meta_[e].params);
        info.reserved[0] = static_cast<uint8_t>(meta_[e].kind);  // 0 effect, 1 generator (engine-state.md)
        for (int p = 0; p < meta_[e].params; ++p) {
            std::snprintf(info.param_name[p], sizeof(info.param_name[p]), "%s", meta_[e].param[p].name);
            info.param_default[p] = meta_[e].param[p].def;
        }
    }
}

const Library& builtin() {
    // Start-up only: the first call parses (the engine calls it before the frame loop).
    static const std::unique_ptr<Library> lib = [] {
        auto l = std::make_unique<Library>();
        std::string err;
        if (!l->load_builtin(&err)) std::fprintf(stderr, "himmel fx: %s\n", err.c_str());
        return l;
    }();
    return *lib;
}

}  // namespace himmel::fx
