// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel.exe - the supervisor (CLAUDE.md "Processes").
//
//   himmel [--no-controller] [--weapons LIST | --no-weapons] [--sim] [--no-ui] [--legacy]
//          [-- render arguments ...]
//   e.g. himmel -- --outputs 3 --fullscreen
//   --sim: no hardware; the MK2 controller and the weapon drivers talk to the
//          simulators in tools/mk2_live.py (MK2 8766, Lightpad 8767, LUMI 8769, NuVJ 8774,
//          APC20 8782).
//   --weapons lightpad,lumi,nuvj,apc20: the weapon drivers to run (default: all four);
//          a driver whose device is not plugged in just waits for it.
//   --no-ui: do not start himmel_ui.exe (the web GUI bridge, started only if it
//          exists next to himmel.exe). The UI is just another child: it never
//          talks to the renderer except through the shared blocks, so a dead or
//          hung UI is restarted without touching the outputs.
//   --no-audio: do not start himmel_audio.exe (music in, M8). --audio "ARGS": its
//          arguments, e.g. --audio "--loopback" or --audio "--device Focusrite --channels 1-2"
//          (default: the Windows default capture device, auto gain on).
//   --no-curtains: no black curtain windows behind the outputs (M6; on by default).
//   --no-ingest: do not start himmel_ingest.exe (the library import, M7). It imports into the
//          renderer's library (render args --library DIR, or --show-root DIR -> DIR\Library);
//          with --no-show-files and no --library it is not started (test runs, M7).
//   --no-osc: do not start himmel_osc.exe (OSC in on UDP 7000, M13). --osc "ARGS": its
//          arguments, e.g. --osc "--port 9000 --allow 192.168.1.0/24" (default: this PC only).
//   --legacy: run the parked pre-slice renderer (himmel_render_legacy.exe: test
//          card, camera, Touch FX, Entrancer) instead of the deck engine.
//
// Starts mk2_controller.exe, the weapon drivers (weapon_blocks.exe lightpad / lumi,
// weapon_nuvj.exe, weapon_apc20.exe) and himmel_render.exe from its own folder and keeps them alive
// for a whole night:
//   * a child that exits is restarted (0.25 s, doubling up to 10 s if it keeps
//     dying quickly; back to 0.25 s after it has run for a minute);
//   * a child whose heartbeat in shared memory is older than 2 s is treated as
//     hung, killed and restarted;
//   * every child is in a job object, so they all die with the supervisor;
//   * everything is logged to the console and logs\himmel-YYYY-MM-DD.log.
// Ctrl+C / closing the window stops the renderer cleanly (stop_request), then
// the controller.
// Curtains (M6): one black window per output, held by this process just below the
// renderer's output windows, so a renderer crash / restart shows black, not the desktop.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <filesystem>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "himmel/audio/block.h"
#include "himmel/engine/engine_shared.h"
#include "himmel/engine/shm_block.h"
#include "himmel/library/library_block.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"
#include "himmel/protocol/commands.h"
#include "himmel/protocol/outputs.h"
#include "himmel/protocol/show.h"
#include "himmel/protocol/state.h"
#include "himmel/weapon/shared.h"

namespace fs = std::filesystem;
using himmel::mk2::now_us;

namespace {

std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;  // keep running until the children are down
}

std::FILE* g_logfile = nullptr;

void log(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[512];
    int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d  himmel  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (g_logfile) {
        std::fprintf(g_logfile, "%s\n", line);
        std::fflush(g_logfile);
    }
}

