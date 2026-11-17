// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The ingest command self-test (library-db.md §10.5): every command through the real
// transport (an in-memory LibraryBlock, not the shared one) against a temporary library.
// No ffmpeg: "conversion" is simulated by writing the files a ready clip has.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "himmel/ingest/selftest.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>

#include "himmel/ingest/ffmpeg.h"
#include "himmel/ingest/ingest_ops.h"
#include "himmel/library/sidecar.h"

namespace himmel::ingest {

using library::ClipRecord;
using library::IngestCommand;
using library::IngestCommandType;
using library::IngestReply;
using library::IngestStatus;
using library::State;

namespace {

struct TestHost : OpsHost {
    std::deque<int64_t> q;
    int rescans = 0;
    library::IngestModeRequest mode = library::IngestModeRequest::Auto;
    void queue(int64_t id) override {
        if (std::find(q.begin(), q.end(), id) == q.end()) q.push_back(id);
    }
    void unqueue(int64_t id) override { q.erase(std::remove(q.begin(), q.end(), id), q.end()); }
    void rescan_now() override { ++rescans; }
    void set_mode(library::IngestModeRequest m) override { mode = m; }
};

void write_file(const fs::path& p, uint32_t seed, size_t n = 4096) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        o.put(static_cast<char>(x >> 24));
    }
}

// What the worker would have made: the HAP file, still, thumbnails and the sidecar; then READY.
void fake_ready(library::LibraryDb& db, const fs::path& root, int64_t id) {
    ClipRecord r;
    if (!db.get(id, r)) return;
    r.play_path = "hap/" + r.hash + ".mov";
    r.thumb_path = "thumbs/" + r.hash + ".jpg";
    write_file(root / "hap" / (r.hash + ".mov"), 7);
    write_file(root / "thumbs" / (r.hash + ".jpg"), 8);
    write_file(root / "thumbs" / (r.hash + "-strip.jpg"), 9);
    r.state = State::Ready;
    r.stage_done = library::kStageProbe | library::kStageConvert | library::kStageAnalyse | library::kStageThumbs;
    r.analysis.version = library::kAnalysisVersion;
    library::write_sidecar(root / "meta" / (r.hash + ".json"), r);
    db.upsert(r);
}

}  // namespace

