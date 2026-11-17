// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/sidecar.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace himmel::library {

using nlohmann::json;

std::string rgb_hex(uint32_t rgb) {
    char b[8];
    std::snprintf(b, sizeof(b), "#%06x", rgb & 0xFFFFFFu);
    return b;
}

uint32_t hex_rgb(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return 0;
    return static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16));
}

std::string iso_utc(int64_t t) {
    if (t <= 0) return "";
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

int64_t parse_iso_utc(const std::string& s) {
    std::tm tm{};
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900, tm.tm_mon -= 1;
    return static_cast<int64_t>(_mkgmtime(&tm));
}

std::string sidecar_to_json(const ClipRecord& r) {
    const Analysis& a = r.analysis;
    json j;
    j["format"] = kSidecarFormat;
    j["hash"] = r.hash;
    j["kind"] = kind_name(r.kind);
    j["name"] = r.name;
    j["source"] = r.source;
    j["play"] = r.play_path;
    j["still"] = r.still_path;
    j["thumb"] = r.thumb_path;
    j["probe"] = {{"codec", r.probe.codec}, {"pix_fmt", r.probe.pix_fmt}, {"width", r.probe.width},
                  {"height", r.probe.height}, {"fps", r.probe.fps}, {"frames", r.probe.frames},
                  {"seconds", r.probe.seconds}, {"alpha", r.probe.alpha}};
    j["hap"] = {{"codec", r.codec}, {"width", r.width}, {"height", r.height}, {"chunks", r.chunks},
                {"fps", r.fps}, {"frames", r.frames}, {"seconds", r.seconds}};
    j["judder"] = r.judder;
    if (a.version > 0) {
        json pal = json::array();
        for (const auto& e : a.palette) pal.push_back({rgb_hex(e.rgb), e.weight});
        j["analysis"] = {
            {"version", a.version}, {"avg_rgb", rgb_hex(a.avg_rgb)}, {"brightness", a.brightness},
            {"contrast", a.contrast}, {"saturation", a.saturation}, {"warmth", a.warmth}, {"palette", pal},
            {"motion", a.motion}, {"motion_dir", a.motion_dir}, {"steadiness", a.steadiness},
            {"motion_curve", a.motion_curve}, {"cuts", a.cuts}, {"cuts_per_min", a.cuts_per_min},
            {"energy", a.energy},
            {"mood", {{"chill", a.mood_chill}, {"groove", a.mood_groove}, {"techno", a.mood_techno}, {"crazy", a.mood_crazy}}},
            {"loop", {{"quality", a.loop_quality}, {"in", a.loop_in}, {"out", a.loop_out}, {"score", a.loop_score},
                      {"style", a.loop_style}, {"needs_crossfade", a.needs_crossfade}}},
            {"slices", a.slices}, {"natural_beats", a.natural_beats}, {"bars_at_120", a.bars_at_120},
            {"flicker", a.flicker}, {"black_frames", a.black_frames},
            {"proxy", {{"width", a.proxy_w}, {"height", a.proxy_h}, {"fps", a.proxy_fps}, {"frames", a.proxy_frames}}}};
    }
    j["added"] = iso_utc(r.added_unix);
    j["ready"] = iso_utc(r.ready_unix);
    j["stars"] = r.stars;
    j["favourite"] = r.favourite;
    j["user_tags"] = r.user_tags;
    // v2 keys (library-db.md §5): absent = false.
    if (r.source_missing) j["source_missing"] = true;
    if (r.state == State::Removed) j["removed"] = true;
    if (!r.import.is_default())
        j["import"] = {{"max_h", r.import.max_h}, {"codec", r.import.codec}, {"conform_hz", r.import.conform_hz},
                       {"est_bytes", r.import.est_bytes}};
    return j.dump(1);
}

bool sidecar_from_json(const std::string& text, ClipRecord& r, std::string* err) {
    const json j = json::parse(text, nullptr, false);
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (j.is_discarded() || !j.is_object()) return fail("not JSON");
    if (j.value("format", "") != kSidecarFormat) return fail("not a himmel-clip/1 sidecar");
    r = ClipRecord{};
    r.hash = j.value("hash", "");
    if (r.hash.size() != 16) return fail("bad hash");
    if (!kind_from(j.value("kind", ""), r.kind)) return fail("bad kind");
    r.name = j.value("name", "");
    r.source = j.value("source", "");
    r.play_path = j.value("play", "");
    r.still_path = j.value("still", "");
    r.thumb_path = j.value("thumb", "");
    r.source_missing = j.value("source_missing", false);
    if (j.value("removed", false)) r.state = State::Removed;
    if (const auto im = j.find("import"); im != j.end() && im->is_object()) {
        r.import.max_h = im->value("max_h", 0);
        r.import.codec = im->value("codec", std::string());
        r.import.conform_hz = im->value("conform_hz", 0);
        r.import.est_bytes = im->value("est_bytes", int64_t{0});
    }
    if (const auto p = j.find("probe"); p != j.end() && p->is_object()) {
        r.probe.codec = p->value("codec", "");
        r.probe.pix_fmt = p->value("pix_fmt", "");
        r.probe.width = p->value("width", 0);
        r.probe.height = p->value("height", 0);
        r.probe.fps = p->value("fps", 0.0);
        r.probe.frames = p->value("frames", int64_t{0});
        r.probe.seconds = p->value("seconds", 0.0);
        r.probe.alpha = p->value("alpha", false);
    }
    if (const auto h = j.find("hap"); h != j.end() && h->is_object()) {
        r.codec = h->value("codec", "");
        r.width = h->value("width", 0);
        r.height = h->value("height", 0);
        r.chunks = h->value("chunks", 0);
        r.fps = h->value("fps", 0.0);
        r.frames = h->value("frames", int64_t{0});
        r.seconds = h->value("seconds", 0.0);
    }
    if (const auto jd = j.find("judder"); jd != j.end() && jd->is_array())
        for (const auto& v : *jd)
            if (v.is_number_integer()) r.judder.push_back(v.get<int>());
    if (const auto an = j.find("analysis"); an != j.end() && an->is_object()) {
        Analysis& a = r.analysis;
        a.version = an->value("version", 0);
        a.avg_rgb = hex_rgb(an->value("avg_rgb", "#404040"));
        a.brightness = an->value("brightness", 0.0f);
        a.contrast = an->value("contrast", 0.0f);
        a.saturation = an->value("saturation", 0.0f);
        a.warmth = an->value("warmth", 0.0f);
        if (const auto pal = an->find("palette"); pal != an->end() && pal->is_array())
            for (const auto& e : *pal)
                if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_number())
                    a.palette.push_back({hex_rgb(e[0].get<std::string>()), e[1].get<float>()});
        a.motion = an->value("motion", 0.0f);
        a.motion_dir = an->value("motion_dir", 0.0f);
        a.steadiness = an->value("steadiness", 1.0f);
        a.motion_curve = an->value("motion_curve", std::vector<float>{});
        a.cuts = an->value("cuts", std::vector<int64_t>{});
        a.cuts_per_min = an->value("cuts_per_min", 0.0f);
        a.energy = an->value("energy", 1);
        if (const auto m = an->find("mood"); m != an->end() && m->is_object()) {
            a.mood_chill = m->value("chill", 0.0f);
            a.mood_groove = m->value("groove", 0.0f);
            a.mood_techno = m->value("techno", 0.0f);
            a.mood_crazy = m->value("crazy", 0.0f);
        }
        if (const auto l = an->find("loop"); l != an->end() && l->is_object()) {
            a.loop_quality = l->value("quality", 0.0f);
            a.loop_in = l->value("in", int64_t{0});
            a.loop_out = l->value("out", int64_t{0});
            a.loop_score = l->value("score", 0.0f);
            a.loop_style = l->value("style", "loopable");
            a.needs_crossfade = l->value("needs_crossfade", false);
        }
        a.slices = an->value("slices", std::vector<int64_t>{});
        a.natural_beats = an->value("natural_beats", 0);
        a.bars_at_120 = an->value("bars_at_120", 0);
        a.flicker = an->value("flicker", "ok");
        a.black_frames = an->value("black_frames", 0);
        if (const auto p = an->find("proxy"); p != an->end() && p->is_object()) {
            a.proxy_w = p->value("width", 0);
            a.proxy_h = p->value("height", 0);
            a.proxy_fps = p->value("fps", 0.0f);
            a.proxy_frames = p->value("frames", 0);
        }
    }
    r.added_unix = parse_iso_utc(j.value("added", ""));
    r.ready_unix = parse_iso_utc(j.value("ready", ""));
    r.stars = j.value("stars", 0);
    r.favourite = j.value("favourite", false);
    if (const auto t = j.find("user_tags"); t != j.end() && t->is_array())
        for (const auto& v : *t)
            if (v.is_string()) r.user_tags.push_back(v.get<std::string>());
    return true;
}

bool write_sidecar(const std::filesystem::path& file, const ClipRecord& r) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << sidecar_to_json(r);
        if (!out.good()) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool read_sidecar(const std::filesystem::path& file, ClipRecord& r, std::string* err) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return sidecar_from_json(ss.str(), r, err);
}

}  // namespace himmel::library