// ==== ENGINE (M6): curtains ====
// Black windows held by the supervisor, one per output, just below the renderer's
// output window (operations.md, build-v1 M6): when the renderer dies or restarts,
// its windows vanish and the curtain shows black - never the desktop. The curtain
// thread follows Local\HimmelOutputs (window rects + HWNDs, written by the renderer)
// and never moves a curtain unless the renderer's window moved. Windows may try to
// move a curtain when its display goes away (onto another output): refused.
class Curtains {
public:
    void start() { thread_ = std::thread([this] { run(); }); }
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    uint32_t shown() const { return shown_.load(); }

private:
    struct Curtain {
        HWND hwnd = nullptr;
        RECT rect{};
        uint64_t below = 0;   // the renderer window it sits under
        bool topmost = false;
    };
    static inline thread_local bool t_self_move = false;

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_WINDOWPOSCHANGING:
                if (!t_self_move) reinterpret_cast<WINDOWPOS*>(l)->flags |= SWP_NOMOVE | SWP_NOSIZE;
                break;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_CLOSE: return 0;  // only the supervisor closes them
            case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
            case WM_ERASEBKGND: {
                RECT r;
                GetClientRect(h, &r);
                FillRect(reinterpret_cast<HDC>(w), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                return 1;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }

    void place(Curtain& c, int index, const himmel::protocol::OutputInfo& oi, bool fullscreen) {
        const RECT r{oi.rect[0], oi.rect[1], oi.rect[2], oi.rect[3]};
        if (r.right <= r.left || r.bottom <= r.top) return;
        if (c.hwnd && c.topmost != fullscreen) {  // switched between windows and full screen
            DestroyWindow(c.hwnd);
            c = Curtain{};
        }
        t_self_move = true;
        if (!c.hwnd) {
            wchar_t title[48];
            swprintf_s(title, L"Himmel curtain %d", index + 1);
            c.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (fullscreen ? WS_EX_TOPMOST : 0),
                                     L"HimmelCurtain", title, WS_POPUP, r.left, r.top, r.right - r.left,
                                     r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            c.topmost = fullscreen;
            c.rect = r;
            c.below = 0;
            if (c.hwnd) {
                const BOOL off = TRUE;  // no fade-in / out animations
                DwmSetWindowAttribute(c.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
                ShowWindow(c.hwnd, SW_SHOWNOACTIVATE);
                log("curtain %d: up at %ld,%ld %ldx%ld", index + 1, r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
        }
        if (c.hwnd) {
            const HWND out = reinterpret_cast<HWND>(oi.hwnd);
            const bool moved = !EqualRect(&r, &c.rect);
            // Just below the renderer's window (only while that window exists).
            const bool restack = oi.hwnd && oi.hwnd != c.below && IsWindow(out);
            if (moved || restack) {
                SetWindowPos(c.hwnd, restack ? out : nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW | (restack ? 0 : SWP_NOZORDER));
                c.rect = r;
                if (restack) c.below = oi.hwnd;
            }
        }
        t_self_move = false;
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"himmel curtains");
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"HimmelCurtain";
        RegisterClassW(&wc);
        himmel::engine::ShmBlock shm;
        const himmel::protocol::OutputsBlock* blk = nullptr;
        auto sec = std::make_unique<himmel::protocol::OutputsSection>();
        Curtain cur[himmel::protocol::kMaxOutputs];
        uint64_t next_try = 0;
        while (!stop_.load()) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            const uint64_t t = now_us();
            if (!blk && t >= next_try) {
                next_try = t + 1000000;
                if (shm.open_existing(himmel::protocol::kOutputsShmName, sizeof(himmel::protocol::OutputsBlock))) {
                    const auto* b = static_cast<const himmel::protocol::OutputsBlock*>(shm.data());
                    if (himmel::engine::header_ok(b->header, himmel::protocol::kOutputsMagic, himmel::protocol::kOutputsVersion,
                                                  static_cast<uint32_t>(sizeof(himmel::protocol::OutputsBlock)))) {
                        blk = b;
                        log("curtains: following %ls", himmel::protocol::kOutputsShmName);
                    } else {
                        shm.close();
                    }
                }
            }
            if (blk && himmel::protocol::seq_read(blk->seq, blk->s, *sec)) {
                uint32_t up = 0;
                for (int i = 0; i < himmel::protocol::kMaxOutputs; ++i) {
                    const auto& oi = sec->out[i];
                    const bool want = oi.state != static_cast<uint8_t>(himmel::protocol::OutputState::Off) &&
                                      static_cast<uint32_t>(i) < sec->count;
                    if (want) {
                        place(cur[i], i, oi, sec->fullscreen != 0);
                    } else if (cur[i].hwnd) {
                        DestroyWindow(cur[i].hwnd);
                        cur[i] = Curtain{};
                        log("curtain %d: down (output not used)", i + 1);
                    }
                    up += cur[i].hwnd ? 1 : 0;
                }
                shown_.store(up);
            }
            Sleep(50);
        }
        for (auto& c : cur)
            if (c.hwnd) DestroyWindow(c.hwnd);
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> shown_{0};
};
// ==== end ENGINE (M6) ====

struct Child {
    std::string name;
    std::wstring exe;
    std::wstring args;
    PROCESS_INFORMATION pi{};
    bool running = false;
    uint64_t started_us = 0;
    uint64_t next_start_us = 0;
    uint64_t backoff_us = 125000;  // doubled before use: the first restart waits 0.25 s
    uint64_t max_backoff_us = 10000000;
    uint32_t restarts = 0;
    // Heartbeat source (0 if none yet).
    uint64_t (*heartbeat)(void*) = nullptr;
    void* ctx = nullptr;
};

bool start(Child& c, HANDLE job) {
    std::wstring cmd = L"\"" + c.exe + L"\" " + c.args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(c.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        fs::path(c.exe).parent_path().c_str(), &si, &c.pi)) {
        log("%s: could not start (error %lu)", c.name.c_str(), GetLastError());
        return false;
    }
    AssignProcessToJobObject(job, c.pi.hProcess);
    SetPriorityClass(c.pi.hProcess, HIGH_PRIORITY_CLASS);
    ResumeThread(c.pi.hThread);
    c.running = true;
    c.started_us = now_us();
    log("%s: started (pid %lu)", c.name.c_str(), c.pi.dwProcessId);
    return true;
}

void reap(Child& c, const char* why) {
    if (!c.running) return;
    DWORD code = 0;
    GetExitCodeProcess(c.pi.hProcess, &code);
    CloseHandle(c.pi.hThread);
    CloseHandle(c.pi.hProcess);
    c.pi = {};
    c.running = false;
    const uint64_t t = now_us();
    const uint64_t ran = t - c.started_us;
    // Fast first restart (show-safe); back off if it keeps dying, reset after a minute of running.
    c.backoff_us = ran > 60000000 ? 250000 : std::min<uint64_t>(c.backoff_us * 2, c.max_backoff_us);
    c.next_start_us = t + c.backoff_us;
    ++c.restarts;
    log("%s: %s (exit code %lu, ran %.1f s) - restart in %.2f s", c.name.c_str(), why, code, ran / 1e6, c.backoff_us / 1e6);
}

void stop_child(Child& c, DWORD wait_ms) {
    if (!c.running) return;
    if (WaitForSingleObject(c.pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
        TerminateProcess(c.pi.hProcess, 1);
        WaitForSingleObject(c.pi.hProcess, 2000);
    }
    CloseHandle(c.pi.hThread);
    CloseHandle(c.pi.hProcess);
    c.running = false;
    log("%s: stopped", c.name.c_str());
}

std::wstring widen(const std::string& s) { return fs::path(s).wstring(); }

// Local\HimmelAudio, for himmel_audio's heartbeat and stop request (opened, never created).
struct AudioShm {
    HANDLE mapping = nullptr;
    himmel::audio::AudioBlock* b = nullptr;
    himmel::audio::AudioBlock* get() {
        if (b) return b;
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, himmel::audio::kBlockName);
        if (!mapping) return nullptr;
        b = static_cast<himmel::audio::AudioBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(himmel::audio::AudioBlock)));
        if (!b) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
        return b;
    }
    ~AudioShm() {
        if (b) UnmapViewOfFile(b);
        if (mapping) CloseHandle(mapping);
    }
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(on_console_event, TRUE);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path dir = fs::path(self).parent_path();

