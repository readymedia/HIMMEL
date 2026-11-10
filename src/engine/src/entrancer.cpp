// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/engine/entrancer.h"

#include <algorithm>
#include <array>

namespace himmel::engine::entrancer {

namespace {

using E = touchfx::Effect;
constexpr uint8_t e(E v) { return static_cast<uint8_t>(v); }
constexpr Program mix(E v, float fy = 0.5f) { return {e(v), true, fy}; }  // X = effect, Y = how much
constexpr Program xy(E v) { return {e(v), false, 0.5f}; }                  // X and Y both move the effect

// The unit's 100 video programs (Korg's Effect Program List, see the design
// note) played with the closest of Himmel's own effects. Only 9 effects exist
// yet, so many programs share one; the comment is the unit's program kind.
constexpr std::array<Program, kPrograms> kTable = {{
    // 00-13 colour / tone
    mix(E::HueTurn, 0.0f), mix(E::HueTurn, 1.0f), xy(E::Duotone), mix(E::HueTurn, 1.0f),   // 00-03
    mix(E::Waves, 0.6f), mix(E::Duotone, 0.12f), xy(E::Duotone), xy(E::Duotone),          // 04-07
    mix(E::Duotone, 0.0f), mix(E::Duotone, 1.0f), mix(E::HueTurn, 0.8f), mix(E::HueTurn, 0.9f),  // 08-11
    mix(E::HueTurn, 0.5f), mix(E::HueTurn, 0.5f),                                          // 12-13
    // 14-26 tiling (24-26 mirror tiles)
    mix(E::Grid, 0.0f), mix(E::Grid, 0.0f), mix(E::Grid, 0.0f), mix(E::Grid, 0.0f),        // 14-17
    mix(E::Grid, 0.0f), mix(E::Grid, 1.0f), mix(E::Grid, 1.0f), mix(E::Grid, 0.0f),        // 18-21
    mix(E::Grid, 0.0f), mix(E::Grid, 0.0f), mix(E::Grid, 1.0f), mix(E::Grid, 1.0f),        // 22-25
    mix(E::Grid, 1.0f),                                                                    // 26
    // 27-37 geometry and time
    xy(E::Bulge), xy(E::Slide), mix(E::Strobe, 0.0f), mix(E::Trails, 0.5f),                // 27-30
    mix(E::Trails, 0.5f), mix(E::Trails, 0.55f), mix(E::Trails, 0.6f), xy(E::Slide),       // 31-34
    mix(E::Slide, 0.5f), xy(E::Kaleido), mix(E::Kaleido, 0.0f),                            // 35-37
    // 38-53 stylise / break-up
    mix(E::Trails, 0.7f), mix(E::Grid, 0.0f), mix(E::Bulge, 0.5f), mix(E::Bulge, 0.5f),    // 38-41
    xy(E::Slide), mix(E::Grid, 0.0f), mix(E::Grid, 0.0f), xy(E::Waves),                    // 42-45
    mix(E::Grid, 1.0f), xy(E::Waves), mix(E::Waves, 0.8f), mix(E::Waves, 0.5f),            // 46-49
    mix(E::Strobe, 0.0f), mix(E::Strobe, 1.0f), xy(E::Slide), xy(E::Slide),                // 50-53
    // 54-60 tempo-synced
    mix(E::Strobe, 0.5f), mix(E::Grid, 0.0f), mix(E::Bulge, 0.5f), mix(E::Grid, 1.0f),     // 54-57
    mix(E::Strobe, 1.0f), mix(E::Strobe, 0.0f), mix(E::Trails, 0.5f),                      // 58-60
    // 61-75 flip / rotate / zoom
    mix(E::Kaleido, 0.0f), mix(E::Kaleido, 0.25f), mix(E::Kaleido, 0.75f), mix(E::Kaleido, 0.5f),  // 61-64
    mix(E::Kaleido, 0.25f), mix(E::Trails, 0.7f), mix(E::Bulge, 0.5f), xy(E::Bulge),       // 65-68
    mix(E::Bulge, 0.5f), xy(E::Bulge), mix(E::Waves, 0.6f), xy(E::Waves),                  // 69-72
    mix(E::Bulge, 0.5f), mix(E::Trails, 0.8f), mix(E::Trails, 0.9f),                       // 73-75
    // 76-93 split / slip / wave
    mix(E::Grid, 0.0f), mix(E::Grid, 0.0f), mix(E::Grid, 1.0f), mix(E::Grid, 1.0f),        // 76-79
    mix(E::Grid, 1.0f), mix(E::Grid, 0.0f), mix(E::Trails, 0.5f), mix(E::Trails, 0.5f),    // 80-83
    mix(E::Trails, 0.5f), mix(E::Trails, 0.5f), xy(E::Kaleido), xy(E::Slide),              // 84-87
    xy(E::Slide), xy(E::Bulge), xy(E::Waves), xy(E::Waves),                                // 88-91
    xy(E::Waves), mix(E::Waves, 0.5f),                                                     // 92-93
    // 94-99 sample / play (no sampler in Himmel yet: time effects)
    mix(E::Trails, 0.5f), mix(E::Trails, 0.5f), mix(E::Trails, 0.5f), mix(E::Strobe, 0.5f),  // 94-97
    mix(E::Trails, 0.5f), mix(E::Trails, 0.5f),                                            // 98-99
}};

}  // namespace

const Program& program(int n) { return kTable[static_cast<size_t>(std::clamp(n, 0, kPrograms - 1))]; }

void Mapper::feed(const uint8_t* data, size_t n, touchfx::TouchFx& fx) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (b >= 0xF8) continue;            // real-time: clock, start/stop, active sensing
        if (b & 0x80) {
            status_ = b >= 0xF0 ? 0 : b;    // system common / SysEx cancel running status
            have_ = 0;
            continue;
        }
        if (!status_) continue;             // data with no status (or inside SysEx)
        const uint8_t kind = status_ & 0xF0;
        const int need = kind == 0xC0 || kind == 0xD0 ? 1 : 2;
        data_[have_++] = b;
        if (have_ == need) {
            message(status_, data_[0], need == 2 ? data_[1] : 0, fx);
            have_ = 0;                      // running status: the next data starts a new message
        }
    }
}

