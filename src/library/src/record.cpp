// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/library/record.h"

#include <cmath>

namespace himmel::library {

namespace {
constexpr const char* kKinds[] = {"video", "still", "vector", "animation", "sequence"};
constexpr const char* kStates[] = {"queued", "probing", "converting", "analysing", "ready", "failed"};
}  // namespace

const char* kind_name(Kind k) { return kKinds[static_cast<int>(k)]; }
const char* state_name(State s) { return kStates[static_cast<int>(s)]; }

bool kind_from(const std::string& s, Kind& out) {
    for (int i = 0; i < 5; ++i)
        if (s == kKinds[i]) return out = static_cast<Kind>(i), true;
    return false;
}
bool state_from(const std::string& s, State& out) {
    for (int i = 0; i < 6; ++i)
        if (s == kStates[i]) return out = static_cast<State>(i), true;
    return false;
}

std::vector<int> judder_for(double fps) {
    std::vector<int> out;
    if (fps <= 1.0) return out;  // stills never judder
    for (int hz : {50, 60}) {
        const double ratio = hz / fps;
        // A whole number of refreshes per frame (or of frames per refresh) is smooth.
        const double r = ratio >= 1.0 ? ratio : 1.0 / ratio;
        if (std::fabs(r - std::round(r)) > 0.02) out.push_back(hz);
    }
    return out;
}

}  // namespace himmel::library
