// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// mk2_controller.exe - the Maschine MK2 controller process (spec 5.1).
//
//   mk2_controller [--map path\to\maschine_mk2_map.json] [--sim [--sim-port N]]
//
// --sim: no hardware. Reads a simulated MK2 over UDP on 127.0.0.1:8766 (the
// browser panel in tools/mk2_live.py, Simulate) and sends LEDs and screens back.
//
// Creates the shared block "Local\HimmelMk2", loads the map (falls back to the
// built-in verified layout if the file is missing or broken), and runs until
// Ctrl+C / close. Reconnects by itself when the USB cable is pulled.
//
// Log: the console, and logs\mk2-YYYY-MM-DD.log next to the exe (shared for
// reading, so it can be read while Himmel runs). The first lines say whether the
// REAL MK2 (USB) or a SIMULATED one is used, then "MK2 opened over USB ..." and
// an "input: ..." summary whenever the controls are used.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <share.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "himmel/mk2/controller.h"
#include "himmel/mk2/map.h"
#include "himmel/mk2/shared.h"

using namespace himmel::mk2;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

std::FILE* g_log = nullptr;

void open_log_file() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path() / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t name[40];
    std::wcsftime(name, 40, L"mk2-%Y-%m-%d.log", &tm);
    g_log = _wfsopen((dir / name).c_str(), L"a", _SH_DENYNO);  // others may read it while we run
}

void log_line(const std::string& s) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    std::printf("%s  mk2: %s\n", ts, s.c_str());
    std::fflush(stdout);
    if (g_log) {
        std::fprintf(g_log, "%s  %s\n", ts, s.c_str());
        std::fflush(g_log);
    }
}

std::filesystem::path find_map(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--map") return argv[i + 1];
    // Default: walk up from the working directory to find the repo's map file.
    for (auto dir = std::filesystem::current_path(); !dir.empty(); dir = dir.parent_path()) {
        auto p = dir / "maschine_mk2_map.json";
        if (std::filesystem::exists(p)) return p;
        if (dir == dir.parent_path()) break;
    }
    return "maschine_mk2_map.json";
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(on_console_event, TRUE);
    timeBeginPeriod(1);  // 1 ms sleep resolution for the writer thread
    open_log_file();
    log_line("mk2_controller started (pid " + std::to_string(GetCurrentProcessId()) + ")");

    const auto map_path = find_map(argc, argv);
    std::string err;
    Map map;
    bool from_file = false;
    if (auto m = load_map(map_path, &err)) {
        map = *m;
        from_file = true;
        log_line("Map: " + map_path.string() + (map.verified ? " (verified)" : " (NOT verified)"));
    } else {
        map = default_map();
        log_line("Map: " + err + " - using the built-in verified layout");
    }

    SharedMemory shm;
    if (!shm.create()) {
        log_line("Could not create shared memory (error " + std::to_string(GetLastError()) + ")");
        return 1;
    }
    shm.get()->status.map_verified.store(from_file && map.verified ? 1 : 0);
    log_line("Shared memory ready: Local\\HimmelMk2 (" + std::to_string(sizeof(SharedBlock)) + " bytes)");

    ControllerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--sim") cfg.sim = true;
        else if (a == "--sim-port" && i + 1 < argc) cfg.sim_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
    if (cfg.sim) {
        // Python is not a USB device: allow it a hiccup before calling the MK2 lost.
        cfg.heartbeat_timeout_us = 1000000;
        log_line("Mode: SIMULATED MK2 on udp 127.0.0.1:" + std::to_string(cfg.sim_port) +
                 " - the real MK2 is NOT used (start himmel.exe without --sim for the hardware)");
    } else {
        log_line("Mode: REAL MK2 over USB (hidapi). Close NI Maschine software and every MK2 Live page first.");
    }
    Controller controller(map, cfg, shm.get(), log_line);
    controller.run(g_stop);

    log_line("Stopped");
    if (g_log) std::fclose(g_log);
    timeEndPeriod(1);
    return 0;
}
