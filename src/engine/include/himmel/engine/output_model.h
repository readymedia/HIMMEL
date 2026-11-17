// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The output model (M6): canvas geometry, output regions, edge blend, layer places,
// black per output, IDENTIFY and display matching by identity
// (design/engine.md §2.0, design/operations.md §1-3, design/projects.md §5,
// docs/interfaces/outputs-block.md).
//
// Pure: no heap after construction, no locks, no Windows. The render process owns
// one, DeckModel validates + applies the output commands (types 70..73) through it
// (so they share the rate limit and the ack), and the renderer reads the layout
// from it. Layout changes (preset, mode, blend) only set layout_dirty(): the
// renderer rebuilds the canvas at the next frame start (a setup action).
#pragma once

#include <cstdint>

#include "himmel/protocol/commands.h"
#include "himmel/protocol/outputs.h"

namespace himmel::engine {

struct OutputRegion {
    int x = 0, y = 0, w = 0, h = 0;   // canvas pixels
    float uv[4] = {0, 0, 1, 1};       // offset.xy, scale.zw in canvas uv
    float blend_left = 0, blend_right = 0;  // ramp widths, canvas pixels
};

struct OutputSetup {
    int count = 1;                                                    // 1..kMaxOutputs
    protocol::ResolutionPreset preset = protocol::ResolutionPreset::P1080;
    uint32_t custom_w = 640, custom_h = 360;                          // ResolutionPreset::Custom
    protocol::CanvasMode mode = protocol::CanvasMode::Span;
    uint32_t blend_px = 0;
    float blend_gamma = 2.2f;
    protocol::RefreshRate refresh = protocol::RefreshRate::Hz60;
    bool fullscreen = false;
};

class OutputModel {
public:
    static constexpr uint32_t kMaxBlendPx = 1024;
    static constexpr uint32_t kFadeUs = 500000;  // IDENTIFY fade in / out

    explicit OutputModel(const OutputSetup& s = OutputSetup{});

    // ---- commands (slot = the layer slot DeckModel resolved, -1 if none) ----
    protocol::Reject validate(const protocol::Command& c, int layer_slot) const;
    // `producer` = the slot that sent it (-1 = local, e.g. the M key): a held IDENTIFY
    // (i = 2) belongs to it, and is released when that producer goes quiet.
    void apply(const protocol::Command& c, int layer_slot, uint64_t now_us, int producer = -1);
    // A producer has had no heartbeat for 2 s: if it holds IDENTIFY, let go (fade out), so an
    // output never sticks in IDENTIFY because a GUI or a weapon died while holding it.
    void producer_quiet(int producer, uint64_t now_us);
    int identify_holder() const { return ident_holder_; }

    // ---- layout ----
    int count() const { return setup_.count; }
    const OutputSetup& setup() const { return setup_; }
    uint32_t canvas_w() const { return canvas_w_; }
    uint32_t canvas_h() const { return canvas_h_; }
    const OutputRegion& region(int o) const { return region_[o]; }
    bool layout_dirty() const { return layout_dirty_; }
    void clear_layout_dirty() { layout_dirty_ = false; }
    // Custom preset: the size the canvas parts get (windowed test runs follow --size).
    void set_custom_size(uint32_t w, uint32_t h);

    // ---- layer places ----
    // The shader's l_xform for a clip of cw x ch on this layer slot: canvas uv -> clip uv
    // (scale.xy, offset.zw: cu = (uv - 0.5) * scale + 0.5 + offset). Outside the place the
    // clip uv leaves 0..1 and the layer is transparent there.
    void layer_xform(int slot, float cw, float ch, float out[4]) const;
    void place_rect(int slot, float out[4]) const;  // x, y, w, h in canvas uv
    const protocol::LayerPlaceInfo& place(int slot) const { return place_[slot]; }
    uint32_t place_gen() const { return place_gen_; }  // bumps when a place or the canvas changes

    // ---- per output ----
    bool black(int o) const { return o >= 0 && o < protocol::kMaxOutputs && black_[o]; }
    bool sim_lost(int o) const { return o >= 0 && o < protocol::kMaxOutputs && sim_lost_[o]; }
    uint32_t adopt_gen(int o) const { return adopt_gen_[o]; }  // bumps on OutputConfig Adopt i = 1
    uint32_t refresh_gen() const { return refresh_gen_; }

    // ---- IDENTIFY ----
    float identify_alpha(uint64_t now_us) const;     // 0..1 with the fades
    bool identify_on(uint64_t now_us) const { return identify_alpha(now_us) > 0.0f; }
    uint32_t identify_ms_left(uint64_t now_us) const;

    // ---- state block ----
    // Writes the config fields (count, mode, canvas, blend, black, presets, places, identify).
    // The renderer adds the runtime fields (state, display ids, windows, Hz).
    void fill(protocol::OutputsSection& s, uint64_t now_us) const;
    // A renderer restart: black per output and the layer places always come back; the
    // canvas config (preset, mode, blend, refresh) only if it was changed by a command.
    void restore(const protocol::OutputsSection& old);
    bool config_by_command() const { return config_by_command_; }

private:
    void relayout();
    uint32_t preset_w(int o) const;
    uint32_t preset_h(int o) const;

    OutputSetup setup_;
    protocol::ResolutionPreset out_preset_[protocol::kMaxOutputs];
    uint32_t canvas_w_ = 0, canvas_h_ = 0;
    OutputRegion region_[protocol::kMaxOutputs];
    protocol::LayerPlaceInfo place_[protocol::kPlaceSlots];
    bool black_[protocol::kMaxOutputs] = {};
    bool sim_lost_[protocol::kMaxOutputs] = {};
    uint32_t adopt_gen_[protocol::kMaxOutputs] = {};
    uint32_t refresh_gen_ = 0;
    uint32_t place_gen_ = 1;
    bool layout_dirty_ = true;
    bool config_by_command_ = false;
    // IDENTIFY: on at on_us, fade-out starts at off_us (0 = held until Identify 0).
    uint64_t ident_on_us_ = 0, ident_off_us_ = 0;
    bool ident_active_ = false;
    int ident_holder_ = -1;  // the producer holding IDENTIFY (i = 2), -1 = none
};

// ---- display identity (operations.md §1) ------------------------------------
// Same display: EDID maker + product + serial (number or text) when it has one,
// on any connector; without a serial, maker + product on the same connector.
bool same_display(const protocol::DisplayId& a, const protocol::DisplayId& b);
bool id_known(const protocol::DisplayId& id);

// Slots -> displays. `slot_id[i]` is what slot i belongs to (unknown = take the next
// free display, left to right, in `display_order`). Result: `out_display[i]` = index
// into `displays` or -1, `out_other[i]` = true when slot i lost its display and some
// other display is free ("Use the new display as OUT n?"). Bounded, no heap.
void match_displays(const protocol::DisplayId* slot_id, int slots, const protocol::DisplayId* displays,
                    const int* display_order, int display_count, int* out_display, bool* out_other);

}  // namespace himmel::engine
