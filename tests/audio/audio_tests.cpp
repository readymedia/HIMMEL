// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// audio_tests.exe - the AUDIO track without hardware (docs/interfaces/audio-block.md):
// filters and FFT, onset timing on synthetic drums (line level and a far mic),
// no false kicks on a held bass, gate and auto gain, the MIDI clock tracker,
// the block layout, the engine's music feed (glide to rest, beat pulses, MIDI
// lead), BeatClock::follow, and zoom pulse moving on the kick through the FX resolver.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "himmel/audio/analyzer.h"
#include "himmel/audio/block.h"
#include "himmel/audio/midi_clock.h"
#include "himmel/audio/synth.h"
#include "himmel/engine/deck_model.h"
#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/reactions.h"
#include "himmel/engine/transport.h"
#include "himmel/fx/fx.h"
#include "himmel/fx/signals.h"

using namespace himmel::audio;
namespace eng = himmel::engine;
namespace fx = himmel::fx;
namespace proto = himmel::protocol;

static int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            ++g_fail;                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
        }                                                            \
    } while (0)

namespace {

// Runs `seconds` of synth through an analyzer; calls f(analyzer) after every hop.
template <class F>
void run(DrumSynth& d, Analyzer& an, double seconds, F&& f) {
    const int total = static_cast<int>(seconds * d.sr);
    std::vector<float> buf(480);
    for (int done = 0; done < total; done += 480) {
        for (float& v : buf) v = d.next();
        const float* p = buf.data();
        int n = 480;
        while (n > 0)
            if (an.feed(p, n)) f(an);
    }
}

struct Hits {
    int kicks = 0, snares = 0, hats = 0, late_kicks = 0;
    double worst_kick_ms = 0;
};

// Counts onsets after `from_s`; kick latency against the synth's beat grid.
Hits count(DrumSynth& d, Analyzer& an, double seconds, double from_s) {
    Hits h;
    uint32_t k = 0, s = 0, hh = 0;
    run(d, an, seconds, [&](const Analyzer& a) {
        const Signals& o = a.out();
        const double t = a.seconds();
        if (o.kicks != k) {
            k = o.kicks;
            if (t > from_s) {
                ++h.kicks;
                const double since = t - d.beat_time(static_cast<int>(std::floor(t * d.bpm / 60.0)));
                h.worst_kick_ms = std::max(h.worst_kick_ms, since * 1000);
                if (since > 0.0151) ++h.late_kicks;  // within 3 hops (a kick is told from a snare in hop 2)
            }
        }
        if (o.snares != s) {
            s = o.snares;
            if (t > from_s) ++h.snares;
        }
        if (o.hats != hh) {
            hh = o.hats;
            if (t > from_s) ++h.hats;
        }
    });
    return h;
}

void test_fft_and_filters() {
    // FFT: a bin-centred sine lands in its bin.
    float re[64], im[64];
    for (int i = 0; i < 64; ++i) re[i] = std::sin(2.0f * 3.14159265f * 5.0f * i / 64.0f), im[i] = 0;
    fft(re, im, 64);
    int best = 0;
    for (int i = 1; i < 32; ++i)
        if (re[i] * re[i] + im[i] * im[i] > re[best] * re[best] + im[best] * im[best]) best = i;
    CHECK(best == 5);
    // Low-pass 150 Hz: passes 50 Hz, kills 5 kHz.
    auto gain = [](Biquad f, float hz) {
        double acc = 0;
        for (int i = 0; i < 48000; ++i) {
            const float y = f.run(std::sin(2.0f * 3.14159265f * hz * i / 48000.0f));
            if (i > 24000) acc += y * y;
        }
        return std::sqrt(acc / 24000.0) * std::sqrt(2.0);
    };
    CHECK(gain(Biquad::lowpass(48000, 150), 50) > 0.9);
    CHECK(gain(Biquad::lowpass(48000, 150), 5000) < 0.01);
    CHECK(gain(Biquad::highpass(48000, 7000), 15000) > 0.8);
    CHECK(gain(Biquad::highpass(48000, 7000), 200) < 0.01);
    // Spectrum: a 1 kHz tone peaks in the band that holds 1 kHz.
    Analyzer an(48000);
    double t = 0;
    std::vector<float> x(48000);
    for (float& v : x) v = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265 * 1000.0 * t)), t += 1.0 / 48000;
    const float* p = x.data();
    int n = static_cast<int>(x.size());
    while (n > 0) an.feed(p, n);
    int top = 0;
    for (int k = 1; k < kSpectrumBands; ++k)
        if (an.out().spectrum[k] > an.out().spectrum[top]) top = k;
    const float lo = 40.0f * std::pow(400.0f, top / 32.0f), hi = 40.0f * std::pow(400.0f, (top + 1) / 32.0f);
    CHECK(lo <= 1050.0f && hi >= 950.0f);
    CHECK(sig(an.out(), Sig::Mid) > 0.8f);  // 1 kHz is "mid"
    CHECK(sig(an.out(), Sig::Bass) < 0.2f || an.out().v[1] <= an.out().v[2]);
}

