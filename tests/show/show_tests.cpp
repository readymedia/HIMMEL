// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// SHOW (M3): file round trips, looks -> commands, recall cost, morph, undo,
// reset, dice + locks, rotate, kits / macros / songs, the project worker
// (save / save as / open / export / revert / autosave restore). No GPU.
#include <algorithm>
#include <cmath>
#include <vector>

#include "himmel/show/dice.h"
#include "himmel/show/files.h"
#include "himmel/show/look_ops.h"
#include "show_test_util.h"

using namespace show_test;
using namespace himmel::show;
namespace F = himmel::show::files;

namespace {

Look deck_look(Rig& r, int d) {
    Look l;
    look_from_live(r.model->live(), r.model->transport(), *r.cat, d, l);
    return l;
}

void round_look(Look& l);
bool looks_equal(const Look& a0, const Look& b0) {
    // The JSON files keep 4 decimals: compare rounded copies.
    Look a = a0, b = b0;
    round_look(a), round_look(b);
    if (same_look(a, b)) return true;
    for (int l = 0; l < kLayers; ++l) {
        if (std::memcmp(&a.layer[l], &b.layer[l], sizeof(a.layer[l])))
            std::printf("    layer %d differs (clip %d/%d opacity %.3f/%.3f)\n", l, a.layer[l].clip, b.layer[l].clip,
                        a.layer[l].opacity, b.layer[l].opacity);
        if (std::memcmp(&a.transport[l], &b.transport[l], sizeof(a.transport[l]))) std::printf("    transport %d differs\n", l);
    }
    if (std::memcmp(a.fx, b.fx, sizeof(a.fx))) std::printf("    deck fx differ\n");
    return false;
}

// A show with banks, variations, kits, macros and songs, built through commands.
void build_show(Rig& r) {
    r.fill_deck(0, 0);
    CHECK(r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 0)) == Reject::None);  // pad 1
    r.fill_deck(0, 3);
    CHECK(r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, -1)) == Reject::None);  // next free = pad 2
    r.deck(cmd(CommandType::LayerOpacity, 0, 1, kNone, kNone, 0, 0.11f));
    r.tick();
    CHECK(r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, -1, 0.0f, kVariation)) == Reject::None);  // 2b
    CHECK(r.send(cmd(CommandType::KitSet, 0, static_cast<uint8_t>(PadTarget::Layer2), 3, static_cast<uint8_t>(PadKind::Clip), 4)) == Reject::None);
    CHECK(r.send(cmd(CommandType::KitSet, 0, static_cast<uint8_t>(PadTarget::Hit), 0, static_cast<uint8_t>(PadKind::Action),
                     static_cast<int>(PadAction::Black))) == Reject::None);
    Command mb = cmd(CommandType::MacroSet, 0, 1, 2, 0, 0 * 16 + 1, 0.2f);
    mb.f[1] = 0.9f;
    CHECK(r.send(mb) == Reject::None);
    // A song with two scenes.
    const uint32_t opener = r.show->set_text(static_cast<int>(Producer::Tool), "Opener");
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, kNone, static_cast<uint8_t>(SongAction::New), static_cast<int>(opener + 1))) ==
          Reject::BadValue);  // the box holds another id: refused
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, kNone, static_cast<uint8_t>(SongAction::New), static_cast<int>(opener))) == Reject::None);
    CHECK(r.send(cmd(CommandType::SongEdit, 0, kNone, kNone, static_cast<uint8_t>(SongAction::AddScene), 0)) == Reject::None);
    r.fill_deck(1, 6);
    CHECK(r.send(cmd(CommandType::SongEdit, 1, kNone, kNone, static_cast<uint8_t>(SongAction::AddScene), 0)) == Reject::None);
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, kNone, static_cast<uint8_t>(SongAction::Bpm), 0, 128.0f)) == Reject::None);
    CHECK(r.send(cmd(CommandType::Locks, kNone, kNone, kNone, static_cast<uint8_t>(LockOp::Set), kLockClip | kLockColour)) == Reject::None);
    r.tick();
}

bool shows_equal(const ShowData& a, const ShowData& b) {
    bool ok = std::strcmp(a.name, b.name) == 0 && a.locks == b.locks && a.setlist_count == b.setlist_count &&
              a.outputs.count == b.outputs.count && a.outputs.preset == b.outputs.preset && a.bank == b.bank;
    // Compare by place (bank pad / song position + variations), not by scene id.
    auto cmp_scene = [&](int ia, int ib) {
        if ((ia < 0) != (ib < 0)) return false;
        if (ia < 0) return true;
        const Scene& x = a.scene[ia];
        const Scene& y = b.scene[ib];
        bool e = std::strcmp(x.name, y.name) == 0 && looks_equal(x.look, y.look) &&
                 std::memcmp(x.kit, y.kit, sizeof(x.kit)) == 0 && x.transition.kind == y.transition.kind;
        for (int m = 0; m < kMacros; ++m) {
            e = e && std::strcmp(x.macro[m].name, y.macro[m].name) == 0 && std::fabs(x.macro[m].value - y.macro[m].value) < 1e-4f;
            for (int k = 0; k < kMacroBinds; ++k)
                e = e && x.macro[m].bind[k].used == y.macro[m].bind[k].used &&
                    (!x.macro[m].bind[k].used || (x.macro[m].bind[k].slot == y.macro[m].bind[k].slot &&
                                                  x.macro[m].bind[k].param == y.macro[m].bind[k].param &&
                                                  std::fabs(x.macro[m].bind[k].max - y.macro[m].bind[k].max) < 1e-4f));
        }
        int va[kMaxVariations], vb[kMaxVariations];
        const int na = variations_of(a, ia, va), nb = variations_of(b, ib, vb);
        e = e && na == nb;
        for (int k = 1; k < na && k < nb; ++k) e = e && looks_equal(a.scene[va[k]].look, b.scene[vb[k]].look);
        return e;
    };
    for (int bk = 0; bk < kMaxBanks; ++bk)
        for (int p = 0; p < kPads; ++p) ok = ok && cmp_scene(a.bank_pad[bk][p], b.bank_pad[bk][p]);
    for (int s = 0; s < a.setlist_count && s < b.setlist_count; ++s) {
        const Song& x = a.songs[a.setlist[s]];
        const Song& y = b.songs[b.setlist[s]];
        ok = ok && std::strcmp(x.name, y.name) == 0 && std::fabs(x.bpm - y.bpm) < 1e-3f;
        for (int p = 0; p < kPads; ++p) ok = ok && cmp_scene(x.scene[p], y.scene[p]);
    }
    return ok;
}

