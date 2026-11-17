// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/show/model.h"

#include <cstdio>
#include <cstring>

#include "himmel/engine/deck_model.h"

namespace himmel::show {

using namespace himmel::protocol;

void copy_str(char* dst, int cap, const char* src) {
    if (cap <= 0) return;
    if (!src) src = "";
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
    // UTF-8: never leave half a character at the cut.
    if (src[i] && i > 0) {
        int j = i;
        while (j > 0 && (static_cast<unsigned char>(dst[j - 1]) & 0xC0) == 0x80) --j;
        if (j > 0 && (static_cast<unsigned char>(dst[j - 1]) & 0x80)) dst[j - 1] = 0;
    }
}

namespace {

void zero_status(LayerState& l) {
    l.position = 0.0f;
    l.missing = 0;
    l.playing = 0;
    l.reserved[0] = l.reserved[1] = 0;
    for (auto& f : l.fx) f.press = 0;
}

void zero_status(LayerTransport& t) {
    t.reverse_hold = 0;
    t.phase = 0;
    t.beat_loop = 0;
    t.scratching = 0;
    t.pending = 0;
    t.beat_loop_beats = 0.0f;
    t.speed_now = 0.0f;
    t.gain = 0.0f;
    t.transition_progress = 0.0f;
    t.pending_beats = 0.0f;
    t.velocity = 0.0f;
    t.loops = 0;
    t.frame = 0;
    for (auto& c : t.cue) c = 0.0f;  // cue points belong to the clip, not to the scene
}

void empty_fx(FxSlotState& s) {
    std::memset(&s, 0, sizeof(s));
    s.effect = -1;
    s.mix = 1.0f;
}

}  // namespace

void clear_look(Look& l) {
    std::memset(&l, 0, sizeof(l));
    for (int i = 0; i < kLayers; ++i) {
        LayerState& s = l.layer[i];
        s.clip = -1;
        s.opacity = 1.0f;
        s.speed = 1.0f;
        for (auto& f : s.fx) empty_fx(f);
        himmel::engine::default_transport(l.transport[i], i);
        zero_status(s);
        zero_status(l.transport[i]);
    }
    for (auto& f : l.fx) empty_fx(f);
}

void default_macros(Scene& s) {
    static const char* kNames[kMacros] = {"INTENSITY", "COLOUR", "SPACE", "MASTER FX"};
    for (int m = 0; m < kMacros; ++m) {
        std::memset(&s.macro[m], 0, sizeof(Macro));
        copy_str(s.macro[m].name, sizeof(s.macro[m].name), kNames[m]);
        s.macro[m].value = 0.5f;
        for (auto& b : s.macro[m].bind) b.layer = b.slot = b.param = kNone;
    }
}

void clear_scene(Scene& s) {
    std::memset(&s, 0, sizeof(s));
    s.parent = -1;
    clear_look(s.look);
    for (auto& p : s.kit) p.id = -1, p.slot = kNone;
    default_macros(s);
    s.transition.kind = static_cast<uint8_t>(TransitionKind::Cut);
    s.transition.seconds = 2.0f;
}

void clear_show(ShowData& s, int screens) {
    std::memset(&s, 0, sizeof(s));
    copy_str(s.name, sizeof(s.name), "Untitled");
    s.outputs.count = static_cast<uint8_t>(screens == 1 ? 1 : 3);
    s.outputs.preset = static_cast<uint8_t>(OutputPreset::P1080);
    s.outputs.span = 1;
    s.song = -1;
    s.default_take_ms = himmel::engine::kDefaultTakeMs;
    for (auto& sc : s.scene) clear_scene(sc);
    for (auto& b : s.bank_pad)
        for (auto& p : b) p = -1;
    for (auto& so : s.songs) {
        std::memset(&so, 0, sizeof(so));
        for (auto& p : so.scene) p = -1;
    }
    for (auto& x : s.setlist) x = -1;
}

int page_scene(const ShowData& s, int pad) {
    if (pad < 0 || pad >= kPads) return -1;
    if (s.use_setlist && s.song >= 0 && s.song < s.setlist_count) {
        const int id = s.setlist[s.song];
        if (id < 0 || id >= kMaxSongs || !s.songs[id].used) return -1;
        return s.songs[id].scene[pad];
    }
    return s.bank < kMaxBanks ? s.bank_pad[s.bank][pad] : -1;
}

int alloc_scene(const ShowData& s) {
    for (int i = 0; i < kMaxScenes; ++i)
        if (!s.scene[i].used) return i;
    return -1;
}

int main_of(const ShowData& s, int scene) {
    if (scene < 0 || scene >= kMaxScenes) return -1;
    const int p = s.scene[scene].parent;
    return p >= 0 && p < kMaxScenes && s.scene[p].used ? p : scene;
}

int variations_of(const ShowData& s, int main_scene, int out[kMaxVariations]) {
    if (main_scene < 0 || main_scene >= kMaxScenes || !s.scene[main_scene].used) return 0;
    int n = 0;
    out[n++] = main_scene;
    for (int v = 1; v < kMaxVariations; ++v)
        for (int i = 0; i < kMaxScenes; ++i)
            if (s.scene[i].used && s.scene[i].parent == main_scene && s.scene[i].variation == v) {
                out[n++] = i;
                break;
            }
    return n;
}

int scene_count(const ShowData& s) {
    int n = 0;
    for (const auto& sc : s.scene) n += sc.used ? 1 : 0;
    return n;
}

void look_from_live(const LiveState& live, const TransportSection& tr, const Catalog& cat, int deck, Look& out) {
    std::memset(&out, 0, sizeof(out));
    for (int l = 0; l < kLayers; ++l) {
        out.layer[l] = live.deck[deck].layer[l];
        zero_status(out.layer[l]);
        out.transport[l] = tr.layer[deck][l];
        zero_status(out.transport[l]);
        const int c = out.layer[l].clip;
        if (c >= 0 && c < static_cast<int>(cat.clip_count)) copy_str(out.clip_name[l], kNameLen, cat.clip[c].name);
    }
    for (int f = 0; f < kFxSlots; ++f) {
        out.fx[f] = live.deck[deck].fx[f];
        out.fx[f].press = 0;
    }
}

void snapshot_from_live(const LiveState& live, const TransportSection& tr, const Catalog& cat, LiveSnapshot& out) {
    std::memset(&out, 0, sizeof(out));
    for (int d = 0; d < kDecks; ++d) look_from_live(live, tr, cat, d, out.deck[d]);
    for (int f = 0; f < kFxSlots; ++f) {
        out.master_fx[f] = live.master_fx[f];
        out.master_fx[f].press = 0;
    }
    out.crossfader = live.crossfader;
    out.program_deck = live.program_deck;
    out.black = live.black;
}

bool same_look(const Look& a, const Look& b) {
    // Names follow the ids; compare the settings only.
    return std::memcmp(a.layer, b.layer, sizeof(a.layer)) == 0 &&
           std::memcmp(a.transport, b.transport, sizeof(a.transport)) == 0 &&
           std::memcmp(a.fx, b.fx, sizeof(a.fx)) == 0;
}

}  // namespace himmel::show