void Mapper::push(touchfx::TouchFx& fx) {
    const Program& p = program(st_.program);
    const float y = p.y_is_mix ? p.fixed_y : st_.y;
    fx.set_depth(st_.fx_balance * (p.y_is_mix ? st_.y : 1.0f));
    if (st_.touch && !st_.video_off) fx.touch_move(touchfx::Source::Entrancer, st_.x, y, st_.pressure);
}

void Mapper::message(uint8_t status, uint8_t d1, uint8_t d2, touchfx::TouchFx& fx) {
    const uint8_t kind = status & 0xF0;
    if (channel_ && (status & 0x0F) + 1 != channel_) return;  // another device on the same cable
    ++st_.messages;
    if (kind == 0xC0) {                     // program change
        st_.program = std::min<int>(d1, kPrograms - 1);
        fx.select(program(st_.program).effect);
        if (!st_.touch) {                   // the effect's own start position, then ours
            const Program& p = program(st_.program);
            if (p.y_is_mix) fx.set_xy(st_.x, p.fixed_y);
        }
        push(fx);
        return;
    }
    if (kind != 0xB0) return;               // notes, pitch bend: not used by the unit's pad
    const float v = d2 / 127.0f;
    switch (d1) {
        case kCcX: st_.x = v; push(fx); break;
        case kCcY: st_.y = v; push(fx); break;
        case kCcPressure: st_.pressure = v; push(fx); break;
        case kCcFxBalance: st_.fx_balance = v; push(fx); break;
        case kCcTouch: {
            const bool on = d2 >= 64;
            if (on == st_.touch) break;
            st_.touch = on;
            if (on && !st_.video_off) {
                const Program& p = program(st_.program);
                fx.touch_down(touchfx::Source::Entrancer, st_.x, p.y_is_mix ? p.fixed_y : st_.y, st_.pressure);
                push(fx);
            } else if (!on) {
                fx.touch_up(touchfx::Source::Entrancer);
            }
            break;
        }
        case kCcFreeze: st_.freeze = d2 >= 64; break;
        case kCcVideoOff:
            st_.video_off = d2 >= 64;
            if (st_.video_off) fx.touch_up(touchfx::Source::Entrancer);
            else if (st_.touch) fx.touch_down(touchfx::Source::Entrancer, st_.x, st_.y, st_.pressure), push(fx);
            break;
        case kCcBpmMsb: bpm_msb_ = d2; break;
        case kCcBpmLsb:
            if (bpm_msb_ >= 0) st_.bpm = std::clamp(bpm_msb_ * 128 + d2, 20, 400);
            bpm_msb_ = -1;
            break;
        default: break;
    }
}

void Mapper::lost(touchfx::TouchFx& fx) {
    st_.touch = false;
    st_.freeze = false;
    fx.source_lost(touchfx::Source::Entrancer);
}

}  // namespace himmel::engine::entrancer