// Rounds every float of a look like the JSON writer does (4 decimals), so a model look compares to a loaded one.
void round_look(Look& l) {
    auto r4 = [](float& v) { v = std::round(v * 10000.0f) / 10000.0f; };
    for (int i = 0; i < kLayers; ++i) {
        r4(l.layer[i].opacity), r4(l.layer[i].speed);
        for (auto& f : l.layer[i].fx) {
            for (auto& p : f.param) r4(p);
            r4(f.mix);
        }
        LayerTransport& t = l.transport[i];
        for (float* v : {&t.in, &t.out, &t.loop_in, &t.loop_out, &t.beats, &t.interval, &t.distance, &t.step_div, &t.glide,
                         &t.fade_in, &t.fade_out, &t.transition_time})
            r4(*v);
    }
    for (auto& f : l.fx) {
        for (auto& p : f.param) r4(p);
        r4(f.mix);
    }
}

void round_show(ShowData& s) {
    for (auto& sc : s.scene)
        if (sc.used) round_look(sc.look);
}

// ---------------------------------------------------------------------------
void test_diff_roundtrip() {
    std::printf("diff: empty -> full -> empty through the deck model\n");
    Rig a(make_catalog(12));
    a.start({.disk = false});
    a.fill_deck(0, 2);
    const Look target = deck_look(a, 0);
    Rig b(make_catalog(12));
    b.start({.disk = false});
    CommandList list;
    Look empty = deck_look(b, 0);
    diff_look(empty, target, 0, *b.cat, list);
    CHECK(!list.overflow && list.n > 20);
    for (int i = 0; i < list.n; ++i) CHECK(b.deck(list.c[i]) == Reject::None);
    CHECK(looks_equal(deck_look(b, 0), target));
    list.clear();
    diff_look(deck_look(b, 0), target, 0, *b.cat, list);
    CHECK(list.n == 0);  // nothing differs -> nothing sent
    list.clear();
    diff_look(deck_look(b, 0), empty, 0, *b.cat, list);
    for (int i = 0; i < list.n; ++i) b.deck(list.c[i]);
    CHECK(looks_equal(deck_look(b, 0), empty));
}

void test_files_roundtrip() {
    std::printf("files: project folder, document, zip, missing clip\n");
    Rig r(make_catalog(12));
    r.start({.disk = false});
    build_show(r);
    const ShowData& show = r.show->show();
    CHECK(scene_count(show) == 5);  // 2 bank scenes + 1 variation + 2 song scenes
    LiveSnapshot live;
    snapshot_from_live(r.model->live(), r.model->transport(), *r.cat, live);

    const fs::path root = temp_root("files");
    const fs::path folder = root / "Round trip.himmel";
    std::string err;
    CHECK(F::save_project(folder, show, live, *r.cat, 1234567890123ull, &err));
    CHECK(fs::exists(folder / "project.json") && fs::exists(folder / "banks" / "bank-01.json") &&
          fs::exists(folder / "songs" / "song-01.json") && fs::exists(folder / "live.json"));
    auto a = std::make_unique<ShowData>();
    LiveSnapshot la{};
    F::LoadInfo info = F::load_project(folder, *a, la, *r.cat);
    CHECK(info.ok && !info.interrupted && info.have_live && info.missing == 0 && info.saved_ms == 1234567890123ull);
    auto expect = std::make_unique<ShowData>(show);
    round_show(*expect);
    CHECK(shows_equal(*expect, *a));
    LiveSnapshot lr = live;
    round_look(lr.deck[0]), round_look(lr.deck[1]);
    CHECK(looks_equal(lr.deck[0], la.deck[0]) && looks_equal(lr.deck[1], la.deck[1]));

    // Saving what was loaded gives the same bytes (stable, diff-friendly files).
    const F::FileSet s1 = F::project_files(*a, la, *r.cat, 1);
    auto b = std::make_unique<ShowData>();
    LiveSnapshot lb{};
    F::load_project(folder, *b, lb, *r.cat);
    const F::FileSet s2 = F::project_files(*b, lb, *r.cat, 1);
    CHECK(s1.files == s2.files);

    // One-document form (autosave/show.json).
    auto c = std::make_unique<ShowData>();
    LiveSnapshot lc{};
    info = F::load_document(F::show_document(show, live, *r.cat, 5), *c, lc, *r.cat);
    CHECK(info.ok && info.have_live && shows_equal(*expect, *c));

    // .himmelshow: zip and back.
    const fs::path zip = root / "Round trip.himmelshow";
    CHECK(F::zip_folder(folder, zip, &err));
    CHECK(F::unzip_to(zip, root / "Unpacked.himmel", &err));
    auto d = std::make_unique<ShowData>();
    LiveSnapshot ld{};
    info = F::load_project(root / "Unpacked.himmel", *d, ld, *r.cat);
    CHECK(info.ok && !info.interrupted && shows_equal(*expect, *d));

    // A clip missing on this PC: empty layer, name kept, written back unchanged.
    auto cat2 = make_catalog(12, /*skip*/ 1);
    auto e = std::make_unique<ShowData>();
    LiveSnapshot le{};
    info = F::load_project(folder, *e, le, *cat2);
    CHECK(info.ok && info.missing == 1 && info.missing_names.size() == 1 && info.missing_names[0] == "clip01.mov");
    const int s0 = e->bank_pad[0][0];
    CHECK(s0 >= 0 && e->scene[s0].look.layer[1].clip < 0 && std::strcmp(e->scene[s0].look.clip_name[1], "clip01.mov") == 0);
    CHECK(F::save_project(root / "Missing.himmel", *e, le, *cat2, 1, &err));
    auto f = std::make_unique<ShowData>();
    LiveSnapshot lf{};
    info = F::load_project(root / "Missing.himmel", *f, lf, *r.cat);  // back on the PC that has it
    CHECK(info.ok && info.missing == 0 && f->scene[f->bank_pad[0][0]].look.layer[1].clip >= 0);

    // A file changed after the save (a cut-off save) is noticed.
    std::string text;
    F::read_file(folder / "banks" / "bank-01.json", text);
    F::write_atomic(folder / "banks" / "bank-01.json", text + " ", &err);
    info = F::load_project(folder, *a, la, *r.cat);
    CHECK(info.ok && info.interrupted);
    // A torn project.json is refused.
    F::write_atomic(folder / "project.json", "{\"format\": \"himmel-sh", &err);
    info = F::load_project(folder, *a, la, *r.cat);
    CHECK(!info.ok && info.interrupted);
}

