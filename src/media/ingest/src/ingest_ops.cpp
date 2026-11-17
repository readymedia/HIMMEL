// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/ingest/ingest_ops.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>

#include <nlohmann/json.hpp>

#include "himmel/ingest/ffmpeg.h"
#include "himmel/library/hash.h"
#include "himmel/library/sidecar.h"

namespace himmel::ingest {

using library::ClipRecord;
using library::IngestCommand;
using library::IngestCommandType;
using library::IngestReply;
using library::IngestStatus;
using library::State;

namespace {

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::wstring key_of(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    std::wstring w = a.lexically_normal().wstring();
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    for (auto& ch : w) ch = static_cast<wchar_t>(std::towlower(ch == L'/' ? L'\\' : ch));
    return w;
}

std::wstring lower(std::wstring w) {
    for (auto& ch : w) ch = static_cast<wchar_t>(std::towlower(ch));
    return w;
}

IngestReply make_reply(IngestStatus st, int32_t count, int64_t clip, const char* fmt, ...) {
    IngestReply r{};
    r.status = static_cast<uint8_t>(st);
    r.count = count;
    r.clip = clip;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

bool is_hash_name(const std::string& stem) {
    if (stem.size() < 16) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(stem[i]))) return false;
    return true;
}

}  // namespace

bool same_path(const fs::path& a, const fs::path& b) { return key_of(a) == key_of(b); }

IngestOps::IngestOps(library::LibraryDb& db, fs::path root, std::vector<fs::path> fixed_watch, OpsHost& host)
    : db_(db), root_(std::move(root)), fixed_watch_(std::move(fixed_watch)), host_(host) {}

std::vector<std::string> IngestOps::json_list(const std::string& key) {
    std::vector<std::string> out;
    const auto j = nlohmann::json::parse(db_.meta_get(key), nullptr, false);
    if (j.is_array())
        for (const auto& v : j)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

void IngestOps::set_json_list(const std::string& key, const std::vector<std::string>& v) {
    db_.meta_set(key, nlohmann::json(v).dump());
}

std::vector<fs::path> IngestOps::watch_folders() {
    std::vector<fs::path> out = fixed_watch_;
    for (const auto& s : json_list("watch")) {
        const fs::path p = from_utf8(s);
        if (std::none_of(out.begin(), out.end(), [&](const fs::path& q) { return same_path(p, q); })) out.push_back(p);
    }
    return out;
}

std::vector<fs::path> IngestOps::skip_folders() const {
    std::vector<fs::path> skip;
    for (const char* d : {"hap", "stills", "meta", "thumbs", "work"}) skip.push_back(root_ / d);
    return skip;
}

std::string IngestOps::hash_of(const Candidate& c) {
    const std::string src = u8path(c.path);
    std::string hash;
    if (db_.cached_hash(src, c.size, c.mtime, hash)) return hash;
    hash = c.kind == library::Kind::Sequence ? library::hash_sequence(c.seq_files) : library::hash_file(c.path);
    if (!hash.empty()) db_.cache_hash(src, c.size, c.mtime, hash);
    return hash;
}

bool IngestOps::candidate_for(const fs::path& p, Candidate& out) { return candidate_from_path(p, out); }

EnqueueResult IngestOps::enqueue(const Candidate& c, bool revive_removed) {
    EnqueueResult res;
    const std::string src = u8path(c.path);
    const std::string hash = hash_of(c);
    if (hash.empty()) return res;
    ClipRecord r;
    if (db_.find_by_hash(hash, r)) {
        res.id = r.id;
        res.known = true;
        if (r.state == State::Removed) {
            if (!revive_removed) return res;
            // An explicit import brings a removed clip back (same id, same catalog place).
            auto pend = json_list("pending_delete");
            pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const std::string& f) { return f.find(hash) != std::string::npos; }),
                       pend.end());
            set_json_list("pending_delete", pend);
            r.state = State::Queued;
            r.stage_done = 0;
            r.error.clear();
            r.source = src;
            r.source_missing = false;
            r.play_path.clear(), r.still_path.clear(), r.thumb_path.clear();
            if (!db_.upsert(r)) return res;
            host_.queue(r.id);
            res.queued = true;
            changed_ = true;
            return res;
        }
        if (r.source != src) {  // moved / renamed / a copy: no new import (design §1.3)
            std::error_code ec;
            if (r.source_missing || !fs::exists(from_utf8(r.source), ec)) {
                r.source = src;
                r.source_missing = false;
                db_.upsert(r);
                changed_ = true;
            }
        }
        if (r.state != State::Ready && r.state != State::Failed) host_.queue(r.id);
        return res;
    }
    r.hash = hash;
    r.kind = c.kind;
    r.name = c.name;
    r.source = src;
    r.state = State::Queued;
    r.added_unix = static_cast<int64_t>(std::time(nullptr));
    if (!db_.upsert(r)) return res;
    host_.queue(r.id);
    res.id = r.id;
    res.queued = true;
    changed_ = true;
    return res;
}