    // Log file next to the repo/binary: <dir>\logs\himmel-YYYY-MM-DD.log
    std::error_code ec;
    fs::create_directories(dir / "logs", ec);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    std::snprintf(name, sizeof(name), "himmel-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
    // Shared for reading (_SH_DENYWR), so the log can be read / tailed while Himmel runs.
    g_logfile = _wfsopen((dir / "logs" / name).c_str(), L"a", _SH_DENYWR);

    bool with_controller = true, with_ui = true, with_audio = true, legacy = false, with_curtains = true, with_ingest = true;
    bool with_osc = true;  // CONTROL (M13)
    std::wstring render_args, ctl_args, audio_args, osc_args;
    std::string weapons = "lightpad,lumi,nuvj,apc20";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-controller") with_controller = false;
        else if (a == "--no-weapons") weapons.clear();
        else if (a == "--weapons" && i + 1 < argc) weapons = argv[++i];
        else if (a == "--sim") ctl_args = L"--sim";
        else if (a == "--no-ui") with_ui = false;
        else if (a == "--no-audio") with_audio = false;
        else if (a == "--audio" && i + 1 < argc) audio_args = widen(argv[++i]);
        else if (a == "--legacy") legacy = true;
        else if (a == "--no-curtains") with_curtains = false;
        else if (a == "--no-ingest") with_ingest = false;  // MEDIA (M7)
        else if (a == "--no-osc") with_osc = false;        // CONTROL (M13)
        else if (a == "--osc" && i + 1 < argc) osc_args = widen(argv[++i]);
        else if (a == "--") {
            for (++i; i < argc; ++i) render_args += widen(argv[i]) + L" ";
        }
    }

