// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The output model (M6). See output_model.h.
#include "himmel/engine/output_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace himmel::engine {

using namespace himmel::protocol;

namespace {
bool finite(float f) { return std::isfinite(f); }
bool is_toggle(int32_t i) { return i >= 0 && i <= 2; }
template <class E>
bool enum_ok(int32_t i) { return i >= 0 && i < static_cast<int32_t>(E::Count); }
bool toggle(bool cur, int32_t i) { return i == kToggle ? !cur : i == kOn; }
}  // namespace

OutputModel::OutputModel(const OutputSetup& s) : setup_(s) {
    setup_.count = std::clamp(setup_.count, 1, kMaxOutputs);
    setup_.blend_px = std::min(setup_.blend_px, kMaxBlendPx);
    setup_.blend_gamma = std::clamp(setup_.blend_gamma, 1.0f, 3.0f);
    for (auto& p : out_preset_) p = setup_.preset;
    for (auto& p : place_) p = LayerPlaceInfo{static_cast<uint8_t>(Place::FillAll), 0, {0, 0}, {0, 0, 1, 1}};
    relayout();
}

void OutputModel::set_custom_size(uint32_t w, uint32_t h) {
    w = std::clamp<uint32_t>(w, 64, 8192), h = std::clamp<uint32_t>(h, 64, 8192);
    if (w == setup_.custom_w && h == setup_.custom_h) return;
    setup_.custom_w = w, setup_.custom_h = h;
    relayout();
}

uint32_t OutputModel::preset_w(int o) const {
    uint32_t w, h;
    preset_size(out_preset_[o], w, h);
    return w ? w : setup_.custom_w;
}
uint32_t OutputModel::preset_h(int o) const {
    uint32_t w, h;
    preset_size(out_preset_[o], w, h);
    return h ? h : setup_.custom_h;
}

// Span: side by side, neighbours overlap by blend_px (projectors); the canvas
// width = sum(w) - (n - 1) * overlap. Mirror: every output shows the whole canvas.
void OutputModel::relayout() {
    const int n = setup_.count;
    const bool mirror = setup_.mode == CanvasMode::Mirror;
    uint32_t total = 0, max_w = 0, max_h = 0, min_w = 1u << 30;
    for (int o = 0; o < n; ++o) {
        total += preset_w(o);
        max_w = std::max(max_w, preset_w(o));
        max_h = std::max(max_h, preset_h(o));
        min_w = std::min(min_w, preset_w(o));
    }
    // The overlap never eats more than half of the narrowest output.
    const uint32_t blend = mirror || n < 2 ? 0 : std::min(setup_.blend_px, min_w / 2);
    canvas_w_ = mirror ? max_w : total - (n - 1) * blend;
    canvas_h_ = max_h;
    int x = 0;
    for (int o = 0; o < kMaxOutputs; ++o) {
        OutputRegion& r = region_[o];
        r = OutputRegion{};
        if (o >= n) continue;
        if (mirror) {
            r.w = static_cast<int>(canvas_w_), r.h = static_cast<int>(canvas_h_);
        } else {
            r.x = x, r.w = static_cast<int>(preset_w(o)), r.h = static_cast<int>(preset_h(o));
            r.blend_left = o > 0 ? static_cast<float>(blend) : 0.0f;
            r.blend_right = o < n - 1 ? static_cast<float>(blend) : 0.0f;
            x += r.w - static_cast<int>(blend);
        }
        r.uv[0] = static_cast<float>(r.x) / canvas_w_;
        r.uv[1] = static_cast<float>(r.y) / canvas_h_;
        r.uv[2] = static_cast<float>(r.w) / canvas_w_;
        r.uv[3] = static_cast<float>(r.h) / canvas_h_;
    }
    for (int s = 0; s < kPlaceSlots; ++s) place_rect(s, place_[s].rect);
    layout_dirty_ = true;
    ++place_gen_;
}

void OutputModel::place_rect(int slot, float out[4]) const {
    const LayerPlaceInfo& p = place_[slot];
    switch (static_cast<Place>(p.place)) {
        case Place::Out:
            if (p.out < setup_.count) {
                std::memcpy(out, region_[p.out].uv, sizeof(float) * 4);
                return;
            }
            break;  // an output that is not there: the whole canvas
        case Place::Free:
            std::memcpy(out, p.rect, sizeof(float) * 4);
            return;
        default: break;
    }
    out[0] = 0, out[1] = 0, out[2] = 1, out[3] = 1;
}

