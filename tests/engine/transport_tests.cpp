// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// M2 clip transport (docs/design/clips-and-playback.md §2-3): the beat clock,
// every play mode, in / out, inner loop, cue points + warm frames, beat loops
// with catch-up, scratch, reverse hold, speed glide, trigger styles, beat snap,
// clip targets incl. the HIT layer, autopilot, fades and layer transitions,
// validation and restore. Pure logic: no GPU, no Windows.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/transport.h"

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
#define NEAR(a, b, eps) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))

// Clips: 0..3 are 300 frames at 30 fps (10 s); 4 is 60 frames at 30 fps.
static std::unique_ptr<Catalog> make_catalog() {
    auto c = std::make_unique<Catalog>();
    std::memset(c.get(), 0, sizeof(Catalog));
    c->clip_count = 5;
    for (int i = 0; i < 5; ++i) {
        std::snprintf(c->clip[i].name, kNameLen, "clip%02d", i);
        c->clip[i].fps = 30.0f;
        c->clip[i].seconds = i == 4 ? 2.0f : 10.0f;
    }
    fx::fill_catalog(*c);
    return c;
}

// A model and a clock running at 60 fps.
struct Rig {
    std::unique_ptr<Catalog> cat = make_catalog();
    std::unique_ptr<DeckModel> m = std::make_unique<DeckModel>(*cat);
    uint64_t t = 10'000'000;  // 10 s: beat 20 at 120 BPM
    uint32_t seq = 0;
    Rig() { m->tick(t); }
    Reject send(Command c) {
        c.seq = ++seq;
        return m->submit(static_cast<int>(Producer::Tool), c, t);
    }
    Reject set(int deck, int layer, LayerParam p, int32_t i, float f = 0.f, uint16_t flags = 0) {
        Command c = make(CommandType::LayerSet, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), i, f, flags);
        c.param = static_cast<uint8_t>(p);
        return send(c);
    }
    Reject setf(int deck, int layer, LayerParam p, float f) { return set(deck, layer, p, 0, f); }
    Reject clip(int deck, int layer, int id) { return send(make(CommandType::LayerClip, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), id)); }
    Reject trigger(int deck, int layer, int id, float vel = 1.f, bool release = false) {
        return send(make(CommandType::LayerTrigger, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer), id, vel,
                         release ? kRelease : 0));
    }
    Reject cue(int deck, int layer, int k, CueAction a, float f = 0.f) {
        Command c = make(CommandType::LayerCue, static_cast<uint8_t>(deck), static_cast<uint8_t>(layer),
                         static_cast<int32_t>(a), f);
        c.slot = static_cast<uint8_t>(k);
        return send(c);
    }
    Reject clock(ClockParam p, int32_t i = 0, float f = 0.f) {
        Command c = make(CommandType::Clock, kNone, kNone, i, f);
        c.param = static_cast<uint8_t>(p);
        return send(c);
    }
    void step(double seconds) {  // ticks at 60 fps
        const int n = static_cast<int>(std::lround(seconds * 60.0));
        for (int i = 0; i < n; ++i) {
            t += 16667;
            m->tick(t);
        }
    }
    int64_t frame(int deck = 0, int layer = 0) const { return m->frame(slot_of(deck, layer)); }
    const LayerState& L(int deck = 0, int layer = 0) const { return m->layer(slot_of(deck, layer)); }
    const LayerTransport& T(int deck = 0, int layer = 0) const { return m->layer_transport(slot_of(deck, layer)); }
    // Put both decks off the beat snap: PREVIEW is deck 1 at start; deck 0 is PROGRAM.
};

static void test_clock() {
    std::printf("beat clock + tap tempo\n");
    BeatClock c;
    c.reset(0);
    NEAR(c.bpm(), 120.0, 1e-9);
    NEAR(c.beat(1'000'000), 2.0, 1e-9);
    c.set_bpm(60.0, 1'000'000);  // phase continuous: beat 2 at 1 s, then one beat per second
    NEAR(c.beat(1'000'000), 2.0, 1e-9);
    NEAR(c.beat(2'000'000), 3.0, 1e-9);
    c.set_bpm(1000.0, 2'000'000);
    NEAR(c.bpm(), BeatClock::kMaxBpm, 1e-9);
    c.reset(0);
    for (int i = 0; i < 4; ++i) c.tap(5'000'000 + i * 400'000);  // 150 BPM
    NEAR(c.bpm(), 150.0, 1e-6);
    CHECK(c.taps() == 4 && c.tapped());
    NEAR(c.beat(5'000'000 + 3 * 400'000), std::round(c.beat(5'000'000 + 3 * 400'000)), 1e-9);  // a tap lands on a beat
    c.tap(9'000'000);  // after a 2.8 s gap: a new tap run, the tempo stays
    CHECK(c.taps() == 1);
    NEAR(c.bpm(), 150.0, 1e-6);
    c.resync(10'000'000);
    NEAR(c.beat(10'000'000), 0.0, 1e-9);
    c.nudge(0.5);
    NEAR(c.beat(10'000'000), 0.5, 1e-9);
    NEAR(grid_beats(GridDiv::Off), 0.0, 0);
    NEAR(grid_beats(GridDiv::Quarter), 0.25, 0);
    NEAR(grid_beats(GridDiv::Bar), 4.0, 0);
    NEAR(grid_beats(GridDiv::FourBars), 16.0, 0);
    // Through the model: Clock commands and the state section.
    Rig r;
    CHECK(r.clock(ClockParam::Bpm, 0, 128.f) == Reject::None);
    r.step(0.1);
    NEAR(r.m->transport().clock.bpm, 128.0, 1e-3);
    CHECK(r.clock(ClockParam::Bpm, 0, -8.f) == Reject::None);  // absolute -8 is clamped, not refused
    NEAR(r.m->bpm(), BeatClock::kMinBpm, 1e-9);
    CHECK(r.clock(ClockParam::Grid, static_cast<int32_t>(GridDiv::Bar)) == Reject::None);
    CHECK(r.m->transport().clock.grid == static_cast<uint8_t>(GridDiv::Bar));
    CHECK(r.clock(ClockParam::Grid, 9) == Reject::BadValue);
    CHECK(r.clock(ClockParam::Resync) == Reject::None);
    NEAR(r.m->beat(r.t), 0.0, 1e-9);
}