void test_journal() {
    std::printf("journal: records, torn tail, effects remapped by name\n");
    auto cat = make_catalog(8);
    const fs::path root = temp_root("journal");
    LiveSnapshot s{};
    for (auto& d : s.deck) clear_look(d);
    for (auto& f : s.master_fx) f.effect = -1, f.mix = 1.0f;
    s.deck[0].layer[0].clip = 3;
    copy_str(s.deck[0].clip_name[0], kNameLen, cat->clip[3].name);
    s.deck[0].layer[0].fx[0].effect = 5;
    {
        F::Journal j;
        CHECK(j.open_new(root / "journal.bin", *cat, nullptr));
        for (int i = 0; i < 20; ++i) {
            s.deck[0].layer[0].opacity = i / 20.0f;
            CHECK(j.append(s, 1000 + i));
        }
        CHECK(j.records() == 20);
    }
    // A torn record at the end (the process died mid-write).
    {
        FILE* fp = nullptr;
        _wfopen_s(&fp, (root / "journal.bin").c_str(), L"ab");
        const char junk[100] = {'H', 'R', 'E', 'C'};
        std::fwrite(junk, 1, sizeof(junk), fp);
        std::fclose(fp);
    }
    LiveSnapshot out{};
    uint64_t ms = 0;
    uint32_t n = 0;
    CHECK(F::Journal::read_last(root / "journal.bin", *cat, out, ms, &n));
    CHECK(n == 20 && ms == 1019 && std::fabs(out.deck[0].layer[0].opacity - 19 / 20.0f) < 1e-6f);
    CHECK(out.deck[0].layer[0].clip == 3 && out.deck[0].layer[0].fx[0].effect == 5);
    // Another catalog order: ids follow the names.
    auto cat2 = make_catalog(8);
    std::swap(cat2->effect[5], cat2->effect[6]);
    std::swap(cat2->clip[3], cat2->clip[0]);
    CHECK(F::Journal::read_last(root / "journal.bin", *cat2, out, ms, &n));
    CHECK(out.deck[0].layer[0].fx[0].effect == 6 && out.deck[0].layer[0].clip == 0);
}

void test_recall_cost() {
    std::printf("recall: a full 4-layer scene, in one frame\n");
    Rig r(make_catalog(16));
    r.start({.disk = false});
    r.fill_deck(0, 1);
    r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 0));
    const Look a = deck_look(r, 0);
    r.fill_deck(0, 7);
    r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 1));
    const Look b = deck_look(r, 0);
    CHECK(!same_look(a, b));
    uint32_t worst = 0, total = 0;
    int cmds = 0;
    const int kRuns = 400;
    for (int i = 0; i < kRuns; ++i) {
        const int pad = i & 1;
        CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, pad)) == Reject::None);
        worst = std::max(worst, r.show->last_recall_us());
        total += r.show->last_recall_us();
        cmds = std::max(cmds, r.show->last_recall_commands());
        // The very next state is the whole scene: nothing waits for a later frame.
        Look now = deck_look(r, 0);
        if (i < 2 || i == kRuns - 1) CHECK(looks_equal(now, pad ? b : a));
        r.tick();
    }
    // Recall onto an empty deck B: every layer and FX slot changes, still one frame (cap 384).
    const uint32_t before_b = r.model->live().acked_seq[static_cast<int>(Producer::Show)];
    CHECK(r.send(cmd(CommandType::SceneRecall, 1, kNone, kNone, kNone, 1)) == Reject::None);
    CHECK(looks_equal(deck_look(r, 1), b) && r.show->last_recall_commands() <= kShowCommandsPerFrame);
    CHECK(r.model->live().acked_seq[static_cast<int>(Producer::Show)] > before_b);
    std::printf("  %d recalls: mean %.1f us, worst %u us, up to %d commands (a frame is 16667 us)\n", kRuns,
                static_cast<double>(total) / kRuns, worst, cmds);
    CHECK(worst < 2000);  // well under one frame, even on a slow laptop
    CHECK(r.model->live().rejected[static_cast<int>(Producer::Show)] == 0);
}