// ---- the commands ----

IngestReply IngestOps::apply(const IngestCommand& c) {
    if (c.type < library::kIngestFirst || c.type > library::kIngestLast)
        return make_reply(IngestStatus::UnknownType, 0, 0, "unknown ingest command %u", c.type);
    if (!std::memchr(c.path, 0, sizeof(c.path))) return make_reply(IngestStatus::BadPath, 0, c.clip, "the path is not terminated");
    const fs::path p = from_utf8(c.path);
    const bool has_path = c.path[0] != 0;
    auto need_path = [&](bool must_exist) -> const char* {
        if (!has_path) return "no path given";
        if (!p.is_absolute()) return "the path is not absolute";
        std::error_code ec;
        if (must_exist && !fs::exists(p, ec)) return "the path does not exist";
        return nullptr;
    };
    switch (static_cast<IngestCommandType>(c.type)) {
        case IngestCommandType::Import:
            if (const char* e = need_path(true)) return make_reply(IngestStatus::BadPath, 0, 0, "%s", e);
            return import_path(c, p);
        case IngestCommandType::Rescan:
            return rescan(c.clip);
        case IngestCommandType::Remove:
            return remove(c.clip);
        case IngestCommandType::Relink:
            if (const char* e = need_path(true)) return make_reply(IngestStatus::BadPath, 0, c.clip, "%s", e);
            return c.clip == 0 ? relink_folder(p) : relink_one(c.clip, p, (c.flags & library::kIngestForce) != 0);
        case IngestCommandType::Reanalyse:
            return reanalyse(c.clip, (c.flags & library::kIngestThumbs) != 0);
        case IngestCommandType::Retry:
            return retry(c.clip);
        case IngestCommandType::Mode:
            if (c.i < 0 || c.i > static_cast<int32_t>(library::IngestModeRequest::Hold))
                return make_reply(IngestStatus::BadValue, 0, 0, "mode %d is out of range", c.i);
            host_.set_mode(static_cast<library::IngestModeRequest>(c.i));
            return make_reply(IngestStatus::Ok, 0, 0, "%s",
                              c.i == 0 ? "import mode: auto" : c.i == 1 ? "import mode: full speed" : c.i == 2 ? "import mode: background" : "import on hold");
        case IngestCommandType::Unwatch:
            if (const char* e = need_path(false)) return make_reply(IngestStatus::BadPath, 0, 0, "%s", e);
            return unwatch(p);
    }
    return make_reply(IngestStatus::UnknownType, 0, 0, "ingest command %u is not used yet", c.type);
}