static void test_loop_and_range() {
    std::printf("loop, in / out, direction, speed\n");
    Rig r;
    CHECK(r.clip(1, 0, 0) == Reject::None);
    CHECK(r.frame(1, 0) == 0);
    r.step(1.0);
    NEAR(r.frame(1, 0), 30, 1);
    r.step(9.5);  // 10.5 s: wrapped once
    NEAR(r.frame(1, 0), 15, 1);
    CHECK(r.T(1, 0).loops == 1);
    CHECK(r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::Playing));
    NEAR(r.L(1, 0).position, r.frame(1, 0) / 300.0, 1.0 / 300.0);
    // in / out: 0.5 .. 0.6 = frames 150..180
    r.setf(1, 0, LayerParam::In, 0.5f);
    r.setf(1, 0, LayerParam::Out, 0.6f);
    r.step(0.1);  // outside the range: wraps in
    for (int i = 0; i < 120; ++i) {
        r.step(1.0 / 60);
        CHECK(r.frame(1, 0) >= 150 && r.frame(1, 0) < 180);
    }
    // backward
    r.set(1, 0, LayerParam::Direction, 1);
    const int64_t f0 = r.frame(1, 0);
    r.step(0.2);
    CHECK(r.frame(1, 0) != f0);
    for (int i = 0; i < 60; ++i) {
        const int64_t a = r.frame(1, 0);
        r.step(1.0 / 60);
        const int64_t b = r.frame(1, 0);
        CHECK((b <= a || (a < 152 && b > 175)) && b >= 150 && b < 180);  // moves down, wraps from in to out
    }
    CHECK(r.m->direction(slot_of(1, 0)) == -1);
    // pause
    r.send(make(CommandType::LayerPlay, 1, 0, static_cast<int32_t>(PlayAction::Pause)));
    const int64_t p = r.frame(1, 0);
    r.step(0.5);
    CHECK(r.frame(1, 0) == p && r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::Paused));
    // in > out is the same range swapped; a range never shrinks below one frame
    LayerTransport tt{};
    default_transport(tt, 0);
    tt.in = 0.7f, tt.out = 0.7f;
    double a, b;
    Playhead::range(ClipGeom{300, 30}, tt, a, b);
    CHECK(b - a >= 1.0 && a >= 0 && b <= 300);
}

static void test_glide() {
    std::printf("speed glide\n");
    Rig r;
    r.clip(1, 0, 0);
    r.step(0.1);
    NEAR(r.T(1, 0).speed_now, 1.0, 1e-6);
    r.send(make(CommandType::LayerSpeed, 1, 0, 0, 2.0f));  // glide 1 beat = 0.5 s at 120 BPM
    CHECK(r.L(1, 0).speed == 2.0f);
    r.step(0.25);
    NEAR(r.T(1, 0).speed_now, 1.5, 0.05);
    r.step(0.3);
    NEAR(r.T(1, 0).speed_now, 2.0, 1e-6);
    r.setf(1, 0, LayerParam::Glide, 0.0f);
    r.send(make(CommandType::LayerSpeed, 1, 0, 0, -1.0f));
    r.step(1.0 / 60);
    NEAR(r.T(1, 0).speed_now, -1.0, 1e-6);
    CHECK(r.m->direction(slot_of(1, 0)) == -1);
    // relative knob deltas glide too
    r.setf(1, 0, LayerParam::Glide, 1.0f);
    r.send(make(CommandType::LayerSpeed, 1, 0, 0, 0.5f, kRelative));
    CHECK(r.L(1, 0).speed == -0.5f);
}

static void test_bpm_time_base() {
    std::printf("BPM time base\n");
    Rig r;
    r.clip(1, 0, 0);
    r.set(1, 0, LayerParam::TimeBase, static_cast<int32_t>(TimeBase::Bpm));
    r.setf(1, 0, LayerParam::Beats, 4.0f);  // 300 frames in 4 beats = 2 s at 120 BPM
    r.step(1.0);
    NEAR(r.frame(1, 0), 150, 2);
    r.clock(ClockParam::Bpm, 0, 60.0f);  // 4 beats = 4 s now
    r.step(1.0);
    NEAR(r.frame(1, 0), 225, 2);
    NEAR(Playhead::base_rate(ClipGeom{300, 30}, r.T(1, 0), 60.0), 75.0, 1e-9);
}

