// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel.exe - the supervisor (CLAUDE.md "Processes").
//
//   himmel [--no-controller] [-- render arguments ...]
//   e.g. himmel -- --outputs 3 --fullscreen
//
// Starts mk2_controller.exe and himmel_render.exe from its own folder and keeps
// them alive for a whole night:
//   * a child that exits is restarted (0.25 s, doubling up to 10 s if it keeps
//     dying quickly; back to 0.25 s after it has run for a minute);
//   * a child whose heartbeat in shared memory is older than 2 s is treated as
//     hung, killed and restarted;
//   * every child is in a job object, so they all die with the supervisor;
//   * everything is logged to the console and logs\himmel-YYYY-MM-DD.log.
// Ctrl+C / closing the window stops the renderer cleanly (stop_request), then
// the controller.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "himmel/engine/engine_shared.h"
#include "himmel/mk2/shared.h"
#include "himmel/mk2/types.h"

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

struct Child {
    const char* name;
    std::wstring exe;
    std::wstring args;
    PROCESS_INFORMATION pi{};
    bool running = false;
    uint64_t started_us = 0;
    uint64_t next_start_us = 0;
    uint64_t backoff_us = 125000;  // doubled before use: the first restart waits 0.25 s
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
        log("%s: could not start (error %lu)", c.name, GetLastError());
        return false;
    }
    AssignProcessToJobObject(job, c.pi.hProcess);
    SetPriorityClass(c.pi.hProcess, HIGH_PRIORITY_CLASS);
    ResumeThread(c.pi.hThread);
    c.running = true;
    c.started_us = now_us();
    log("%s: started (pid %lu)", c.name, c.pi.dwProcessId);
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
    c.backoff_us = ran > 60000000 ? 250000 : std::min<uint64_t>(c.backoff_us * 2, 10000000);
    c.next_start_us = t + c.backoff_us;
    ++c.restarts;
    log("%s: %s (exit code %lu, ran %.1f s) - restart in %.2f s", c.name, why, code, ran / 1e6, c.backoff_us / 1e6);
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
    log("%s: stopped", c.name);
}

std::wstring widen(const std::string& s) { return fs::path(s).wstring(); }

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
    _wfopen_s(&g_logfile, (dir / "logs" / name).c_str(), L"a");

    bool with_controller = true;
    std::wstring render_args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-controller") with_controller = false;
        else if (a == "--") {
            for (++i; i < argc; ++i) render_args += widen(argv[i]) + L" ";
        }
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));

    himmel::engine::EngineShm eshm;
    himmel::mk2::SharedMemory mshm;

    Child ctl{"controller", (dir / "mk2_controller.exe").wstring(), L""};
    Child ren{"render", (dir / "himmel_render.exe").wstring(), render_args};
    ctl.ctx = &mshm;
    ctl.heartbeat = [](void* p) -> uint64_t {
        auto* m = static_cast<himmel::mk2::SharedMemory*>(p);
        if (!m->get() && !m->open()) return 0;
        return m->get()->status.heartbeat_us.load();
    };
    ren.ctx = &eshm;
    ren.heartbeat = [](void* p) -> uint64_t {
        auto* m = static_cast<himmel::engine::EngineShm*>(p);
        if (!m->get() && !m->open()) return 0;
        return m->get()->heartbeat_us.load();
    };

    log("supervisor up in %s%s", dir.string().c_str(), with_controller ? "" : " (no controller)");
    Child* children[2] = {&ctl, &ren};
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
            const uint64_t hb = c->heartbeat(c->ctx);
            const uint64_t since = std::max(hb, c->started_us);
            // Signed: the child may write a heartbeat newer than t between our reads.
            const int64_t silent = static_cast<int64_t>(t) - static_cast<int64_t>(since);
            if (silent > static_cast<int64_t>(hb > c->started_us ? kHungUs : kGraceUs)) {
                log("%s: no heartbeat for %.1f s - killing it", c->name, silent / 1e6);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    log("stopping");
    if (auto* s = eshm.get()) s->stop_request.store(1);
    stop_child(ren, 3000);
    stop_child(ctl, 3000);  // got the console Ctrl+C too; otherwise terminated
    log("bye");
    if (g_logfile) std::fclose(g_logfile);
    CloseHandle(job);
    return 0;
}
