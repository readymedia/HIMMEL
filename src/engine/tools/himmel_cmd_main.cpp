// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_cmd.exe - send commands to the engine from the command line (producer
// slot 3 "Tool", docs/interfaces/command-protocol.md) and print the live state.
//
//   himmel_cmd state                       the show on air (default)
//   himmel_cmd catalog                     clips (ids 0..255) and effects
//   himmel_cmd clips [FROM [N]]            catalog v3: every clip by id, with library id and state (wave 7)
//   himmel_cmd cliplib D L LIBID           load a library clip by its library id (LayerClipLib 75)
//   himmel_cmd smooth auto|off|blend|even  global frame smoothing; per layer: set D L smooth auto|off|blend|even|global
//   himmel_cmd playback                    frame smoothing + cadence (judder) per layer
//   himmel_cmd take [ms] | cut | black [on|off|toggle]
//   himmel_cmd xfade V | nudge DELTA       crossfader 0 (A) .. 1 (B)
//   himmel_cmd clip D L ID                 D = a|b|0|1, L = 1..4, ID = clip id or -1
//   himmel_cmd opacity D L V | speed D L V | jump D L V
//   himmel_cmd blend D L normal|add|screen|multiply|difference|lighten
//   himmel_cmd mute D L [on|off|toggle] | play D L pause|play|toggle|restart
//   himmel_cmd fx T S EFFECT               T = a1..a4 / b1..b4 (layer), a / b (deck), m (master);
//                                          S = slot 1..4; EFFECT = id, name or -1
//   himmel_cmd fx T S EFFECT PRESET        ... then set the preset's knobs (subtle, extreme, ...: fx/<id>/effect.json)
//   himmel_cmd press T S V                 PRESS 0..1 on the slot (0 = release)
//   himmel_cmd param T S P V | mix T S V | bypass T S [on|off|toggle]   P = 1..8
//   himmel_cmd react T S K kick 0.5 add   D131: knob K (1..4) gets its own reaction (FxReact); "default" / "off"
//   himmel_cmd focus D L | hello | raw TYPE DECK LAYER SLOT PARAM I F   (raw: protocol numbers)
//   himmel_cmd flood N                     N opacity commands at once (rate-limit test)
//   himmel_cmd watch [seconds]             print the state twice a second
// Clip transport (M2, docs/design/clips-and-playback.md §2-3); D may also be h = the HIT layer (L = 1):
//   himmel_cmd set D L PARAM VALUE         PARAM: mode dir in out loopin loopout inner timebase beats
//                                          interval distance slices stepframes stepdiv glide trigger start
//                                          fadein fadeout snap target autopilot apcount transition ttime
//                                          catchup scratchsnap reverse seed (VALUE: a number or a name)
//   himmel_cmd trigger D L ID [VEL] [up]   a pad press (up = release); ID -1 = the layer's clip
//   himmel_cmd cue D L K jump|set [V]|clear   K = 1..16; set without V = at the playhead
//   himmel_cmd beatloop D L BEATS          0 = off
//   himmel_cmd scratch D L grab|release|move V|rel SECONDS
//   himmel_cmd play D L release|stop       (besides pause|play|toggle|restart)
//   himmel_cmd tap | bpm V | grid off|1/4|1|bar|2bar|4bar | resync | phase BEATS
//   himmel_cmd cuebench D L N [MS]         N cue jumps, one every MS (default 250) - the renderer logs latency
//   himmel_cmd soak SECONDS                cycles transport modes on every loaded layer (soak test)
// Outputs (M6, docs/interfaces/outputs-block.md); N = output 1..8:
//   himmel_cmd outputs                     the outputs block (slots, displays, canvas, places)
//   himmel_cmd outblack N|all [on|off|toggle]   black per output
//   himmel_cmd identify [on|hold|off]      IDENTIFY: 5 s (on), until off (hold)
//   himmel_cmd quit --confirm              quit the whole of Himmel (SystemQuit 74, command-protocol.md §4e)
//   himmel_cmd place D L fill|fit|out N|free X Y W H   the layer's place (canvas 0..1)
//   himmel_cmd outres 720p|1080p|1440p|4k|custom [N]   resolution preset (all outputs, or N)
//   himmel_cmd refresh 60|50|59.94|30|25 | blendw PX | blendgamma G | outmode span|mirror
//   himmel_cmd adopt N | simlost N on|off  take the new display / act as if N's cable was pulled
// Several commands may be chained with "--": himmel_cmd clip a 1 0 -- take 500
// Layers and slots are 1-based on the command line, 0-based on the wire.
// Exit code: 0 = all applied, 1 = usage, 2 = no engine / no block, 3 = refused.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "himmel/engine/deck_model.h"
#include "himmel/engine/shm_block.h"
#include "himmel/fx/fx.h"
#include "himmel/protocol/clip_pages.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/outputs.h"
#include "himmel/protocol/state.h"

using namespace himmel::protocol;
using himmel::engine::ShmBlock;