    // ==== MEDIA (M7) hook 1/3: himmel_ingest.exe imports into the renderer's library ====
    std::wstring ingest_args, ui_args;
    {
        bool render_part = false, no_files = false, named = false, no_lib = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--") render_part = true;
            if (!render_part) continue;
            if (a == "--no-show-files") no_files = true;
            else if (a == "--library" && i + 1 < argc) ingest_args = L"--library \"" + widen(argv[++i]) + L"\"", named = true;
            else if (a == "--show-root" && i + 1 < argc && !named)
                ingest_args = L"--library \"" + (fs::path(widen(argv[++i])) / L"Library").wstring() + L"\"", named = true;
            else if (a == "--no-library") with_ingest = false, no_lib = true;
        }
        if (no_files && !named) with_ingest = false;  // a test run: never the owner's library
        // ==== GUI (M11 part 2) hook: himmel_ui lists the same library the ingest worker fills ====
        ui_args = named ? ingest_args : (no_files || no_lib) ? std::wstring(L"--no-library") : std::wstring();
    }
    // ==== end MEDIA hook 1/3 ====

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));

    himmel::engine::EngineShm eshm;
    himmel::mk2::SharedMemory mshm;

    Child ctl{"controller", (dir / "mk2_controller.exe").wstring(), ctl_args};
    Child ren{"render", (dir / (legacy ? "himmel_render_legacy.exe" : "himmel_render.exe")).wstring(), render_args};
    // The web GUI bridge (S3). No heartbeat block of its own yet: the supervisor only
    // restarts it when it exits. GUESS: a hung UI is not detected (it cannot hurt the
    // show: the renderer never waits for it); add a heartbeat when himmel_ui has one.
    Child ui{"ui", (dir / "himmel_ui.exe").wstring(), ui_args};
    if (with_ui && !fs::exists(ui.exe, ec)) {
        log("ui: %s not found - running without the web GUI", fs::path(ui.exe).filename().string().c_str());
        with_ui = false;
    }
    // Music in (M8): himmel_audio.exe, heartbeat in Local\HimmelAudio. The renderer never
    // waits for it: without it the reactions glide to rest.
    AudioShm ashm;
    Child aud{"audio", (dir / "himmel_audio.exe").wstring(), audio_args};
    if (with_audio && !fs::exists(aud.exe, ec)) {
        log("audio: %s not found - running without music in", fs::path(aud.exe).filename().string().c_str());
        with_audio = false;
    }
    aud.ctx = &ashm;
    aud.heartbeat = [](void* p) -> uint64_t {
        auto* b = static_cast<AudioShm*>(p)->get();
        return b ? b->status.heartbeat_us.load() : 0;
    };
    ctl.ctx = &mshm;
    ctl.heartbeat = [](void* p) -> uint64_t {
        auto* m = static_cast<himmel::mk2::SharedMemory*>(p);
        if (!m->get() && !m->open()) return 0;
        return m->get()->status.heartbeat_us.load();
    };
    // M6: the outputs show black (curtains) while the renderer is down, so it comes back fast
    // every time: at most 0.5 s between restarts, even in a crash loop (GUESS: a GPU that keeps
    // failing gains nothing from waiting longer; the log shows the loop).
    ren.max_backoff_us = 500000;
    ren.ctx = &eshm;
    ren.heartbeat = [](void* p) -> uint64_t {
        auto* m = static_cast<himmel::engine::EngineShm*>(p);
        if (!m->get() && !m->open()) return 0;
        return m->get()->heartbeat_us.load();
    };

    // Weapon drivers: one process per weapon, each with its shared block Local\HimmelWeapon<N>.
    struct WeaponDef {
        const char* id;
        const wchar_t* exe;
        const wchar_t* args;
        int slot;
    };
    const WeaponDef defs[] = {{"lightpad", L"weapon_blocks.exe", L"lightpad", 2},
                              {"lumi", L"weapon_blocks.exe", L"lumi", 3},
                              {"nuvj", L"weapon_nuvj.exe", L"", 5},
                              {"apc20", L"weapon_apc20.exe", L"", 9}};
    std::vector<std::unique_ptr<Child>> weapon_children;
    std::vector<std::unique_ptr<himmel::weapon::SharedMemory>> weapon_shm;
    for (const auto& d : defs) {
        if (("," + weapons + ",").find(std::string(",") + d.id + ",") == std::string::npos) continue;
        auto c = std::make_unique<Child>();
        c->name = d.id;
        c->exe = (dir / d.exe).wstring();
        c->args = std::wstring(d.args) + (ctl_args.empty() ? L"" : L" --sim");
        weapon_shm.push_back(std::make_unique<himmel::weapon::SharedMemory>());
        struct Ctx {
            himmel::weapon::SharedMemory* shm;
            int slot;
        };
        static std::vector<std::unique_ptr<Ctx>> ctxs;  // lives as long as the supervisor
        ctxs.push_back(std::make_unique<Ctx>(Ctx{weapon_shm.back().get(), d.slot}));
        c->ctx = ctxs.back().get();
        c->heartbeat = [](void* p) -> uint64_t {
            auto* x = static_cast<Ctx*>(p);
            if (!x->shm->get() && !x->shm->open(x->slot)) return 0;
            return x->shm->get()->status.heartbeat_us.load();
        };
        weapon_children.push_back(std::move(c));
    }

    log("supervisor up in %s%s, weapons: %s", dir.string().c_str(),
        !with_controller ? " (no controller)" : ctl_args.empty() ? "" : " (simulated hardware)",
        weapons.empty() ? "none" : weapons.c_str());
    std::vector<Child*> children = {&ctl};
    for (auto& w : weapon_children) children.push_back(w.get());
    if (with_audio) children.push_back(&aud);
    // ==== MEDIA (M7) hook 2/3: the import worker (lowers its own priority; heartbeat in Local\HimmelLibrary) ====
    struct LibShm {
        HANDLE mapping = nullptr;
        himmel::library::LibraryBlock* b = nullptr;
        himmel::library::LibraryBlock* get() {
            if (b) return b;
            mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, himmel::library::kLibraryShmName);
            if (!mapping) return nullptr;
            b = static_cast<himmel::library::LibraryBlock*>(
                MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(himmel::library::LibraryBlock)));
            if (!b) CloseHandle(mapping), mapping = nullptr;
            return b;
        }
        ~LibShm() {
            if (b) UnmapViewOfFile(b);
            if (mapping) CloseHandle(mapping);
        }
    } lshm;
    Child ing{"ingest", (dir / "himmel_ingest.exe").wstring(), ingest_args};
    if (with_ingest && !fs::exists(ing.exe, ec)) {
        log("ingest: %s not found - running without the library import", fs::path(ing.exe).filename().string().c_str());
        with_ingest = false;
    }
    ing.ctx = &lshm;
    ing.heartbeat = [](void* p) -> uint64_t {
        auto* b = static_cast<LibShm*>(p)->get();
        return b ? b->heartbeat_us.load() : 0;
    };
    if (with_ingest) children.push_back(&ing);
    else log("ingest: off");
    // ==== end MEDIA hook 2/3 ====
    // ==== CONTROL (M13): OSC in. Only restarted when it exits (GUESS: like the UI, a hung
    // himmel_osc cannot hurt the show; it writes its heartbeat in Local\HimmelOsc for the GUI) ====
    Child osc{"osc", (dir / "himmel_osc.exe").wstring(), osc_args};
    if (with_osc && !fs::exists(osc.exe, ec)) {
        log("osc: %s not found - running without OSC in", fs::path(osc.exe).filename().string().c_str());
        with_osc = false;
    }
    if (with_osc) children.push_back(&osc);
    // ==== end CONTROL (M13) ====
    children.push_back(&ren);
    if (with_ui) children.push_back(&ui);
    // ==== ENGINE (M6): keep the blocks open, curtains ====
    // Kept open for the whole night: the outputs block (and the curtains' rects in it) survives
    // a renderer restart, so the new renderer finds its display identities and settings.
    himmel::engine::ShmBlock outputs_keep;
    if (outputs_keep.open_or_create(himmel::protocol::kOutputsShmName, sizeof(himmel::protocol::OutputsBlock)) ==
        himmel::engine::ShmBlock::Result::Failed)
        log("outputs: cannot map the outputs block - curtains off");
    // The show must survive a renderer restart even when no GUI holds the blocks open:
    // keep the state (the show to restore), the commands and the show block mapped too.
    himmel::engine::ShmBlock state_keep, commands_keep, show_keep;
    state_keep.open_or_create(himmel::protocol::kStateShmName, sizeof(himmel::protocol::StateBlock));
    // The state block is left without a header when we create it: the renderer writes it (and
    // knows an empty block is not a show to restore). The command block's creator writes its
    // header (command-protocol.md §2), so that is us when we come first.
    if (commands_keep.open_or_create(himmel::protocol::kCommandShmName, sizeof(himmel::protocol::CommandBlock)) ==
        himmel::engine::ShmBlock::Result::Created) {
        auto* cb = static_cast<himmel::protocol::CommandBlock*>(commands_keep.data());
        std::memset(static_cast<void*>(cb), 0, sizeof(*cb));
        himmel::engine::write_header(cb->header, himmel::protocol::kCommandMagic, himmel::protocol::kCommandVersion,
                                     static_cast<uint32_t>(sizeof(himmel::protocol::CommandBlock)));
    }
    show_keep.open_or_create(himmel::protocol::kShowShmName, sizeof(himmel::protocol::ShowBlock));
    Curtains curtains;
    if (with_curtains && !legacy) curtains.start();
    // ==== end ENGINE (M6) ====
    constexpr uint64_t kHungUs = 2000000, kGraceUs = 8000000;  // grace: start-up (GPU, shaders, USB)
    uint64_t next_report = now_us() + 60000000;

    while (!g_stop.load()) {
        const uint64_t t = now_us();
        for (Child* c : children) {
            if (c == &ctl && !with_controller) continue;
            if (!c->running) {
                if (t >= c->next_start_us) {
                    if (!start(*c, job)) c->next_start_us = t + 5000000;
                    if (c == &ren && eshm.get()) eshm.get()->restarts_seen.store(ren.restarts);
                }
                continue;
            }
            if (WaitForSingleObject(c->pi.hProcess, 0) == WAIT_OBJECT_0) {
                reap(*c, "exited");
                continue;
            }
            if (!c->heartbeat) continue;  // exit detection only
            const uint64_t hb = c->heartbeat(c->ctx);
            const uint64_t since = std::max(hb, c->started_us);
            // Signed: the child may write a heartbeat newer than t between our reads.
            const int64_t silent = static_cast<int64_t>(t) - static_cast<int64_t>(since);
            if (silent > static_cast<int64_t>(hb > c->started_us ? kHungUs : kGraceUs)) {
                log("%s: no heartbeat for %.1f s - killing it", c->name.c_str(), silent / 1e6);
                TerminateProcess(c->pi.hProcess, 99);
                WaitForSingleObject(c->pi.hProcess, 2000);
                reap(*c, "hung");
            }
        }
        if (t >= next_report) {  // one status line a minute
            next_report = t + 60000000;
            if (auto* s = eshm.get())
                log("status: render %.1f fps, frame max %.1f ms, late %u, MK2 %s, input->present %.1f ms, restarts r%u c%u",
                    s->fps_x100.load() / 100.0, s->frame_ms_max_x100.load() / 100.0, s->late_frames.load(),
                    s->mk2_connected.load() ? "ok" : "off", s->input_to_present_us.load() / 1000.0, ren.restarts,
                    ctl.restarts);
        }
        // ==== ENGINE (quit): the renderer took a confirmed SystemQuit (command-protocol.md §4e) ====
        if (auto* s = eshm.get(); s && s->quit_request.load()) {
            log("quit requested (producer %u) - stopping Himmel", s->quit_request.load() - 1);
            // The same clean stop as Ctrl+C in this console: every child shares it, and the ones
            // without a stop request of their own (mk2_controller) let go of their device on it.
            if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
                log("quit: no console Ctrl+C (error %lu) - the stop requests do it", GetLastError());
            g_stop.store(true);
            break;
        }
        // ==== end ENGINE (quit) ====
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // exit seen within 50 ms
    }

    log("stopping");
    if (auto* s = eshm.get()) s->stop_request.store(1);
    // ==== CONTROL (mk2 stop): mk2_controller lets go in parallel with the others (LEDs off,
    // "stopped" page, exit 0) instead of being terminated after 3 s (mk2-controller-spec §5.1) ====
    if (mshm.get() || mshm.open()) mshm.get()->status.stop_request.store(1);
    // ==== end CONTROL (mk2 stop) ====
    stop_child(ren, 3000);
    stop_child(ui, 1000);  // after the renderer: the outputs never wait for the GUI
    if (auto* b = ashm.get()) b->status.stop_request.store(1);
    stop_child(aud, 2000);
    if (auto* b = lshm.get()) b->stop_request.store(1);  // MEDIA (M7) hook 3/3
    stop_child(ing, 3000);
    // The drivers put their devices back (config, LEDs) first. They got the console Ctrl+C
    // too; the stop request also reaches them when the window was closed instead.
    for (auto& shm : weapon_shm)
        if (shm->get()) shm->get()->status.stop_request.store(1);
    for (auto& w : weapon_children) stop_child(*w, 3000);
    stop_child(ctl, 3000);  // otherwise terminated
    curtains.stop();  // ENGINE (M6): after every child - black until the very end
    log("bye");
    if (g_logfile) std::fclose(g_logfile);
    CloseHandle(job);
    return 0;
}