void test_morph_and_reset() {
    std::printf("morph + reset to scene\n");
    Rig r(make_catalog(16));
    r.start({.disk = false});
    r.fill_deck(0, 1);
    r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 0));
    const Look a = deck_look(r, 0);
    // Scene 2 = same clips, other opacities / params (so they glide).
    for (int l = 0; l < kLayers; ++l) r.deck(cmd(CommandType::LayerOpacity, 0, static_cast<uint8_t>(l), kNone, kNone, 0, 1.0f));
    r.tick();
    r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 1));
    const Look b = deck_look(r, 0);
    r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, 0));
    CHECK(looks_equal(deck_look(r, 0), a));
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, 1, 1.0f)) == Reject::None);  // 1 s morph
    CHECK(r.show->morphing(0));
    for (int i = 0; i < 30; ++i) r.tick();  // 0.5 s
    const float mid = r.model->live().deck[0].layer[1].opacity;
    CHECK(mid > a.layer[1].opacity + 0.05f && mid < 1.0f - 0.05f);
    for (int i = 0; i < 40; ++i) r.tick();
    CHECK(!r.show->morphing(0));
    CHECK(looks_equal(deck_look(r, 0), b));
    // Live edits, then hold ERASE + group = reset the layer to the scene, over 1 beat.
    r.deck(cmd(CommandType::LayerOpacity, 0, 2, kNone, kNone, 0, 0.05f));
    r.deck(cmd(CommandType::LayerOpacity, 0, 3, kNone, kNone, 0, 0.07f));
    r.tick();
    CHECK(r.show->status().deck[0].edited == 1);
    CHECK(r.send(cmd(CommandType::SceneReset, 0, 2, kNone, kNone, 0, 0.2f)) == Reject::None);
    for (int i = 0; i < 20; ++i) r.tick();
    CHECK(std::fabs(r.model->live().deck[0].layer[2].opacity - b.layer[2].opacity) < 1e-5f);
    CHECK(std::fabs(r.model->live().deck[0].layer[3].opacity - 0.07f) < 1e-5f);  // other layers untouched
    CHECK(r.send(cmd(CommandType::SceneReset, 0, kNone, kNone, kNone, 0, 0.2f)) == Reject::None);  // ERASE + ALL
    for (int i = 0; i < 20; ++i) r.tick();
    CHECK(looks_equal(deck_look(r, 0), b));
    CHECK(r.show->status().deck[0].edited == 0);
    // A morph to a scene with other clips switches them once, at the middle.
    r.fill_deck(1, 9);
    r.send(cmd(CommandType::SceneSave, 1, kNone, kNone, kNone, 2));
    const Look c = deck_look(r, 1);
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, 2, 0.5f)) == Reject::None);
    for (int i = 0; i < 10; ++i) r.tick();
    CHECK(r.model->live().deck[0].layer[0].clip == b.layer[0].clip);
    for (int i = 0; i < 30; ++i) r.tick();
    CHECK(looks_equal(deck_look(r, 0), c));
}

void test_undo() {
    std::printf("undo / redo: 64 deep, coalesced knob turns\n");
    Rig r(make_catalog(8));
    r.start({.disk = false});
    r.fill_deck(0, 0);
    for (int i = 0; i < 30; ++i) r.tick(100000);
    const Look start = deck_look(r, 0);
    // A knob turn: 20 small moves inside the coalesce window = one undo step.
    for (int i = 0; i < 20; ++i) {
        r.deck(cmd(CommandType::LayerOpacity, 0, 0, kNone, kNone, 0, -0.01f, kRelative));
        r.tick(16667);
    }
    r.tick(1000000);
    const Look turned = deck_look(r, 0);
    r.deck(cmd(CommandType::FxSet, 0, 1, 2, kNone, 3));
    r.tick(1000000);
    const Look fx = deck_look(r, 0);
    const int depth = r.show->undo_depth();
    CHECK(depth >= 2);
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 0)) == Reject::None);
    CHECK(looks_equal(deck_look(r, 0), turned));
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 0)) == Reject::None);
    CHECK(looks_equal(deck_look(r, 0), start));
    CHECK(r.show->undo_depth() == depth - 2 && r.show->redo_depth() == 2);
    r.tick();
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 1)) == Reject::None);  // redo
    CHECK(looks_equal(deck_look(r, 0), turned));
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 1)) == Reject::None);
    CHECK(looks_equal(deck_look(r, 0), fx));
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 1)) == Reject::None);  // nothing left: no change
    CHECK(looks_equal(deck_look(r, 0), fx));
    // TAKE / CUT are not undo steps.
    r.deck(cmd(CommandType::Cut));
    r.tick(1000000);
    CHECK(r.model->live().program_deck == 1);
    r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 0));
    CHECK(r.model->live().program_deck == 1);
    // The stack keeps the last 64.
    for (int i = 0; i < 100; ++i) {
        r.deck(cmd(CommandType::LayerOpacity, 0, 2, kNone, kNone, 0, (i % 50) / 50.0f + 0.01f));
        r.tick(500000);
    }
    CHECK(r.show->undo_depth() == kUndoDepth);
    CHECK(r.send(cmd(CommandType::Undo, kNone, kNone, kNone, kNone, 5)) == Reject::BadValue);
}