IngestReply IngestOps::import_path(const IngestCommand& c, const fs::path& p) {
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
    const bool watch = (c.flags & library::kIngestWatch) != 0;
    if (watch && !dir) return make_reply(IngestStatus::BadPath, 0, 0, "only a folder can be watched");
    for (const auto& s : skip_folders())
        if (same_path(p, s) || key_of(p).rfind(key_of(s) + L"\\", 0) == 0)
            return make_reply(IngestStatus::BadPath, 0, 0, "that is inside the library's own folders");
    std::vector<Candidate> cands;
    Candidate one;
    if (candidate_for(p, one)) cands.push_back(std::move(one));  // a file, or a folder that is one image sequence
    else if (dir) cands = scan_folder(p, skip_folders());
    if (watch) {
        auto stored = json_list("watch");
        const auto all = watch_folders();
        if (std::none_of(all.begin(), all.end(), [&](const fs::path& q) { return same_path(p, q); })) {
            stored.push_back(u8path(p));
            set_json_list("watch", stored);
            host_.rescan_now();
        }
    }
    if (cands.empty())
        return watch ? make_reply(IngestStatus::Ok, 0, 0, "watching the folder; nothing to import yet")
                     : make_reply(IngestStatus::BadPath, 0, 0, "nothing importable there");
    int queued = 0;
    int64_t first = 0;
    std::string known_name;
    for (const auto& cand : cands) {
        const auto res = enqueue(cand, true);
        if (res.queued) ++queued;
        if (!first) first = res.id;
        if (res.known && !res.queued && known_name.empty()) {
            ClipRecord r;
            if (db_.get(res.id, r)) known_name = r.name;
        }
    }
    if (cands.size() == 1 && queued == 0 && !known_name.empty())
        return make_reply(IngestStatus::Accepted, 0, first, "already in the library as \"%s\"", known_name.c_str());
    return make_reply(IngestStatus::Accepted, queued, first, "queued %d of %zu", queued, cands.size());
}

int IngestOps::check_source(ClipRecord& r) {
    std::error_code ec;
    const fs::path src = from_utf8(r.source);
    bool missing = !fs::exists(src, ec);
    int queued = 0;
    if (!missing && r.kind != library::Kind::Sequence) {
        Candidate c;
        if (candidate_for(src, c)) {
            const std::string h = hash_of(c);
            if (!h.empty() && h != r.hash) {
                // Other content at the same path: the old original is gone; the new one is a new clip.
                missing = true;
                const auto res = enqueue(c, false);
                queued += res.queued ? 1 : 0;
            }
        }
    }
    if (missing == r.source_missing) return queued;
    r.source_missing = missing;
    db_.upsert(r);
    if (r.state == State::Ready && !r.play_path.empty())
        library::write_sidecar(root_ / "meta" / (r.hash + ".json"), r);
    changed_ = true;
    return queued + 1;
}

IngestReply IngestOps::rescan(int64_t id) {
    if (id != 0) {
        ClipRecord r;
        if (!db_.get(id, r) || r.state == State::Removed) return make_reply(IngestStatus::BadId, 0, id, "no clip %lld", static_cast<long long>(id));
        const int n = check_source(r);
        return make_reply(IngestStatus::Ok, n, id, "%s: original %s", r.name.c_str(), r.source_missing ? "missing" : "found");
    }
    host_.rescan_now();
    int n = 0, missing = 0;
    for (auto& r : db_.list()) {
        if (r.state == State::Removed) continue;
        n += check_source(r);
        missing += r.source_missing ? 1 : 0;
    }
    retry_pending_deletes();
    return make_reply(IngestStatus::Ok, n, 0, "rescanned: %d original(s) missing", missing);
}