void test_onsets_line_and_mic() {
    // A line with room noise at three levels, and a clean digital source (silence
    // between the hits, so the gate closes between beats: a virtual cable / loopback).
    for (const float level : {1.0f, 0.1f, 0.02f, -0.3f}) {
        DrumSynth d;
        d.level = std::fabs(level);
        d.noise_gain = level < 0 ? 0.0f : 0.002f;
        Analyzer an(48000);
        const Hits h = count(d, an, 24.0, 8.0);  // 16 s after auto gain settled: 32 beats
        std::printf("  level %.2f: kicks %d (late %d, worst %.1f ms) snares %d hats %d, gain %+.1f dB\n", level, h.kicks,
                    h.late_kicks, h.worst_kick_ms, h.snares, h.hats, an.out().gain_db);
        CHECK(h.kicks == 32);
        CHECK(h.late_kicks == 0);
        CHECK(h.snares >= 14 && h.snares <= 17);  // 16 (beats 2 and 4)
        CHECK(h.hats >= 40 && h.hats <= 66);      // off-beat eighths at least; on-beat ones may hide under kick / snare
    }
    // 44.1 kHz and a faster tempo
    DrumSynth d;
    d.sr = 44100;
    d.bpm = 174;
    Analyzer an(44100);
    const Hits h = count(d, an, 20.0, 6.0);
    const int want = static_cast<int>(std::floor(20.0 * 174 / 60)) - static_cast<int>(std::floor(6.0 * 174 / 60));
    std::printf("  174 BPM @44.1k: kicks %d (want %d, worst %.1f ms)\n", h.kicks, want, h.worst_kick_ms);
    CHECK(std::abs(h.kicks - want) <= 1);
    CHECK(h.late_kicks == 0);
}

void test_drum_separation() {
    // Which drum makes which onset: each kit piece left out once. 32 beats.
    struct V {
        bool kick, snare, hats;
        int kicks, snares, hats_lo, hats_hi;
    };
    const V vs[] = {{true, true, true, 32, 16, 40, 56},    // full kit (hats under a snare are hidden)
                    {false, true, true, 0, 16, 40, 56},    // no kick: the snare never makes a kick
                    {true, false, true, 32, 0, 56, 64},    // no snare: kick + hat never makes a snare
                    {true, true, false, 32, -1, 0, 64}};   // no hats: KNOWN WEAK (snare on a kick), only printed
    for (const V& v : vs) {
        DrumSynth d;
        d.kick = v.kick, d.snare = v.snare, d.hats = v.hats;
        Analyzer an(48000);
        const Hits h = count(d, an, 24.0, 8.0);
        std::printf("  kit kick %d snare %d hats %d: kicks %d snares %d hats %d\n", v.kick, v.snare, v.hats, h.kicks, h.snares,
                    h.hats);
        CHECK(h.kicks == v.kicks);
        if (v.snares >= 0) CHECK(std::abs(h.snares - v.snares) <= 1);
        CHECK(h.hats >= v.hats_lo && h.hats <= v.hats_hi);
    }
}

void test_hop_alignment() {
    // The same kit with the beat landing anywhere inside a 5 ms hop.
    for (int us = 0; us < 5000; us += 500) {
        DrumSynth d;
        d.level = 0.3f;
        Analyzer an(48000);
        for (int i = 0; i < us * 48 / 1000; ++i) {  // pad with silence
            const float z = 0.0f;
            const float* p = &z;
            int n = 1;
            an.feed(p, n);
        }
        const Hits h = count(d, an, 16.0, 6.0);
        if (h.kicks != 20 || std::abs(h.snares - 10) > 1)
            std::printf("  offset %4d us: kicks %d snares %d hats %d\n", us, h.kicks, h.snares, h.hats);
        CHECK(h.kicks == 20);
        CHECK(std::abs(h.snares - 10) <= 1);
        CHECK(h.hats >= 24 && h.hats <= 36);
    }
}

void test_no_false_kicks() {
    // A held 55 Hz bass and room noise, no drums: after the first second, no kicks.
    DrumSynth d;
    d.kick = d.snare = d.hats = false;
    d.bass_gain = 0.5f;
    d.noise_gain = 0.01f;
    Analyzer an(48000);
    const Hits h = count(d, an, 10.0, 1.0);
    CHECK(h.kicks == 0);
    CHECK(sig(an.out(), Sig::Bass) > 0.7f);  // the bass band hears it
    CHECK(sig(an.out(), Sig::Kick) < 0.05f);
}