void test_dice() {
    std::printf("dice: seeded, locks, history, mutate, rotate\n");
    auto cat = make_catalog(20);
    Look base;
    clear_look(base);
    Look x, y;
    dice_roll(base, x, *cat, 0, {}, 42);
    dice_roll(base, y, *cat, 0, {}, 42);
    CHECK(std::memcmp(&x, &y, sizeof(Look)) == 0);  // same seed, same look
    dice_roll(base, y, *cat, 0, {}, 43);
    CHECK(std::memcmp(&x, &y, sizeof(Look)) != 0);
    CHECK(x.layer[0].clip >= 0);
    // CLIP + FX + LAYOUT locks keep clips, effects, opacity and blend.
    for (uint64_t seed = 1; seed < 50; ++seed) {
        Look z;
        dice_roll(x, z, *cat, kLockClip | kLockFx | kLockLayout, {}, seed);
        bool ok = true;
        for (int l = 0; l < kLayers; ++l) {
            ok = ok && z.layer[l].clip == x.layer[l].clip && z.layer[l].opacity == x.layer[l].opacity &&
                 z.layer[l].blend == x.layer[l].blend;
            for (int s = 0; s < kFxSlots; ++s) ok = ok && z.layer[l].fx[s].effect == x.layer[l].fx[s].effect;
        }
        CHECK(ok);
    }
    // LAYER scope touches one layer only; FX SLOT scope one slot only.
    Look z;
    dice_roll(x, z, *cat, 0, {2, -1}, 7);
    CHECK(std::memcmp(&z.layer[0], &x.layer[0], sizeof(LayerState)) == 0 && std::memcmp(z.fx, x.fx, sizeof(z.fx)) == 0);
    dice_roll(x, z, *cat, 0, {1, 0}, 9);
    CHECK(z.layer[1].clip == x.layer[1].clip && std::memcmp(&z.layer[1].fx[1], &x.layer[1].fx[1], sizeof(FxSlotState)) == 0);
    CHECK(z.layer[1].fx[0].effect >= 0);
    // MUTATE keeps the structure.
    dice_mutate(x, z, *cat, 0, {}, 5, 0.2f);
    for (int l = 0; l < kLayers; ++l) {
        CHECK(z.layer[l].clip == x.layer[l].clip);
        for (int s = 0; s < kFxSlots; ++s) CHECK(z.layer[l].fx[s].effect == x.layer[l].fx[s].effect);
    }
    CHECK(param_lock("Hue") == kLockColour && param_lock("SPIN SPEED") == kLockMotion && param_lock("Segments") == 0);

    // Through the runtime: roll, roll, back, back, forward, forward, forward = a new roll.
    Rig r(make_catalog(20));
    r.start({.disk = false});
    r.fill_deck(1, 0);
    const Look orig = deck_look(r, 1);
    auto dice = [&](DiceVerb v, int i = 0) { return r.send(cmd(CommandType::Dice, 1, kNone, kNone, static_cast<uint8_t>(v), i)); };
    CHECK(dice(DiceVerb::Roll, 100) == Reject::None);
    const Look r1 = deck_look(r, 1);
    CHECK(!same_look(r1, orig));
    CHECK(dice(DiceVerb::Roll, 200) == Reject::None);
    const Look r2 = deck_look(r, 1);
    CHECK(dice(DiceVerb::Back) == Reject::None);
    CHECK(same_look(deck_look(r, 1), r1));
    CHECK(dice(DiceVerb::Back) == Reject::None);
    CHECK(same_look(deck_look(r, 1), orig));
    CHECK(dice(DiceVerb::Forward) == Reject::None);
    CHECK(dice(DiceVerb::Forward) == Reject::None);
    CHECK(same_look(deck_look(r, 1), r2));
    CHECK(dice(DiceVerb::Forward) == Reject::None);  // at the end: a new roll
    r.tick();
    CHECK(!same_look(deck_look(r, 1), r2) && r.show->status().deck[1].dice_count == 3);
    CHECK(r.send(cmd(CommandType::Locks, kNone, kNone, kNone, static_cast<uint8_t>(LockOp::Toggle), kLockClip)) == Reject::None);
    const Look before = deck_look(r, 1);
    CHECK(dice(DiceVerb::Roll, 300) == Reject::None);
    for (int l = 0; l < kLayers; ++l) CHECK(r.model->live().deck[1].layer[l].clip == before.layer[l].clip);
    CHECK(dice(DiceVerb::Mutate, 301) == Reject::None);
    CHECK(r.send(cmd(CommandType::Dice, 1, kNone, kNone, 9)) == Reject::BadValue);

    // ROTATE: main -> variations -> next scene.
    r.send(cmd(CommandType::SceneSave, 1, kNone, kNone, kNone, 0));  // 1a
    r.deck(cmd(CommandType::LayerOpacity, 1, 0, kNone, kNone, 0, 0.2f));
    r.tick();
    r.send(cmd(CommandType::SceneSave, 1, kNone, kNone, kNone, -1, 0.0f, kVariation));  // 1b
    r.deck(cmd(CommandType::LayerOpacity, 1, 0, kNone, kNone, 0, 0.4f));
    r.tick();
    r.send(cmd(CommandType::SceneSave, 1, kNone, kNone, kNone, -1, 0.0f, kVariation));  // 1c
    CHECK(std::strstr(r.show->status().message, "1c saved") != nullptr);
    r.fill_deck(1, 5);
    r.send(cmd(CommandType::SceneSave, 1, kNone, kNone, kNone, 1));  // 2a
    r.send(cmd(CommandType::SceneRecall, 1, kNone, kNone, kNone, 0));
    r.tick();
    CHECK(r.show->status().deck[1].variation == 'a');
    dice(DiceVerb::Rotate, 1);
    r.tick();
    CHECK(r.show->status().deck[1].variation == 'b' && std::fabs(r.model->live().deck[1].layer[0].opacity - 0.2f) < 1e-5f);
    dice(DiceVerb::Rotate, 1);
    r.tick();
    CHECK(r.show->status().deck[1].variation == 'c');
    dice(DiceVerb::Rotate, 1);
    r.tick();
    CHECK(r.show->status().deck[1].pad == 1 && r.show->status().deck[1].variation == 'a');
    dice(DiceVerb::Rotate, -1);
    r.tick();
    CHECK(r.show->status().deck[1].pad == 0 && r.show->status().deck[1].variation == 'c');
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, 1, kNone, 0)) == Reject::None);  // slot = variation
    r.tick();
    CHECK(r.show->status().deck[0].variation == 'b');
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, 5, kNone, 0)) == Reject::BadId);
}