// Canvas uv -> clip uv per axis: cu = uv * A + B. In the place rect R the local
// coordinate is (uv - R.x) / R.w; cover (FILL ALL / OUT n / FREE) crops the clip to
// the rect's aspect, FIT shows all of it (the rest of the rect is transparent).
void OutputModel::layer_xform(int slot, float cw, float ch, float out[4]) const {
    float r[4];
    place_rect(slot, r);
    const float rw = std::max(r[2], 1e-4f), rh = std::max(r[3], 1e-4f);
    const float rect_aspect = rw * canvas_w_ / (rh * canvas_h_);
    const float clip_aspect = cw > 0 && ch > 0 ? cw / ch : rect_aspect;
    const float k = rect_aspect / clip_aspect;  // > 1: the rect is wider than the clip
    const bool fit = static_cast<Place>(place_[slot].place) == Place::Fit;
    // Clip uv per local uv. Cover: <= 1 (crop); fit: >= 1 (the clip uses part of the rect).
    const float fx = fit ? std::max(1.0f, k) : std::min(1.0f, k);
    const float fy = fit ? std::max(1.0f, 1.0f / k) : std::min(1.0f, 1.0f / k);
    const float ax = fx / rw, ay = fy / rh;
    const float bx = 0.5f - fx * (r[0] / rw + 0.5f), by = 0.5f - fy * (r[1] / rh + 0.5f);
    out[0] = ax, out[1] = ay;
    out[2] = bx - 0.5f + 0.5f * ax, out[3] = by - 0.5f + 0.5f * ay;
}

Reject OutputModel::validate(const Command& c, int layer_slot) const {
    const bool slot_all = c.slot == kNone;
    const bool slot_ok = c.slot < setup_.count;
    switch (c.type) {
        case CommandType::OutputBlack:
            if (!slot_all && !slot_ok) return Reject::BadTarget;
            return is_toggle(c.i) ? Reject::None : Reject::BadValue;
        case CommandType::Identify:
            return c.i >= 0 && c.i <= 2 ? Reject::None : Reject::BadValue;
        case CommandType::LayerPlace:
            if (layer_slot < 0 || layer_slot >= kPlaceSlots) return Reject::BadTarget;
            if (!enum_ok<Place>(c.i)) return Reject::BadValue;
            if (static_cast<Place>(c.i) == Place::Out && c.param >= setup_.count) return Reject::BadTarget;
            if (static_cast<Place>(c.i) == Place::Free) {
                for (float f : c.f)
                    if (!finite(f)) return Reject::BadValue;
                if (c.f[2] <= 0.0f || c.f[3] <= 0.0f) return Reject::BadValue;
            }
            return Reject::None;
        case CommandType::OutputConfig: {
            if (c.param >= static_cast<uint8_t>(OutputParam::Count)) return Reject::BadTarget;
            if (!finite(c.f[0])) return Reject::BadValue;
            switch (static_cast<OutputParam>(c.param)) {
                case OutputParam::Resolution:
                    if (!slot_all && !slot_ok) return Reject::BadTarget;
                    return enum_ok<ResolutionPreset>(c.i) ? Reject::None : Reject::BadValue;
                case OutputParam::Refresh: return enum_ok<RefreshRate>(c.i) ? Reject::None : Reject::BadValue;
                case OutputParam::BlendWidth:
                    return c.i >= 0 && c.i <= static_cast<int32_t>(kMaxBlendPx) ? Reject::None : Reject::BadValue;
                case OutputParam::BlendGamma: return Reject::None;  // clamped
                case OutputParam::Mode: return enum_ok<CanvasMode>(c.i) ? Reject::None : Reject::BadValue;
                case OutputParam::Adopt:
                case OutputParam::SimLost:
                    if (!slot_ok) return Reject::BadTarget;
                    return c.i == 0 || c.i == 1 ? Reject::None : Reject::BadValue;
                default: return Reject::BadTarget;
            }
        }
        default: return Reject::UnknownType;
    }
}

void OutputModel::producer_quiet(int producer, uint64_t now_us) {
    if (producer < 0 || producer != ident_holder_ || !ident_active_ || ident_off_us_ != 0) return;
    Command off = make(CommandType::Identify);
    off.i = 0;
    apply(off, -1, now_us, producer);
}