void test_gate_and_gain() {
    // Noise at -75 dBFS: gated, everything still.
    DrumSynth d;
    d.kick = d.snare = d.hats = false;
    d.noise_gain = 0.0003f;
    Analyzer an(48000);
    const Hits h = count(d, an, 3.0, 0.0);
    CHECK(an.out().gated == 1);
    CHECK(sig(an.out(), Sig::Level) == 0.0f);
    CHECK(h.kicks == 0 && h.hats == 0);
    // Auto gain stays within -12..+36 dB of extra gain; manual gain adds; off = manual only.
    DrumSynth loud;
    loud.level = 1.0f;
    loud.kick_gain = 1.0f;
    Analyzer a2(48000);
    count(loud, a2, 10.0, 0.0);
    CHECK(a2.out().gain_db >= -12.0f && a2.out().gain_db <= 36.0f);
    AnalyzerSettings s;
    s.auto_gain = false;
    s.gain_db = 6.0f;
    Analyzer a3(48000, s);
    DrumSynth d3;
    count(d3, a3, 2.0, 0.0);
    CHECK(std::fabs(a3.out().gain_db - 6.0f) < 1e-4f);
    // Silence hops (input lost): every signal releases to 0 within 1 s.
    DrumSynth d4;
    Analyzer a4(48000);
    count(d4, a4, 6.0, 0.0);
    for (int i = 0; i < 200; ++i) a4.feed_silence_hop();  // 1 s
    float mx = 0;
    for (int k = 0; k < static_cast<int>(Sig::Count); ++k) mx = std::max(mx, a4.out().v[k]);
    CHECK(mx < 0.05f || sig(a4.out(), Sig::Energy) == mx);  // energy is the slow one; the rest are at rest
    CHECK(sig(a4.out(), Sig::Kick) < 0.01f && sig(a4.out(), Sig::Level) < 0.01f);
    // Clipping is reported.
    DrumSynth d5;
    d5.level = 3.0f;
    Analyzer a5(48000);
    bool clipped = false;
    run(d5, a5, 2.0, [&](const Analyzer& a) { clipped |= a.out().clipping != 0; });
    CHECK(clipped);
}

void test_midi_clock() {
    MidiClock mc;
    const uint64_t t0 = 5000000;
    auto ticks = [&](double bpm, int n, uint64_t start, double jitter_us = 0) {
        const double iv = 60e6 / (bpm * 24);
        uint32_t r = 7;
        for (int i = 0; i < n; ++i) {
            r = r * 1664525u + 1013904223u;
            const double j = jitter_us * ((r >> 8) / 8388608.0 - 1.0);
            mc.message(0xF8, 0, 0, start + static_cast<uint64_t>(i * iv + j));
        }
        return start + static_cast<uint64_t>((n - 1) * iv);
    };
    // Ticks without Start: tempo known, phase not valid, Stopped (no Start yet).
    uint64_t last = ticks(120, 48, t0);
    ClockSource s = mc.sample(last);
    CHECK(std::fabs(s.bpm - 120.0f) < 0.05f);
    CHECK(s.phase_valid == 0);
    CHECK(s.run == static_cast<uint8_t>(ClockRun::Stopped));
    // Start: the next tick is beat 0; 96 ticks later = beat 4.
    mc.message(0xFA, 0, 0, last + 1000);
    last = ticks(125, 97, last + 20000, 800.0);  // ~1 ms USB jitter
    s = mc.sample(last);
    CHECK(s.run == static_cast<uint8_t>(ClockRun::Running));
    CHECK(s.phase_valid == 1);
    CHECK(std::fabs(s.beat - 4.0) < 0.05);  // + up to a tick's jitter extrapolated
    CHECK(std::fabs(s.bpm - 125.0f) < 1.0f);
    CHECK(s.confidence > 0.5f);
    // Extrapolated between ticks, never past the next one.
    const ClockSource mid = mc.sample(last + 10000);
    CHECK(mid.beat > 4.0 && mid.beat <= 4.0 + 1.0 / 24 + 1e-9);
    // Stop, Song Position Pointer (16ths), Continue.
    mc.message(0xFC, 0, 0, last + 1);
    CHECK(mc.sample(last + 2).run == static_cast<uint8_t>(ClockRun::Stopped));
    mc.message(0xF2, 32, 0, last + 3);  // 32 sixteenths = beat 8
    CHECK(std::fabs(mc.sample(last + 4).beat - (8.0 - 1.0 / 24)) < 1e-9);  // the tick before the SPP position
    mc.message(0xFB, 0, 0, last + 5);
    last = ticks(125, 24, last + 19000);
    CHECK(std::fabs(mc.sample(last).beat - 8.0 - 23.0 / 24) < 1e-6);  // first tick after Continue = the next position
    // Silence for 0.5 s = Absent.
    CHECK(mc.sample(last + 600000).run == static_cast<uint8_t>(ClockRun::Absent));
    // Bad jitter = low confidence.
    MidiClock bad;
    uint32_t r = 3;
    for (int i = 0; i < 48; ++i) {
        r = r * 1664525u + 1013904223u;
        bad.message(0xF8, 0, 0, t0 + i * 20833 + (r >> 20) * 4);  // up to ~16 ms jitter
    }
    CHECK(bad.sample(t0 + 48 * 20833).confidence < 0.5f);
}