namespace {

constexpr int kSlot = static_cast<int>(Producer::Tool);

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void usage() {
    std::printf(
        "himmel_cmd: send commands to himmel_render (producer slot 3) and print the state\n"
        "  state | catalog | clips [from [n]] | playback | watch [s]\n"
        "  cliplib D L LIBID | smooth auto|off|blend|even\n"
        "  take [ms] | cut | black [on|off|toggle] | xfade V | nudge DELTA\n"
        "  clip D L ID | opacity D L V | speed D L V | jump D L V | blend D L MODE\n"
        "  mute D L [on|off|toggle] | play D L pause|play|toggle|restart | focus D L\n"
        "  fx T S EFFECT [PRESET] | param T S P V | mix T S V | press T S V | bypass T S [on|off|toggle]\n"
        "  react T S K|all SIGNAL [AMOUNT] [add|follow|trigger] [ATTACK RELEASE THRESHOLD] | react T S K|all default|off\n"
        "  raw TYPE DECK LAYER SLOT PARAM I F | hello | flood N\n"
        "  set D L PARAM VALUE | trigger D L ID [VEL] [up] | cue D L K jump|set [V]|clear\n"
        "  beatloop D L BEATS | scratch D L grab|release|move V|rel S | play D L release|stop\n"
        "  tap | bpm V | grid off|1/4|1|bar|2bar|4bar | resync | phase BEATS | cuebench D L N [MS] | soak S\n"
        "  outputs | outblack N|all [on|off|toggle] | identify [on|hold|off] | place D L fill|fit|out N|free X Y W H\n"
        "  outres 720p|1080p|1440p|4k|custom [N] | refresh HZ | blendw PX | blendgamma G | outmode span|mirror\n"
        "  adopt N | simlost N on|off | quit --confirm (stop the whole of Himmel cleanly)\n"
        "  D = a|b, L = 1..4, T = a1..b4 (layer) | a | b (deck) | m (master), S = 1..4, P = 1..8\n"
        "  chain with --:  himmel_cmd clip a 1 0 -- clip a 2 1 -- take\n");
}

int deck_of(const std::string& s) {
    if (s == "a" || s == "A" || s == "0") return 0;
    if (s == "b" || s == "B" || s == "1") return 1;
    if (s == "h" || s == "H" || s == "hit") return kHitDeck;
    return -1;
}

// ---- LayerSet names (M2) ----
struct ParamName {
    const char* name;
    LayerParam p;
    bool is_float;
    const char* const* values;  // enum names (index = value), or null
};
const char* const kModeNames[] = {"loop", "pingpong", "oncehold", "onceeject", "oncenext", "random", "chop", "step", "scratch", nullptr};
const char* const kTimeBaseNames[] = {"free", "bpm", nullptr};
const char* const kTriggerNames[] = {"normal", "toggle", "piano", "legato", nullptr};
const char* const kStartNames[] = {"restart", "continue", nullptr};
const char* const kTargetNames[] = {"own", "free", "hit", nullptr};
const char* const kAutopilotNames[] = {"off", "next", "previous", "random", "same", nullptr};
const char* const kTransitionNames[] = {"cut", "fade", "blackdip", nullptr};
const char* const kGridNames[] = {"off", "1/4", "1", "bar", "2bar", "4bar", nullptr};
const char* const kDirNames[] = {"fwd", "back", nullptr};
const char* const kSmoothNames[] = {"auto", "off", "blend", "even", nullptr};  // FrameSmooth (wave 7)
const char* const kPhaseNames[] = {"empty", "play", "pause", "hold", "intro", "inner", "outro"};
const ParamName kParams[] = {
    {"mode", LayerParam::Mode, false, kModeNames},          {"dir", LayerParam::Direction, false, kDirNames},
    {"in", LayerParam::In, true, nullptr},                   {"out", LayerParam::Out, true, nullptr},
    {"loopin", LayerParam::LoopIn, true, nullptr},           {"loopout", LayerParam::LoopOut, true, nullptr},
    {"inner", LayerParam::InnerLoop, false, nullptr},        {"timebase", LayerParam::TimeBase, false, kTimeBaseNames},
    {"beats", LayerParam::Beats, true, nullptr},             {"interval", LayerParam::Interval, true, nullptr},
    {"distance", LayerParam::Distance, true, nullptr},       {"slices", LayerParam::Slices, false, nullptr},
    {"stepframes", LayerParam::StepFrames, false, nullptr},  {"stepdiv", LayerParam::StepDiv, true, nullptr},
    {"glide", LayerParam::Glide, true, nullptr},             {"trigger", LayerParam::Trigger, false, kTriggerNames},
    {"start", LayerParam::StartPos, false, kStartNames},     {"fadein", LayerParam::FadeIn, true, nullptr},
    {"fadeout", LayerParam::FadeOut, true, nullptr},         {"snap", LayerParam::Snap, false, kGridNames},
    {"target", LayerParam::Target, false, kTargetNames},     {"autopilot", LayerParam::Autopilot, false, kAutopilotNames},
    {"apcount", LayerParam::AutopilotCount, false, nullptr}, {"transition", LayerParam::Transition, false, kTransitionNames},
    {"ttime", LayerParam::TransitionTime, true, nullptr},    {"catchup", LayerParam::CatchUp, false, nullptr},
    {"scratchsnap", LayerParam::ScratchSnap, false, nullptr}, {"reverse", LayerParam::ReverseHold, false, nullptr},
    {"seed", LayerParam::Seed, false, nullptr},              {"smooth", LayerParam::Smooth, false, kSmoothNames},
};
int name_index(const char* const* names, const std::string& v) {
    for (int k = 0; names && names[k]; ++k)
        if (v == names[k]) return k;
    return -1;
}
Command layer_set(uint8_t deck, uint8_t layer, LayerParam p, int32_t i, float f) {
    Command c = make(CommandType::LayerSet, deck, layer, i, f);
    c.param = static_cast<uint8_t>(p);
    return c;
}
int toggle_of(const std::string& s) {
    if (s == "on" || s == "1") return kOn;
    if (s == "off" || s == "0") return kOff;
    return kToggle;
}
// Fx target: "a1".."b4" = layer, "a"/"b" = deck, "m" = master.
bool fx_target(const std::string& s, uint8_t& deck, uint8_t& layer) {
    if (s == "m" || s == "M" || s == "master") return deck = kMasterDeck, layer = kNone, true;
    if (s.empty()) return false;
    const int d = deck_of(s.substr(0, 1));
    if (d < 0) return false;
    deck = static_cast<uint8_t>(d);
    if (s.size() == 1) return layer = kNone, true;
    const int l = std::atoi(s.c_str() + 1);
    if (l < 1 || l > kLayersPerDeck) return false;
    layer = static_cast<uint8_t>(l - 1);
    return true;
}

struct Engine {
    ShmBlock cmd_shm, state_shm;
    CommandBlock* cmd = nullptr;
    StateBlock* state = nullptr;
    uint32_t seq = 0;

