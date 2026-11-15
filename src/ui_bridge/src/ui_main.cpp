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
    std::string live, catalog, bridge;
    uint64_t live_ver = 0, catalog_ver = 0, bridge_ver = 0;

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
// The poller: ~30 Hz. Opens blocks (retry every second), reads live +
// catalog, publishes JSON, sends Hello every second.
void poller() {
    const DWORD pid = GetCurrentProcessId();
    auto last_try = Clock::now() - std::chrono::seconds(10);
    auto last_hello = Clock::now() - std::chrono::seconds(10);
    auto last_bridge = Clock::now() - std::chrono::seconds(10);
    auto last_beat_change = Clock::now();
    uint64_t last_beat = 0;
    bool seen_beat = false;
    uint32_t last_catalog_seq = 0, last_epoch = 0;
    std::string last_state_word, last_cmd_word, last_prev_word;
    auto live = std::make_unique<LiveState>();
    auto cat = std::make_unique<Catalog>();
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
    uint64_t lv = 0, cv = 0, bv = 0;
    auto last_send = Clock::now();
    while (!g_quit.load()) {
        std::string out;
        {
            std::unique_lock lk(g_hub.m);
            g_hub.cv.wait_for(lk, std::chrono::seconds(1), [&] {
                return g_quit.load() || g_hub.live_ver != lv || g_hub.catalog_ver != cv || g_hub.bridge_ver != bv;
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
    // Leave the slot quiet: the engine sees the heartbeat age out (command-protocol.md §7).
    return 0;
}