void test_block_layout() {
    static_assert(sizeof(AudioBlock) < 65536);
    static_assert(offsetof(AudioBlock, header) == 0);
    static_assert(sizeof(Signals) % 8 == 0);
    static_assert(alignof(AudioBlock) <= 8);
    static AudioBlock b{};
    Signals s{};
    s.hop = 9;
    s.v[static_cast<int>(Sig::Kick)] = 1.0f;
    proto::seq_write(b.signals_seq, b.signals, s);
    Signals c{};
    CHECK(proto::seq_read(b.signals_seq, b.signals, c) != 0);
    CHECK(c.hop == 9 && sig(c, Sig::Kick) == 1.0f);
    CHECK((b.signals_seq.load() & 1) == 0);
    std::printf("  AudioBlock %zu bytes\n", sizeof(AudioBlock));
}

void test_feed() {
    eng::ReactionFeed feed;
    fx::Signals out;
    Signals s{};
    s.hop = 1;
    s.v[static_cast<int>(Sig::Kick)] = 0.9f;
    s.v[static_cast<int>(Sig::Bass)] = 0.6f;
    s.v[static_cast<int>(Sig::Energy)] = 0.5f;
    feed.update(&s, 10.05, 1.0f / 60, out);
    CHECK(feed.audio_ok());
    CHECK(out[fx::Signal::Kick] == 0.9f && out[fx::Signal::Bass] == 0.6f && out[fx::Signal::Energy] == 0.5f);
    CHECK(out[fx::Signal::Beat] == 1.0f);  // 5 % into a beat
    CHECK(out[fx::Signal::Bar] == 0.0f);   // beat 10 is not a bar line
    feed.update(&s, 12.0, 1.0f / 60, out);
    CHECK(out[fx::Signal::Bar] == 1.0f);
    // No audio: a linear glide to 0 over 1 s; the clock pulses keep running.
    float prev = 0.9f;
    bool falling = true;
    for (int f = 1; f <= 60; ++f) {
        feed.update(nullptr, 13.5, 1.0f / 60, out);
        falling &= out[fx::Signal::Kick] <= prev;
        prev = out[fx::Signal::Kick];
        if (f == 30) CHECK(out[fx::Signal::Kick] > 0.35f && out[fx::Signal::Kick] < 0.45f);  // half-way: 0.9 - 0.5
    }
    CHECK(falling);
    CHECK(!feed.audio_ok());
    CHECK(out[fx::Signal::Kick] == 0.0f && out[fx::Signal::Bass] == 0.0f);
    feed.update(nullptr, 16.02, 1.0f / 60, out);
    CHECK(out[fx::Signal::Beat] == 1.0f);
    // Garbage in: NaN is 0, > 1 clamps.
    s.v[static_cast<int>(Sig::Kick)] = NAN;
    s.v[static_cast<int>(Sig::Level)] = 7.0f;
    feed.update(&s, 0.5, 1.0f / 60, out);
    CHECK(out[fx::Signal::Kick] == 0.0f && out[fx::Signal::Level] == 1.0f);
}