void test_pages_kits_macros() {
    std::printf("pages, songs, GO, kits, macros\n");
    Rig r(make_catalog(12));
    r.start({.disk = false});
    build_show(r);
    r.tick();
    const ShowStatus& st = r.show->status();
    CHECK(st.pad[0].used && st.pad[1].used && st.pad[1].variations == 1 && !st.pad[2].used);
    CHECK(st.song_count == 1 && std::strcmp(st.song_name, "Opener") == 0 && std::fabs(st.song_bpm - 128.0f) < 1e-3f);
    // GO walks the page; the bank wraps.
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, 0)) == Reject::None);
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, kSceneNext)) == Reject::None);
    r.tick();
    CHECK(r.show->status().deck[0].pad == 1);
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, kNone, kNone, kSceneNext)) == Reject::None);
    r.tick();
    CHECK(r.show->status().deck[0].pad == 0);
    // With the setlist on, pages are the song's scenes; the last song ends.
    CHECK(r.send(cmd(CommandType::ShowSelect, kNone, kNone, kNone, static_cast<uint8_t>(SelectWhat::UseSetlist), 1)) == Reject::None);
    r.tick();
    CHECK(r.show->status().use_setlist == 1 && r.show->status().pad[0].used && r.show->status().pad[1].used);
    CHECK(r.send(cmd(CommandType::SceneRecall, 1, kNone, kNone, kNone, 1)) == Reject::None);
    CHECK(r.send(cmd(CommandType::SceneRecall, 1, kNone, kNone, kNone, kSceneNext)) == Reject::BadId);
    CHECK(r.send(cmd(CommandType::ShowSelect, kNone, kNone, kNone, static_cast<uint8_t>(SelectWhat::Bank), 20)) == Reject::BadValue);
    CHECK(r.send(cmd(CommandType::ShowSelect, kNone, kNone, kNone, static_cast<uint8_t>(SelectWhat::UseSetlist), 0)) == Reject::None);
    // Kits live in the scene on the deck (pad 2 has the kit, set after its variation was saved).
    CHECK(r.send(cmd(CommandType::SceneRecall, 0, kNone, 1, kNone, 1)) == Reject::None);
    const Scene& s1 = r.show->show().scene[r.show->deck_scene(0)];
    CHECK(s1.kit[3].kind == static_cast<uint8_t>(PadKind::Clip) && s1.kit[3].target == static_cast<uint8_t>(PadTarget::Layer2) &&
          std::strcmp(s1.kit[3].name, "clip04.mov") == 0);
    CHECK(r.send(cmd(CommandType::KitSet, 0, 0, 5, static_cast<uint8_t>(PadKind::Clip), 99)) == Reject::BadId);
    CHECK(r.send(cmd(CommandType::KitSet, 0, 0, 16, static_cast<uint8_t>(PadKind::Clip), 1)) == Reject::BadTarget);
    // Macro 3 (index 2) of the PROGRAM deck (A) drives layer 2, slot 1, param 2 between 0.2 and 0.9.
    const int fx = r.model->live().deck[0].layer[1].fx[0].effect;
    CHECK(fx >= 0);
    CHECK(r.send(cmd(CommandType::MacroValue, kNone, kNone, 2, kNone, 0, 1.0f)) == Reject::None);
    CHECK(std::fabs(r.model->live().deck[0].layer[1].fx[0].param[1] - 0.9f) < 1e-5f);
    CHECK(r.send(cmd(CommandType::MacroValue, kNone, kNone, 2, kNone, 0, -0.5f, kRelative)) == Reject::None);
    CHECK(std::fabs(r.model->live().deck[0].layer[1].fx[0].param[1] - (0.2f + 0.7f * 0.5f)) < 1e-4f);
    // 400 knob moves in one frame: the show never trips the model's rate limit, the rest waits.
    for (int k = 0; k < 400; ++k) r.send(cmd(CommandType::MacroValue, kNone, kNone, 0, kNone, 0, (k % 100) / 100.0f));
    for (int k = 0; k < 120; ++k) r.tick();
    CHECK(r.model->live().rejected[static_cast<int>(Producer::Show)] == 0);
    // Unbound INTENSITY (macro 1) drives every layer effect's mix.
    CHECK(r.send(cmd(CommandType::MacroValue, kNone, kNone, 0, kNone, 0, 0.25f)) == Reject::None);
    CHECK(std::fabs(r.model->live().deck[0].layer[0].fx[0].mix - 0.25f) < 1e-5f);
    // Songs: new, move, delete.
    const uint32_t closer = r.show->set_text(static_cast<int>(Producer::Tool), "Closer");
    r.send(cmd(CommandType::SongEdit, kNone, kNone, kNone, static_cast<uint8_t>(SongAction::New), static_cast<int>(closer)));
    const uint32_t ren = r.show->set_text(static_cast<int>(Producer::Tool), "Encore");
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, 1, static_cast<uint8_t>(SongAction::Rename), static_cast<int>(ren))) == Reject::None);
    CHECK(std::strcmp(r.show->show().songs[r.show->show().setlist[1]].name, "Encore") == 0);
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, 1, static_cast<uint8_t>(SongAction::Rename), static_cast<int>(ren))) == Reject::None);
    const uint32_t back = r.show->set_text(static_cast<int>(Producer::Tool), "Closer");
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, 1, static_cast<uint8_t>(SongAction::Rename), static_cast<int>(ren))) == Reject::BadValue);
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, 1, static_cast<uint8_t>(SongAction::Rename), static_cast<int>(back))) == Reject::None);
    r.send(cmd(CommandType::SongEdit, kNone, kNone, 0, static_cast<uint8_t>(SongAction::Move), 1));
    CHECK(std::strcmp(r.show->show().songs[r.show->show().setlist[0]].name, "Closer") == 0);
    CHECK(r.send(cmd(CommandType::SongEdit, kNone, kNone, kNone, static_cast<uint8_t>(SongAction::Delete), 1)) == Reject::None);
    CHECK(r.show->show().setlist_count == 1 && scene_count(r.show->show()) == 3);
    // Refusals are counted in the show block; deck types are not show commands.
    CHECK(r.show->status().rejected[static_cast<int>(Producer::Tool)] >= 4);
    CHECK(r.show->submit(3, cmd(CommandType::LayerClip, 0, 0), r.t, *r.model) == Reject::UnknownType);
    CHECK(r.show->submit(3, cmd(static_cast<CommandType>(39)), r.t, *r.model) == Reject::UnknownType);  // SHOW range, not used yet
    CHECK(is_show_command(static_cast<CommandType>(39)) && !is_show_command(static_cast<CommandType>(40)));
    Command nan = cmd(CommandType::MacroValue, kNone, kNone, 0);
    nan.f[0] = std::nanf("");
    CHECK(r.send(nan) == Reject::BadValue);
}

