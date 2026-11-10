// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// mk2_controller.exe - the Maschine MK2 controller process (spec 5.1).
//
//   mk2_controller [--map path\to\maschine_mk2_map.json]
//
// Creates the shared block "Local\HimmelMk2", loads the map (falls back to the
// built-in verified layout if the file is missing or broken), and runs until
// Ctrl+C / close. Reconnects by itself when the USB cable is pulled.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cstdio>
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

void log_line(const std::string& s) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    std::printf("%s  %s\n", ts, s.c_str());
    std::fflush(stdout);
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

    Controller controller(map, ControllerConfig{}, shm.get(), log_line);
    controller.run(g_stop);

    log_line("Stopped");
    timeEndPeriod(1);
    return 0;
}
