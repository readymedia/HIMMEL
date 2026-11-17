// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// audio_probe.exe - the renderer's music path without a renderer: maps
// Local\HimmelAudio with the engine's own AudioLink, feeds ReactionFeed and
// resolves zoom pulse (its default kick reaction) at 60 Hz, like himmel_render.
//
//   audio_probe [--seconds S] [--quiet]
//
// Prints one line per kick (age of the signal when the "frame" saw it, the zoom
// amount) and a summary: kicks seen, median / worst signal age, how long the
// signals took to rest after audio stopped. Safe next to a running Himmel stack
// (it only reads the block).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "himmel/engine/fx_catalog.h"
#include "himmel/engine/reactions.h"
#include "himmel/fx/fx.h"

namespace {
uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 20;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--quiet") quiet = true;
    }
    timeBeginPeriod(1);
    himmel::engine::AudioLink link;
    if (!link.open()) {
        std::printf("cannot map the audio block\n");
        return 2;
    }
    const himmel::fx::Library& lib = himmel::fx::builtin();
    const int id = static_cast<int>(himmel::engine::fx::Effect::ZoomPulse);
    const himmel::fx::EffectMeta& m = lib.meta(id);
    himmel::protocol::FxSlotState slot{};
    slot.effect = static_cast<int16_t>(id);
    slot.mix = 1.0f;
    for (int p = 0; p < m.params; ++p) slot.param[p] = m.param[p].def;
    himmel::fx::SlotRuntime rt;
    himmel::fx::Globals globals;
    himmel::engine::ReactionFeed feed;
    himmel::audio::Signals sig{};
    himmel::audio::ClockSource midi{};

    const uint64_t t0 = now_us(), end = t0 + static_cast<uint64_t>(seconds * 1e6);
    uint64_t last = t0;
    uint32_t kicks_seen = 0, last_kicks = 0;
    bool first = true, was_ok = false;
    std::vector<double> ages;
    double rest_start = -1, rested_after = -1;
    const float rest_v = slot.param[m.reaction.param];
    while (now_us() < end) {
        const uint64_t t = now_us();
        const float dt = static_cast<float>((t - last) / 1e6);
        last = t;
        const bool ok = link.read(t, sig, midi);
        if (ok != was_ok) {
            std::printf("%7.2f s  audio %s%s%s\n", (t - t0) / 1e6, ok ? "OK: " : "gone - gliding to rest",
                        ok ? link.input().device : "", "");
            if (!ok) rest_start = (t - t0) / 1e6, rested_after = -1;
            was_ok = ok;
        }
        himmel::fx::FrameCtx fc;
        fc.time_s = (t - t0) / 1e6;
        fc.beats = fc.time_s * 2.0;
        fc.dt = dt;
        feed.update(ok ? &sig : nullptr, fc.beats, dt, fc.sig);
        const himmel::fx::Resolved r = himmel::fx::resolve(m, id, slot, rt, globals, fc);
        const float zoom = r.p[m.reaction.param];
        if (!ok && rest_start >= 0 && rested_after < 0 && zoom <= rest_v + 0.001f) {
            rested_after = fc.time_s - rest_start;
            std::printf("%7.2f s  at rest after %.2f s\n", fc.time_s, rested_after);
        }
        if (ok) {
            if (first) last_kicks = sig.kicks, first = false;
            if (sig.kicks < last_kicks) last_kicks = 0;  // himmel_audio restarted: its counters start again
            if (sig.kicks != last_kicks) {
                kicks_seen += sig.kicks - last_kicks;
                last_kicks = sig.kicks;
                const double age_ms = (static_cast<double>(t) - static_cast<double>(sig.time_us)) / 1000.0;
                ages.push_back(age_ms);
                if (!quiet)
                    std::printf("%7.2f s  KICK  signal age %5.1f ms  kick %.2f  zoom amount %.3f (rest %.3f)\n", fc.time_s, age_ms,
                                fc.sig[himmel::fx::Signal::Kick], zoom, rest_v);
            }
        }
        Sleep(16);  // ~60 Hz frames
    }
    std::sort(ages.begin(), ages.end());
    std::printf("summary: %u kicks seen; signal age median %.1f ms, worst %.1f ms%s\n", kicks_seen,
                ages.empty() ? 0.0 : ages[ages.size() / 2], ages.empty() ? 0.0 : ages.back(),
                rested_after >= 0 ? "" : "");
    if (rested_after >= 0) std::printf("summary: rest %.2f s after the audio went away\n", rested_after);
    timeEndPeriod(1);
    return 0;
}