static void test_pingpong() {
    std::printf("ping-pong\n");
    Rig r;
    r.clip(1, 0, 0);
    r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::PingPong));
    r.setf(1, 0, LayerParam::Out, 0.1f);  // frames 0..30
    r.step(1.5);                          // 45 frames: up to 30, back to 15
    NEAR(r.frame(1, 0), 15, 1);
    CHECK(r.m->direction(slot_of(1, 0)) == -1);
    CHECK(r.T(1, 0).loops == 0);
    r.step(0.6);  // back past the start: one full cycle
    CHECK(r.T(1, 0).loops == 1 && r.m->direction(slot_of(1, 0)) == 1);
    for (int i = 0; i < 300; ++i) {
        r.step(1.0 / 60);
        CHECK(r.frame(1, 0) >= 0 && r.frame(1, 0) < 30);
    }
}

static void test_once() {
    std::printf("once: hold, eject, next\n");
    Rig r;
    r.clip(1, 0, 0);
    r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::OnceHold));
    r.setf(1, 0, LayerParam::Out, 0.1f);
    r.step(2.0);
    CHECK(r.frame(1, 0) == 29 && r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::Holding));
    r.step(1.0);
    CHECK(r.frame(1, 0) == 29 && r.L(1, 0).clip == 0);
    r.send(make(CommandType::LayerPlay, 1, 0, static_cast<int32_t>(PlayAction::Restart)));
    r.step(0.5);
    NEAR(r.frame(1, 0), 15, 1);  // plays again after a restart

    r.clip(1, 1, 0);
    r.set(1, 1, LayerParam::Mode, static_cast<int32_t>(PlayMode::OnceEject));
    r.setf(1, 1, LayerParam::Out, 0.1f);
    r.step(1.1);
    CHECK(r.L(1, 1).clip == -1 && r.frame(1, 1) == -1);
    // with a fade-out: the gain goes down first
    r.clip(1, 1, 0);
    r.setf(1, 1, LayerParam::FadeOut, 1.0f);
    r.step(1.5);
    CHECK(r.L(1, 1).clip == 0 && r.T(1, 1).gain < 0.6f && r.T(1, 1).gain > 0.4f);
    r.step(0.6);
    CHECK(r.L(1, 1).clip == -1 && r.T(1, 1).gain == 1.0f);

    r.clip(1, 2, 1);
    r.set(1, 2, LayerParam::Mode, static_cast<int32_t>(PlayMode::OnceNext));
    r.setf(1, 2, LayerParam::Out, 0.1f);
    r.step(1.1);
    CHECK(r.L(1, 2).clip == 2 && r.frame(1, 2) < 10);  // the next clip, from its start
}

static void test_random_chop_step() {
    std::printf("random, chop, step\n");
    // random: deterministic with the seed, stays in the range, jumps every interval
    int64_t seq_a[40], seq_b[40];
    for (int run = 0; run < 2; ++run) {
        Rig r;
        r.clip(1, 0, 0);
        r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Random));
        r.setf(1, 0, LayerParam::Interval, 0.5f);
        r.setf(1, 0, LayerParam::Distance, 0.5f);
        r.setf(1, 0, LayerParam::In, 0.2f);
        r.setf(1, 0, LayerParam::Out, 0.8f);
        r.set(1, 0, LayerParam::Seed, 1234);
        int jumps = 0;
        int64_t last = r.frame(1, 0);
        for (int i = 0; i < 40; ++i) {
            r.step(0.25);
            const int64_t f = (run ? seq_b : seq_a)[i] = r.frame(1, 0);
            CHECK(f >= 60 && f < 240);
            if (std::llabs(f - last) > 9) ++jumps;  // more than a quarter second of playing
            last = f;
        }
        CHECK(jumps >= 12);  // 20 intervals in 10 s; a few random jumps land close by
    }
    CHECK(std::memcmp(seq_a, seq_b, sizeof(seq_a)) == 0);

    // chop: every interval the playhead lands on a slice start
    {
        Rig r;
        r.clip(1, 0, 0);
        r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Chop));
        r.set(1, 0, LayerParam::Slices, 4);  // 0, 75, 150, 225
        r.setf(1, 0, LayerParam::Interval, 0.5f);
        int seen[4] = {};
        for (int i = 0; i < 30; ++i) {
            r.step(0.5);
            const int64_t f = r.frame(1, 0);
            const int64_t off = f % 75;
            CHECK(off <= 2);  // on the slice start (+ at most one tick of playing)
            ++seen[std::min<int64_t>(3, f / 75)];
        }
        CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
        CHECK(r.set(1, 0, LayerParam::Slices, 99) == Reject::None && r.T(1, 0).slices == 16);  // clamped
    }

    // step: BPM time base, 2 frames every 1/4 beat at 120 BPM = 16 frames / s, and nothing in between
    {
        Rig r;
        r.clip(1, 0, 0);
        r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Step));
        r.set(1, 0, LayerParam::TimeBase, static_cast<int32_t>(TimeBase::Bpm));
        r.set(1, 0, LayerParam::StepFrames, 2);
        r.setf(1, 0, LayerParam::StepDiv, 0.25f);
        int changes = 0;
        int64_t last = r.frame(1, 0);
        for (int i = 0; i < 60; ++i) {
            r.step(1.0 / 60);
            const int64_t f = r.frame(1, 0);
            if (f != last) {
                ++changes;
                CHECK(f - last == 2);
            }
            last = f;
        }
        NEAR(changes, 8, 1);
        NEAR(r.frame(1, 0), 16, 2);
    }
}

