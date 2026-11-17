// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/library_db.h"

#include <cstring>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "himmel/library/sidecar.h"

namespace himmel::library {

namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS clips (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  hash TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  source TEXT NOT NULL,
  state TEXT NOT NULL,
  stage_done INTEGER NOT NULL DEFAULT 0,
  error TEXT NOT NULL DEFAULT '',
  play_path TEXT NOT NULL DEFAULT '',
  still_path TEXT NOT NULL DEFAULT '',
  thumb_path TEXT NOT NULL DEFAULT '',
  src_codec TEXT NOT NULL DEFAULT '',
  codec TEXT NOT NULL DEFAULT '',
  width INTEGER, height INTEGER, fps REAL, frames INTEGER, seconds REAL,
  alpha INTEGER NOT NULL DEFAULT 0,
  judder TEXT NOT NULL DEFAULT '',
  avg_rgb INTEGER, brightness REAL, contrast REAL, saturation REAL, warmth REAL,
  motion REAL, motion_dir REAL, steadiness REAL, cuts_per_min REAL, energy INTEGER,
  mood_chill REAL, mood_groove REAL, mood_techno REAL, mood_crazy REAL,
  loop_quality REAL, loop_in INTEGER, loop_out INTEGER, loop_score REAL,
  loop_style TEXT, natural_beats INTEGER, flicker TEXT, black_frames INTEGER,
  palette TEXT NOT NULL DEFAULT '[]',
  slices TEXT NOT NULL DEFAULT '[]',
  analysis_version INTEGER NOT NULL DEFAULT 0,
  added_unix INTEGER NOT NULL, ready_unix INTEGER NOT NULL DEFAULT 0,
  stars INTEGER NOT NULL DEFAULT 0, favourite INTEGER NOT NULL DEFAULT 0,
  user_tags TEXT NOT NULL DEFAULT '[]',
  sidecar TEXT NOT NULL DEFAULT '',
  ready_order INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS clips_state ON clips(state, id);
CREATE INDEX IF NOT EXISTS clips_ready ON clips(ready_order);
CREATE TABLE IF NOT EXISTS sources (path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, hash TEXT);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
)SQL";

// Every column after id, in bind order (insert / update).
constexpr const char* kCols[] = {
    "hash", "kind", "name", "source", "state", "stage_done", "error", "play_path", "still_path", "thumb_path",
    "src_codec", "codec", "width", "height", "fps", "frames", "seconds", "alpha", "judder", "avg_rgb",
    "brightness", "contrast", "saturation", "warmth", "motion", "motion_dir", "steadiness", "cuts_per_min",
    "energy", "mood_chill", "mood_groove", "mood_techno", "mood_crazy", "loop_quality", "loop_in", "loop_out",
    "loop_score", "loop_style", "natural_beats", "flicker", "black_frames", "palette", "slices",
    "analysis_version", "added_unix", "ready_unix", "stars", "favourite", "user_tags", "sidecar", "ready_order"};
constexpr int kNCols = static_cast<int>(sizeof(kCols) / sizeof(kCols[0]));

std::string col_list() {
    std::string s = "id";
    for (const char* c : kCols) s += std::string(",") + c;
    return s;
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const std::string& sql) { sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
};

std::string judder_text(const std::vector<int>& v) {
    std::string s;
    for (int hz : v) s += (s.empty() ? "" : ",") + std::to_string(hz);
    return s;
}

void bind_record(sqlite3_stmt* s, const ClipRecord& r) {
    const Analysis& a = r.analysis;
    int i = 1;
    auto t = [&](const std::string& v) { sqlite3_bind_text(s, i++, v.c_str(), -1, SQLITE_TRANSIENT); };
    auto n = [&](int64_t v) { sqlite3_bind_int64(s, i++, v); };
    auto d = [&](double v) { sqlite3_bind_double(s, i++, v); };
    nlohmann::json pal = nlohmann::json::array();
    for (const auto& e : a.palette) pal.push_back({e.rgb, e.weight});
    t(r.hash), t(kind_name(r.kind)), t(r.name), t(r.source), t(state_name(r.state)), n(r.stage_done), t(r.error);
    t(r.play_path), t(r.still_path), t(r.thumb_path), t(r.probe.codec), t(r.codec);
    n(r.width), n(r.height), d(r.fps), n(r.frames), d(r.seconds), n(r.probe.alpha ? 1 : 0), t(judder_text(r.judder));
    n(a.avg_rgb), d(a.brightness), d(a.contrast), d(a.saturation), d(a.warmth), d(a.motion), d(a.motion_dir),
        d(a.steadiness), d(a.cuts_per_min), n(a.energy), d(a.mood_chill), d(a.mood_groove), d(a.mood_techno),
        d(a.mood_crazy), d(a.loop_quality), n(a.loop_in), n(a.loop_out), d(a.loop_score), t(a.loop_style),
        n(a.natural_beats), t(a.flicker), n(a.black_frames), t(pal.dump()), t(nlohmann::json(a.slices).dump());
    n(a.version), n(r.added_unix), n(r.ready_unix), n(r.stars), n(r.favourite ? 1 : 0),
        t(nlohmann::json(r.user_tags).dump());
    // The full sidecar text keeps what has no column (motion curve, cuts, probe details).
    t(sidecar_to_json(r));
    n(r.ready_order);
}

std::string text_at(sqlite3_stmt* s, int c) {
    const auto* p = sqlite3_column_text(s, c);
    return p ? reinterpret_cast<const char*>(p) : "";
}

ClipRecord read_row(sqlite3_stmt* s) {
    ClipRecord r;
    // The sidecar text first (analysis arrays, probe), then the columns win (user fields, state).
    const std::string side = text_at(s, kNCols - 1);
    if (!side.empty()) sidecar_from_json(side, r);
    r.id = sqlite3_column_int64(s, 0);
    int c = 1;
    r.hash = text_at(s, c++);
    kind_from(text_at(s, c++), r.kind);
    r.name = text_at(s, c++);
    r.source = text_at(s, c++);
    state_from(text_at(s, c++), r.state);
    r.stage_done = static_cast<uint32_t>(sqlite3_column_int64(s, c++));
    r.error = text_at(s, c++);
    r.play_path = text_at(s, c++);
    r.still_path = text_at(s, c++);
    r.thumb_path = text_at(s, c++);
    r.probe.codec = text_at(s, c++);
    r.codec = text_at(s, c++);
    r.width = sqlite3_column_int(s, c++);
    r.height = sqlite3_column_int(s, c++);
    r.fps = sqlite3_column_double(s, c++);
    r.frames = sqlite3_column_int64(s, c++);
    r.seconds = sqlite3_column_double(s, c++);
    r.probe.alpha = sqlite3_column_int(s, c++) != 0;
    c++;  // judder: from the sidecar
    Analysis& a = r.analysis;
    a.avg_rgb = static_cast<uint32_t>(sqlite3_column_int64(s, c++));
    c += 21;  // brightness .. black_frames: from the sidecar (same values)
    c += 2;   // palette, slices
    a.version = sqlite3_column_int(s, c++);
    r.added_unix = sqlite3_column_int64(s, c++);
    r.ready_unix = sqlite3_column_int64(s, c++);
    r.stars = sqlite3_column_int(s, c++);
    r.favourite = sqlite3_column_int(s, c++) != 0;
    {
        const auto tags = nlohmann::json::parse(text_at(s, c++), nullptr, false);
        r.user_tags.clear();
        if (tags.is_array())
            for (const auto& v : tags)
                if (v.is_string()) r.user_tags.push_back(v.get<std::string>());
    }
    c++;  // sidecar
    r.ready_order = sqlite3_column_int64(s, c++);
    return r;
}

}  // namespace

