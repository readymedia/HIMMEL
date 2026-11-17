// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// ENGINE wave 7: catalog v3 (clip pages, engine-state.md version 4), load by library id
// (LayerClipLib 75), restore by library id, frame smoothing (Even / Blend cadence),
// pre-roll, and the clip file watch (Ready / Missing / Busy).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "himmel/engine/clip_directory.h"
#include "himmel/engine/clip_watch.h"
#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/protocol/clip_pages.h"

using namespace himmel::protocol;
using namespace himmel::engine;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static ClipInfo info(int i, float fps = 30.0f, float seconds = 10.0f) {
    ClipInfo c{};
    std::snprintf(c.name, kNameLen, "clip%04d", i);
    c.width = 1920, c.height = 1080, c.fps = fps, c.seconds = seconds;
    c.thumb_rgb = static_cast<uint32_t>(i);
    return c;
}

// The state block's clip pages without a real shared block.
struct Pages {
    std::unique_ptr<ClipPagesHeader> h = std::make_unique<ClipPagesHeader>();
    std::unique_ptr<ClipPage[]> p = std::make_unique<ClipPage[]>(kClipPageCount);
};

static void test_paging() {
    std::printf("paging: 1000 clips, only changed pages re-read\n");
    auto dir = std::make_unique<ClipDirectory>();
    Pages pg;
    auto copy = std::make_unique<ClipPagesCopy>();
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == 0 && copy->count == 0);  // an empty block reads as empty

    for (int i = 0; i < 1000; ++i) CHECK(dir->add(info(i), 100 + i) == i);
    CHECK(dir->count() == 1000);
    const int written = dir->publish(*pg.h, pg.p.get());
    CHECK(written == (1000 + kClipPageSize - 1) / kClipPageSize);  // 16 pages
    CHECK(pg.h->count.load() == 1000 && pg.h->capacity == static_cast<uint32_t>(kMaxCatalogClips));
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == written);
    CHECK(copy->count == 1000);
    bool all = true;
    for (int i = 0; i < 1000; ++i) {
        const ClipEntry& e = copy->entry(static_cast<uint32_t>(i));
        all &= std::strcmp(e.info.name, info(i).name) == 0 && e.library_id == 100 + i &&
               e.state == static_cast<uint8_t>(ClipState::Ready) && e.info.thumb_rgb == static_cast<uint32_t>(i);
    }
    CHECK(all);
    CHECK(copy->find_library(100 + 777) == 777 && copy->find_library(5) == -1);
    // Nothing changed: nothing is read, nothing is written.
    CHECK(dir->publish(*pg.h, pg.p.get()) == 0);
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == 0);
    // One clip goes missing (id 700 = page 10): one page.
    CHECK(dir->set_state(700, ClipState::Missing));
    CHECK(!dir->set_state(700, ClipState::Missing));  // no change, no page
    CHECK(dir->publish(*pg.h, pg.p.get()) == 1);
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == 1);
    CHECK(copy->entry(700).state == static_cast<uint8_t>(ClipState::Missing));
    // 300 more (1000..1299): the last partial page and 4 new ones.
    for (int i = 1000; i < 1300; ++i) dir->add(info(i), 100 + i);
    CHECK(dir->publish(*pg.h, pg.p.get()) == 6);  // pages 15 (960..1023) to 20
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == 6 && copy->count == 1300);
    CHECK(std::strcmp(copy->entry(1299).info.name, "clip1299") == 0);
    // The catalog section's window: ids 0..255 only.
    auto cat = std::make_unique<Catalog>();
    std::memset(cat.get(), 0, sizeof(Catalog));
    CHECK(dir->mirror_window(*cat));
    CHECK(cat->clip_count == static_cast<uint32_t>(kMaxClips) && std::strcmp(cat->clip[255].name, "clip0255") == 0);
    CHECK(!dir->mirror_window(*cat));  // unchanged
    dir->set_state(3, ClipState::Busy);
    CHECK(dir->mirror_window(*cat));   // ids < 256 changed
    // A reader that sees a page mid-write retries (seqlock): simulate an odd seq.
    dir->set_state(5, ClipState::Ready);
    pg.p[0].seq.fetch_add(1);  // writer "busy"
    const uint32_t before = pg.h->gen.load();
    pg.h->gen.fetch_add(1);
    pg.h->page_gen[0].fetch_add(1);
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == -1);  // busy: try again next poll
    pg.p[0].seq.fetch_add(1);
    CHECK(read_clip_pages(*pg.h, pg.p.get(), *copy) == 1);
    CHECK(pg.h->gen.load() == before + 1);
    // Full: the capacity is the hard limit, refused adds are counted.
    auto big = std::make_unique<ClipDirectory>();
    for (int i = 0; i < kMaxCatalogClips; ++i) big->add(info(i));
    CHECK(big->add(info(0)) == -1 && big->full() == 1 && big->count() == static_cast<uint32_t>(kMaxCatalogClips));
    Pages pg2;
    CHECK(big->publish(*pg2.h, pg2.p.get()) == kClipPageCount);
    auto c2 = std::make_unique<ClipPagesCopy>();
    CHECK(read_clip_pages(*pg2.h, pg2.p.get(), *c2) == kClipPageCount && c2->count == static_cast<uint32_t>(kMaxCatalogClips));
    CHECK(c2->full == 1);
    // 8192 ids: a change at the last id writes (and is read as) exactly one page.
    CHECK(big->set_state(kMaxCatalogClips - 1, ClipState::Missing));
    CHECK(big->publish(*pg2.h, pg2.p.get()) == 1);
    CHECK(read_clip_pages(*pg2.h, pg2.p.get(), *c2) == 1);
    CHECK(c2->entry(kMaxCatalogClips - 1).state == static_cast<uint8_t>(ClipState::Missing));
    CHECK(big->find_library(0) == -1 && big->find_name("clip8191") == kMaxCatalogClips - 1);
    // A second renderer (new directory) on the same block continues the page generations.
    auto again = std::make_unique<ClipDirectory>();
    for (int i = 0; i < 10; ++i) again->add(info(i));
    const uint32_t g0 = pg2.h->page_gen[0].load();
    again->publish(*pg2.h, pg2.p.get());  // clears the pages the old one used
    CHECK(pg2.h->page_gen[0].load() == g0 + 1 && pg2.h->page_gen[kClipPageCount - 1].load() > 0);
    CHECK(read_clip_pages(*pg2.h, pg2.p.get(), *c2) == kClipPageCount && c2->count == 10);
    CHECK(c2->entry(4000).state == static_cast<uint8_t>(ClipState::Empty));
}