    bool open_state() {
        if (!state_shm.open_existing(kStateShmName, sizeof(StateBlock))) return false;
        state = static_cast<StateBlock*>(state_shm.data());
        return himmel::engine::header_ok(state->header, kStateMagic, kStateVersion, sizeof(StateBlock));
    }
    bool open_commands() {
        const auto r = cmd_shm.open_or_create(kCommandShmName, sizeof(CommandBlock));
        if (r == ShmBlock::Result::Failed) return false;
        cmd = static_cast<CommandBlock*>(cmd_shm.data());
        if (r == ShmBlock::Result::Created)
            himmel::engine::write_header(cmd->header, kCommandMagic, kCommandVersion, sizeof(CommandBlock));
        if (!himmel::engine::header_ok(cmd->header, kCommandMagic, kCommandVersion, sizeof(CommandBlock))) return false;
        cmd->producers[kSlot].pid.store(GetCurrentProcessId());
        cmd->producers[kSlot].heartbeat_us.store(now_us());
        // A restarted producer continues from the engine's last ack (so "acked" below is unambiguous).
        if (state) {
            LiveState l{};
            if (seq_read(state->live_seq, state->live, l)) seq = l.acked_seq[kSlot];
        }
        return true;
    }
    bool live(LiveState& out) const { return state && seq_read(state->live_seq, state->live, out) != 0; }
    bool transport(TransportSection& out) const {
        return state && seq_read(state->transport_seq, state->transport, out) != 0;
    }
    bool catalog(Catalog& out) const { return state && seq_read(state->catalog_seq, state->catalog, out) != 0; }

    // Push and wait (max 1 s) until the engine acks or refuses it.
    int send(Command c, bool quiet = false) {
        c.seq = ++seq;
        LiveState before{};
        live(before);
        cmd->producers[kSlot].heartbeat_us.store(now_us());
        if (!cmd->rings[kSlot].push(c)) {
            std::printf("ring full (dropped %u)\n", cmd->rings[kSlot].dropped.load());
            return 3;
        }
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            LiveState l{};
            if (!live(l)) continue;
            if (l.acked_seq[kSlot] == c.seq) {
                if (!quiet) std::printf("%s: applied (seq %u)\n", himmel::engine::command_name(c.type), c.seq);
                return 0;
            }
            if (l.rejected[kSlot] != before.rejected[kSlot]) {
                std::printf("%s: REFUSED (%s)\n", himmel::engine::command_name(c.type),
                            himmel::engine::reject_name(static_cast<Reject>(l.last_reject[kSlot])));
                return 3;
            }
        }
        std::printf("%s: no answer from the engine in 1 s (is himmel_render running?)\n",
                    himmel::engine::command_name(c.type));
        return 2;
    }
};

const char* kBlend[] = {"normal", "add", "screen", "multiply", "difference", "lighten"};

void print_fx(const char* label, const FxSlotState (&fx)[kFxSlots], const Catalog& cat) {
    for (int s = 0; s < kFxSlots; ++s) {
        const FxSlotState& f = fx[s];
        if (f.effect < 0) continue;
        const bool known = f.effect < static_cast<int>(cat.effect_count);
        std::printf("      %s fx%d %-10s mix %.2f%s ", label, s + 1, known ? cat.effect[f.effect].name : "?", f.mix,
                    f.bypass ? " BYPASS" : "");
        if (known)
            for (int p = 0; p < cat.effect[f.effect].param_count; ++p)
                std::printf(" %s %.2f", cat.effect[f.effect].param_name[p], f.param[p]);
        std::printf("\n");
    }
}

void print_transport(const char* label, const LayerState& s, const LayerTransport& t) {
    if (s.clip < 0 && !t.pending) return;
    std::printf("      %s %-9s %-5s frame %5d  spd %+.2f  gain %.2f  loops %u  in %.2f out %.2f", label,
                t.mode < 9 ? kModeNames[t.mode] : "?", t.phase < 7 ? kPhaseNames[t.phase] : "?", t.frame, t.speed_now,
                t.gain, t.loops, t.in, t.out);
    if (t.time_base) std::printf("  BPM %.1f beats", t.beats);
    if (t.beat_loop) std::printf("  BEATLOOP %.3g", t.beat_loop_beats);
    if (t.scratching) std::printf("  SCRATCH");
    if (t.pending) std::printf("  WAITING %.2f beats", t.pending_beats);
    if (t.transition_progress < 1.0f) std::printf("  transition %.0f %%", t.transition_progress * 100);
    int cues = 0;
    for (float q : t.cue) cues += q >= 0.0f;
    if (cues) std::printf("  cues %d", cues);
    std::printf("\n");
}

void print_state(const LiveState& l, const Catalog& cat, const TransportSection* tr = nullptr) {
    std::printf("epoch %u  frame %llu  %.1f fps   crossfader %.3f  PROGRAM %c  PREVIEW %c%s%s  take %u ms",
                l.epoch, static_cast<unsigned long long>(l.frame), l.fps, l.crossfader, l.program_deck ? 'B' : 'A',
                l.program_deck ? 'A' : 'B', l.take_active ? "  TAKE" : "", l.black ? "  BLACK" : "", l.take_ms);
    if (l.take_active) std::printf(" (%.0f %%)", l.take_progress * 100);
    if (l.focus_deck < kDecks) std::printf("  focus %c%d", 'A' + l.focus_deck, l.focus_layer + 1);
    std::printf("\n");
    for (int d = 0; d < kDecks; ++d) {
        std::printf("  deck %c %s\n", 'A' + d, d == l.program_deck ? "(PROGRAM)" : "(PREVIEW)");
        for (int k = 0; k < kLayersPerDeck; ++k) {
            const LayerState& s = l.deck[d].layer[k];
            if (s.clip < 0) {
                std::printf("    %c%d  -\n", 'A' + d, k + 1);
            } else {
                const char* name = s.clip < static_cast<int>(cat.clip_count) ? cat.clip[s.clip].name : "?";
                std::printf("    %c%d  clip %d %-28s %-10s op %.2f spd %+.2f pos %.3f %s%s%s\n", 'A' + d, k + 1, s.clip,
                            name, s.blend < 6 ? kBlend[s.blend] : "?", s.opacity, s.speed, s.position,
                            s.playing ? "play" : "PAUSE", s.mute ? " MUTE" : "", s.missing ? " MISSING" : "");
            }
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%c%d", 'A' + d, k + 1);
            print_fx(lbl, s.fx, cat);
            if (tr) print_transport(lbl, s, tr->layer[d][k]);
        }
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "deck %c", 'A' + d);
        print_fx(lbl, l.deck[d].fx, cat);
    }
    print_fx("master", l.master_fx, cat);
    if (tr) {
        const LayerState& h = tr->hit;
        if (h.clip >= 0)
            std::printf("  HIT  clip %d %-28s op %.2f\n", h.clip, h.clip < static_cast<int>(cat.clip_count) ? cat.clip[h.clip].name : "?",
                        h.opacity);
        print_transport("HIT", h, tr->hit_transport);
        std::printf("  clock %.1f BPM%s  beat %.2f  grid %s\n", tr->clock.bpm, tr->clock.source ? " (tapped)" : "",
                    tr->clock.beat, tr->clock.grid < 6 ? kGridNames[tr->clock.grid] : "?");
    }
    static const char* kProducerNames[kProducers] = {"perform", "gui", "osc", "tool", "4", "5", "6", "7"};
    std::printf("  producers:");
    for (int p = 0; p < kProducers; ++p)
        if (l.producer_alive[p] || l.acked_seq[p] || l.rejected[p])
            std::printf("  %s%s ack %u rej %u%s%s", kProducerNames[p], l.producer_alive[p] ? "*" : "", l.acked_seq[p],
                        l.rejected[p], l.rejected[p] ? " last " : "",
                        l.rejected[p] ? himmel::engine::reject_name(static_cast<Reject>(l.last_reject[p])) : "");
    std::printf("\n");
}