int run_command_selftest(const fs::path& tmp, const std::function<void(const std::string&)>& say) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) ++fails;
        say(std::string(ok ? "  ok   " : "  FAIL ") + what);
    };
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path root = tmp / "Library", drop = tmp / "drop", other = tmp / "other";
    for (const char* d : {"originals", "hap", "stills", "meta", "thumbs", "work"}) fs::create_directories(root / d, ec);

    library::LibraryDb db;
    std::string err;
    check(db.open(root / library::kDbFile, false, &err), "open a temporary library DB " + err);
    TestHost host;
    IngestOps ops(db, root, {root / "originals"}, host);
    auto block = std::make_unique<library::LibraryBlock>();  // value-initialised: zero
    block->header.magic = library::kLibraryMagic;
    block->header.version = library::kLibraryVersion;
    block->header.size = sizeof(library::LibraryBlock);
    CommandPump pump(block.get());
    auto& gui = block->channels[1];
    uint32_t seq = 0;

    // Send through the real rings, apply, read the reply back.
    auto send = [&](IngestCommandType t, int64_t clip, const fs::path& p, uint16_t flags = 0, int32_t i = 0) {
        IngestCommand c;
        make_ingest(c, t, clip, p.empty() ? nullptr : u8path(p).c_str(), flags, i);
        push_ingest(gui, c, ++seq, 1234, 1);
        int prod = -1;
        IngestCommand got;
        IngestReply out{};
        if (!pump.take(prod, got)) return out;
        pump.reply(prod, got, ops.apply(got));
        ops.take_changed();
        gui.replies.pop(out);
        return out;
    };
    auto st = [](const IngestReply& r) { return static_cast<IngestStatus>(r.status); };

    // Originals: three files, one image sequence, one file elsewhere.
    write_file(drop / "a.mp4", 1);
    write_file(drop / "b.png", 2);
    write_file(drop / "sub" / "c.mov", 3);
    for (int k = 1; k <= 3; ++k) write_file(drop / "seq" / ("shot_000" + std::to_string(k) + ".png"), 10 + k);
    write_file(other / "x.mp4", 4);

    // 60 Import
    auto r = send(IngestCommandType::Import, 0, drop);
    check(st(r) == IngestStatus::Accepted && r.count == 4 && r.seq == seq, "Import folder queues 4 (3 files + 1 sequence): " + std::string(r.message));
    check(db.count() == 4 && host.q.size() == 4, "4 rows queued");
    r = send(IngestCommandType::Import, 0, drop / "a.mp4");
    check(st(r) == IngestStatus::Accepted && r.count == 0, "Import of known content queues nothing: " + std::string(r.message));
    r = send(IngestCommandType::Import, 0, drop / "seq");
    check(st(r) == IngestStatus::Accepted && r.count == 0, "Import of a sequence folder finds it as one clip");
    r = send(IngestCommandType::Import, 0, "relative\\path.mp4");
    check(st(r) == IngestStatus::BadPath, "Import of a relative path: BadPath");
    r = send(IngestCommandType::Import, 0, tmp / "nope.mp4");
    check(st(r) == IngestStatus::BadPath, "Import of a missing path: BadPath");
    r = send(IngestCommandType::Import, 0, root / "hap");
    check(st(r) == IngestStatus::BadPath, "Import from the library's own folders: BadPath");
    r = send(IngestCommandType::Import, 0, drop / "a.mp4", library::kIngestWatch);
    check(st(r) == IngestStatus::BadPath, "Watching a file: BadPath");
    r = send(IngestCommandType::Import, 0, other, library::kIngestWatch);
    check(st(r) == IngestStatus::Accepted && r.count == 1, "Import + watch of another folder");
    const auto watch = ops.watch_folders();
    check(watch.size() == 2 && same_path(watch[1], other), "the folder is in the stored watch list");
    check(db.meta_get("watch").find("other") != std::string::npos, "meta.watch holds it");

    ClipRecord a, b, x;
    for (auto& row : db.list()) {
        if (row.name == "a") a = row;
        if (row.name == "b") b = row;
        if (row.name == "x") x = row;
    }
    check(a.id && b.id && x.id, "rows for a, b, x");
    for (auto& row : db.list()) fake_ready(db, root, row.id);
    host.q.clear();

    // 67 Unwatch
    r = send(IngestCommandType::Unwatch, 0, other);
    check(st(r) == IngestStatus::Ok && ops.watch_folders().size() == 1, "Unwatch removes the stored folder");
    r = send(IngestCommandType::Unwatch, 0, root / "originals");
    check(st(r) == IngestStatus::BadPath, "Unwatch of the default folder: BadPath");
    r = send(IngestCommandType::Unwatch, 0, other);
    check(st(r) == IngestStatus::BadPath, "Unwatch of a folder that is not watched: BadPath");
    db.get(x.id, x);
    check(x.state == State::Ready, "an unwatched folder's clips stay");

    // 61 Rescan
    fs::rename(drop / "a.mp4", drop / "moved_a.mp4", ec);
    r = send(IngestCommandType::Rescan, 0, {});
    db.get(a.id, a);
    check(st(r) == IngestStatus::Ok && a.source_missing && a.state == State::Ready && host.rescans > 0,
          "Rescan marks a gone original missing; the clip stays ready: " + std::string(r.message));
    ClipRecord side;
    check(library::read_sidecar(root / "meta" / (a.hash + ".json"), side) && side.source_missing, "the sidecar says source_missing");
    r = send(IngestCommandType::Rescan, 99999, {});
    check(st(r) == IngestStatus::BadId, "Rescan of an unknown clip: BadId");

    // 63 Relink
    r = send(IngestCommandType::Relink, a.id, drop / "moved_a.mp4");
    db.get(a.id, a);
    check(st(r) == IngestStatus::Ok && !a.source_missing && same_path(fs::path(std::u8string(a.source.begin(), a.source.end())), drop / "moved_a.mp4"),
          "Relink one clip to its moved file");
    r = send(IngestCommandType::Relink, a.id, drop / "b.png");
    check(st(r) == IngestStatus::Mismatch, "Relink to other content: Mismatch");
    r = send(IngestCommandType::Relink, a.id, drop / "b.png", library::kIngestForce);
    check(st(r) == IngestStatus::Duplicate && r.clip == b.id, "forced Relink to content of another clip: Duplicate");
    fs::create_directories(tmp / "found" / "deep", ec);
    fs::rename(drop / "moved_a.mp4", tmp / "found" / "deep" / "moved_a.mp4", ec);
    fs::rename(drop / "b.png", tmp / "found" / "renamed_b.png", ec);  // other name, same size
    r = send(IngestCommandType::Relink, 0, tmp / "found");
    db.get(a.id, a), db.get(b.id, b);
    check(st(r) == IngestStatus::Ok && r.count == 2 && !a.source_missing && !b.source_missing,
          "Relink a folder finds both (by name, and by size): " + std::string(r.message));
    r = send(IngestCommandType::Relink, 0, tmp / "found" / "renamed_b.png");
    check(st(r) == IngestStatus::BadPath, "Relink a folder that is a file: BadPath");
    write_file(tmp / "new_a.mp4", 77);
    const int64_t a_order = a.ready_order;
    const std::string a_old_hash = a.hash;
    r = send(IngestCommandType::Relink, a.id, tmp / "new_a.mp4", library::kIngestForce);
    db.get(a.id, a);
    check(st(r) == IngestStatus::Accepted && a.hash != a_old_hash && a.state == State::Queued && a.name == "a" &&
              a.ready_order == 0 && host.q.size() == 1,
          "forced Relink imports again under the same id and name");
    check(!fs::exists(root / "hap" / (a_old_hash + ".mov"), ec) && !fs::exists(root / "meta" / (a_old_hash + ".json"), ec),
          "the old content's files are gone");
    fake_ready(db, root, a.id);
    db.get(a.id, a);
    check(a.ready_order > a_order, "it gets a new catalog place");
    host.q.clear();

    // 64 Reanalyse
    const int64_t b_order = b.ready_order;
    r = send(IngestCommandType::Reanalyse, b.id, {}, library::kIngestThumbs);
    db.get(b.id, b);
    check(st(r) == IngestStatus::Accepted && r.count == 1 && b.state == State::Analysing &&
              !(b.stage_done & library::kStageAnalyse) && !(b.stage_done & library::kStageThumbs) &&
              (b.stage_done & library::kStageConvert) && b.ready_order == b_order,
          "Reanalyse one clip keeps its HAP file and its catalog place");
    fake_ready(db, root, b.id);
    db.get(b.id, b);
    check(b.ready_order == b_order, "ready again with the same ready_order (no second catalog entry)");
    r = send(IngestCommandType::Reanalyse, 0, {});
    check(st(r) == IngestStatus::Accepted && r.count == db.count(static_cast<int>(State::Analysing)) && r.count >= 4,
          "Reanalyse all ready clips");
    for (auto id : host.q) fake_ready(db, root, id);
    host.q.clear();

    // 65 Retry
    r = send(IngestCommandType::Retry, b.id, {});
    check(st(r) == IngestStatus::BadId, "Retry of a clip that has not failed: BadId");
    db.get(b.id, b);
    b.state = State::Failed, b.error = "test";
    db.upsert(b);
    r = send(IngestCommandType::Retry, 0, {});
    db.get(b.id, b);
    check(st(r) == IngestStatus::Accepted && r.count == 1 && b.state == State::Queued && b.stage_done == 0 && b.error.empty(),
          "Retry all queues the failed clip again from the start");
    fake_ready(db, root, b.id);
    host.q.clear();

    // 62 Remove (and a busy HAP file: the delete waits)
    db.get(b.id, b);
    const fs::path b_hap = root / "hap" / (b.hash + ".mov");
    HANDLE busy = CreateFileW(b_hap.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    r = send(IngestCommandType::Remove, b.id, {});
    db.get(b.id, b);
    check(st(r) == IngestStatus::Ok && b.state == State::Removed && b.play_path.empty(), "Remove makes a tombstone");
    check(fs::exists(tmp / "found" / "renamed_b.png", ec), "the original is never touched");
    check(!fs::exists(root / "thumbs" / (b.hash + ".jpg"), ec), "the thumbnails are deleted");
    check(busy == INVALID_HANDLE_VALUE || fs::exists(b_hap, ec), "a HAP file a layer still reads is kept for now");
    check(library::read_sidecar(root / "meta" / (b.hash + ".json"), side) && side.state == State::Removed, "the sidecar says removed");
    if (busy != INVALID_HANDLE_VALUE) CloseHandle(busy);
    check(ops.retry_pending_deletes() >= (busy != INVALID_HANDLE_VALUE ? 1 : 0) && !fs::exists(b_hap, ec),
          "the delete is retried once the file is free");
    r = send(IngestCommandType::Remove, b.id, {});
    check(st(r) == IngestStatus::BadId, "Remove twice: BadId");
    // The watch-folder scan never brings it back; an explicit Import does (same id).
    Candidate cb;
    candidate_from_path(tmp / "found" / "renamed_b.png", cb);
    const auto er = ops.enqueue(cb, false);
    db.get(b.id, b);
    check(er.known && !er.queued && b.state == State::Removed, "a removed clip is not re-imported by the scan");
    r = send(IngestCommandType::Import, 0, tmp / "found" / "renamed_b.png");
    db.get(b.id, b);
    check(st(r) == IngestStatus::Accepted && r.count == 1 && r.clip == b.id && b.state == State::Queued, "an explicit Import brings it back");
    // --rebuild keeps tombstones.
    db.get(x.id, x);
    send(IngestCommandType::Remove, x.id, {});
    check(db.rebuild_from_sidecars(root) >= 1, "rebuild from the sidecars");
    ClipRecord xr;
    check(db.find_by_hash(x.hash, xr) && xr.state == State::Removed, "--rebuild keeps a removed clip removed");

    // 66 Mode, unknown types, path checks
    r = send(IngestCommandType::Mode, 0, {}, 0, static_cast<int32_t>(library::IngestModeRequest::Hold));
    check(st(r) == IngestStatus::Ok && host.mode == library::IngestModeRequest::Hold, "Mode = hold");
    r = send(IngestCommandType::Mode, 0, {}, 0, 9);
    check(st(r) == IngestStatus::BadValue, "Mode 9: BadValue");
    r = send(static_cast<IngestCommandType>(68), 0, {});
    check(st(r) == IngestStatus::UnknownType, "68 (free): UnknownType");
    {
        IngestCommand c;
        make_ingest(c, IngestCommandType::Import);
        std::memset(c.path, 'x', sizeof(c.path));
        push_ingest(gui, c, ++seq, 1234, 1);
        int prod;
        IngestCommand got;
        pump.take(prod, got);
        pump.reply(prod, got, ops.apply(got));
        IngestReply out{};
        gui.replies.pop(out);
        check(static_cast<IngestStatus>(out.status) == IngestStatus::BadPath, "a path without its NUL: BadPath");
    }
    check(gui.done_seq.load() == seq && gui.applying_seq.load() == 0 && gui.rejected.load() > 0, "done_seq / applying_seq / rejected");

    // Restart safety: a command taken but never replied (the worker died) -> Interrupted, once.
    {
        IngestCommand c;
        make_ingest(c, IngestCommandType::Rescan);
        push_ingest(gui, c, ++seq, 1234, 1);
        int prod;
        IngestCommand got;
        pump.take(prod, got);  // ... and the worker dies here
        CommandPump next(block.get());
        check(next.recover() == 1 && next.recover() == 0, "a new worker replies Interrupted once");
        IngestReply out{};
        gui.replies.pop(out);
        check(out.seq == seq && static_cast<IngestStatus>(out.status) == IngestStatus::Interrupted && gui.done_seq.load() == seq,
              "the Interrupted reply carries the seq");
    }
    // A command for the clip being converted waits; Remove / forced Relink cancel the job.
    {
        IngestCommand c;
        bool cancel = false;
        make_ingest(c, IngestCommandType::Remove, 5);
        check(IngestOps::touches(c, 5, cancel) && cancel, "Remove of the running clip cancels it");
        check(!IngestOps::touches(c, 6, cancel), "Remove of another clip does not wait");
        make_ingest(c, IngestCommandType::Import, 0, "C:\\x");
        check(!IngestOps::touches(c, 5, cancel), "Import never waits");
        make_ingest(c, IngestCommandType::Rescan);
        check(IngestOps::touches(c, 5, cancel) && !cancel, "Rescan all waits for the job, without cancelling it");
        make_ingest(c, IngestCommandType::Mode, 0, nullptr, 0, 3);
        check(!IngestOps::touches(c, 5, cancel), "Mode is applied at once");
    }

    db.close();
    fs::remove_all(tmp, ec);
    say(fails ? "SELFTEST FAIL (" + std::to_string(fails) + ")" : std::string("SELFTEST PASS"));
    return fails;
}

}  // namespace himmel::ingest