struct Rig {
    std::unique_ptr<Catalog> cat;
    std::unique_ptr<ClipDirectory> dir = std::make_unique<ClipDirectory>();
    std::unique_ptr<DeckModel> m;
    uint64_t t = 10'000'000;
    uint32_t seq = 0;
    explicit Rig(int clips) {
        cat = std::make_unique<Catalog>();
        std::memset(cat.get(), 0, sizeof(Catalog));
        for (int i = 0; i < clips; ++i) dir->add(info(i, i == 299 ? 24.0f : 30.0f), 5000 + i);
        dir->mirror_window(*cat);
        fx::fill_catalog(*cat);
        m = std::make_unique<DeckModel>(*cat);
        m->set_clips(dir.get());
        m->tick(t);
    }
    Reject send(Command c) {
        c.seq = ++seq;
        return m->submit(static_cast<int>(Producer::Tool), c, t);
    }
    Reject lib(int deck, int layer, int32_t id) { return send(make(CommandType::LayerClipLib, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), id)); }
    Reject set(int deck, int layer, LayerParam p, int32_t i) {
        Command c = make(CommandType::LayerSet, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), i);
        c.param = static_cast<uint8_t>(p);
        return send(c);
    }
    void tick(uint64_t us) {
        t += us;
        m->tick(t);
    }
};

