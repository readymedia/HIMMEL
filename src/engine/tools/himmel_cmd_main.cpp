// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_cmd.exe - send commands to the engine from the command line (producer
// slot 3 "Tool", docs/interfaces/command-protocol.md) and print the live state.
//
//   himmel_cmd state                       the show on air (default)
//   himmel_cmd catalog                     clips and effects
//   himmel_cmd take [ms] | cut | black [on|off|toggle]
//   himmel_cmd xfade V | nudge DELTA       crossfader 0 (A) .. 1 (B)
//   himmel_cmd clip D L ID                 D = a|b|0|1, L = 1..4, ID = clip id or -1
//   himmel_cmd opacity D L V | speed D L V | jump D L V
//   himmel_cmd blend D L normal|add|screen|multiply|difference|lighten
//   himmel_cmd mute D L [on|off|toggle] | play D L pause|play|toggle|restart
//   himmel_cmd fx T S EFFECT               T = a1..a4 / b1..b4 (layer), a / b (deck), m (master);
//                                          S = slot 1..4; EFFECT = id, name or -1
//   himmel_cmd param T S P V | mix T S V | bypass T S [on|off|toggle]   P = 1..8
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
#include "himmel/protocol/commands.h"
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
        "  state | catalog | watch [s]\n"
        "  take [ms] | cut | black [on|off|toggle] | xfade V | nudge DELTA\n"
        "  clip D L ID | opacity D L V | speed D L V | jump D L V | blend D L MODE\n"
        "  mute D L [on|off|toggle] | play D L pause|play|toggle|restart | focus D L\n"
        "  fx T S EFFECT | param T S P V | mix T S V | bypass T S [on|off|toggle]\n"
        "  raw TYPE DECK LAYER SLOT PARAM I F | hello | flood N\n"
        "  set D L PARAM VALUE | trigger D L ID [VEL] [up] | cue D L K jump|set [V]|clear\n"
        "  beatloop D L BEATS | scratch D L grab|release|move V|rel S | play D L release|stop\n"
        "  tap | bpm V | grid off|1/4|1|bar|2bar|4bar | resync | phase BEATS | cuebench D L N [MS] | soak S\n"
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
    {"seed", LayerParam::Seed, false, nullptr},
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
    if (c == "take") return e.send(make(CommandType::Take, kNone, kNone, n(1)));
    if (c == "cut") return e.send(make(CommandType::Cut));
    if (c == "black") return e.send(make(CommandType::Black, kNone, kNone, a.size() > 1 ? toggle_of(a[1]) : kToggle));
    if (c == "xfade") return e.send(make(CommandType::Crossfader, kNone, kNone, 0, f(1)));
    if (c == "nudge") return e.send(make(CommandType::Crossfader, kNone, kNone, 0, f(1), kRelative));
    if (c == "hello") return e.send(make(CommandType::Hello, kNone, kNone, static_cast<int32_t>(GetCurrentProcessId())));
    if (c == "clip") return layer_cmd(CommandType::LayerClip, n(3), 0);
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
            if (p.p == LayerParam::Snap && a[4] == "global") iv = -1;
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
    if (c == "fx") return a.size() < 4 ? 1 : fx_cmd(CommandType::FxSet, kNone, effect_id(a[3], cat), 0);
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