static void test_scratch() {
    std::printf("scratch (mode and hold), reverse hold\n");
    Rig r;
    r.clip(1, 0, 0);
    r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Scratch));
    r.step(1.0);
    CHECK(r.frame(1, 0) == 0);  // only the controller moves it
    r.send(make(CommandType::LayerScratch, 1, 0, 1, 0.5f));
    CHECK(r.frame(1, 0) == 150);
    r.send(make(CommandType::LayerScratch, 1, 0, 1, 1.0f, kRelative));  // + 1 s of clip
    CHECK(r.frame(1, 0) == 180);
    r.step(1.0);
    CHECK(r.frame(1, 0) == 180);

    // Hold in LOOP mode: stay (default) vs snap back
    r.set(1, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Loop));
    r.send(make(CommandType::LayerJump, 1, 0, 0, 0.0f));
    r.step(1.0);  // ~30
    r.send(make(CommandType::LayerScratch, 1, 0, 1, 0.5f));
    CHECK(r.frame(1, 0) == 150 && r.T(1, 0).scratching == 0);  // status updates on the next tick
    r.step(1.0);
    CHECK(r.frame(1, 0) == 150 && r.T(1, 0).scratching == 1);  // held: does not play
    r.send(make(CommandType::LayerScratch, 1, 0, 0));
    r.step(0.5);
    NEAR(r.frame(1, 0), 165, 1);  // stay: continues from where it was let go
    r.set(1, 0, LayerParam::ScratchSnap, kOn);
    const int64_t before = r.frame(1, 0);
    r.send(make(CommandType::LayerScratch, 1, 0, 1, 0.9f));
    r.step(1.0);
    r.send(make(CommandType::LayerScratch, 1, 0, 0));
    NEAR(r.frame(1, 0), before + 30, 1);  // snap back: where it would have been

    // REVERSE held: plays backward, comes back with catch-up (§2.1)
    r.send(make(CommandType::LayerJump, 1, 0, 0, 0.5f));
    r.set(1, 0, LayerParam::ReverseHold, 1);
    r.step(1.0);
    NEAR(r.frame(1, 0), 120, 1);
    CHECK(r.m->direction(slot_of(1, 0)) == -1);
    r.set(1, 0, LayerParam::ReverseHold, 0);
    NEAR(r.frame(1, 0), 180, 1);
}

static void test_inner_loop() {
    std::printf("inner loop (4-point)\n");
    Rig r;
    r.clip(1, 0, 0);
    r.setf(1, 0, LayerParam::LoopIn, 0.2f);   // 60
    r.setf(1, 0, LayerParam::LoopOut, 0.4f);  // 120
    r.set(1, 0, LayerParam::InnerLoop, kOn);
    r.step(1.0 / 60);
    CHECK(r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::Intro));
    r.step(5.0);  // intro 4 s, then loops in 60..120
    CHECK(r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::InnerLoop));
    for (int i = 0; i < 200; ++i) {
        r.step(1.0 / 60);
        CHECK(r.frame(1, 0) >= 60 && r.frame(1, 0) < 120);
    }
    CHECK(r.T(1, 0).loops >= 2);
    r.send(make(CommandType::LayerPlay, 1, 0, static_cast<int32_t>(PlayAction::Release)));
    r.step(1.0 / 60);
    CHECK(r.T(1, 0).phase == static_cast<uint8_t>(PlayheadPhase::Outro));
    r.step(2.5);
    CHECK(r.frame(1, 0) >= 120);  // played out past loop-out
}

static void test_beat_loop() {
    std::printf("beat loop + catch-up\n");
    for (int catch_up = 1; catch_up >= 0; --catch_up) {
        Rig r;
        r.clip(1, 0, 0);
        r.set(1, 0, LayerParam::CatchUp, catch_up);
        r.step(1.0);
        const int64_t start = r.frame(1, 0);  // ~30
        r.send(make(CommandType::LayerBeatLoop, 1, 0, 0, 1.0f));  // 1 beat = 0.5 s = 15 frames
        for (int i = 0; i < 120; ++i) {
            r.step(1.0 / 60);
            CHECK(r.frame(1, 0) >= start && r.frame(1, 0) < start + 16);
        }
        CHECK(r.T(1, 0).beat_loop == 1 && r.T(1, 0).beat_loop_beats == 1.0f);
        r.send(make(CommandType::LayerBeatLoop, 1, 0, 0, 0.25f));  // harder press: shorter, same start
        for (int i = 0; i < 60; ++i) {
            r.step(1.0 / 60);
            CHECK(r.frame(1, 0) >= start && r.frame(1, 0) < start + 5);
        }
        r.send(make(CommandType::LayerBeatLoop, 1, 0, 0, 0.0f));
        if (catch_up) NEAR(r.frame(1, 0), start + 90, 2);  // 3 s later: where it would have been
        else CHECK(r.frame(1, 0) >= start && r.frame(1, 0) < start + 5);
    }
    // a cue jump ends a beat loop
    Rig r;
    r.clip(1, 0, 0);
    r.send(make(CommandType::LayerBeatLoop, 1, 0, 0, 0.5f));
    r.step(0.2);
    r.cue(1, 0, 8, CueAction::Jump);
    r.step(1.0 / 60);
    CHECK(r.T(1, 0).beat_loop == 0);
    NEAR(r.frame(1, 0), 150, 1);
}