void IngestOps::delete_derived(const ClipRecord& r) {
    std::vector<std::string> rel = {r.play_path, r.still_path, r.thumb_path, "hap/" + r.hash + ".mov",
                                    "stills/" + r.hash + ".png", "thumbs/" + r.hash + ".jpg",
                                    "thumbs/" + r.hash + "-strip.jpg"};
    std::sort(rel.begin(), rel.end());
    rel.erase(std::unique(rel.begin(), rel.end()), rel.end());
    auto pend = json_list("pending_delete");
    std::error_code ec;
    for (const auto& f : rel) {
        if (f.empty()) continue;
        const fs::path p = root_ / from_utf8(f);
        if (!fs::exists(p, ec)) continue;
        fs::remove(p, ec);
        if (ec && std::find(pend.begin(), pend.end(), f) == pend.end()) pend.push_back(f);  // busy: a layer plays it
    }
    for (auto it = fs::directory_iterator(root_ / "work", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (u8path(it->path().filename()).rfind(r.hash, 0) == 0) fs::remove(it->path(), ec);
    set_json_list("pending_delete", pend);
}

int IngestOps::retry_pending_deletes() {
    auto pend = json_list("pending_delete");
    if (pend.empty()) return 0;
    std::vector<std::string> keep;
    int done = 0;
    for (const auto& f : pend) {
        const fs::path p = root_ / from_utf8(f);
        const std::string stem = u8path(p.stem());
        // Only files of clips that are still removed (a revived clip makes them again).
        ClipRecord r;
        if (is_hash_name(stem) && db_.find_by_hash(stem.substr(0, 16), r) && r.state != State::Removed) continue;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        fs::remove(p, ec);
        if (ec) keep.push_back(f);
        else ++done;
    }
    set_json_list("pending_delete", keep);
    return done;
}

IngestReply IngestOps::remove(int64_t id) {
    ClipRecord r;
    if (id == 0 || !db_.get(id, r) || r.state == State::Removed)
        return make_reply(IngestStatus::BadId, 0, id, "no clip %lld to remove", static_cast<long long>(id));
    host_.unqueue(id);
    delete_derived(r);
    r.state = State::Removed;
    r.stage_done = 0;
    r.error.clear();
    r.play_path.clear(), r.still_path.clear(), r.thumb_path.clear();
    library::write_sidecar(root_ / "meta" / (r.hash + ".json"), r);  // a tombstone: --rebuild keeps it removed
    if (!db_.upsert(r)) return make_reply(IngestStatus::Failed, 0, id, "DB error: %s", db_.last_error().c_str());
    changed_ = true;
    return make_reply(IngestStatus::Ok, 1, id, "removed \"%s\" (the original is kept)", r.name.c_str());
}

IngestReply IngestOps::relink_one(int64_t id, const fs::path& p, bool force) {
    ClipRecord r;
    if (!db_.get(id, r) || r.state == State::Removed) return make_reply(IngestStatus::BadId, 0, id, "no clip %lld", static_cast<long long>(id));
    Candidate c;
    if (!candidate_for(p, c)) return make_reply(IngestStatus::BadPath, 0, id, "not an importable file");
    const std::string h = hash_of(c);
    if (h.empty()) return make_reply(IngestStatus::BadPath, 0, id, "cannot read the file");
    if (h == r.hash) {
        r.source = u8path(p);
        r.source_missing = false;
        if (!db_.upsert(r)) return make_reply(IngestStatus::Failed, 0, id, "DB error: %s", db_.last_error().c_str());
        if (r.state == State::Ready && !r.play_path.empty()) library::write_sidecar(root_ / "meta" / (r.hash + ".json"), r);
        changed_ = true;
        return make_reply(IngestStatus::Ok, 1, id, "relinked \"%s\"", r.name.c_str());
    }
    if (!force) return make_reply(IngestStatus::Mismatch, 0, id, "the file has other content than \"%s\"", r.name.c_str());
    ClipRecord other;
    if (db_.find_by_hash(h, other)) return make_reply(IngestStatus::Duplicate, 0, other.id, "that content already is \"%s\"", other.name.c_str());
    // Imported again under the same id and name: new hash, new files, a new catalog place.
    host_.unqueue(id);
    delete_derived(r);
    std::error_code ec;
    fs::remove(root_ / "meta" / (r.hash + ".json"), ec);
    ClipRecord n;
    n.id = r.id;
    n.name = r.name;
    n.hash = h;
    n.kind = c.kind;
    n.source = u8path(p);
    n.state = State::Queued;
    n.added_unix = r.added_unix;
    n.stars = r.stars;
    n.favourite = r.favourite;
    n.user_tags = r.user_tags;
    if (!db_.upsert(n)) return make_reply(IngestStatus::Failed, 0, id, "DB error: %s", db_.last_error().c_str());
    host_.queue(id);
    changed_ = true;
    return make_reply(IngestStatus::Accepted, 1, id, "\"%s\" is imported again from the new file", n.name.c_str());
}

IngestReply IngestOps::relink_folder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return make_reply(IngestStatus::BadPath, 0, 0, "not a folder");
    std::vector<ClipRecord> missing;
    for (auto& r : db_.list()) {
        if (r.state == State::Removed) continue;
        if (!r.source_missing && !fs::exists(from_utf8(r.source), ec)) {
            r.source_missing = true;
            db_.upsert(r);
            changed_ = true;
        }
        if (r.source_missing) missing.push_back(r);
    }
    if (missing.empty()) return make_reply(IngestStatus::Ok, 0, 0, "no missing originals");
    std::map<std::wstring, std::vector<fs::path>> by_name;
    std::map<int64_t, std::vector<fs::path>> by_size;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        by_name[lower(it->path().filename().wstring())].push_back(it->path());
        if (it->is_regular_file(ec)) by_size[static_cast<int64_t>(it->file_size(ec))].push_back(it->path());
    }
    int done = 0;
    for (auto& r : missing) {
        std::vector<fs::path> tries;
        const auto nm = by_name.find(lower(from_utf8(r.source).filename().wstring()));
        if (nm != by_name.end()) tries = nm->second;
        int64_t size = 0, mtime = 0;
        if (r.kind != library::Kind::Sequence && db_.source_info(r.source, size, mtime))
            if (const auto sz = by_size.find(size); sz != by_size.end())
                for (const auto& f : sz->second)
                    if (std::find(tries.begin(), tries.end(), f) == tries.end()) tries.push_back(f);
        if (tries.size() > 16) tries.resize(16);
        for (const auto& f : tries) {
            Candidate c;
            if (!candidate_for(f, c) || hash_of(c) != r.hash) continue;
            r.source = u8path(f);
            r.source_missing = false;
            db_.upsert(r);
            if (r.state == State::Ready && !r.play_path.empty()) library::write_sidecar(root_ / "meta" / (r.hash + ".json"), r);
            changed_ = true;
            ++done;
            break;
        }
    }
    return make_reply(IngestStatus::Ok, done, 0, "relinked %d of %zu missing clips", done, missing.size());
}

