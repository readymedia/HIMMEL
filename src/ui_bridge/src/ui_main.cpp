// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_ui: the bridge between the web GUI (gui/) and the engine (D43).
//
//   GET  /             the web app (static files from gui/)
//   GET  /events       Server-Sent Events: "bridge" (1 Hz + on change), "catalog" (on change),
//                      "live" (~30 Hz, only when the model changed)
//   POST /cmd          JSON command (or array) -> validated Command on producer slot 1 (Gui)
//   GET  /preview/N    newest picture of preview stream N (0 DeckA, 1 DeckB, 2 Main, 3..5 outputs):
//                      16-byte header ("HPV1", u16 width, u16 height, u32 reserved, u32 published-low)
//                      followed by width*height RGBA bytes; 204 if nothing new (?since=published) or inactive
//   GET  /state        everything as one JSON (debugging)
//
// Rules (docs/interfaces/*.md, gui.md §1.5): the bridge never blocks the
// engine and never takes it down. Engine absent / restarted / wrong version
// only changes what the "bridge" event says; the bridge keeps retrying.
// Killing the bridge changes nothing on the outputs.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "himmel/ui/json_codec.h"
#include "himmel/ui/shm_win.h"
#include "http_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "himmel/mk2/shared.h"
#include "himmel/protocol/preview.h"

namespace fs = std::filesystem;
using namespace himmel::protocol;
using himmel::ui::ShmStatus;
using himmel::ui::ShmView;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
std::atomic<int> g_connections{0};
constexpr int kMaxConnections = 64;

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::printf("%02d:%02d:%02d.%03d [himmel_ui] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    std::fflush(stdout);
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

// ---------------------------------------------------------------------------
// The hub: the newest JSON texts, shared by the poller and the SSE clients.
struct Hub {
    std::mutex m;
    std::condition_variable cv;
    std::string live, catalog, bridge, transport, mk2;
    uint64_t live_ver = 0, catalog_ver = 0, bridge_ver = 0, transport_ver = 0, mk2_ver = 0;

    void set(std::string& slot, uint64_t& ver, std::string text) {
        {
            std::lock_guard lk(m);
            if (slot == text) return;
            slot = std::move(text);
            ++ver;
        }
        cv.notify_all();
    }
};
Hub g_hub;

// ---------------------------------------------------------------------------
// The engine blocks. The poller (re)opens them; request threads read them
// under a shared lock, so a block is never unmapped under a reader.
struct Engine {
    std::shared_mutex views;     // unique: poller (re)opening; shared: readers
    ShmView state, preview, commands;
    ShmStatus state_st = ShmStatus::Absent, preview_st = ShmStatus::Absent, commands_st = ShmStatus::Absent;

    std::mutex push_m;           // the ring is SPSC: this mutex makes the bridge one producer
    uint32_t seq = 0;
    uint64_t pushed = 0, refused_local = 0;

    std::atomic<int> clips{-1}, effects{-1};  // catalog limits for local validation
};
Engine g_eng;

const char* engine_word(ShmStatus st, bool alive, bool seen_beat) {
    if (st == ShmStatus::Version) return "version";
    if (st != ShmStatus::Ok) return "absent";
    if (alive) return "ok";
    return seen_beat ? "stale" : "starting";
}

// Push one command on slot 1. Never blocks; a full ring is counted.
bool push_command(Command c, uint32_t& seq_out, std::string& err) {
    std::shared_lock vl(g_eng.views);
    std::lock_guard pl(g_eng.push_m);
    if (g_eng.commands_st != ShmStatus::Ok) {
        err = "the command block is not there (engine not started yet)";
        return false;
    }
    auto* blk = g_eng.commands.as<CommandBlock>();
    const int slot = static_cast<int>(Producer::Gui);
    c.seq = ++g_eng.seq;
    if (!blk->rings[slot].push(c)) {
        err = "the command ring is full (engine not reading)";
        return false;
    }
    blk->producers[slot].heartbeat_us.store(now_us(), std::memory_order_release);
    ++g_eng.pushed;
    seq_out = c.seq;
    return true;
}

// ---------------------------------------------------------------------------
// The MK2 mirror (gui.md §4.5): what the engine sends to the MK2 (LEDs, both
// screens) and what the hands do on it (buttons, knobs, pads), read from the
// controller's block "Local\HimmelMk2". Read-only except for one consumer ring:
// GUESS: ring 3 belongs to the GUI bridge (ring 0 = engine; mk2_monitor and
// mk2_pages_demo take the first free one, normally 1). If the bridge dies the
// ring stays "attached" and fills up; the controller then counts drops on it and
// nothing else changes. A restarted bridge attaches again (resets the tail).
constexpr int kMk2GuiRing = 3;

struct Mk2Mirror {
    HANDLE map = nullptr;
    himmel::mk2::SharedBlock* blk = nullptr;
    bool attached = false;
    uint64_t last_beat = 0;
    Clock::time_point last_beat_change{};
    uint64_t pressed = 0;                          // bit = himmel::mk2::Button
    int pads[himmel::mk2::kPads] = {};             // pressure 0..4095 by pad number - 1
    int knobs[himmel::mk2::kKnobs] = {};           // accumulated raw units (one turn ~1008)
    int encoder = 0;                               // accumulated detents
    int last_knob = -1;
    uint32_t led_seq = 0, scr_seq[2] = {};
    himmel::mk2::LedState leds{};
    uint8_t screen[2][himmel::mk2::kScreenBytes] = {};

    void close() {
        if (blk && attached) blk->rings[kMk2GuiRing].detach();
        if (blk) UnmapViewOfFile(blk);
        if (map) CloseHandle(map);
        blk = nullptr;
        map = nullptr;
        attached = false;
    }
    bool open() {
        close();
        map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, himmel::mk2::kShmName);
        if (!map) return false;
        blk = static_cast<himmel::mk2::SharedBlock*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!blk) {
            close();
            return false;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(blk, &mbi, sizeof mbi) == 0 || mbi.RegionSize < sizeof(himmel::mk2::SharedBlock) ||
            blk->magic != himmel::mk2::kShmMagic || blk->version != himmel::mk2::kShmVersion ||
            blk->size != sizeof(himmel::mk2::SharedBlock)) {
            close();
            return false;
        }
        blk->rings[kMk2GuiRing].attach();
        attached = true;
        led_seq = scr_seq[0] = scr_seq[1] = 0;
        return true;
    }
};
Mk2Mirror g_mk2;

