// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The stub signal source until the audio track (M8) delivers real signals.
//
// TestSignal makes a deterministic, music-like test signal from the show clock:
// a kick on every beat, a snare on 2 and 4, hats on the eighths, bass that
// follows the kick, slow mid / high / energy swells. It is what fx_check and
// fx_tests use, and what `himmel_render --test-signal` feeds the reactions.
// Without that flag the engine feeds silence (all zero), so the reactions are
// data only until M8 (build-v1.md, M5).
#pragma once

#include "himmel/fx/fx.h"

namespace himmel::fx {

struct TestSignal {
    // Fills every signal for the time / beat given. Pure function of its inputs.
    static Signals at(double time_s, double beats);
};

}  // namespace himmel::fx