int effect_id(const std::string& s, const Catalog& cat) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end && *end == 0) return static_cast<int>(v);
    for (uint32_t e = 0; e < cat.effect_count; ++e)
        if (_stricmp(cat.effect[e].name, s.c_str()) == 0) return static_cast<int>(e);
    std::string compact;  // "rgbsplit" matches "RGB Split"
    for (uint32_t e = 0; e < cat.effect_count; ++e) {
        compact.clear();
        for (const char* p = cat.effect[e].name; *p; ++p)
            if (*p != ' ' && *p != '/') compact += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        if (compact == s) return static_cast<int>(e);
    }
    // The metadata id (fx/<id>, e.g. gen_ticker), if the engine runs the same built-in set.
    const auto& lib = himmel::fx::builtin();
    if (const int id = lib.find(s.c_str()); id >= 0 && id < static_cast<int>(cat.effect_count) &&
                                            std::strcmp(cat.effect[id].name, lib.meta(id).name) == 0)
        return id;
    return 9999;  // the engine refuses it (BadId)
}

// One command from argv[i..]; returns its exit code, or 1 on bad usage.
int run_one(Engine& e, const std::vector<std::string>& a, const Catalog& cat) {
    if (a.empty()) return 1;
    const std::string& c = a[0];
    auto f = [&](size_t i) { return i < a.size() ? static_cast<float>(std::atof(a[i].c_str())) : 0.0f; };
    auto n = [&](size_t i) { return i < a.size() ? std::atoi(a[i].c_str()) : 0; };
    auto layer_cmd = [&](CommandType t, int32_t iv, float fv) -> int {
        if (a.size() < 3 || deck_of(a[1]) < 0 || n(2) < 1) return 1;
        return e.send(make(t, static_cast<uint8_t>(deck_of(a[1])), static_cast<uint8_t>(n(2) - 1), iv, fv));
    };
    auto fx_cmd = [&](CommandType t, uint8_t param, int32_t iv, float fv) -> int {
        uint8_t deck = 0, layer = 0;
        if (a.size() < 3 || !fx_target(a[1], deck, layer) || n(2) < 1) return 1;
        return e.send(make_fx(t, deck, layer, static_cast<uint8_t>(n(2) - 1), param, iv, fv));
    };
    // ---- outputs (M6) ----
    auto out_cfg = [&](OutputParam p, uint8_t slot, int32_t iv, float fv) {
        Command x = make(CommandType::OutputConfig, kNone, kNone, iv, fv);
        x.param = static_cast<uint8_t>(p);
        x.slot = slot;
        return e.send(x);
    };
    auto out_slot = [&](size_t i) -> int {  // "all" = kNone, 1..8 -> 0..7, -1 = bad
        if (i >= a.size()) return -1;
        if (a[i] == "all") return kNone;
        const int v = n(i);
        return v >= 1 && v <= kMaxOutputs ? v - 1 : -1;
    };
    if (c == "outblack") {
        const int sl = out_slot(1);
        if (sl < 0) return 1;
        Command x = make(CommandType::OutputBlack, kNone, kNone, a.size() > 2 ? toggle_of(a[2]) : kToggle);
        x.slot = static_cast<uint8_t>(sl);
        return e.send(x);
    }
    if (c == "quit") {  // ENGINE (quit): never by accident - the tool's confirm is --confirm
        if (a.size() < 2 || a[1] != "--confirm") {
            std::fprintf(stderr, "quit stops the whole of Himmel: run 'himmel_cmd quit --confirm'\n");
            return 1;
        }
        return e.send(make(CommandType::SystemQuit, kNone, kNone, 0, 1.0f));
    }
    if (c == "identify") {
        const std::string m = a.size() > 1 ? a[1] : "on";
        return e.send(make(CommandType::Identify, kNone, kNone, m == "off" ? 0 : m == "hold" ? 2 : 1));
    }
    if (c == "place") {
        if (a.size() < 4 || deck_of(a[1]) < 0 || n(2) < 1) return 1;
        Command x = make(CommandType::LayerPlace, static_cast<uint8_t>(deck_of(a[1])), static_cast<uint8_t>(n(2) - 1));
        const std::string& w = a[3];
        if (w == "fill") x.i = static_cast<int32_t>(Place::FillAll);
        else if (w == "fit") x.i = static_cast<int32_t>(Place::Fit);
        else if (w == "out") {
            if (n(4) < 1) return 1;
            x.i = static_cast<int32_t>(Place::Out);
            x.param = static_cast<uint8_t>(n(4) - 1);
        } else if (w == "free") {
            if (a.size() < 8) return 1;
            x.i = static_cast<int32_t>(Place::Free);
            for (int k = 0; k < 4; ++k) x.f[k] = f(4 + static_cast<size_t>(k));
        } else return 1;
        return e.send(x);
    }
    if (c == "outres") {
        if (a.size() < 2) return 1;
        const std::string& r = a[1];
        const int p = r == "720p" ? 0 : r == "1080p" ? 1 : r == "1440p" ? 2 : (r == "4k" || r == "4K") ? 3 : r == "custom" ? 4 : -1;
        if (p < 0) return 1;
        const int sl = a.size() > 2 ? out_slot(2) : kNone;
        if (sl < 0) return 1;
        return out_cfg(OutputParam::Resolution, static_cast<uint8_t>(sl), p, 0);
    }
    if (c == "refresh") {
        const float hz = f(1);
        const int r = hz > 59.5f && hz < 59.99f ? 2 : hz == 50 ? 1 : hz == 30 ? 3 : hz == 25 ? 4 : hz == 60 ? 0 : -1;
        if (r < 0) return 1;
        return out_cfg(OutputParam::Refresh, kNone, r, 0);
    }
    if (c == "blendw") return out_cfg(OutputParam::BlendWidth, kNone, n(1), 0);
    if (c == "blendgamma") return out_cfg(OutputParam::BlendGamma, kNone, 0, f(1));
    if (c == "outmode") {
        if (a.size() < 2 || (a[1] != "span" && a[1] != "mirror")) return 1;
        return out_cfg(OutputParam::Mode, kNone, a[1] == "mirror" ? 1 : 0, 0);
    }
    if (c == "adopt" || c == "simlost") {
        const int sl = out_slot(1);
        if (sl < 0 || sl == kNone) return 1;
        const int32_t v = c == "adopt" ? 1 : (a.size() > 2 && a[2] == "off" ? 0 : 1);
        return out_cfg(c == "adopt" ? OutputParam::Adopt : OutputParam::SimLost, static_cast<uint8_t>(sl), v, 0);
    }
    if (c == "take") return e.send(make(CommandType::Take, kNone, kNone, n(1)));
    if (c == "cut") return e.send(make(CommandType::Cut));
    if (c == "black") return e.send(make(CommandType::Black, kNone, kNone, a.size() > 1 ? toggle_of(a[1]) : kToggle));
    if (c == "xfade") return e.send(make(CommandType::Crossfader, kNone, kNone, 0, f(1)));
    if (c == "nudge") return e.send(make(CommandType::Crossfader, kNone, kNone, 0, f(1), kRelative));
    if (c == "hello") return e.send(make(CommandType::Hello, kNone, kNone, static_cast<int32_t>(GetCurrentProcessId())));
    if (c == "clip") return layer_cmd(CommandType::LayerClip, n(3), 0);
    if (c == "cliplib") return layer_cmd(CommandType::LayerClipLib, n(3), 0);  // wave 7: by library id
    if (c == "smooth") {  // wave 7: the global frame smoothing
        const int v = a.size() > 1 ? name_index(kSmoothNames, a[1]) : -1;
        if (v < 0) return 1;
        return out_cfg(OutputParam::FrameSmooth, kNone, v, 0);
    }
    if (c == "opacity") return layer_cmd(CommandType::LayerOpacity, 0, f(3));
    if (c == "speed") return layer_cmd(CommandType::LayerSpeed, 0, f(3));
    if (c == "jump") return layer_cmd(CommandType::LayerJump, 0, f(3));
    if (c == "mute") return layer_cmd(CommandType::LayerMute, a.size() > 3 ? toggle_of(a[3]) : kToggle, 0);
    if (c == "focus") return layer_cmd(CommandType::Focus, 0, 0);
    if (c == "blend") {
        int m = a.size() > 3 ? std::atoi(a[3].c_str()) : 0;
        for (int k = 0; k < 6; ++k)
            if (a.size() > 3 && (a[3] == kBlend[k] || (a[3] == "diff" && k == 4))) m = k;
        return layer_cmd(CommandType::LayerBlend, m, 0);
    }
    if (c == "play") {
        const std::string v = a.size() > 3 ? a[3] : "toggle";
        const int act = v == "pause" ? 0 : v == "play" ? 1 : v == "restart" ? 3 : v == "release" ? 4 : v == "stop" ? 5 : 2;
        return layer_cmd(CommandType::LayerPlay, act, 0);
    }
    // ---- M2 transport ----
    auto layer_target = [&](uint8_t& deck, uint8_t& layer) {
        if (a.size() < 3 || deck_of(a[1]) < 0 || n(2) < 1) return false;
        deck = static_cast<uint8_t>(deck_of(a[1]));
        layer = static_cast<uint8_t>(n(2) - 1);
        return true;
    };
    uint8_t D = 0, L = 0;
    if (c == "set") {
        if (a.size() < 5 || !layer_target(D, L)) return 1;
        for (const auto& p : kParams) {
            if (a[3] != p.name) continue;
            int iv = p.values ? name_index(p.values, a[4]) : -1;
            if ((p.p == LayerParam::Snap || p.p == LayerParam::Smooth) && a[4] == "global") iv = -1;
            else if (iv < 0) iv = a[4] == "on" ? 1 : a[4] == "off" ? 0 : a[4] == "toggle" ? 2 : n(4);
            return e.send(layer_set(D, L, p.p, iv, p.is_float ? f(4) : 0.0f));
        }
        return 1;
    }
    if (c == "trigger") {
        if (a.size() < 4 || !layer_target(D, L)) return 1;
        const bool up = a.back() == "up";
        Command t = make(CommandType::LayerTrigger, D, L, n(3), a.size() > 4 && a[4] != "up" ? f(4) : 1.0f, up ? kRelease : 0);
        return e.send(t);
    }
    if (c == "cue") {
        if (a.size() < 5 || !layer_target(D, L) || n(3) < 1 || n(3) > kCuePoints) return 1;
        const int act = a[4] == "jump" ? 0 : a[4] == "set" ? 1 : a[4] == "clear" ? 2 : -1;
        if (act < 0) return 1;
        Command q = make(CommandType::LayerCue, D, L, act, act == 1 && a.size() > 5 ? f(5) : -1.0f);
        q.slot = static_cast<uint8_t>(n(3) - 1);
        return e.send(q);
    }
    if (c == "beatloop") return layer_cmd(CommandType::LayerBeatLoop, 0, f(3));
    if (c == "scratch") {
        if (a.size() < 4 || !layer_target(D, L)) return 1;
        if (a[3] == "release") return e.send(make(CommandType::LayerScratch, D, L, 0));
        if (a[3] == "grab") {
            LiveState l{};
            e.live(l);
            const float pos = D < kDecks ? l.deck[D].layer[L].position : 0.0f;
            return e.send(make(CommandType::LayerScratch, D, L, 1, pos));
        }
        if (a[3] == "move") return e.send(make(CommandType::LayerScratch, D, L, 1, f(4)));
        if (a[3] == "rel") return e.send(make(CommandType::LayerScratch, D, L, 1, f(4), kRelative));
        return 1;
    }
    auto clock_cmd = [&](ClockParam p, int32_t iv, float fv) {
        Command k = make(CommandType::Clock, kNone, kNone, iv, fv);
        k.param = static_cast<uint8_t>(p);
        return e.send(k);
    };
    if (c == "tap") return clock_cmd(ClockParam::Tap, 0, 0);
    if (c == "bpm") return clock_cmd(ClockParam::Bpm, 0, f(1));
    if (c == "resync") return clock_cmd(ClockParam::Resync, 0, 0);
    if (c == "phase") return clock_cmd(ClockParam::Nudge, 0, f(1));
    if (c == "grid") {
        const int g = a.size() > 1 ? name_index(kGridNames, a[1]) : -1;
        return g < 0 ? 1 : clock_cmd(ClockParam::Grid, g, 0);
    }
    if (c == "cuebench") {
        if (a.size() < 4 || !layer_target(D, L)) return 1;
        const int count = std::max(1, n(3));
        const int ms = a.size() > 4 ? std::max(20, n(4)) : 250;
        int rc = 0;
        uint32_t rng = 12345;
        for (int k = 0; k < count && rc == 0; ++k) {
            rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
            Command q = make(CommandType::LayerCue, D, L, 0);
            q.slot = static_cast<uint8_t>(rng % kCuePoints);
            rc = e.send(q, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        std::printf("cuebench: %d cue jumps sent on %s%d (see the renderer's STATS: jumps / next-frame / worst)\n", count,
                    a[1].c_str(), n(2));
        return rc;
    }
    if (c == "soak") {
        // Cycles every transport feature on the loaded layers; the renderer's STATS lines are the result.
        const int secs = std::max(1, n(1));
        const uint64_t until = now_us() + static_cast<uint64_t>(secs) * 1000000;
        const int modes = static_cast<int>(PlayMode::Count);
        const int clips = static_cast<int>(cat.clip_count);
        if (clips <= 0) return 2;
        uint32_t rng = 777;
        auto rnd = [&](uint32_t m) {
            rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
            return rng % m;
        };
        int step = 0, rc = 0;
        while (now_us() < until && rc != 2) {
            const uint8_t d = static_cast<uint8_t>(rnd(2)), l = static_cast<uint8_t>(rnd(kLayersPerDeck));
            switch (step++ % 12) {
                case 0: rc = e.send(make(CommandType::LayerClip, d, l, static_cast<int32_t>(rnd(clips))), true); break;
                case 1: rc = e.send(layer_set(d, l, LayerParam::Mode, static_cast<int32_t>(rnd(modes)), 0), true); break;
                case 2: {
                    Command q = make(CommandType::LayerCue, d, l, 0);
                    q.slot = static_cast<uint8_t>(rnd(kCuePoints));
                    rc = e.send(q, true);
                    break;
                }
                case 3: rc = e.send(make(CommandType::LayerBeatLoop, d, l, 0, 0.25f * static_cast<float>(1 + rnd(4))), true); break;
                case 4: rc = e.send(make(CommandType::LayerBeatLoop, d, l, 0, 0.0f), true); break;
                case 5: rc = e.send(make(CommandType::LayerSpeed, d, l, 0, -2.0f + 0.5f * static_cast<float>(rnd(9))), true); break;
                case 6: rc = e.send(layer_set(d, l, LayerParam::Transition, static_cast<int32_t>(rnd(3)), 0), true); break;
                case 7: rc = e.send(make(CommandType::LayerTrigger, d, l, static_cast<int32_t>(rnd(clips)), 0.8f), true); break;
                case 8: rc = e.send(make(CommandType::LayerScratch, d, l, 1, static_cast<float>(rnd(100)) / 100.0f), true); break;
                case 9: rc = e.send(make(CommandType::LayerScratch, d, l, 0), true); break;
                case 10: rc = e.send(make(CommandType::LayerTrigger, kHitDeck, 0, static_cast<int32_t>(rnd(clips)), 0.6f), true); break;
                case 11: rc = e.send(make(CommandType::LayerTrigger, kHitDeck, 0, -1, 0.0f, kRelease), true); break;
            }
            if (rc == 3) rc = 0;  // a refusal (e.g. a cue on an empty layer) is part of the test
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        std::printf("soak: %d transport commands in %d s\n", step, secs);
        return rc;
    }
    if (c == "fx") {
        if (a.size() < 4) return 1;
        const int id = effect_id(a[3], cat);
        const int r = fx_cmd(CommandType::FxSet, kNone, id, 0);
        if (r != 0 || a.size() < 5 || id < 0) return r;
        const auto& lib = himmel::fx::builtin();
        if (id >= lib.count()) return 1;
        const auto& m = lib.meta(id);
        const int pr = m.find_preset(a[4].c_str());
        if (pr < 0) {
            std::printf("%s has no preset '%s' (", m.name, a[4].c_str());
            for (int k = 0; k < m.presets; ++k) std::printf("%s%s", k ? ", " : "", m.preset[k].name);
            std::printf(")\n");
            return 1;
        }
        for (int p = 0; p < m.params; ++p)
            if (const int rp = fx_cmd(CommandType::FxParam, static_cast<uint8_t>(p), 0, m.preset[pr].v[p]); rp != 0) return rp;
        return 0;
    }
    if (c == "react") {  // D131: react T S K|all SIGNAL [AMOUNT] [add|follow|trigger] [ATTACK RELEASE THRESHOLD] | default | off
        uint8_t deck = 0, layer = 0;
        if (a.size() < 5 || !fx_target(a[1], deck, layer) || n(2) < 1) return 1;
        const uint8_t slot = static_cast<uint8_t>(n(2) - 1);
        const uint8_t knob = a[3] == "all" ? kNone : static_cast<uint8_t>(n(3) - 1);
        if (knob != kNone && knob >= kReactParams) return 1;
        ReactArgs ra;
        if (a[4] == "default" || a[4] == "off") {
            ra.op = a[4] == "default" ? ReactOp::Default : ReactOp::Off;
            return e.send(make_react(deck, layer, slot, knob, ra));
        }
        himmel::fx::Signal sg;
        if (!himmel::fx::signal_from_name(a[4].c_str(), sg)) return 1;
        ra.signal = static_cast<ReactSignal>(sg);
        if (a.size() > 6) ra.mode = a[6] == "follow" ? ReactMode::Follow : a[6] == "trigger" ? ReactMode::Trigger : ReactMode::Add;
        const float amount = a.size() > 5 ? f(5) : 0.5f;
        const float att = a.size() > 7 ? f(7) : 10.0f, rel = a.size() > 8 ? f(8) : 200.0f, thr = a.size() > 9 ? f(9) : 0.0f;
        return e.send(make_react(deck, layer, slot, knob, ra, amount, att, rel, thr));
    }
    if (c == "press") return a.size() < 4 ? 1 : fx_cmd(CommandType::FxPress, kNone, 0, f(3));
    if (c == "param") return a.size() < 5 || n(3) < 1 ? 1 : fx_cmd(CommandType::FxParam, static_cast<uint8_t>(n(3) - 1), 0, f(4));
    if (c == "mix") return fx_cmd(CommandType::FxMix, kNone, 0, f(3));
    if (c == "bypass") return fx_cmd(CommandType::FxBypass, kNone, a.size() > 3 ? toggle_of(a[3]) : kToggle, 0);
    if (c == "raw") {
        if (a.size() < 8) return 1;
        Command r = make_fx(static_cast<CommandType>(n(1)), static_cast<uint8_t>(n(2)), static_cast<uint8_t>(n(3)),
                            static_cast<uint8_t>(n(4)), static_cast<uint8_t>(n(5)), n(6), f(7));
        return e.send(r);
    }
    if (c == "flood") {
        const int count = std::max(1, n(1));
        uint32_t pushed = 0;
        LiveState before{};
        e.live(before);
        // Keep the ring topped up for up to 2 s, so more than 4000 / s really arrive.
        const uint64_t until = now_us() + 2000000;
        for (int k = 0; k < count && now_us() < until;) {
            Command o = make(CommandType::LayerOpacity, 0, 0, 0, (k % 100) / 100.0f);
            o.seq = e.seq + 1;
            if (e.cmd->rings[kSlot].size() < kCommandRingSize && e.cmd->rings[kSlot].push(o)) {
                ++e.seq, ++pushed, ++k;
                e.cmd->producers[kSlot].heartbeat_us.store(now_us());
            } else {
                std::this_thread::yield();
            }
        }
        e.cmd->producers[kSlot].heartbeat_us.store(now_us());
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        LiveState l{};
        e.live(l);
        std::printf("flood: pushed %u of %d (ring dropped %u total); engine acked up to %u, refused %u more (last: %s)\n",
                    pushed, count, e.cmd->rings[kSlot].dropped.load(), l.acked_seq[kSlot],
                    l.rejected[kSlot] - before.rejected[kSlot],
                    himmel::engine::reject_name(static_cast<Reject>(l.last_reject[kSlot])));
        return 0;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) args.push_back("state");
    if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        usage();
        return 0;
    }
    auto e = std::make_unique<Engine>();
    if (!e->open_state()) {
        std::printf("no engine state block (%ls): start himmel_render / himmel.exe first\n", kStateShmName);
        return 2;
    }
    auto cat = std::make_unique<Catalog>();
    e->catalog(*cat);
    auto live = std::make_unique<LiveState>();

    auto tr = std::make_unique<TransportSection>();
    if (args[0] == "state") {
        if (!e->live(*live)) return 2;
        print_state(*live, *cat, e->transport(*tr) ? tr.get() : nullptr);
        return 0;
    }
    if (args[0] == "outputs") {
        ShmBlock ob;
        if (!ob.open_existing(kOutputsShmName, sizeof(OutputsBlock))) {
            std::printf("no outputs block (%ls)\n", kOutputsShmName);
            return 2;
        }
        const auto* b = static_cast<const OutputsBlock*>(ob.data());
        if (!himmel::engine::header_ok(b->header, kOutputsMagic, kOutputsVersion, static_cast<uint32_t>(sizeof(OutputsBlock)))) {
            std::printf("outputs block: another version\n");
            return 2;
        }
        auto sec = std::make_unique<OutputsSection>();
        if (!seq_read(b->seq, b->s, *sec)) return 2;
        const char* const st[] = {"off", "OK", "LOST", "window", "NEW DISPLAY?"};
        const char* const pr[] = {"720p", "1080p", "1440p", "4K", "custom"};
        const char* const pl[] = {"fill", "out", "fit", "free"};
        std::printf("outputs: %u %s, canvas %ux%u %s, blend %u px gamma %.2f, refresh %.2f Hz (measured %.1f fps), "
                    "%u displays, renderer pid %u%s\n",
                    sec->count, sec->fullscreen ? "full screen" : "window(s)", sec->canvas_w, sec->canvas_h,
                    sec->mode ? "mirror" : "span", sec->blend_px, sec->blend_gamma, sec->hz_target, sec->hz_measured,
                    sec->displays, b->status.pid.load(), sec->identify ? "  IDENTIFY" : "");
        for (uint32_t o = 0; o < sec->count && o < static_cast<uint32_t>(kMaxOutputs); ++o) {
            const auto& oi = sec->out[o];
            std::printf("  OUT %u  %-12s %-6s canvas x %5d %4ux%-4u  swap %4ux%-4u  %5.2f Hz  %s%s  \"%s\" %s %04X #%u %s  "
                        "rect %d,%d %d,%d  lost %u (back in %u ms)\n",
                        o + 1, oi.state < 5 ? st[oi.state] : "?", oi.preset < 5 ? pr[oi.preset] : "?", oi.canvas_x,
                        oi.width, oi.height, oi.swap_w, oi.swap_h, oi.hz, oi.black ? "BLACK " : "",
                        oi.blend_left > 0 || oi.blend_right > 0 ? "blend " : "", oi.name, oi.id.maker, oi.id.product,
                        oi.id.serial, oi.id.serial_text, oi.rect[0], oi.rect[1], oi.rect[2], oi.rect[3], oi.lost_count,
                        oi.returned_ms);
        }
        for (int i = 0; i < kPlaceSlots; ++i) {
            const auto& p = sec->layer[i];
            if (p.place == 0) continue;
            std::printf("  layer %c%d  %s%s  rect %.3f %.3f %.3f %.3f\n", i == 8 ? 'H' : 'A' + i / 4, i == 8 ? 1 : i % 4 + 1,
                        p.place < 4 ? pl[p.place] : "?", p.place == 1 ? (std::string(" ") + std::to_string(p.out + 1)).c_str() : "",
                        p.rect[0], p.rect[1], p.rect[2], p.rect[3]);
        }
        return 0;
    }
    if (args[0] == "clips" || args[0] == "playback") {  // wave 7: catalog v3 pages / frame smoothing
        if (!e->state) return 2;
        if (args[0] == "playback") {
            PlaybackSection pb{};
            if (!seq_read(e->state->playback_seq, e->state->playback, pb)) return 2;
            const char* sm[] = {"auto", "off", "blend", "even"};
            std::printf("frame smoothing %s, refresh %.2f Hz\n", sm[pb.smooth & 3], pb.out_hz);
            for (int s = 0; s < kDecks * kLayersPerDeck + 1; ++s) {
                const auto& l = pb.layer[s];
                if (l.clip_fps <= 0) continue;
                std::printf("  layer %c%d  %s -> %s%s  %.3f fps  %.3f refreshes/frame  judder %.3f  irregular %u\n",
                            s == 8 ? 'H' : 'A' + s / 4, s == 8 ? 1 : s % 4 + 1, l.smooth == kSmoothFollow ? "global" : sm[l.smooth & 3],
                            sm[l.in_use & 3], l.loading ? " (loading)" : "", l.clip_fps, l.refreshes_per_frame, l.judder,
                            l.irregular);
            }
            return 0;
        }
        auto copy = std::make_unique<ClipPagesCopy>();
        const int pages = read_clip_pages(e->state->clips, e->state->clip_page, *copy);
        const uint32_t from = args.size() > 1 ? static_cast<uint32_t>(std::max(0, std::atoi(args[1].c_str()))) : 0;
        const uint32_t n = args.size() > 2 ? static_cast<uint32_t>(std::max(1, std::atoi(args[2].c_str()))) : 64;
        const char* st[] = {"-", "ready", "MISSING", "REMOVED", "BUSY"};
        std::printf("%u clips (capacity %u, %d pages read, %u could not be taken), showing %u..%u:\n", copy->count,
                    e->state->clips.capacity, pages, copy->full, from, std::min(copy->count, from + n));
        for (uint32_t i = from; i < copy->count && i < from + n; ++i) {
            const auto& c = copy->entry(i);
            std::printf("  %4u  lib %-6lld %-32s %4ux%-4u %6.2f fps %7.1f s  %s\n", i, static_cast<long long>(c.library_id),
                        c.info.name, c.info.width, c.info.height, c.info.fps, c.info.seconds, c.state < 5 ? st[c.state] : "?");
        }
        return 0;
    }
    if (args[0] == "catalog") {
        std::printf("%u clips:\n", cat->clip_count);
        for (uint32_t i = 0; i < cat->clip_count; ++i) {
            const auto& c = cat->clip[i];
            std::printf("  %3u  %-32s %4ux%-4u %6.2f fps %7.1f s  codec %u  #%06X\n", i, c.name, c.width, c.height, c.fps,
                        c.seconds, c.codec, c.thumb_rgb);
        }
        std::printf("%u effects:\n", cat->effect_count);
        for (uint32_t i = 0; i < cat->effect_count; ++i) {
            const auto& f = cat->effect[i];
            std::printf("  %3u  %-12s", i, f.name);
            for (int p = 0; p < f.param_count; ++p) std::printf("  %d %s=%.2f", p + 1, f.param_name[p], f.param_default[p]);
            std::printf("\n");
        }
        return 0;
    }
    if (args[0] == "watch") {
        const int secs = args.size() > 1 ? std::max(1, std::atoi(args[1].c_str())) : 10;
        for (int i = 0; i < secs * 2; ++i) {
            if (e->live(*live)) print_state(*live, *cat, e->transport(*tr) ? tr.get() : nullptr);
            std::printf("\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 0;
    }
    if (!e->open_commands()) {
        std::printf("cannot open %ls (another version?)\n", kCommandShmName);
        return 2;
    }
    int rc = 0;
    std::vector<std::string> one;
    auto flush = [&]() {
        if (one.empty()) return;
        const int r = run_one(*e, one, *cat);
        if (r == 1) {
            std::printf("bad command:");
            for (auto& s : one) std::printf(" %s", s.c_str());
            std::printf("\n\n");
            usage();
        }
        rc = std::max(rc, r);
        one.clear();
    };
    for (auto& s : args) {
        if (s == "--") flush();
        else one.push_back(s);
    }
    flush();
    if (rc != 1 && e->live(*live)) print_state(*live, *cat, e->transport(*tr) ? tr.get() : nullptr);
    return rc;
}