static void test_load_by_library_id() {
    std::printf("load by library id (LayerClipLib 75), ids >= 256\n");
    static_assert(static_cast<int>(CommandType::LayerClipLib) == 75);
    Rig r(300);
    CHECK(r.m->clip_total() == 300);
    CHECK(r.lib(0, 0, 5000 + 280) == Reject::None);
    CHECK(r.m->layer(0).clip == 280);
    CHECK(std::strcmp(r.m->clip_info(280)->name, "clip0280") == 0);
    r.tick(100000);
    CHECK(r.m->frame(0) > 0);  // it plays (geometry from the directory)
    // A plain LayerClip with an id past the old 256 window is valid too.
    CHECK(r.send(make(CommandType::LayerClip, 1, 2, 299)) == Reject::None && r.m->layer(slot_of(1, 2)).clip == 299);
    CHECK(r.send(make(CommandType::LayerClip, 1, 2, 300)) == Reject::BadId);
    // Unknown library id: not ready (maybe it is still importing); 0 / negative: bad value.
    CHECK(r.lib(0, 1, 999999) == Reject::NotReady);
    CHECK(r.lib(0, 1, 0) == Reject::BadValue && r.lib(0, 1, -3) == Reject::BadValue);
    CHECK(r.lib(7, 1, 5000) == Reject::BadTarget);
    // The file is gone / still being written: refused, the layer keeps what it had.
    r.send(make(CommandType::LayerClip, 0, 1, 3));
    r.dir->set_state(10, ClipState::Missing);
    CHECK(r.lib(0, 1, 5010) == Reject::Missing && r.m->layer(1).clip == 3);
    r.dir->set_state(11, ClipState::Busy);
    CHECK(r.lib(0, 1, 5011) == Reject::Missing && r.m->layer(1).clip == 3);
    r.dir->set_state(11, ClipState::Ready);
    CHECK(r.lib(0, 1, 5011) == Reject::None && r.m->layer(1).clip == 11);
    // The same library clip twice (re-imported with a new file): the newest READY one wins.
    const int dup = r.dir->add(info(10), 5010);
    CHECK(dup == 300 && r.lib(0, 1, 5010) == Reject::None && r.m->layer(1).clip == 300);
    r.dir->set_state(dup, ClipState::Missing);
    r.dir->set_state(10, ClipState::Ready);
    CHECK(r.lib(0, 1, 5010) == Reject::None && r.m->layer(1).clip == 10);
    // Without a directory (old tests / tools): refused as not ready, nothing breaks.
    auto cat = std::make_unique<Catalog>();
    std::memset(cat.get(), 0, sizeof(Catalog));
    cat->clip_count = 2;
    DeckModel bare(*cat);
    Command c = make(CommandType::LayerClipLib, 0, 0, 5);
    c.seq = 1;
    CHECK(bare.submit(3, c, 1000) == Reject::NotReady);
    CHECK(bare.live().last_reject[3] == static_cast<uint8_t>(Reject::NotReady));
    // HIT layer by library id.
    CHECK(r.lib(kHitDeck, 0, 5002) == Reject::None && r.m->layer(kHitSlot).clip == 2);
    // Cue points of an id >= 256 are kept per clip.
    r.lib(1, 0, 5290);
    Command q = make(CommandType::LayerCue, 1, 0, static_cast<int32_t>(CueAction::Set), 0.5f);
    q.slot = 3;
    CHECK(r.send(q) == Reject::None);
    r.send(make(CommandType::LayerClip, 1, 0, 1));
    r.send(make(CommandType::LayerClip, 1, 0, 290));
    CHECK(std::fabs(r.m->layer_transport(slot_of(1, 0)).cue[3] - 0.5f) < 1e-6f);
}

static void test_restore_v4() {
    std::printf("restore: ids >= 256 by library id, then by name\n");
    Rig a(400);
    a.lib(0, 0, 5000 + 350);
    a.send(make(CommandType::LayerClip, 0, 1, 20));
    a.send(make(CommandType::LayerClip, 1, 3, 399));
    a.tick(50000);
    // The previous renderer's pages, as a reader copy -> a directory.
    Pages pg;
    a.dir->publish(*pg.h, pg.p.get());
    auto copy = std::make_unique<ClipPagesCopy>();
    read_clip_pages(*pg.h, pg.p.get(), *copy);
    std::vector<ClipEntry> flat(copy->count);
    for (uint32_t i = 0; i < copy->count; ++i) flat[i] = copy->entry(i);
    auto old_dir = std::make_unique<ClipDirectory>();
    old_dir->load_from(flat.data(), copy->count);
    auto old_cat = std::make_unique<Catalog>(*a.cat);
    // The new renderer's library came back in another order (ids moved); clip 399's library row
    // is gone but a media clip with its name exists.
    auto cat = std::make_unique<Catalog>();
    std::memset(cat.get(), 0, sizeof(Catalog));
    auto dir = std::make_unique<ClipDirectory>();
    for (int i = 399; i >= 0; --i) dir->add(info(i), i == 399 ? 0 : 5000 + i);
    dir->mirror_window(*cat);
    fx::fill_catalog(*cat);
    DeckModel b(*cat);
    b.set_clips(dir.get());
    const int lost = b.restore(a.m->live(), *old_cat, &a.m->transport(), nullptr, old_dir.get());
    CHECK(lost == 0);
    CHECK(b.layer(0).clip == 399 - 350);  // by library id
    CHECK(b.layer(1).clip == 399 - 20);
    CHECK(b.layer(slot_of(1, 3)).clip == 0);  // by name (no library id)
    // Without the pages (a version 3 block) ids >= 256 cannot be named: empty, not wrong.
    DeckModel c(*cat);
    c.set_clips(dir.get());
    c.restore(a.m->live(), *old_cat, &a.m->transport(), nullptr, nullptr);
    CHECK(c.layer(0).clip == -1 && c.layer(1).clip == 399 - 20);
}