void OutputModel::apply(const Command& c, int layer_slot, uint64_t now_us, int producer) {
    switch (c.type) {
        case CommandType::OutputBlack:
            for (int o = 0; o < setup_.count; ++o)
                if (c.slot == kNone || c.slot == o) black_[o] = toggle(black_[o], c.i);
            return;
        case CommandType::Identify: {
            const uint64_t now = now_us ? now_us : 1;
            ident_holder_ = c.i == 2 ? producer : -1;
            if (c.i == 0) {
                // Fade out from where it is now (a fade-in still running fades back the same way).
                if (ident_active_ && (ident_off_us_ == 0 || ident_off_us_ > now)) {
                    const float a = identify_alpha(now);
                    ident_off_us_ = now - static_cast<uint64_t>((1.0f - a) * kFadeUs);
                }
                return;
            }
            const float a = identify_alpha(now);  // already showing: no new fade-in
            ident_on_us_ = now - static_cast<uint64_t>(a * kFadeUs);
            ident_off_us_ = c.i == 1 ? now + kIdentifyMs * 1000ull - kFadeUs : 0;
            ident_active_ = true;
            return;
        }
        case CommandType::LayerPlace: {
            LayerPlaceInfo& p = place_[layer_slot];
            p.place = static_cast<uint8_t>(c.i);
            p.out = static_cast<Place>(c.i) == Place::Out ? c.param : 0;
            if (static_cast<Place>(c.i) == Place::Free) {
                const float w = std::clamp(c.f[2], 0.01f, 4.0f), h = std::clamp(c.f[3], 0.01f, 4.0f);
                const float rect[4] = {std::clamp(c.f[0], -2.0f, 2.0f), std::clamp(c.f[1], -2.0f, 2.0f), w, h};
                std::memcpy(p.rect, rect, sizeof(rect));
            }
            place_rect(layer_slot, p.rect);
            ++place_gen_;
            return;
        }
        case CommandType::OutputConfig:
            switch (static_cast<OutputParam>(c.param)) {
                case OutputParam::Resolution:
                    for (int o = 0; o < setup_.count; ++o)
                        if (c.slot == kNone || c.slot == o) out_preset_[o] = static_cast<ResolutionPreset>(c.i);
                    if (c.slot == kNone) setup_.preset = static_cast<ResolutionPreset>(c.i);
                    config_by_command_ = true;
                    relayout();
                    return;
                case OutputParam::Refresh:
                    setup_.refresh = static_cast<RefreshRate>(c.i);
                    config_by_command_ = true;
                    ++refresh_gen_;
                    return;
                case OutputParam::BlendWidth:
                    setup_.blend_px = static_cast<uint32_t>(c.i);
                    config_by_command_ = true;
                    relayout();
                    return;
                case OutputParam::BlendGamma:
                    setup_.blend_gamma = std::clamp(c.f[0], 1.0f, 3.0f);
                    config_by_command_ = true;
                    return;
                case OutputParam::Mode:
                    setup_.mode = static_cast<CanvasMode>(c.i);
                    config_by_command_ = true;
                    relayout();
                    return;
                case OutputParam::Adopt:
                    if (c.i == 1) ++adopt_gen_[c.slot];
                    return;
                case OutputParam::SimLost: sim_lost_[c.slot] = c.i == 1; return;
                default: return;
            }
        default: return;
    }
}

float OutputModel::identify_alpha(uint64_t now_us) const {
    if (!ident_active_ || now_us < ident_on_us_) return 0.0f;
    const float in = std::min(1.0f, static_cast<float>(now_us - ident_on_us_) / kFadeUs);
    if (ident_off_us_ == 0 || now_us < ident_off_us_) return in;
    const float out = 1.0f - static_cast<float>(now_us - ident_off_us_) / kFadeUs;
    return std::clamp(std::min(in, out), 0.0f, 1.0f);
}

uint32_t OutputModel::identify_ms_left(uint64_t now_us) const {
    if (identify_alpha(now_us) <= 0.0f) return 0;
    if (ident_off_us_ == 0) return 0xFFFFFFFFu;  // held
    const uint64_t end = ident_off_us_ + kFadeUs;
    return now_us >= end ? 0 : static_cast<uint32_t>((end - now_us) / 1000);
}