IngestReply IngestOps::reanalyse(int64_t id, bool thumbs) {
    std::vector<ClipRecord> rows;
    if (id != 0) {
        ClipRecord r;
        if (!db_.get(id, r) || r.state != State::Ready)
            return make_reply(IngestStatus::BadId, 0, id, "clip %lld is not ready", static_cast<long long>(id));
        rows.push_back(std::move(r));
    } else {
        rows = db_.list(static_cast<int>(State::Ready));
    }
    int n = 0;
    for (auto& r : rows) {
        r.stage_done &= ~library::kStageAnalyse;
        if (thumbs) r.stage_done &= ~library::kStageThumbs;
        r.state = State::Analysing;  // keeps ready_order: no second catalog entry
        if (!db_.upsert(r)) continue;
        host_.queue(r.id);
        ++n;
    }
    if (n) changed_ = true;
    return make_reply(IngestStatus::Accepted, n, id, "%d clip(s) queued for analysis", n);
}

IngestReply IngestOps::retry(int64_t id) {
    std::vector<ClipRecord> rows;
    if (id != 0) {
        ClipRecord r;
        if (!db_.get(id, r) || r.state != State::Failed)
            return make_reply(IngestStatus::BadId, 0, id, "clip %lld has not failed", static_cast<long long>(id));
        rows.push_back(std::move(r));
    } else {
        rows = db_.list(static_cast<int>(State::Failed));
    }
    int n = 0;
    for (auto& r : rows) {
        r.state = State::Queued;
        r.error.clear();
        r.stage_done = 0;
        if (!db_.upsert(r)) continue;
        host_.queue(r.id);
        ++n;
    }
    if (n) changed_ = true;
    return make_reply(IngestStatus::Accepted, n, id, "%d failed clip(s) queued again", n);
}