// Plays slot 0 for `refreshes` frames of a 60 Hz loop whose wake-ups jitter by +-jitter_us,
// and returns the holds (refreshes per clip frame) after the first frame change.
static std::vector<int> holds(Rig& r, int refreshes, int jitter_us, bool blend_check = false, int* blended = nullptr) {
    std::vector<int> out;
    int64_t last = r.m->frame(0);
    int hold = 0;
    uint32_t x = 12345;
    bool started = false;
    const int period = 16667;
    for (int k = 0; k < refreshes; ++k) {
        x = x * 1664525u + 1013904223u;
        const int j = jitter_us ? static_cast<int>(x >> 8) % (2 * jitter_us + 1) - jitter_us : 0;
        r.t += static_cast<uint64_t>(period + j);
        r.m->tick(r.t);
        r.t -= static_cast<uint64_t>(j);  // the jitter is only in the wake-up, the refresh grid stays
        ++hold;
        const int64_t f = r.m->frame(0);
        if (blend_check) {
            int64_t nf;
            float w;
            r.m->blend_frames(0, nf, w);
            if (w > 0.0f && nf == f + 1 && blended) ++*blended;
        }
        if (f != last) {
            if (started) out.push_back(hold);
            started = true;
            hold = 0;
            last = f;
        }
    }
    return out;
}