void test_projects() {
    std::printf("projects: save as, recent, autosave restore, revert, new, open, export, relink, untitled\n");
    const fs::path root = temp_root("projects");
    RuntimeConfig cfg;
    cfg.projects_root = root / "Projects";
    cfg.app_dir = root / "App";
    constexpr int kTool = static_cast<int>(Producer::Tool);
    auto project = [](ProjectAction a, int i = 0, uint8_t slot = kNone) {
        return cmd(CommandType::Project, kNone, kNone, slot, static_cast<uint8_t>(a), i);
    };
    Look saved_a, live_after;
    {
        Rig r(make_catalog(12));
        r.start(cfg);
        CHECK(r.settle());
        build_show(r);
        CHECK(r.show->dirty());
        CHECK(r.send(project(ProjectAction::SaveAs)) == Reject::BadValue);  // no text id
        const uint32_t name = r.show->set_text(kTool, "My Show");
        CHECK(r.send(project(ProjectAction::SaveAs, static_cast<int>(name))) == Reject::None);
        CHECK(r.settle());
        const fs::path folder = cfg.projects_root / "My Show.himmel";
        CHECK(fs::exists(folder / "project.json") && fs::exists(folder / "autosave" / "journal.bin"));
        CHECK(!r.show->dirty());
        CHECK(std::strcmp(r.show->show().name, "My Show") == 0);
        FilesStatus fsx = r.show->files();
        CHECK(fsx.recent_count == 1 && std::strstr(fsx.recent[0].path, "My Show.himmel") && fsx.save_ok == 1);
        saved_a = deck_look(r, 0);
        // A change after the save: journalled, not saved.
        r.deck(cmd(CommandType::LayerOpacity, 0, 0, kNone, kNone, 0, 0.123f));
        r.send(cmd(CommandType::Locks, kNone, kNone, kNone, static_cast<uint8_t>(LockOp::Set), kLockMotion));
        for (int i = 0; i < 40; ++i) r.tick(20000), Sleep(1);
        CHECK(r.settle());
        CHECK(r.show->dirty());
        live_after = deck_look(r, 0);
        CHECK(r.send(project(ProjectAction::Export)) == Reject::None);
        CHECK(r.settle());
        CHECK(fs::exists(cfg.projects_root / "My Show.himmelshow"));
        r.show->stop();  // "the power goes": no save after the change
    }
    {
        Rig r(make_catalog(12));
        r.start(cfg);
        CHECK(r.settle());
        CHECK(r.show->files().restored == static_cast<uint8_t>(Restored::Autosave));
        CHECK(looks_equal(deck_look(r, 0), live_after));
        CHECK(r.show->show().locks == kLockMotion && scene_count(r.show->show()) == 5);
        CHECK(r.show->dirty());  // restored work is not saved yet
        CHECK(r.send(project(ProjectAction::Revert)) == Reject::None);
        CHECK(r.settle());
        CHECK(r.show->show().locks == (kLockClip | kLockColour));
        CHECK(looks_equal(deck_look(r, 0), saved_a));
        CHECK(!r.show->dirty());
        // The newer autosave is still offered.
        CHECK(r.send(project(ProjectAction::RestoreAutosave)) == Reject::None);
        CHECK(r.settle());
        CHECK(r.show->show().locks == kLockMotion && looks_equal(deck_look(r, 0), live_after));
        CHECK(r.send(project(ProjectAction::Save)) == Reject::None);
        CHECK(r.settle());
        CHECK(!r.show->dirty());
        // New: an empty 1-screen project; the outputs keep playing.
        CHECK(r.send(project(ProjectAction::New, 1)) == Reject::None);
        CHECK(r.settle());
        CHECK(scene_count(r.show->show()) == 0 && std::strcmp(r.show->show().name, "Untitled") == 0 &&
              r.show->show().outputs.count == 1);
        CHECK(looks_equal(deck_look(r, 0), live_after));
        // Open recent 0 mid-show: PROGRAM (A) untouched, the project's look on PREVIEW (B).
        r.fill_deck(0, 2);
        const Look program_before = deck_look(r, 0);
        CHECK(r.send(project(ProjectAction::Open, 0, 0)) == Reject::None);
        CHECK(r.settle());
        CHECK(std::strcmp(r.show->show().name, "My Show") == 0 && scene_count(r.show->show()) == 5);
        CHECK(looks_equal(deck_look(r, 0), program_before));
        CHECK(looks_equal(deck_look(r, 1), live_after));
        // Open the exported file: unpacked into the projects folder.
        uint32_t id = r.show->set_text(kTool, F::utf8(cfg.projects_root / "My Show.himmelshow").c_str());
        CHECK(r.send(project(ProjectAction::Open, static_cast<int>(id))) == Reject::None);
        CHECK(r.settle());
        CHECK(fs::exists(cfg.projects_root / "My Show (2).himmel" / "project.json"));
        CHECK(scene_count(r.show->show()) == 5 && r.show->files().recent_count == 2);
        // Opening nonsense fails in the worker; the show stays.
        id = r.show->set_text(kTool, F::utf8(root / "nothing-here").c_str());
        r.send(project(ProjectAction::Open, static_cast<int>(id)));
        CHECK(r.settle());
        CHECK(scene_count(r.show->show()) == 5 && r.show->files().error[0] != 0);
        CHECK(r.send(project(ProjectAction::Open, 0, 12)) == Reject::BadId);
    }
    {
        // A missing clip is listed on open; Relink finds it by name in a folder.
        fs::create_directories(root / "Media" / "sub");
        F::write_atomic(root / "Media" / "sub" / "clip01.mov", "x", nullptr);
        RuntimeConfig c2 = cfg;
        c2.open = cfg.projects_root / "My Show.himmel";
        Rig r(make_catalog(12, 1));
        r.start(c2);
        CHECK(r.settle());
        FilesStatus fsx = r.show->files();
        CHECK(fsx.missing_count == 1 && std::strcmp(fsx.missing[0], "clip01.mov") == 0);
        if (fsx.missing_count != 1)
            std::printf("    files: missing %u restored %u path \"%s\" error \"%s\" msg \"%s\"\n", fsx.missing_count, fsx.restored,
                        fsx.path, fsx.error, r.show->status().message);
        const uint32_t media = r.show->set_text(kTool, F::utf8(root / "Media").c_str());
        r.send(project(ProjectAction::Relink, static_cast<int>(media)));
        CHECK(r.settle());
        CHECK(r.show->files().relink_found == 1);
    }
    {
        // Untitled work survives a restart too, and Save names it.
        RuntimeConfig c3;
        c3.projects_root = root / "P3";
        c3.app_dir = root / "App3";
        {
            Rig r(make_catalog(12));
            r.start(c3);
            CHECK(r.settle());
            r.fill_deck(0, 4);
            r.send(cmd(CommandType::SceneSave, 0, kNone, kNone, kNone, 0));
            for (int i = 0; i < 30; ++i) r.tick(20000), Sleep(1);
            CHECK(r.settle());
            r.show->stop();
        }
        Rig r(make_catalog(12));
        r.start(c3);
        CHECK(r.settle());
        CHECK(scene_count(r.show->show()) == 1 && r.model->live().deck[0].layer[0].clip >= 0);
        CHECK(r.send(project(ProjectAction::Save)) == Reject::None);
        CHECK(r.settle());
        CHECK(fs::exists(c3.projects_root / "Untitled.himmel" / "project.json"));
    }
}