static void test_cues_and_warm_frames() {
    std::printf("cue points, slices, warm frames\n");
    Rig r;
    r.clip(1, 0, 0);
    r.step(0.3);
    // no cue points: 16 equal slices of in..out
    for (int k = 0; k < kCuePoints; ++k) {
        const uint32_t g = r.m->jump_gen_slot(slot_of(1, 0));
        CHECK(r.cue(1, 0, k, CueAction::Jump) == Reject::None);
        CHECK(r.frame(1, 0) == k * 300 / 16);  // at once, before the next tick: visible on the next frame
        CHECK(r.m->jump_gen_slot(slot_of(1, 0)) == g + 1);
    }
    CHECK(r.cue(1, 0, 3, CueAction::Set, 0.5f) == Reject::None);
    CHECK(r.T(1, 0).cue[3] == 0.5f);
    r.cue(1, 0, 3, CueAction::Jump);
    CHECK(r.frame(1, 0) == 150);
    // set at the playhead
    r.send(make(CommandType::LayerJump, 1, 0, 0, 0.0f));
    r.step(1.0);
    const float here = r.L(1, 0).position;
    r.cue(1, 0, 5, CueAction::Set, -1.0f);
    CHECK(r.T(1, 0).cue[5] == here);
    // the warm frames are exactly where the jumps land, for every pad (cue points and slices, with in / out)
    r.setf(1, 0, LayerParam::In, 0.13f);
    r.setf(1, 0, LayerParam::Out, 0.77f);
    r.cue(1, 0, 7, CueAction::Set, 0.3333f);
    int64_t warm[kCuePoints];
    r.m->warm_frames(slot_of(1, 0), warm);
    for (int k = 0; k < kCuePoints; ++k) {
        r.cue(1, 0, k, CueAction::Jump);
        CHECK(r.frame(1, 0) == warm[k]);
        r.step(1.0 / 60);  // the frame tick right after the jump still shows the landing (warm) frame
        CHECK(r.frame(1, 0) == warm[k]);
        r.step(0.1);
        CHECK(r.frame(1, 0) > warm[k]);  // then it plays on
    }
    // cue points belong to the clip: another layer with the same clip sees them, another clip does not
    r.clip(1, 1, 0);
    CHECK(r.T(1, 1).cue[3] == 0.5f);
    r.clip(1, 1, 1);
    CHECK(r.T(1, 1).cue[3] < 0.0f);
    // clear
    r.cue(1, 0, 3, CueAction::Clear);
    CHECK(r.T(1, 0).cue[3] < 0.0f);
    r.clip(1, 2, 0);
    CHECK(r.T(1, 2).cue[3] < 0.0f && r.T(1, 2).cue[7] > 0.33f);
}

static void test_trigger_styles() {
    std::printf("trigger styles, start position, fades\n");
    Rig r;  // deck 1 = PREVIEW: never waits
    // NORMAL: press = start, again = restart
    r.trigger(1, 0, 2);
    CHECK(r.L(1, 0).clip == 2 && r.frame(1, 0) == 0);
    r.step(1.0);
    r.trigger(1, 0, 2);
    CHECK(r.frame(1, 0) == 0);
    // TOGGLE: press again = stop
    r.set(1, 1, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Toggle));
    r.trigger(1, 1, 3);
    CHECK(r.L(1, 1).clip == 3);
    r.trigger(1, 1, 3);
    CHECK(r.L(1, 1).clip == -1);
    // PIANO: release = stop (fade out), a release of another clip does nothing
    r.set(1, 2, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Piano));
    r.setf(1, 2, LayerParam::FadeOut, 0.5f);
    r.trigger(1, 2, 1, 0.7f);
    CHECK(r.L(1, 2).clip == 1 && r.T(1, 2).velocity == 0.7f);
    r.trigger(1, 2, 0, 1.f, true);
    r.step(0.1);
    CHECK(r.L(1, 2).clip == 1 && r.T(1, 2).gain == 1.0f);
    r.trigger(1, 2, 1, 1.f, true);
    r.step(0.25);
    CHECK(r.L(1, 2).clip == 1 && r.T(1, 2).gain < 0.6f);
    r.step(0.3);
    CHECK(r.L(1, 2).clip == -1);
    // LEGATO: starts where the replaced clip was
    r.set(1, 3, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Legato));
    r.trigger(1, 3, 0);
    r.step(2.0);
    const int64_t at = r.frame(1, 3);
    r.trigger(1, 3, 1);
    CHECK(r.L(1, 3).clip == 1);
    NEAR(r.frame(1, 3), at, 1);
    // CONTINUE: a clip comes back where it was stopped
    r.set(1, 0, LayerParam::StartPos, static_cast<int32_t>(StartPos::Continue));
    r.trigger(1, 0, 0);
    r.step(3.0);
    const int64_t was = r.frame(1, 0);
    r.trigger(1, 0, 1);
    r.step(0.5);
    r.trigger(1, 0, 0);
    NEAR(r.frame(1, 0), was, 1);
    // FADE IN on an empty layer
    r.clip(1, 1, -1);
    r.setf(1, 1, LayerParam::FadeIn, 1.0f);
    r.trigger(1, 1, 2);
    CHECK(r.T(1, 1).gain == 0.0f);
    r.step(0.5);
    NEAR(r.T(1, 1).gain, 0.5, 0.03);
    r.step(0.6);
    CHECK(r.T(1, 1).gain == 1.0f);
    // STOP: fade out, then empty
    r.setf(1, 1, LayerParam::FadeOut, 0.0f);
    r.send(make(CommandType::LayerPlay, 1, 1, static_cast<int32_t>(PlayAction::Stop)));
    CHECK(r.L(1, 1).clip == -1);
    // "the layer's clip" (-1) on an empty layer is refused
    CHECK(r.trigger(1, 1, -1) == Reject::BadValue);
    r.set(1, 1, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Normal));
    r.clip(1, 1, 4);
    CHECK(r.trigger(1, 1, -1) == Reject::None && r.L(1, 1).clip == 4);
}