static void test_cadence() {
    std::printf("frame smoothing: Even cadence, Blend, Auto, pre-roll\n");
    {   // 30 fps on 60 Hz with 3 ms wake-up jitter: Off judders (1 / 3 holds), Even is 2-2-2.
        Rig r(300);
        r.m->set_refresh_hz(60.0);
        r.send(make(CommandType::LayerClip, 0, 0, 1));
        // Put the playhead right on a frame boundary (the worst case for jitter).
        r.send(make(CommandType::LayerJump, 0, 0, 0, 0.5f));
        CHECK(r.set(0, 0, LayerParam::Smooth, static_cast<int32_t>(FrameSmooth::Off)) == Reject::None);
        CHECK(r.m->smooth_in_use(0) == FrameSmooth::Off);
        const auto off = holds(r, 600, 3000);
        int odd = 0;
        for (int h : off) odd += h != 2;
        std::printf("    30 fps @ 60 Hz, off:  %d of %zu holds not 2\n", odd, off.size());
        CHECK(odd > 0);  // the measurement sees the judder
        CHECK(r.set(0, 0, LayerParam::Smooth, static_cast<int32_t>(FrameSmooth::Even)) == Reject::None);
        r.send(make(CommandType::LayerJump, 0, 0, 0, 0.5f));
        const auto even = holds(r, 600, 3000);
        int bad = 0;
        for (int h : even) bad += h != 2;
        std::printf("    30 fps @ 60 Hz, even: %d of %zu holds not 2\n", bad, even.size());
        CHECK(bad == 0 && even.size() > 250);
        // Auto picks Even for 30 @ 60 (and follows the global setting by default).
        CHECK(r.set(0, 0, LayerParam::Smooth, -1) == Reject::None && r.m->smooth_setting(0) == kSmoothFollow);
        CHECK(r.m->frame_smooth() == FrameSmooth::Auto && r.m->smooth_in_use(0) == FrameSmooth::Even);
        CHECK(std::fabs(r.m->refreshes_per_frame(0) - 2.0) < 1e-6);
        // Speed 0.8: no longer a whole multiple -> Blend.
        r.send(make(CommandType::LayerSpeed, 0, 0, 0, 0.8f));
        for (int k = 0; k < 120; ++k) r.tick(16667);  // the speed glides
        CHECK(r.m->smooth_in_use(0) == FrameSmooth::Blend);
        // Global Off via OutputConfig FrameSmooth (works without an output model).
        Command g = make(CommandType::OutputConfig, kNone, kNone, static_cast<int32_t>(FrameSmooth::Off));
        g.param = static_cast<uint8_t>(OutputParam::FrameSmooth);
        CHECK(r.send(g) == Reject::None && r.m->frame_smooth() == FrameSmooth::Off && r.m->smooth_in_use(0) == FrameSmooth::Off);
        g.i = 7;
        CHECK(r.send(g) == Reject::BadValue);
        CHECK(r.set(0, 0, LayerParam::Smooth, 4) == Reject::BadValue && r.set(0, 0, LayerParam::Smooth, -2) == Reject::BadValue);
    }
    {   // 24 fps on 60 Hz: Even = strict 3-2-3-2; Blend mixes the next frame most of the time.
        Rig r(300);
        r.m->set_refresh_hz(60.0);
        r.send(make(CommandType::LayerClip, 0, 0, 299));  // the 24 fps clip
        CHECK(std::fabs(r.m->refreshes_per_frame(0) - 2.5) < 1e-6);
        CHECK(r.m->smooth_in_use(0) == FrameSmooth::Blend);  // Auto: 2.5 is not whole
        r.set(0, 0, LayerParam::Smooth, static_cast<int32_t>(FrameSmooth::Even));
        const auto even = holds(r, 600, 3000);
        bool alt = even.size() > 100;
        for (size_t k = 3; k < even.size(); ++k)  // the first holds: switching from Blend alt &= (even[k] == 2 || even[k] == 3) && even[k] != even[k - 1];
        CHECK(alt);
        if (!alt)
            for (size_t k = 0; k < even.size() && k < 40; ++k) std::printf("%d ", even[k]);
        r.set(0, 0, LayerParam::Smooth, static_cast<int32_t>(FrameSmooth::Blend));
        int blended = 0;
        holds(r, 600, 3000, true, &blended);
        std::printf("    24 fps @ 60 Hz, blend: %d of 600 refreshes mix two frames\n", blended);
        CHECK(blended > 200);
        // Paused: no blend (a still frame stays sharp).
        r.send(make(CommandType::LayerPlay, 0, 0, static_cast<int32_t>(PlayAction::Pause)));
        r.tick(16667);
        int64_t nf;
        float w;
        r.m->blend_frames(0, nf, w);
        CHECK(w == 0.0f && nf == -1);
    }
    {   // Pre-roll: while the clip loads the playhead holds its start frame.
        Rig r(300);
        r.send(make(CommandType::LayerClip, 0, 0, 1));
        r.m->set_loading(0, true);
        const int64_t f0 = r.m->frame(0);
        for (int k = 0; k < 30; ++k) r.tick(16667);
        CHECK(r.m->frame(0) == f0 && r.m->loading(0));
        r.m->set_loading(0, false);
        for (int k = 0; k < 30; ++k) r.tick(16667);
        CHECK(r.m->frame(0) > f0);
    }
}

static void test_watch() {
    std::printf("clip file watch: ready / missing / busy\n");
    const auto dir = std::filesystem::temp_directory_path() / "himmel_catalog_tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "clip.mov";
    { std::ofstream(file, std::ios::binary) << "not empty"; }
    const std::string p = file.string();
    CHECK(ClipWatch::check_path(p.c_str()) == ClipState::Ready);
    CHECK(ClipWatch::check_path((dir / "nope.mov").string().c_str()) == ClipState::Missing);
    CHECK(ClipWatch::check_path("") == ClipState::Missing);
    // A writer that still has it open (ingest copying it in): busy.
    HANDLE w = CreateFileW(file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(w != INVALID_HANDLE_VALUE);
    CHECK(ClipWatch::check_path(p.c_str()) == ClipState::Busy);
    CloseHandle(w);
    CHECK(ClipWatch::check_path(p.c_str()) == ClipState::Ready);
    // An empty file is still being made.
    const auto empty = dir / "empty.mov";
    { std::ofstream(empty, std::ios::binary); }
    CHECK(ClipWatch::check_path(empty.string().c_str()) == ClipState::Busy);
    std::filesystem::remove_all(dir, ec);
}

int main() {
    test_paging();
    test_load_by_library_id();
    test_restore_v4();
    test_cadence();
    test_watch();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