void test_snapshots() {
    std::printf("autosave snapshots: every n seconds, the last n kept\n");
    const fs::path root = temp_root("snapshots");
    RuntimeConfig cfg;
    cfg.projects_root = root / "Projects";
    cfg.app_dir = root / "App";
    cfg.snapshot_every_s = 1;
    cfg.keep_snapshots = 2;
    Rig r(make_catalog(8));
    r.start(cfg);
    CHECK(r.settle());
    const uint32_t snap = r.show->set_text(static_cast<int>(Producer::Tool), "Snap");
    r.send(cmd(CommandType::Project, kNone, kNone, kNone, static_cast<uint8_t>(ProjectAction::SaveAs), static_cast<int>(snap)));
    CHECK(r.settle());
    for (int i = 0; i < 350; ++i) {  // ~3.5 s
        r.deck(cmd(CommandType::LayerOpacity, 0, 0, kNone, kNone, 0, (i % 100) / 100.0f));
        r.tick(10000);
        Sleep(10);
    }
    CHECK(r.settle());
    int snaps = 0;
    for (const auto& e : fs::directory_iterator(cfg.projects_root / "Snap.himmel" / "autosave"))
        snaps += F::utf8(e.path().filename()).rfind("snap-", 0) == 0;
    CHECK(snaps >= 1 && snaps <= 2);
}

}  // namespace

int main() {
    test_diff_roundtrip();
    test_files_roundtrip();
    test_journal();
    test_recall_cost();
    test_morph_and_reset();
    test_undo();
    test_dice();
    test_pages_kits_macros();
    test_projects();
    test_snapshots();
    std::printf("show_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