static void test_beat_snap() {
    std::printf("beat snap (PROGRAM waits, PREVIEW never)\n");
    Rig r;
    r.clock(ClockParam::Grid, static_cast<int32_t>(GridDiv::Bar));
    r.clock(ClockParam::Resync);  // beat 0 now
    r.step(0.5);                  // beat 1
    CHECK(r.trigger(0, 0, 1) == Reject::None);  // deck 0 = PROGRAM
    CHECK(r.L(0, 0).clip == -1 && r.T(0, 0).pending == 1);
    r.step(0.1);
    NEAR(r.T(0, 0).pending_beats, 2.8, 0.05);
    r.trigger(1, 0, 1);  // PREVIEW: at once
    CHECK(r.L(1, 0).clip == 1);
    r.step(1.35);  // beat 3.9
    CHECK(r.L(0, 0).clip == -1);
    r.step(0.1);  // beat 4.1: fired on the bar
    CHECK(r.L(0, 0).clip == 1 && r.T(0, 0).pending == 0);
    NEAR(r.frame(0, 0), 3, 1);
    // a per-layer override: OFF on this layer = at once
    r.set(0, 1, LayerParam::Snap, static_cast<int32_t>(GridDiv::Off));
    r.trigger(0, 1, 2);
    CHECK(r.L(0, 1).clip == 2);
    // a cue jump on PROGRAM waits too; a piano release before the beat cancels the launch
    r.cue(0, 0, 8, CueAction::Jump);
    CHECK(r.frame(0, 0) != 150 && r.T(0, 0).pending == 1);
    r.step(2.1);
    CHECK(r.frame(0, 0) >= 150 && r.frame(0, 0) <= 156);  // jumped on the bar (beat 8), then played on
    r.set(0, 2, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Piano));
    r.trigger(0, 2, 3);
    CHECK(r.T(0, 2).pending == 1);
    r.trigger(0, 2, 3, 1.f, true);
    r.step(2.1);
    CHECK(r.L(0, 2).clip == -1 && r.T(0, 2).pending == 0);
    // a plain clip load never waits
    r.clip(0, 3, 0);
    CHECK(r.L(0, 3).clip == 0);
}

static void test_targets_and_hit() {
    std::printf("clip targets: own, free, HIT layer\n");
    Rig r;
    r.set(1, 0, LayerParam::Target, static_cast<int32_t>(ClipTarget::Free));
    r.set(1, 0, LayerParam::Trigger, static_cast<int32_t>(TriggerStyle::Piano));
    r.clip(1, 0, 4);  // layer 1 is taken
    r.trigger(1, 0, 1);
    r.trigger(1, 0, 2);
    CHECK(r.L(1, 1).clip == 1 && r.L(1, 2).clip == 2 && r.L(1, 0).clip == 4);
    r.trigger(1, 0, 1, 1.f, true);  // the release finds its layer
    CHECK(r.L(1, 1).clip == -1 && r.L(1, 2).clip == 2);
    // HIT: on top of PROGRAM, velocity = start opacity, release fades out
    r.set(1, 3, LayerParam::Target, static_cast<int32_t>(ClipTarget::Hit));
    r.trigger(1, 3, 3, 0.6f);
    const LayerState& hit = r.m->layer(kHitSlot);
    CHECK(hit.clip == 3 && hit.opacity == 0.6f && hit.blend == static_cast<uint8_t>(BlendMode::Add));
    CHECK(r.L(1, 3).clip == -1);
    CHECK(r.m->transport().hit.clip == 3);
    r.step(0.1);
    r.trigger(1, 3, 3, 1.f, true);
    r.step(0.1);
    CHECK(hit.clip == 3 && r.m->layer_transport(kHitSlot).gain < 0.6f);
    r.step(0.15);
    CHECK(hit.clip == -1);
    // the HIT layer addressed directly (SOLO + pad)
    CHECK(r.trigger(kHitDeck, 0, 2) == Reject::None && hit.clip == 2);
    CHECK(r.send(make(CommandType::LayerOpacity, kHitDeck, 0, 0, 0.3f)) == Reject::None && hit.opacity == 0.3f);
    CHECK(r.send(make(CommandType::LayerOpacity, kHitDeck, 1, 0, 0.3f)) == Reject::BadTarget);
    CHECK(r.send(make_fx(CommandType::FxSet, kHitDeck, 0, 0, kNone, 0)) == Reject::BadTarget);
    CHECK(r.send(make(CommandType::Focus, kHitDeck, 0)) == Reject::BadTarget);
}