bool LibraryDb::exec(const char* sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err_ = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

bool LibraryDb::open(const std::filesystem::path& file, bool read_only, std::string* err) {
    close();
    read_only_ = read_only;
    const auto u8 = file.u8string();
    const std::string path(u8.begin(), u8.end());
    const int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!read_only) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    if (sqlite3_open_v2(path.c_str(), &db_, flags | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "cannot open";
        close();
        if (err) *err = err_;
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    bool ok = true;
    if (!read_only) {
        ok = exec("PRAGMA journal_mode=WAL;") && exec("PRAGMA synchronous=NORMAL;") && exec(kSchema);
        if (ok) {
            Stmt v(db_, "PRAGMA user_version");
            int ver = 0;
            if (v && sqlite3_step(v.s) == SQLITE_ROW) ver = sqlite3_column_int(v.s, 0);
            if (ver == 0) ok = exec("PRAGMA user_version = 1;");
            else if (ver > kSchemaVersion) ok = false, err_ = "the library DB is newer than this Himmel";
        }
    } else {
        Stmt v(db_, "SELECT count(*) FROM clips");
        ok = v && sqlite3_step(v.s) == SQLITE_ROW;
        if (!ok) err_ = sqlite3_errmsg(db_);
    }
    if (!ok) {
        if (err) *err = err_;
        close();
    }
    return ok;
}

void LibraryDb::close() {
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool LibraryDb::get(int64_t id, ClipRecord& out) {
    if (!db_) return false;
    Stmt s(db_, "SELECT " + col_list() + " FROM clips WHERE id=?");
    if (!s) return false;
    sqlite3_bind_int64(s.s, 1, id);
    if (sqlite3_step(s.s) != SQLITE_ROW) return false;
    out = read_row(s.s);
    return true;
}

bool LibraryDb::find_by_hash(const std::string& hash, ClipRecord& out) {
    if (!db_) return false;
    Stmt s(db_, "SELECT " + col_list() + " FROM clips WHERE hash=?");
    if (!s) return false;
    sqlite3_bind_text(s.s, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) != SQLITE_ROW) return false;
    out = read_row(s.s);
    return true;
}

std::vector<ClipRecord> LibraryDb::list(int state, int64_t after_id, int limit) {
    std::vector<ClipRecord> out;
    if (!db_) return out;
    std::string sql = "SELECT " + col_list() + " FROM clips WHERE id>?";
    if (state >= 0) sql += " AND state=?";
    sql += " ORDER BY id";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    Stmt s(db_, sql);
    if (!s) {
        err_ = sqlite3_errmsg(db_);
        return out;
    }
    sqlite3_bind_int64(s.s, 1, after_id);
    if (state >= 0) sqlite3_bind_text(s.s, 2, state_name(static_cast<State>(state)), -1, SQLITE_STATIC);
    while (sqlite3_step(s.s) == SQLITE_ROW) out.push_back(read_row(s.s));
    return out;
}

std::vector<ClipRecord> LibraryDb::list_ready_after(int64_t ready_order, int limit) {
    std::vector<ClipRecord> out;
    if (!db_) return out;
    std::string sql = "SELECT " + col_list() + " FROM clips WHERE state='ready' AND ready_order>? ORDER BY ready_order";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    Stmt s(db_, sql);
    if (!s) {
        err_ = sqlite3_errmsg(db_);
        return out;
    }
    sqlite3_bind_int64(s.s, 1, ready_order);
    while (sqlite3_step(s.s) == SQLITE_ROW) out.push_back(read_row(s.s));
    return out;
}

int LibraryDb::count(int state) {
    if (!db_) return 0;
    Stmt s(db_, state >= 0 ? "SELECT count(*) FROM clips WHERE state=?" : "SELECT count(*) FROM clips");
    if (!s) return 0;
    if (state >= 0) sqlite3_bind_text(s.s, 1, state_name(static_cast<State>(state)), -1, SQLITE_STATIC);
    return sqlite3_step(s.s) == SQLITE_ROW ? sqlite3_column_int(s.s, 0) : 0;
}

uint64_t LibraryDb::ready_seq() {
    if (!db_) return 0;
    Stmt s(db_, "SELECT value FROM meta WHERE key='ready_seq'");
    if (!s || sqlite3_step(s.s) != SQLITE_ROW) return 0;
    return std::strtoull(text_at(s.s, 0).c_str(), nullptr, 10);
}

void LibraryDb::bump_ready_seq() {
    exec("INSERT INTO meta(key,value) VALUES('ready_seq','1') "
         "ON CONFLICT(key) DO UPDATE SET value=CAST(CAST(value AS INTEGER)+1 AS TEXT);");
}

std::string LibraryDb::unique_name(const std::string& base, int64_t self_id) {
    const std::string b = base.empty() ? "Clip" : base;
    for (int k = 1; k < 10000; ++k) {
        const std::string cand = k == 1 ? b : b + " (" + std::to_string(k) + ")";
        Stmt s(db_, "SELECT id FROM clips WHERE name=? COLLATE NOCASE AND id<>?");
        if (!s) return cand;
        sqlite3_bind_text(s.s, 1, cand.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s.s, 2, self_id);
        if (sqlite3_step(s.s) != SQLITE_ROW) return cand;
    }
    return b;
}

bool LibraryDb::upsert(ClipRecord& r) {
    if (!db_ || read_only_) return false;
    // v2: ready_order is given the first time a row turns ready and then kept (a re-analysed or
    // removed clip keeps its catalog place); a caller that wants a new place sets it to 0.
    if (r.state == State::Ready && r.ready_order == 0) {
        Stmt m(db_, "SELECT coalesce(max(ready_order),0)+1 FROM clips");
        r.ready_order = m && sqlite3_step(m.s) == SQLITE_ROW ? sqlite3_column_int64(m.s, 0) : 1;
    }
    std::string sql;
    if (r.id == 0) {
        r.name = unique_name(r.name);
        sql = "INSERT INTO clips(";
        for (int i = 0; i < kNCols; ++i) sql += std::string(i ? "," : "") + kCols[i];
        sql += ") VALUES(";
        for (int i = 0; i < kNCols; ++i) sql += i ? ",?" : "?";
        sql += ")";
    } else {
        sql = "UPDATE clips SET ";
        for (int i = 0; i < kNCols; ++i) sql += std::string(i ? "," : "") + kCols[i] + "=?";
        sql += " WHERE id=?";
    }
    Stmt s(db_, sql);
    if (!s) {
        err_ = sqlite3_errmsg(db_);
        return false;
    }
    bind_record(s.s, r);
    if (r.id != 0) sqlite3_bind_int64(s.s, kNCols + 1, r.id);
    if (sqlite3_step(s.s) != SQLITE_DONE) {
        err_ = sqlite3_errmsg(db_);
        return false;
    }
    if (r.id == 0) r.id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool LibraryDb::remove(int64_t id) {
    Stmt s(db_, "DELETE FROM clips WHERE id=?");
    if (!s) return false;
    sqlite3_bind_int64(s.s, 1, id);
    return sqlite3_step(s.s) == SQLITE_DONE;
}

std::string LibraryDb::meta_get(const std::string& key) {
    if (!db_) return {};
    Stmt s(db_, "SELECT value FROM meta WHERE key=?");
    if (!s) return {};
    sqlite3_bind_text(s.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(s.s) == SQLITE_ROW ? text_at(s.s, 0) : std::string{};
}

bool LibraryDb::meta_set(const std::string& key, const std::string& value) {
    if (!db_ || read_only_) return false;
    Stmt s(db_, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)");
    if (!s) return false;
    sqlite3_bind_text(s.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(s.s) == SQLITE_DONE;
}

bool LibraryDb::source_info(const std::string& path, int64_t& size, int64_t& mtime) {
    if (!db_) return false;
    Stmt s(db_, "SELECT size, mtime FROM sources WHERE path=? ORDER BY rowid DESC LIMIT 1");
    if (!s) return false;
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) != SQLITE_ROW) return false;
    size = sqlite3_column_int64(s.s, 0);
    mtime = sqlite3_column_int64(s.s, 1);
    return true;
}

bool LibraryDb::begin() { return exec("BEGIN IMMEDIATE;"); }
bool LibraryDb::commit() { return exec("COMMIT;"); }

bool LibraryDb::cached_hash(const std::string& path, int64_t size, int64_t mtime, std::string& hash) {
    if (!db_) return false;
    Stmt s(db_, "SELECT hash FROM sources WHERE path=? AND size=? AND mtime=?");
    if (!s) return false;
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 2, size);
    sqlite3_bind_int64(s.s, 3, mtime);
    if (sqlite3_step(s.s) != SQLITE_ROW) return false;
    hash = text_at(s.s, 0);
    return !hash.empty();
}

void LibraryDb::cache_hash(const std::string& path, int64_t size, int64_t mtime, const std::string& hash) {
    Stmt s(db_, "INSERT OR REPLACE INTO sources(path,size,mtime,hash) VALUES(?,?,?,?)");
    if (!s) return;
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 2, size);
    sqlite3_bind_int64(s.s, 3, mtime);
    sqlite3_bind_text(s.s, 4, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

int LibraryDb::rebuild_from_sidecars(const std::filesystem::path& root) {
    if (!db_ || read_only_) return -1;
    if (!exec("DELETE FROM clips;")) return -1;
    int made = 0;
    std::error_code ec;
    const auto meta = root / "meta";
    begin();
    for (auto it = std::filesystem::directory_iterator(meta, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        ClipRecord r;
        if (!read_sidecar(it->path(), r)) continue;
        if (r.state == State::Removed) {  // v2: tombstones come back as tombstones
            r.id = 0;
            r.stage_done = 0;
            if (upsert(r)) ++made;
            continue;
        }
        const std::filesystem::path play = root / std::filesystem::path(std::u8string(r.play_path.begin(), r.play_path.end()));
        if (r.play_path.empty() || !std::filesystem::exists(play, ec)) continue;
        r.id = 0;
        r.state = State::Ready;
        r.stage_done = kStageProbe | kStageConvert | kStageAnalyse | kStageThumbs;
        if (r.ready_unix == 0) r.ready_unix = r.added_unix;
        if (upsert(r)) ++made;
    }
    bump_ready_seq();
    commit();
    return made;
}

}  // namespace himmel::library