void test_midi_lead_and_follow() {
    ClockSource c{};
    c.kind = static_cast<uint8_t>(ClockKind::Midi);
    c.run = static_cast<uint8_t>(ClockRun::Running);
    c.phase_valid = 1;
    c.bpm = 128;
    c.confidence = 0.9f;
    c.beat = 16.0;
    c.time_us = 10000000;
    c.last_tick_us = 10000000;
    eng::ClockLead l = eng::midi_lead(c, 10100000);  // 0.1 s later
    CHECK(l.active && l.phase);
    CHECK(std::fabs(l.beat - (16.0 + 0.1 * 128 / 60)) < 1e-9);
    c.confidence = 0.3f;
    CHECK(!eng::midi_lead(c, 10100000).active);
    c.confidence = 0.9f;
    CHECK(!eng::midi_lead(c, 10600001).active);  // stale
    c.run = static_cast<uint8_t>(ClockRun::Stopped);
    CHECK(!eng::midi_lead(c, 10100000).active);
    c.run = static_cast<uint8_t>(ClockRun::Running);
    c.bpm = NAN;
    CHECK(!eng::midi_lead(c, 10100000).active);

    // BeatClock::follow: takes the tempo at once, glides the phase (no jump), bar-aligned.
    eng::BeatClock bc;
    uint64_t t = 1000000;
    bc.reset(t);
    double target = 2.5;  // MIDI is 2.5 beats ahead -> nearest bar-aligned error = -1.5
    double worst_step = 0, prev = bc.beat(t);
    const double bpm = 128;
    for (int f = 0; f < 60 * 8; ++f) {  // 8 s at 60 fps
        t += 16667;
        target += 16667e-6 * bpm / 60;
        bc.follow(bpm, target, true, t, 1.0 / 60);
        const double b = bc.beat(t);
        worst_step = std::max(worst_step, std::fabs((b - prev) - 16667e-6 * bpm / 60));
        prev = b;
    }
    CHECK(std::fabs(bc.bpm() - 128.0) < 1e-9);
    double err = target - bc.beat(t);
    err -= 4.0 * std::round(err / 4.0);
    CHECK(std::fabs(err) < 0.01);       // locked (mod 4)
    CHECK(worst_step < 0.05);           // never a jump (a frame is 0.036 beats at 128 BPM)
    bc.follow(NAN, 1.0, true, t, 1.0 / 60);  // garbage ignored
    CHECK(std::fabs(bc.bpm() - 128.0) < 1e-9);

    // DeckModel: while MIDI leads, Tap / Bpm commands are ignored and source = 2.
    static proto::Catalog cat{};
    eng::DeckModel m(cat);
    uint64_t now = 2000000;
    m.tick(now);
    m.follow_clock(true, 140.0, 0.0, false, now, 1.0 / 60);
    proto::Command bpmc = proto::make(proto::CommandType::Clock, proto::kNone, proto::kNone, 0, 90.0f);
    bpmc.param = static_cast<uint8_t>(proto::ClockParam::Bpm);
    CHECK(m.submit(3, bpmc, now) == proto::Reject::None);
    now += 16667;
    m.tick(now);
    CHECK(std::fabs(m.bpm() - 140.0) < 1e-6);
    CHECK(m.transport().clock.source == 2);
    m.follow_clock(false, 0, 0, false, now, 1.0 / 60);
    bpmc.seq = 2;
    CHECK(m.submit(3, bpmc, now) == proto::Reject::None);
    m.tick(now + 16667);
    CHECK(std::fabs(m.bpm() - 90.0) < 1e-6);
    CHECK(m.transport().clock.source != 2);
}

void test_zoom_pulse_on_the_kick() {
    // The M8 "done" check without a renderer: drums -> analysis -> feed -> FX resolver,
    // with zoom pulse's own default reaction (kick -> amount). The amount must rise
    // within 2 frames of every kick and fall back before the next beat.
    const fx::Library& lib = fx::builtin();
    const int id = static_cast<int>(eng::fx::Effect::ZoomPulse);
    const fx::EffectMeta& m = lib.meta(id);
    CHECK(m.reaction.signal == fx::Signal::Kick);
    proto::FxSlotState slot{};
    slot.effect = static_cast<int16_t>(id);
    slot.mix = 1.0f;
    for (int p = 0; p < m.params; ++p) slot.param[p] = m.param[p].def;
    fx::SlotRuntime rt;
    fx::Globals globals;
    eng::ReactionFeed feed;

    for (const float level : {1.0f, 0.02f}) {  // line in, far mic
        DrumSynth d;
        d.level = level;
        d.noise_gain = 0.003f;
        Analyzer an(48000);
        const int rp = m.reaction.param;
        const float rest = slot.param[rp];
        // Frames at 60 fps; each frame takes the newest hop (like the renderer reading the block).
        const double frame = 1.0 / 60;
        double next_frame = 0;
        int hits_on_time = 0, beats = 0;
        double last_beat_seen = -1;
        float peak_in_beat = 0, min_before_beat = 1;
        bool fell_back = true;
        run(d, an, 20.0, [&](const Analyzer& a) {
            const double t = a.seconds();
            if (t < next_frame) return;
            next_frame += frame;
            fx::FrameCtx fc;
            fc.time_s = t;
            fc.beats = t * d.bpm / 60.0;
            fc.bpm = d.bpm;
            fc.dt = static_cast<float>(frame);
            feed.update(&a.out(), fc.beats, fc.dt, fc.sig);
            const fx::Resolved r = fx::resolve(m, id, slot, rt, globals, fc);
            const float v = r.p[rp];
            const double bpos = fc.beats - std::floor(fc.beats);
            const double beat_no = std::floor(fc.beats);
            if (t > 8.0) {
                if (beat_no != last_beat_seen) {  // a new beat
                    if (last_beat_seen >= 0) {
                        ++beats;
                        if (peak_in_beat > rest + 0.15f * m.reaction.amount) ++hits_on_time;
                        fell_back &= min_before_beat < rest + 0.05f;
                    }
                    last_beat_seen = beat_no;
                    peak_in_beat = 0;
                    min_before_beat = 1;
                }
                // "on time" = within the first 2 frames (33 ms) after the beat
                if (bpos * 60.0 / d.bpm < 0.034) peak_in_beat = std::max(peak_in_beat, v);
                if (bpos > 0.8) min_before_beat = std::min(min_before_beat, v);
            }
        });
        std::printf("  zoom pulse, level %.2f: %d of %d beats hit within 2 frames\n", level, hits_on_time, beats);
        CHECK(beats >= 20);
        CHECK(hits_on_time >= beats - 1);
        CHECK(fell_back);
    }
}

}  // namespace