static void test_autopilot() {
    std::printf("autopilot\n");
    Rig r;
    r.clip(1, 0, 4);  // 2 s clip on PREVIEW
    r.set(1, 0, LayerParam::Autopilot, static_cast<int32_t>(AutopilotMode::Next));
    r.set(1, 0, LayerParam::AutopilotCount, 2);
    r.step(3.9);
    CHECK(r.L(1, 0).clip == 4);
    r.step(0.2);
    CHECK(r.L(1, 0).clip == 0);  // 4 + 1 wraps to 0
    r.set(1, 0, LayerParam::Autopilot, static_cast<int32_t>(AutopilotMode::Previous));
    r.set(1, 0, LayerParam::AutopilotCount, 1);
    r.step(10.1);
    CHECK(r.L(1, 0).clip == 4);
    // RANDOM never picks the same clip
    r.set(1, 0, LayerParam::Autopilot, static_cast<int32_t>(AutopilotMode::Random));
    int last = r.L(1, 0).clip, changes = 0;
    for (int i = 0; i < 60; ++i) {
        r.step(0.5);
        if (r.L(1, 0).clip != last) {
            ++changes;
            last = r.L(1, 0).clip;
        }
    }
    CHECK(changes >= 3);
    // on PROGRAM the change waits for the beat
    Rig p;
    p.clock(ClockParam::Bpm, 0, 30.0f);  // a beat every 2 s
    p.clock(ClockParam::Resync);
    p.clip(0, 0, 4);
    p.set(0, 0, LayerParam::Autopilot, static_cast<int32_t>(AutopilotMode::Next));
    p.set(0, 0, LayerParam::AutopilotCount, 1);
    p.step(1.0);
    p.clock(ClockParam::Nudge, 0, 0.25f);  // the next beat is 1.5 s after the loop ends
    p.step(1.2);
    CHECK(p.L(0, 0).clip == 4 && p.T(0, 0).pending == 1);
    p.step(1.4);
    CHECK(p.L(0, 0).clip == 0);
    // ONCE · NEXT does not need autopilot on
    CHECK(p.T(0, 0).loops == 0);
}

static void test_transitions() {
    std::printf("layer transitions\n");
    Rig r;
    r.clip(1, 0, 0);
    // CUT (default)
    uint32_t tg = r.m->transition_gen(slot_of(1, 0));
    r.clip(1, 0, 1);
    CHECK(r.L(1, 0).clip == 1 && r.m->transition_gen(slot_of(1, 0)) == tg && r.T(1, 0).transition_progress == 1.0f);
    // FADE: new clip at once, the renderer crossfades from the old picture
    r.set(1, 0, LayerParam::Transition, static_cast<int32_t>(TransitionType::Fade));
    r.setf(1, 0, LayerParam::TransitionTime, 1.0f);
    r.clip(1, 0, 2);
    CHECK(r.L(1, 0).clip == 2 && r.m->transition_gen(slot_of(1, 0)) == tg + 1);
    r.step(0.5);
    NEAR(r.T(1, 0).transition_progress, 0.5, 0.03);
    r.step(0.6);
    CHECK(r.T(1, 0).transition_progress == 1.0f);
    // BLACK-DIP: the old clip fades out, then the new one fades in
    r.set(1, 0, LayerParam::Transition, static_cast<int32_t>(TransitionType::BlackDip));
    r.clip(1, 0, 3);
    CHECK(r.L(1, 0).clip == 2);
    r.step(0.25);
    CHECK(r.L(1, 0).clip == 2 && r.T(1, 0).gain < 0.6f);
    r.step(0.3);
    CHECK(r.L(1, 0).clip == 3 && r.T(1, 0).gain < 0.2f);
    r.step(0.5);
    CHECK(r.L(1, 0).clip == 3 && r.T(1, 0).gain == 1.0f);
    // FADE to empty: fades out, then empty
    r.set(1, 0, LayerParam::Transition, static_cast<int32_t>(TransitionType::Fade));
    r.clip(1, 0, -1);
    CHECK(r.L(1, 0).clip == 3);
    r.step(1.1);
    CHECK(r.L(1, 0).clip == -1);
}