std::string base64(const uint8_t* p, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

// One poll of the MK2 block; returns the JSON text of the mirror.
std::string poll_mk2(Clock::time_point now, bool retry) {
    using namespace himmel::mk2;
    Mk2Mirror& m = g_mk2;
    if (!m.blk && retry) m.open();
    if (!m.blk) return R"({"present":false})";
    if (m.blk->magic != kShmMagic) {  // the controller made a new block (other version) or went away
        m.close();
        return R"({"present":false})";
    }
    const uint64_t beat = m.blk->status.heartbeat_us.load(std::memory_order_acquire);
    if (beat != m.last_beat) {
        m.last_beat = beat;
        m.last_beat_change = now;
    }
    const bool alive = now - m.last_beat_change < std::chrono::milliseconds(1500);
    Event e{};
    int n = 0;
    while (n < 4096 && m.blk->rings[kMk2GuiRing].pop(e)) {
        ++n;
        switch (e.type) {
            case EventType::ButtonDown: if (e.id < kButtons) m.pressed |= 1ull << e.id; break;
            case EventType::ButtonUp: if (e.id < kButtons) m.pressed &= ~(1ull << e.id); break;
            case EventType::KnobDelta: if (e.id < kKnobs) { m.knobs[e.id] += e.value; m.last_knob = e.id; } break;
            case EventType::EncoderDelta: m.encoder += e.value; break;
            case EventType::PadHit:
            case EventType::PadPressure:
                if (e.id >= 1 && e.id <= kPads) m.pads[e.id - 1] = e.type == EventType::PadHit ? std::max<int>(e.raw, 1) : e.value;
                break;
            case EventType::PadRelease: if (e.id >= 1 && e.id <= kPads) m.pads[e.id - 1] = 0; break;
            case EventType::Disconnected: m.pressed = 0; std::fill(std::begin(m.pads), std::end(m.pads), 0); break;
            default: break;
        }
    }
    if (const uint32_t s = himmel::mk2::seq_read(m.blk->outputs.led_seq, m.blk->outputs.leds, m.leds)) m.led_seq = s;
    for (int k = 0; k < 2; ++k)
        if (const uint32_t s = himmel::mk2::seq_read_bytes(m.blk->outputs.screen_seq[k], m.blk->outputs.screen[k], m.screen[k], kScreenBytes))
            m.scr_seq[k] = s;

    json j;
    j["present"] = true;
    j["connected"] = m.blk->status.connected.load() != 0;
    j["alive"] = alive;
    j["map_verified"] = m.blk->status.map_verified.load() != 0;
    j["dropped"] = m.blk->rings[kMk2GuiRing].dropped.load();
    json pressed = json::array();
    for (int b = 0; b < kButtons; ++b)
        if (m.pressed & (1ull << b)) pressed.push_back(std::string(kButtonNames[b]));
    j["pressed"] = std::move(pressed);
    j["pads"] = m.pads;
    json kn = json::array();
    for (int k = 0; k < kKnobs; ++k) kn.push_back(((m.knobs[k] % 1008) + 1008) % 1008 / 1008.0);
    j["knobs"] = std::move(kn);
    j["last_knob"] = m.last_knob;
    j["encoder"] = m.encoder;
    json pad_rgb = json::array(), grp = json::array();
    for (int k = 0; k < kPads; ++k) pad_rgb.push_back({m.leds.pad_rgb[k][0], m.leds.pad_rgb[k][1], m.leds.pad_rgb[k][2]});
    for (int k = 0; k < 8; ++k) grp.push_back({m.leds.group_rgb[k][0][0], m.leds.group_rgb[k][0][1], m.leds.group_rgb[k][0][2]});
    json btn = json::object();
    for (int k = 0; k < kButtonLeds; ++k) btn[std::string(kButtonLedNames[k])] = m.leds.buttons[k];
    const char* const tnames[8] = {"Restart", "Transport <", "Transport >", "Grid", "Play", "Rec", "Erase", "Shift"};
    for (int k = 0; k < 8; ++k) btn[tnames[k]] = m.leds.transport[k];
    j["leds"] = {{"pad", std::move(pad_rgb)}, {"group", std::move(grp)}, {"button", std::move(btn)}};
    j["screen"] = base64(&m.screen[0][0], sizeof m.screen);
    return dump(j);
}

// ---------------------------------------------------------------------------
// The poller: ~30 Hz. Opens blocks (retry every second), reads live +
// catalog, publishes JSON, sends Hello every second.
void poller() {
    const DWORD pid = GetCurrentProcessId();
    auto last_try = Clock::now() - std::chrono::seconds(10);
    auto last_hello = Clock::now() - std::chrono::seconds(10);
    auto last_bridge = Clock::now() - std::chrono::seconds(10);
    auto last_mk2_try = Clock::now() - std::chrono::seconds(10);
    auto last_beat_change = Clock::now();
    uint64_t last_beat = 0;
    bool seen_beat = false;
    uint32_t last_catalog_seq = 0, last_epoch = 0;
    std::string last_state_word, last_cmd_word, last_prev_word;
    auto live = std::make_unique<LiveState>();
    auto cat = std::make_unique<Catalog>();
    auto trans = std::make_unique<TransportSection>();
    uint64_t frame = 0;
    float fps = 0;
    uint32_t engine_pid = 0;
    bool alive = false;

    while (!g_quit.load()) {
        const auto now = Clock::now();

        // ---- (re)open blocks ----
        {
            const bool retry = now - last_try >= std::chrono::seconds(1);
            std::unique_lock vl(g_eng.views, std::defer_lock);
            // Recheck held headers every tick (cheap); reopen at most once a second.
            auto recheck = [&](ShmView& v, ShmStatus& st, size_t size, uint32_t magic, uint32_t ver) {
                if (st == ShmStatus::Ok && v.check(size, magic, ver) != ShmStatus::Ok) {
                    if (!vl.owns_lock()) vl.lock();
                    v.close();
                    st = ShmStatus::Version;
                }
            };
            recheck(g_eng.state, g_eng.state_st, sizeof(StateBlock), kStateMagic, kStateVersion);
            recheck(g_eng.preview, g_eng.preview_st, sizeof(PreviewBlock), kPreviewMagic, kPreviewVersion);
            recheck(g_eng.commands, g_eng.commands_st, sizeof(CommandBlock), kCommandMagic, kCommandVersion);
            if (retry) {
                last_try = now;
                if (g_eng.state_st != ShmStatus::Ok) {
                    if (!vl.owns_lock()) vl.lock();
                    g_eng.state_st = g_eng.state.open_existing(kStateShmName, sizeof(StateBlock), kStateMagic,
                                                                kStateVersion, false);
                    if (g_eng.state_st != ShmStatus::Ok) g_eng.state.close();
                    seen_beat = false;
                    last_beat_change = now;
                }
                if (g_eng.preview_st != ShmStatus::Ok) {
                    if (!vl.owns_lock()) vl.lock();
                    g_eng.preview_st = g_eng.preview.open_existing(kPreviewShmName, sizeof(PreviewBlock),
                                                                    kPreviewMagic, kPreviewVersion, false);
                    if (g_eng.preview_st != ShmStatus::Ok) g_eng.preview.close();
                }
                if (g_eng.commands_st != ShmStatus::Ok) {
                    if (!vl.owns_lock()) vl.lock();
                    std::lock_guard pl(g_eng.push_m);
                    ShmStatus st = g_eng.commands.create_or_open(kCommandShmName, sizeof(CommandBlock),
                                                                 kCommandMagic, kCommandVersion);
                    if (st == ShmStatus::Ok && g_eng.commands.created()) {
                        g_eng.commands.write_header(sizeof(CommandBlock), kCommandMagic, kCommandVersion);
                        logf("created %ls", kCommandShmName);
                    }
                    if (st == ShmStatus::Ok) {
                        auto* blk = g_eng.commands.as<CommandBlock>();
                        blk->producers[static_cast<int>(Producer::Gui)].pid.store(pid);
                        last_hello = now - std::chrono::seconds(10);
                    } else {
                        g_eng.commands.close();
                    }
                    g_eng.commands_st = st;
                }
            }
        }

        // ---- read state ----
        {
            std::shared_lock vl(g_eng.views);
            if (g_eng.state_st == ShmStatus::Ok) {
                const auto* sb = g_eng.state.as<const StateBlock>();
                const uint64_t beat = sb->status.heartbeat_us.load(std::memory_order_acquire);
                engine_pid = sb->status.pid.load(std::memory_order_acquire);
                if (beat != last_beat) {
                    last_beat = beat;
                    last_beat_change = now;
                    seen_beat = true;
                }
                alive = seen_beat && now - last_beat_change < std::chrono::milliseconds(1500);
                if (seq_read(sb->live_seq, sb->live, *live)) {
                    frame = live->frame;
                    fps = live->fps;
                    g_hub.set(g_hub.live, g_hub.live_ver, dump(himmel::ui::live_to_json(*live)));
                    // State v2: the transport section (read after live, engine-state.md §2).
                    if (seq_read(sb->transport_seq, sb->transport, *trans)) {
                        json tj = himmel::ui::transport_to_json(*trans);
                        tj["clock"]["beat"] = himmel::ui::clock_to_json(trans->clock, now_us())["beat"];
                        g_hub.set(g_hub.transport, g_hub.transport_ver, dump(tj));
                    }
                    const uint32_t cs = sb->catalog_seq.load(std::memory_order_acquire);
                    if (cs != last_catalog_seq || live->epoch != last_epoch) {
                        if (seq_read(sb->catalog_seq, sb->catalog, *cat)) {
                            last_catalog_seq = cs;
                            g_eng.clips.store(static_cast<int>(std::min<uint32_t>(cat->clip_count, kMaxClips)));
                            g_eng.effects.store(static_cast<int>(std::min<uint32_t>(cat->effect_count, kMaxEffects)));
                            g_hub.set(g_hub.catalog, g_hub.catalog_ver, dump(himmel::ui::catalog_to_json(*cat)));
                            if (live->epoch != last_epoch) logf("engine epoch %u", live->epoch);
                            last_epoch = live->epoch;
                        }
                    }
                }
            } else {
                alive = false;
            }
        }

        // ---- MK2 mirror (retry opening once a second) ----
        g_hub.set(g_hub.mk2, g_hub.mk2_ver, poll_mk2(now, now - last_mk2_try >= std::chrono::seconds(1)));
        if (now - last_mk2_try >= std::chrono::seconds(1)) last_mk2_try = now;

        // ---- Hello ----
        if (now - last_hello >= std::chrono::seconds(1)) {
            last_hello = now;
            uint32_t s = 0;
            std::string err;
            if (g_eng.commands_st == ShmStatus::Ok)
                push_command(make(CommandType::Hello, kNone, kNone, static_cast<int32_t>(pid)), s, err);
        }

        // ---- bridge status ----
        const char* sw = engine_word(g_eng.state_st, alive, seen_beat);
        const char* cw = himmel::ui::shm_status_name(g_eng.commands_st);
        const char* pw = himmel::ui::shm_status_name(g_eng.preview_st);
        const bool changed = last_state_word != sw || last_cmd_word != cw || last_prev_word != pw;
        if (changed) {
            logf("engine %s, commands %s, preview %s", sw, cw, pw);
            last_state_word = sw;
            last_cmd_word = cw;
            last_prev_word = pw;
        }
        if (changed || now - last_bridge >= std::chrono::seconds(1)) {
            last_bridge = now;
            uint32_t dropped = 0, gui_seq = 0;
            uint64_t pushed = 0;
            {
                std::shared_lock vl(g_eng.views);
                std::lock_guard pl(g_eng.push_m);
                if (g_eng.commands_st == ShmStatus::Ok)
                    dropped = g_eng.commands.as<CommandBlock>()->rings[static_cast<int>(Producer::Gui)].dropped.load();
                gui_seq = g_eng.seq;
                pushed = g_eng.pushed;
            }
            json b = {{"engine", sw},
                      {"commands", cw},
                      {"preview", pw},
                      {"engine_pid", engine_pid},
                      {"bridge_pid", pid},
                      {"epoch", last_epoch},
                      {"frame", frame},
                      {"fps", std::round(fps * 10.f) / 10.f},
                      {"seq", gui_seq},
                      {"pushed", pushed},
                      {"dropped", dropped},
                      {"versions", {{"command", kCommandVersion}, {"state", kStateVersion}, {"preview", kPreviewVersion}}},
                      {"t", now_us() / 1000000}};
            g_hub.set(g_hub.bridge, g_hub.bridge_ver, dump(b));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// ---------------------------------------------------------------------------
// HTTP
fs::path g_gui_dir;

bool send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        const int k = send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}
bool send_str(SOCKET s, const std::string& t) { return send_all(s, t.data(), t.size()); }

bool respond(SOCKET s, int code, const char* status, const char* ctype, const std::string& body, bool keep,
             const char* extra = "") {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\nContent-Type: " + ctype +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: " + (keep ? "keep-alive" : "close") + "\r\n" + extra +
                    "\r\n";
    return send_str(s, h) && send_all(s, body.data(), body.size());
}

void handle_events(SOCKET s) {
    const char* head =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
        "Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\nretry: 1000\n\n";
    if (!send_str(s, head)) return;
    uint64_t lv = 0, cv = 0, bv = 0, tv = 0, mv = 0;
    auto last_send = Clock::now();
    while (!g_quit.load()) {
        std::string out;
        {
            std::unique_lock lk(g_hub.m);
            g_hub.cv.wait_for(lk, std::chrono::seconds(1), [&] {
                return g_quit.load() || g_hub.live_ver != lv || g_hub.catalog_ver != cv || g_hub.bridge_ver != bv ||
                       g_hub.transport_ver != tv || g_hub.mk2_ver != mv;
            });
            // Order: bridge, catalog, live (the page needs the catalog before the live view).
            if (g_hub.bridge_ver != bv) {
                bv = g_hub.bridge_ver;
                out += "event: bridge\ndata: " + g_hub.bridge + "\n\n";
            }
            if (g_hub.catalog_ver != cv) {
                cv = g_hub.catalog_ver;
                if (!g_hub.catalog.empty()) out += "event: catalog\ndata: " + g_hub.catalog + "\n\n";
            }
            if (g_hub.live_ver != lv) {
                lv = g_hub.live_ver;
                if (!g_hub.live.empty()) out += "event: live\ndata: " + g_hub.live + "\n\n";
            }
            if (g_hub.transport_ver != tv) {
                tv = g_hub.transport_ver;
                if (!g_hub.transport.empty()) out += "event: transport\ndata: " + g_hub.transport + "\n\n";
            }
            if (g_hub.mk2_ver != mv) {
                mv = g_hub.mk2_ver;
                if (!g_hub.mk2.empty()) out += "event: mk2\ndata: " + g_hub.mk2 + "\n\n";
            }
        }
        if (out.empty() && Clock::now() - last_send > std::chrono::seconds(5)) out = ": ping\n\n";
        if (!out.empty()) {
            if (!send_str(s, out)) return;
            last_send = Clock::now();
        }
    }
}

std::string handle_cmd_body(const std::string& body) {
    json in = json::parse(body, nullptr, false);
    if (in.is_discarded()) return dump(json{{"ok", false}, {"error", "the body is not valid JSON"}});
    const himmel::ui::CatalogLimits lim{g_eng.clips.load(), g_eng.effects.load()};
    auto one = [&](const json& j) -> json {
        const auto r = himmel::ui::command_from_json(j, lim);
        if (!r.ok) {
            std::lock_guard pl(g_eng.push_m);
            ++g_eng.refused_local;
            return {{"ok", false}, {"error", r.error}};
        }
        uint32_t seq = 0;
        std::string err;
        if (!push_command(r.cmd, seq, err)) return {{"ok", false}, {"error", err}};
        return {{"ok", true}, {"seq", seq}};
    };
    if (in.is_array()) {
        if (in.size() > 256) return dump(json{{"ok", false}, {"error", "at most 256 commands per request"}});
        json out = json::array();
        for (const auto& j : in) out.push_back(one(j));
        return dump(out);
    }
    return dump(one(in));
}

// Copies the newest picture of a stream into `buf` as header + RGBA.
// Returns 200, 204 (nothing new / inactive) or 404.
int read_preview(int stream, uint32_t since, std::vector<uint8_t>& buf, std::vector<uint8_t>& raw) {
    if (stream < 0 || stream >= kPreviewStreams) return 404;
    std::shared_lock vl(g_eng.views);
    if (g_eng.preview_st != ShmStatus::Ok) return 204;
    const auto* pb = g_eng.preview.as<const PreviewBlock>();
    const PreviewStreamBlock& sb = pb->stream[stream];
    if (!sb.active.load(std::memory_order_acquire)) return 204;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t pub = sb.published.load(std::memory_order_acquire);
        if (pub == 0 || static_cast<uint32_t>(pub) == since) return 204;
        const uint32_t latest = sb.latest.load(std::memory_order_acquire);
        if (latest >= static_cast<uint32_t>(kPreviewBuffers)) return 204;
        const uint32_t w = std::min(sb.width, kPreviewW), h = std::min(sb.height, kPreviewH);
        if (w == 0 || h == 0) return 204;
        const size_t n = static_cast<size_t>(w) * h * 4;
        raw.resize(n);
        if (!seq_read_bytes(sb.buffer[latest].seq, sb.buffer[latest].pixels, raw.data(), n)) continue;
        buf.resize(16 + n);
        std::memcpy(buf.data(), "HPV1", 4);
        const uint16_t w16 = static_cast<uint16_t>(w), h16 = static_cast<uint16_t>(h);
        const uint32_t pub32 = static_cast<uint32_t>(pub), zero = 0;
        std::memcpy(buf.data() + 4, &w16, 2);
        std::memcpy(buf.data() + 6, &h16, 2);
        std::memcpy(buf.data() + 8, &zero, 4);
        std::memcpy(buf.data() + 12, &pub32, 4);
        uint8_t* d = buf.data() + 16;
        const uint8_t* s = raw.data();
        for (size_t k = 0; k < n; k += 4) {  // BGRA -> RGBA, opaque
            d[k] = s[k + 2];
            d[k + 1] = s[k + 1];
            d[k + 2] = s[k];
            d[k + 3] = 255;
        }
        return 200;
    }
    return 204;
}

std::string state_json() {
    std::lock_guard lk(g_hub.m);
    return "{\"bridge\":" + (g_hub.bridge.empty() ? "null" : g_hub.bridge) +
           ",\"catalog\":" + (g_hub.catalog.empty() ? "null" : g_hub.catalog) +
           ",\"live\":" + (g_hub.live.empty() ? "null" : g_hub.live) + "}";
}

fs::path g_repo_dir;  // the folder that holds gui/, fx/, weapons/

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// GET /fxmeta: fx/catalog.json order + every fx/<id>/effect.json (effect-metadata.md §2), so the
// browser has units, presets, PRESS, help, family and cost. Read per request (small, rare).
// The page matches entries to the engine catalog by position and checks the name.
std::string fx_meta_json() {
    const fs::path fx = g_repo_dir / "fx";
    json cat = json::parse(read_file(fx / "catalog.json"), nullptr, false);
    json out = {{"ok", false}, {"effects", json::array()}};
    if (cat.is_discarded() || !cat.contains("order") || !cat["order"].is_array()) {
        out["error"] = "fx/catalog.json not found or not valid";
        return dump(out);
    }
    for (const auto& id : cat["order"]) {
        if (!id.is_string()) continue;
        const std::string sid = id.get<std::string>();
        if (!himmel::ui::safe_static_path("/" + sid)) continue;
        json e = json::parse(read_file(fx / sid / "effect.json"), nullptr, false);
        if (e.is_discarded()) e = json{{"id", sid}, {"error", "effect.json missing or not valid"}};
        out["effects"].push_back(std::move(e));
    }
    out["ok"] = true;
    return dump(out);
}

bool serve_static(SOCKET s, const std::string& path, bool keep) {
    std::string rel = path == "/" ? "/index.html" : path;
    if (!himmel::ui::safe_static_path(rel)) return respond(s, 400, "Bad Request", "text/plain", "bad path", keep);
    const fs::path p = g_gui_dir / fs::path(rel.substr(1));
    std::ifstream f(p, std::ios::binary);
    if (!f) return respond(s, 404, "Not Found", "text/plain", "not found: " + rel, keep);
    std::ostringstream ss;
    ss << f.rdbuf();
    return respond(s, 200, "OK", himmel::ui::content_type_for(rel), ss.str(), keep);
}

void connection(SOCKET s) {
    struct Guard {
        SOCKET s;
        ~Guard() {
            closesocket(s);
            g_connections.fetch_sub(1);
        }
    } guard{s};
    DWORD tmo = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);

    std::string in;
    std::vector<uint8_t> pic, raw;
    char buf[8192];
    while (!g_quit.load()) {
        size_t end;
        while ((end = in.find("\r\n\r\n")) == std::string::npos) {
            if (in.size() > 16384) return;
            const int k = recv(s, buf, sizeof buf, 0);
            if (k <= 0) return;
            in.append(buf, static_cast<size_t>(k));
        }
        himmel::ui::HttpRequest req;
        if (!himmel::ui::parse_http_head(std::string_view(in).substr(0, end + 2), req)) {
            respond(s, 400, "Bad Request", "text/plain", "bad request", false);
            return;
        }
        if (req.content_length > 256 * 1024) {
            respond(s, 413, "Payload Too Large", "text/plain", "too large", false);
            return;
        }
        std::string body = in.substr(end + 4);
        while (body.size() < req.content_length) {
            const int k = recv(s, buf, sizeof buf, 0);
            if (k <= 0) return;
            body.append(buf, static_cast<size_t>(k));
        }
        in = body.substr(req.content_length);
        body.resize(req.content_length);
        const bool keep = req.keep_alive;

        if (!himmel::ui::host_is_local(req.host) || !himmel::ui::origin_is_local(req.origin)) {
            respond(s, 403, "Forbidden", "text/plain", "himmel_ui only answers pages on this PC", false);
            return;
        }
        bool ok = true;
        if (req.method == "GET" && req.path == "/events") {
            handle_events(s);
            return;
        } else if (req.method == "POST" && req.path == "/cmd") {
            if (req.content_type.find("application/json") != 0)
                ok = respond(s, 415, "Unsupported Media Type", "text/plain", "send application/json", keep);
            else
                ok = respond(s, 200, "OK", "application/json", handle_cmd_body(body), keep);
        } else if (req.method == "GET" && req.path.rfind("/preview/", 0) == 0) {
            const int stream = std::atoi(req.path.c_str() + 9);
            const uint32_t since = static_cast<uint32_t>(std::strtoul(himmel::ui::query_param(req.query, "since").c_str(), nullptr, 10));
            const int code = read_preview(stream, since, pic, raw);
            if (code == 200) {
                std::string h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                                std::to_string(pic.size()) + "\r\nCache-Control: no-store\r\nConnection: " +
                                (keep ? "keep-alive" : "close") + "\r\n\r\n";
                ok = send_str(s, h) && send_all(s, reinterpret_cast<const char*>(pic.data()), pic.size());
            } else if (code == 204) {
                ok = send_str(s, std::string("HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nConnection: ") +
                                     (keep ? "keep-alive" : "close") + "\r\n\r\n");
            } else {
                ok = respond(s, 404, "Not Found", "text/plain", "no such stream", keep);
            }
        } else if (req.method == "GET" && req.path == "/state") {
            ok = respond(s, 200, "OK", "application/json", state_json(), keep);
        } else if (req.method == "GET" && req.path == "/names") {
            ok = respond(s, 200, "OK", "application/json", dump(himmel::ui::transport_names()), keep);
        } else if (req.method == "GET" && req.path == "/fxmeta") {
            ok = respond(s, 200, "OK", "application/json", fx_meta_json(), keep);
        } else if (req.method == "GET" && req.path.rfind("/weapons/", 0) == 0) {
            // Weapon profiles only (weapons/<id>.weapon.json): the MK2 picture is drawn from its profile.
            const std::string name = req.path.substr(9);
            const bool okname = name.size() > 12 && name.find('/') == std::string::npos &&
                                name.compare(name.size() - 12, 12, ".weapon.json") == 0 &&
                                himmel::ui::safe_static_path("/" + name);
            const std::string prof = okname ? read_file(g_repo_dir / "weapons" / name) : std::string();
            ok = prof.empty() ? respond(s, 404, "Not Found", "text/plain", "no such weapon profile", keep)
                              : respond(s, 200, "OK", "application/json", prof, keep);
        } else if (req.method == "GET") {
            ok = serve_static(s, req.path, keep);
        } else {
            ok = respond(s, 405, "Method Not Allowed", "text/plain", "method not allowed", keep);
        }
        if (!ok || !keep) return;
    }
}

fs::path find_gui_dir(const fs::path& exe_dir) {
    fs::path d = exe_dir;
    for (int up = 0; up < 6; ++up) {
        std::error_code ec;
        if (fs::exists(d / "gui" / "index.html", ec)) return d / "gui";
        if (!d.has_parent_path() || d.parent_path() == d) break;
        d = d.parent_path();
    }
    return {};
}

BOOL WINAPI on_ctrl(DWORD) {
    g_quit.store(true);
    g_hub.cv.notify_all();
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8790;
    fs::path gui;
    for (int k = 1; k < argc; ++k) {
        const std::string a = argv[k];
        if (a == "--port" && k + 1 < argc) port = std::atoi(argv[++k]);
        else if (a == "--gui" && k + 1 < argc) gui = argv[++k];
        else if (a == "--help" || a == "-h") {
            std::printf("himmel_ui [--port 8790] [--gui DIR]\n  Serves the web GUI on http://127.0.0.1:PORT\n");
            return 0;
        }
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (gui.empty()) gui = find_gui_dir(fs::path(exe).parent_path());
    if (gui.empty()) gui = find_gui_dir(fs::current_path());
    if (gui.empty() || !fs::exists(gui / "index.html")) {
        logf("cannot find the gui/ folder (index.html); use --gui DIR");
        return 2;
    }
    g_gui_dir = gui;
    g_repo_dir = fs::absolute(gui).parent_path();
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logf("WSAStartup failed");
        return 2;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(ls, SOMAXCONN) != 0) {
        logf("cannot listen on 127.0.0.1:%d (is another himmel_ui running?) error %d", port, WSAGetLastError());
        return 2;
    }
    logf("serving %s on http://127.0.0.1:%d", g_gui_dir.string().c_str(), port);

    std::thread poll(poller);
    while (!g_quit.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        timeval tv{0, 250000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        if (g_connections.load() >= kMaxConnections) {
            closesocket(c);
            continue;
        }
        g_connections.fetch_add(1);
        try {
            std::thread(connection, c).detach();
        } catch (...) {
            closesocket(c);
            g_connections.fetch_sub(1);
        }
    }
    logf("stopping");
    closesocket(ls);
    poll.join();
    g_mk2.close();  // detach our MK2 ring
    // Leave the slot quiet: the engine sees the heartbeat age out (command-protocol.md §7).
    return 0;
}
