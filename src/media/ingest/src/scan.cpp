// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ingest/scan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>

namespace himmel::ingest {

using library::Kind;

namespace {

std::string lower_ext(const fs::path& p) {
    auto u = p.extension().u8string();
    std::string e(u.begin(), u.end());
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

int64_t mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

bool under(const fs::path& p, const fs::path& dir) {
    auto pi = p.begin();
    for (auto di = dir.begin(); di != dir.end(); ++di, ++pi)
        if (pi == p.end() || *pi != *di) return false;
    return true;
}

}  // namespace

bool is_sequence_image(const std::string& e) { return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".tif" || e == ".tiff"; }

bool kind_for_extension(const std::string& e, Kind& k) {
    static const std::set<std::string> video = {".mov", ".mp4", ".m4v", ".mkv", ".avi", ".webm", ".mxf", ".mpg", ".mpeg", ".wmv"};
    static const std::set<std::string> still = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp", ".tga"};
    if (video.count(e)) return k = Kind::Video, true;
    if (still.count(e)) return k = Kind::Still, true;
    if (e == ".svg") return k = Kind::Vector, true;
    if (e == ".gif" || e == ".apng") return k = Kind::Animation, true;
    return false;
}

bool find_sequence(const std::vector<fs::path>& files, Candidate& out) {
    struct Item { int64_t num; fs::path path; };
    std::map<std::string, std::vector<Item>> runs;  // key: prefix | digits | ext
    std::map<std::string, std::tuple<std::string, int, std::string>> parts;
    for (const auto& f : files) {
        const std::string ext = lower_ext(f);
        if (!is_sequence_image(ext)) continue;
        const std::string stem = u8(f.stem());
        size_t d = stem.size();
        while (d > 0 && std::isdigit(static_cast<unsigned char>(stem[d - 1]))) --d;
        const size_t digits = stem.size() - d;
        if (digits == 0 || digits > 9) continue;
        const std::string prefix = stem.substr(0, d);
        const bool padded = stem[d] == '0' && digits > 1;
        const std::string key = prefix + "|" + (padded ? std::to_string(digits) : "v") + "|" + ext;
        runs[key].push_back({std::stoll(stem.substr(d)), f});
        parts[key] = {prefix, padded ? static_cast<int>(digits) : 0, u8(f.extension())};
    }
    const std::vector<Item>* best = nullptr;
    std::string best_key;
    for (const auto& [k, v] : runs)
        if (v.size() >= 2 && (!best || v.size() > best->size())) best = &v, best_key = k;
    if (!best) return false;
    std::vector<Item> items = *best;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.num < b.num; });
    const auto& [prefix, width, ext] = parts[best_key];
    out.seq_files.clear();
    for (const auto& it : items) out.seq_files.push_back(it.path);
    out.seq_start = static_cast<int>(items.front().num);
    out.seq_pattern = prefix + (width ? "%0" + std::to_string(width) + "d" : "%d") + ext;
    return true;
}

std::vector<Candidate> scan_folder(const fs::path& root, const std::vector<fs::path>& skip) {
    std::vector<Candidate> out;
    std::error_code ec;
    std::map<fs::path, std::vector<fs::path>> by_dir;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        bool skipped = false;
        for (const auto& s : skip)
            if (under(p, s)) skipped = true;
        if (skipped) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) {
            const auto name = u8(p.filename());
            if (!name.empty() && (name[0] == '.' || name[0] == '~')) continue;  // hidden / temp files
            by_dir[p.parent_path()].push_back(p);
        }
    }
    for (auto& [dir, files] : by_dir) {
        std::sort(files.begin(), files.end());
        std::set<fs::path> in_seq;
        Candidate seq;
        if (dir != root && find_sequence(files, seq)) {
            seq.kind = Kind::Sequence;
            seq.path = dir;
            seq.name = u8(dir.filename());
            for (const auto& f : seq.seq_files) {
                in_seq.insert(f);
                seq.size += static_cast<int64_t>(fs::file_size(f, ec));
                seq.mtime = std::max(seq.mtime, mtime_of(f));
            }
            out.push_back(std::move(seq));
        }
        for (const auto& f : files) {
            if (in_seq.count(f)) continue;
            Candidate c;
            if (!kind_for_extension(lower_ext(f), c.kind)) continue;
            c.path = f;
            c.name = u8(f.stem());
            c.size = static_cast<int64_t>(fs::file_size(f, ec));
            c.mtime = mtime_of(f);
            out.push_back(std::move(c));
        }
    }
    return out;
}

bool candidate_from_path(const fs::path& p, Candidate& out) {
    std::error_code ec;
    out = Candidate{};
    if (fs::is_regular_file(p, ec)) {
        if (!kind_for_extension(lower_ext(p), out.kind)) return false;
        out.path = p;
        out.name = u8(p.stem());
        out.size = static_cast<int64_t>(fs::file_size(p, ec));
        out.mtime = mtime_of(p);
        return true;
    }
    if (!fs::is_directory(p, ec)) return false;
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    if (!find_sequence(files, out)) return false;
    out.kind = Kind::Sequence;
    out.path = p;
    out.name = u8(p.filename());
    for (const auto& f : out.seq_files) {
        out.size += static_cast<int64_t>(fs::file_size(f, ec));
        out.mtime = std::max(out.mtime, mtime_of(f));
    }
    return true;
}

bool SettleTracker::settled(const Candidate& c, int64_t now_ms) {
    auto& s = seen_[c.path.wstring()];
    if (!s.known || s.size != c.size || s.mtime != c.mtime) {
        s = {c.size, c.mtime, now_ms, true};
        return settle_ms_ <= 0;
    }
    return now_ms - s.since_ms >= settle_ms_;
}

}  // namespace himmel::ingest