void OutputModel::fill(OutputsSection& s, uint64_t now_us) const {
    s.count = static_cast<uint32_t>(setup_.count);
    s.mode = static_cast<uint8_t>(setup_.mode);
    s.refresh = static_cast<uint8_t>(setup_.refresh);
    s.fullscreen = setup_.fullscreen ? 1 : 0;
    s.identify = identify_on(now_us) ? 1 : 0;
    s.canvas_w = canvas_w_, s.canvas_h = canvas_h_;
    s.blend_px = setup_.mode == CanvasMode::Mirror ? 0 : setup_.blend_px;
    s.blend_gamma = setup_.blend_gamma;
    s.hz_target = refresh_hz(setup_.refresh);
    s.identify_ms_left = identify_ms_left(now_us);
    s.config_by_command = config_by_command_ ? 1 : 0;
    for (int o = 0; o < kMaxOutputs; ++o) {
        OutputInfo& oi = s.out[o];
        const OutputRegion& r = region_[o];
        oi.black = black_[o] ? 1 : 0;
        oi.preset = static_cast<uint8_t>(out_preset_[o]);
        oi.identify = s.identify && o < setup_.count ? 1 : 0;
        oi.width = static_cast<uint32_t>(r.w), oi.height = static_cast<uint32_t>(r.h);
        oi.canvas_x = r.x;
        oi.blend_left = r.blend_left, oi.blend_right = r.blend_right;
    }
    for (int i = 0; i < kPlaceSlots; ++i) s.layer[i] = place_[i];
}

void OutputModel::restore(const OutputsSection& old) {
    for (int o = 0; o < kMaxOutputs; ++o) black_[o] = old.out[o].black != 0;
    for (int i = 0; i < kPlaceSlots; ++i)
        if (enum_ok<Place>(old.layer[i].place)) place_[i] = old.layer[i];
    if (old.config_by_command) {
        if (enum_ok<CanvasMode>(old.mode)) setup_.mode = static_cast<CanvasMode>(old.mode);
        if (enum_ok<RefreshRate>(old.refresh)) setup_.refresh = static_cast<RefreshRate>(old.refresh);
        setup_.blend_px = std::min(old.blend_px, kMaxBlendPx);
        if (finite(old.blend_gamma)) setup_.blend_gamma = std::clamp(old.blend_gamma, 1.0f, 3.0f);
        for (int o = 0; o < kMaxOutputs; ++o)
            if (enum_ok<ResolutionPreset>(old.out[o].preset)) out_preset_[o] = static_cast<ResolutionPreset>(old.out[o].preset);
        config_by_command_ = true;
    }
    relayout();
}

// ---------------------------------------------------------------- identity
bool id_known(const DisplayId& id) { return id.maker[0] != 0 || id.product != 0; }

bool same_display(const DisplayId& a, const DisplayId& b) {
    if (!id_known(a) || !id_known(b)) return false;
    if (std::strncmp(a.maker, b.maker, sizeof(a.maker)) != 0 || a.product != b.product) return false;
    const bool a_serial = a.serial != 0 || a.serial_text[0], b_serial = b.serial != 0 || b.serial_text[0];
    if (a_serial || b_serial)
        return a.serial == b.serial && std::strncmp(a.serial_text, b.serial_text, sizeof(a.serial_text)) == 0;
    return a.connector == b.connector && a.technology == b.technology;
}

void match_displays(const DisplayId* slot_id, int slots, const DisplayId* displays, const int* display_order,
                    int display_count, int* out_display, bool* out_other) {
    constexpr int kMaxDisplays = 16;
    bool claimed[kMaxDisplays] = {};
    display_count = std::min(display_count, kMaxDisplays);
    for (int s = 0; s < slots; ++s) out_display[s] = -1, out_other[s] = false;
    // 1. Known slots take their own display, on any connector.
    for (int s = 0; s < slots; ++s) {
        if (!id_known(slot_id[s])) continue;
        for (int d = 0; d < display_count; ++d)
            if (!claimed[d] && same_display(slot_id[s], displays[d])) {
                out_display[s] = d, claimed[d] = true;
                break;
            }
    }
    // 2. Slots that never had a display take the free ones, left to right.
    for (int s = 0; s < slots; ++s) {
        if (id_known(slot_id[s]) || out_display[s] >= 0) continue;
        for (int k = 0; k < display_count; ++k) {
            const int d = display_order ? display_order[k] : k;
            if (d >= 0 && d < display_count && !claimed[d]) {
                out_display[s] = d, claimed[d] = true;
                break;
            }
        }
    }
    // 3. A known slot without its display, while another display is free: ask once.
    bool any_free = false;
    for (int d = 0; d < display_count; ++d) any_free |= !claimed[d];
    for (int s = 0; s < slots; ++s) out_other[s] = id_known(slot_id[s]) && out_display[s] < 0 && any_free;
}

}  // namespace himmel::engine