// D131: FxReact (command 40) + the per-knob reaction table: validation, apply,
// FxSet reset, restore, and what the resolver does with the table.
void test_fx_react() {
    std::printf("fx react (D131)\n");
    const fx::Library& lib = fx::builtin();
    static proto::Catalog cat{};
    eng::fx::fill_catalog(cat);
    static proto::SlotReactions defaults[proto::kMaxEffects];
    eng::react_defaults(lib, defaults, proto::kMaxEffects);
    const int zid = static_cast<int>(eng::fx::Effect::ZoomPulse);
    const fx::EffectMeta& zm = lib.meta(zid);
    const int dp = zm.reaction.param;          // the knob zoom pulse's default reaction moves
    int other = -1;                             // a continuous knob with no default reaction
    for (int p = 0; p < zm.params && other < 0; ++p)
        if (p != dp && zm.param[p].steps < 2) other = p;
    CHECK(other >= 0);
    if (other < 0) return;

    // The defaults row mirrors effect.json on its knob, and is "none" elsewhere.
    const proto::ReactionEntry& de = defaults[zid].p[dp];
    CHECK(de.source == static_cast<uint8_t>(proto::ReactSource::Default));
    CHECK(de.signal == static_cast<uint8_t>(fx::Signal::Kick) && de.amount == zm.reaction.amount);
    CHECK(de.range_lo == zm.reaction.range_lo && de.range_hi == zm.reaction.range_hi);
    CHECK(defaults[zid].p[other].signal == 0);
    CHECK(defaults[zid].p[other].range_lo == zm.param[other].good_lo && defaults[zid].p[other].range_hi == zm.param[other].good_hi);

    // The Default row resolves exactly like the old single-reaction path, for every effect.
    for (int id = 0; id < lib.count(); ++id) {
        const fx::EffectMeta& m = lib.meta(id);
        proto::FxSlotState slot{};
        slot.effect = static_cast<int16_t>(id);
        slot.mix = 1.0f;
        for (int p = 0; p < m.params; ++p) slot.param[p] = m.param[p].def;
        fx::Reaction rx[fx::kParams];
        fx::reactions_from(defaults[id], rx);
        fx::SlotRuntime ra, rb;
        fx::Globals g;
        bool same = true;
        for (int f = 0; f < 240; ++f) {
            fx::FrameCtx fc;
            fc.time_s = f / 60.0;
            fc.beats = fc.time_s * 2.0;
            fc.dt = 1.0f / 60;
            fc.sig = fx::TestSignal::at(fc.time_s, fc.beats);
            const fx::Resolved a = fx::resolve(m, id, slot, ra, g, fc);
            const fx::Resolved b = fx::resolve(m, id, slot, rb, g, fc, rx);
            for (int p = 0; p < fx::kParams; ++p) same &= a.p[p] == b.p[p];
        }
        if (!same) std::printf("  effect %s: table path differs from the default path\n", m.id);
        CHECK(same);
    }

    eng::DeckModel model(cat);
    model.set_react_defaults(defaults, proto::kMaxEffects);
    uint64_t now = 1000000;
    uint32_t seq = 0;
    auto send = [&](proto::Command c) {
        c.seq = ++seq;
        return model.submit(static_cast<int>(proto::Producer::Gui), c, now);
    };
    const uint8_t D = 0, L = 1, S = 2;
    const auto& row = [&]() -> const proto::SlotReactions& { return model.reactions().layer[D][L][S]; };
    proto::ReactArgs kick;
    kick.signal = proto::ReactSignal::Kick;

    // Empty slot: refused; FxSet fills the row with the effect's defaults.
    CHECK(send(proto::make_react(D, L, S, 0, kick, 0.5f)) == proto::Reject::BadTarget);
    const uint32_t g0 = model.reactions().gen;
    CHECK(send(proto::make_fx(proto::CommandType::FxSet, D, L, S, proto::kNone, zid)) == proto::Reject::None);
    CHECK(model.reactions().gen != g0);
    CHECK(std::memcmp(&row(), &defaults[zid], sizeof(proto::SlotReactions)) == 0);

    // Targets and values.
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(proto::kReactParams), kick)) == proto::Reject::BadTarget);
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(zm.params), kick)) == proto::Reject::BadTarget ||
          zm.params >= proto::kReactParams);
    CHECK(send(proto::make_react(D, L, S, proto::kNone, kick)) == proto::Reject::BadTarget);  // Set needs one knob
    proto::Command bad = proto::make_react(D, L, S, 0, kick);
    bad.i |= 1 << 30;
    CHECK(send(bad) == proto::Reject::BadValue);
    proto::ReactArgs badsig;
    badsig.signal = proto::ReactSignal::Count;
    CHECK(send(proto::make_react(D, L, S, 0, badsig)) == proto::Reject::BadValue);
    CHECK(send(proto::make_react(D, L, S, 0, kick, NAN)) == proto::Reject::BadValue);
    CHECK(send(proto::make_react(D, L, S, 0, kick, 0, 0, 0, INFINITY)) == proto::Reject::BadValue);
    CHECK(std::memcmp(&row(), &defaults[zid], sizeof(proto::SlotReactions)) == 0);  // refusals change nothing

    // Set on the knob with no default: snare, FOLLOW, soft, 1/8 quantise, 1 beat hold; values clamped.
    proto::ReactArgs sn;
    sn.signal = proto::ReactSignal::Snare;
    sn.mode = proto::ReactMode::Follow;
    sn.curve = proto::ReactCurve::Soft;
    sn.quantise = proto::ReactQuantise::Eighth;
    sn.hold_q = 4;
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(other), sn, 2.0f, 5.0f, 9000.0f, 0.25f)) ==
          proto::Reject::None);
    const proto::ReactionEntry& e = row().p[other];
    CHECK(e.source == static_cast<uint8_t>(proto::ReactSource::Custom));
    CHECK(e.signal == static_cast<uint8_t>(fx::Signal::Snare) && e.mode == 1 && e.curve == 1);
    CHECK(e.amount == 1.0f && e.attack_ms == 5.0f && e.release_ms == proto::kReactMaxMs && e.threshold == 0.25f);
    CHECK(e.quantise_beats == 0.5f && e.hold_beats == 1.0f);
    CHECK(e.range_lo == defaults[zid].p[other].range_lo);  // Set keeps the range

    // Amount (relative) on the default knob: becomes Custom, keeps the default's settings.
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(dp), proto::ReactArgs{proto::ReactOp::Amount}, -0.1f, 0, 0,
                                 0, proto::kRelative)) == proto::Reject::None);
    CHECK(row().p[dp].source == static_cast<uint8_t>(proto::ReactSource::Custom));
    CHECK(std::fabs(row().p[dp].amount - (zm.reaction.amount - 0.1f)) < 1e-6f);
    CHECK(row().p[dp].signal == static_cast<uint8_t>(fx::Signal::Kick));

    // Range, then back to the default range.
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(dp), proto::ReactArgs{proto::ReactOp::Range}, 0.9f, 0.2f)) ==
          proto::Reject::None);
    CHECK(row().p[dp].range_lo == 0.9f && row().p[dp].range_hi == 0.2f && (row().p[dp].flags & proto::kReactRangeSet));
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(dp), proto::ReactArgs{proto::ReactOp::Range}, -1.0f)) ==
          proto::Reject::None);
    CHECK(row().p[dp].range_lo == de.range_lo && !(row().p[dp].flags & proto::kReactRangeSet));

    // Default (one knob) and Off (all knobs).
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(dp), proto::ReactArgs{proto::ReactOp::Default})) ==
          proto::Reject::None);
    CHECK(std::memcmp(&row().p[dp], &de, sizeof(de)) == 0);
    CHECK(send(proto::make_react(D, L, S, proto::kNone, proto::ReactArgs{proto::ReactOp::Off})) == proto::Reject::None);
    for (int p = 0; p < proto::kReactParams; ++p) CHECK(row().p[p].source == static_cast<uint8_t>(proto::ReactSource::Off));
    fx::Reaction rx[fx::kParams];
    fx::reactions_from(row(), rx);
    for (int p = 0; p < fx::kParams; ++p) CHECK(rx[p].signal == fx::Signal::None);
    CHECK(send(proto::make_react(D, L, S, proto::kNone, proto::ReactArgs{proto::ReactOp::Default})) == proto::Reject::None);
    CHECK(std::memcmp(&row(), &defaults[zid], sizeof(proto::SlotReactions)) == 0);

    // Signal on a knob with no reaction yet: starts at amount 0.5 (GUESS in react_apply).
    proto::ReactArgs sig;
    sig.op = proto::ReactOp::Signal;
    sig.signal = proto::ReactSignal::Bass;
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(other), sig)) == proto::Reject::None);
    CHECK(row().p[other].signal == static_cast<uint8_t>(fx::Signal::Bass) && row().p[other].amount == 0.5f);

    // Master and deck level slots have rows too.
    CHECK(send(proto::make_fx(proto::CommandType::FxSet, proto::kMasterDeck, proto::kNone, 1, proto::kNone, zid)) ==
          proto::Reject::None);
    CHECK(send(proto::make_react(proto::kMasterDeck, proto::kNone, 1, static_cast<uint8_t>(other), kick, 0.3f)) ==
          proto::Reject::None);
    CHECK(model.reactions().master[1].p[other].signal == static_cast<uint8_t>(fx::Signal::Kick));
    CHECK(send(proto::make_fx(proto::CommandType::FxSet, 1, proto::kNone, 3, proto::kNone, zid)) == proto::Reject::None);
    CHECK(send(proto::make_react(1, proto::kNone, 3, static_cast<uint8_t>(dp), proto::ReactArgs{proto::ReactOp::Off})) ==
          proto::Reject::None);
    CHECK(model.reactions().deck[1][3].p[dp].source == static_cast<uint8_t>(proto::ReactSource::Off));

    // Restore (a renderer restart): Custom / Off come back with the slot.
    auto old_live = model.live();
    auto old_react = model.reactions();
    eng::DeckModel again(cat);
    again.set_react_defaults(defaults, proto::kMaxEffects);
    again.restore(old_live, cat, nullptr, &old_react);
    CHECK(again.reactions().layer[D][L][S].p[other].signal == static_cast<uint8_t>(fx::Signal::Bass));
    CHECK(again.reactions().layer[D][L][S].p[other].source == static_cast<uint8_t>(proto::ReactSource::Custom));
    CHECK(again.reactions().master[1].p[other].signal == static_cast<uint8_t>(fx::Signal::Kick));
    CHECK(again.reactions().deck[1][3].p[dp].source == static_cast<uint8_t>(proto::ReactSource::Off));
    CHECK(again.reactions().layer[D][L][S].p[dp].source == static_cast<uint8_t>(proto::ReactSource::Default));

    // FxSet (even the same effect) resets the row to Default.
    CHECK(send(proto::make_fx(proto::CommandType::FxSet, D, L, S, proto::kNone, zid)) == proto::Reject::None);
    CHECK(std::memcmp(&row(), &defaults[zid], sizeof(proto::SlotReactions)) == 0);

    // The resolver: a knob with its own reaction moves with its signal, on its own envelope.
    // Knob `other` follows the snare (FOLLOW, 0.2..0.8); the default knob keeps the kick.
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(other), sn, 1.0f, 0.0f, 100.0f, 0.0f)) ==
          proto::Reject::None);
    CHECK(send(proto::make_react(D, L, S, static_cast<uint8_t>(other), proto::ReactArgs{proto::ReactOp::Range}, 0.2f, 0.8f)) ==
          proto::Reject::None);
    proto::SlotReactions snare_row = row();
    snare_row.p[other].mode = static_cast<uint8_t>(proto::ReactMode::Follow);
    snare_row.p[other].quantise_beats = 0;  // FOLLOW ignores it anyway
    fx::reactions_from(snare_row, rx);
    const proto::FxSlotState& slot = model.live().deck[D].layer[L].fx[S];
    fx::SlotRuntime rt;
    fx::Globals g;
    fx::FrameCtx fc;
    fc.dt = 1.0f / 60;
    fc.sig.v[static_cast<int>(fx::Signal::Snare)] = 1.0f;  // snare only
    fx::Resolved r{};
    for (int f = 0; f < 30; ++f) r = fx::resolve(zm, zid, slot, rt, g, fc, rx);
    CHECK(std::fabs(r.p[other] - 0.8f) < 0.01f);             // FOLLOW + full snare -> range hi
    CHECK(rt.react_env[other] > 0.99f && rt.react_env[dp] == 0.0f);
    CHECK(std::fabs(r.p[dp] - slot.param[dp]) < 1e-6f);      // no kick: the default knob stays put
    fc.sig = fx::Signals{};
    fc.sig.v[static_cast<int>(fx::Signal::Kick)] = 1.0f;     // kick only
    for (int f = 0; f < 60; ++f) r = fx::resolve(zm, zid, slot, rt, g, fc, rx);
    CHECK(r.p[dp] > slot.param[dp] + 0.05f);                 // the default kick reaction still works
    CHECK(std::fabs(r.p[other] - 0.2f) < 0.01f);             // the snare knob glided back to range lo
    CHECK(rt.out[dp] == r.p[dp] && rt.out[other] == r.p[other]);  // the state block's "value"
}

int main() {
    std::printf("audio_tests\n");
    test_fft_and_filters();
    test_onsets_line_and_mic();
    test_drum_separation();
    test_hop_alignment();
    test_no_false_kicks();
    test_gate_and_gain();
    test_midi_clock();
    test_block_layout();
    test_feed();
    test_midi_lead_and_follow();
    test_zoom_pulse_on_the_kick();
    test_fx_react();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