static void test_validation() {
    std::printf("validation\n");
    Rig r;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(r.set(0, 0, LayerParam::Count, 0) == Reject::BadTarget);
    CHECK(r.set(0, 0, LayerParam::Mode, static_cast<int32_t>(PlayMode::Count)) == Reject::BadValue);
    CHECK(r.set(0, 0, LayerParam::Mode, -1) == Reject::BadValue);
    CHECK(r.set(0, 0, LayerParam::Snap, -1) == Reject::None && r.T().snap == kNone);
    CHECK(r.set(0, 0, LayerParam::Snap, -2) == Reject::BadValue);
    CHECK(r.set(0, 0, LayerParam::In, 0, nan) == Reject::BadValue);
    CHECK(r.setf(0, 0, LayerParam::In, 1.7f) == Reject::None && r.T().in == 1.0f);  // clamped
    CHECK(r.setf(0, 0, LayerParam::Beats, 1000.f) == Reject::None && r.T().beats == 64.0f);
    CHECK(r.set(0, 0, LayerParam::Direction, 2) == Reject::BadValue);
    CHECK(r.set(2, 0, LayerParam::Mode, 0) == Reject::BadTarget);
    CHECK(r.cue(0, 0, 0, CueAction::Jump) == Reject::BadTarget);  // empty layer
    r.clip(0, 0, 0);
    CHECK(r.cue(0, 0, 16, CueAction::Jump) == Reject::BadTarget);
    CHECK(r.cue(0, 0, 0, static_cast<CueAction>(3)) == Reject::BadValue);
    CHECK(r.cue(0, 0, 0, CueAction::Set, nan) == Reject::BadValue);
    CHECK(r.trigger(0, 0, 99) == Reject::BadId);
    CHECK(r.trigger(0, 0, 1, nan) == Reject::BadValue);
    CHECK(r.send(make(CommandType::LayerScratch, 0, 0, 2, 0.f)) == Reject::BadValue);
    CHECK(r.send(make(CommandType::LayerBeatLoop, 0, 0, 0, nan)) == Reject::BadValue);
    CHECK(r.send(make(CommandType::LayerBeatLoop, 0, 0, 0, 1.f)) == Reject::None);
    Command c = make(CommandType::Clock);
    c.param = 9;
    CHECK(r.send(c) == Reject::BadTarget);
    CHECK(r.clock(ClockParam::Bpm, 0, nan) == Reject::BadValue);
    CHECK(r.send(make(CommandType::LayerPlay, 0, 0, static_cast<int32_t>(PlayAction::Count))) == Reject::BadValue);
    CHECK(std::strcmp(command_name(CommandType::Clock), "clock") == 0);
    CHECK(std::strcmp(command_name(CommandType::LayerTrigger), "trigger") == 0);
}

static void test_restore() {
    std::printf("restore with transport\n");
    Rig a;
    a.clip(1, 2, 1);
    a.set(1, 2, LayerParam::Mode, static_cast<int32_t>(PlayMode::PingPong));
    a.setf(1, 2, LayerParam::In, 0.25f);
    a.cue(1, 2, 4, CueAction::Set, 0.6f);
    a.clock(ClockParam::Bpm, 0, 133.0f);
    a.clock(ClockParam::Grid, static_cast<int32_t>(GridDiv::Beat));
    a.trigger(kHitDeck, 0, 2);
    a.step(1.0);
    auto old_live = std::make_unique<LiveState>(a.m->live());
    auto old_tr = std::make_unique<TransportSection>(a.m->transport());
    auto old_cat = std::make_unique<Catalog>(*a.cat);
    old_tr->layer[1][2].slices = 200;          // garbage is clamped
    old_tr->layer[1][2].fade_in = std::numeric_limits<float>::infinity();
    Rig b;
    const int lost = b.m->restore(*old_live, *old_cat, old_tr.get());
    CHECK(lost == 0);
    CHECK(b.L(1, 2).clip == 1 && b.T(1, 2).mode == static_cast<uint8_t>(PlayMode::PingPong) && b.T(1, 2).in == 0.25f);
    CHECK(b.T(1, 2).cue[4] == 0.6f && b.T(1, 2).slices == 16 && b.T(1, 2).fade_in == 0.0f);
    NEAR(b.m->bpm(), 133.0, 1e-3);
    CHECK(b.m->transport().clock.grid == static_cast<uint8_t>(GridDiv::Beat));
    NEAR(b.frame(1, 2), a.frame(1, 2), 1);
    CHECK(b.m->layer(kHitSlot).clip == -1);  // the HIT layer is never restored
    b.clip(1, 0, 1);  // the cue points came back with the clip
    CHECK(b.T(1, 0).cue[4] == 0.6f);
    // a version 1 block (no transport) still restores the show
    Rig c;
    CHECK(c.m->restore(*old_live, *old_cat) == 0 && c.L(1, 2).clip == 1 && c.T(1, 2).mode == 0);
}

int main() {
    test_clock();
    test_loop_and_range();
    test_glide();
    test_bpm_time_base();
    test_pingpong();
    test_once();
    test_random_chop_step();
    test_scratch();
    test_inner_loop();
    test_beat_loop();
    test_cues_and_warm_frames();
    test_trigger_styles();
    test_beat_snap();
    test_targets_and_hit();
    test_autopilot();
    test_transitions();
    test_validation();
    test_restore();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