IngestReply IngestOps::unwatch(const fs::path& p) {
    for (const auto& f : fixed_watch_)
        if (same_path(f, p)) return make_reply(IngestStatus::BadPath, 0, 0, "the default watch folder cannot be removed");
    auto stored = json_list("watch");
    const auto it = std::find_if(stored.begin(), stored.end(), [&](const std::string& s) { return same_path(from_utf8(s), p); });
    if (it == stored.end()) return make_reply(IngestStatus::BadPath, 0, 0, "not a watched folder");
    stored.erase(it);
    set_json_list("watch", stored);
    return make_reply(IngestStatus::Ok, 1, 0, "no longer watching the folder (its clips stay)");
}

bool IngestOps::touches(const IngestCommand& c, int64_t current, bool& cancel) {
    cancel = false;
    if (current == 0) return false;
    switch (static_cast<IngestCommandType>(c.type)) {
        case IngestCommandType::Remove:
            cancel = c.clip == current;
            return cancel;
        case IngestCommandType::Relink:
            if (c.clip == current) {
                cancel = (c.flags & library::kIngestForce) != 0;
                return true;
            }
            return c.clip == 0;
        case IngestCommandType::Rescan:
            return c.clip == current || c.clip == 0;
        case IngestCommandType::Reanalyse:
        case IngestCommandType::Retry:
            return c.clip == current;
        default:
            return false;
    }
}

// ---- transport ----

int CommandPump::recover() {
    if (!b_) return 0;
    int n = 0;
    for (int p = 0; p < library::kIngestProducers; ++p) {
        auto& ch = b_->channels[p];
        const uint32_t a = ch.applying_seq.load(std::memory_order_acquire);
        if (a == 0) continue;
        IngestReply r = make_reply(IngestStatus::Interrupted, 0, 0, "the import worker restarted while applying this command");
        r.seq = a;
        ch.replies.push(r);
        ch.rejected.fetch_add(1, std::memory_order_relaxed);
        ch.done_seq.store(a, std::memory_order_release);
        ch.applying_seq.store(0, std::memory_order_release);
        ++n;
    }
    return n;
}

bool CommandPump::take(int& producer, IngestCommand& c) {
    if (!b_) return false;
    for (int k = 0; k < library::kIngestProducers; ++k) {
        const int p = (next_ + k) % library::kIngestProducers;
        auto& ch = b_->channels[p];
        if (!ch.commands.pop(c)) continue;
        ch.applying_seq.store(c.seq ? c.seq : 0xFFFFFFFFu, std::memory_order_release);
        producer = p;
        next_ = (p + 1) % library::kIngestProducers;
        return true;
    }
    return false;
}

void CommandPump::reply(int producer, const IngestCommand& c, IngestReply r) {
    if (!b_ || producer < 0 || producer >= library::kIngestProducers) return;
    auto& ch = b_->channels[producer];
    r.seq = c.seq;
    r.type = c.type;
    r.message[sizeof(r.message) - 1] = 0;
    ch.replies.push(r);
    if (r.status >= static_cast<uint8_t>(IngestStatus::UnknownType)) ch.rejected.fetch_add(1, std::memory_order_relaxed);
    ch.done_seq.store(c.seq, std::memory_order_release);
    ch.applying_seq.store(0, std::memory_order_release);
}

}  // namespace himmel::ingest
