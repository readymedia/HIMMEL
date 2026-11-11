// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// weapon_apc20.exe - the driver process for the Akai APC20 (weapon no. 9, a secondary weapon).
//
//   weapon_apc20 [--sim [--sim-port N]] [--slot N] [--port NAME|NUMBER] [--map apc20_map.json] [--ports]
//
// Creates "Local\HimmelWeapon<slot>" (default 9), opens the APC20 over WinMM, asks for
// its identity, puts it in Ableton mode (the host owns the LEDs) and keeps the LEDs in
// step with the engine's outputs (changes at ~30 Hz, everything every 5 s). Only the
// identity request, the APC mode SysEx and note on (LEDs) go out. Reconnects by itself
// and sends the setup again. Ctrl+C: LEDs off, back to generic mode.
//
// The APC20 needs its 12 V DC 2 A adapter; USB carries MIDI only.
//
// --sim: no hardware. Talks to the simulated APC20 in tools/mk2_live.py --weapon apc20
// over UDP 127.0.0.1:8782.
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

#include "himmel/weapon/apc20.h"
#include "himmel/weapon/driver.h"

using namespace himmel::weapon;

namespace {
std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}
}  // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(on_console_event, TRUE);
    DriverArgs args = parse_args(argc, argv);
    if (args.list_ports) {
        print_ports();
        return 0;
    }
    std::string map_arg;
    for (size_t i = 0; i + 1 < args.rest.size(); ++i)
        if (args.rest[i] == "--map") map_arg = args.rest[i + 1];
    timeBeginPeriod(1);
    const int slot = args.slot > 0 ? args.slot : apc20::kNumber;
    const uint16_t sim_port = args.sim_port ? args.sim_port : kSimPortApc20;

    std::string err;
    auto prof = load_profile(find_repo_file("weapons/apc20.weapon.json"), &err);
    if (!prof) {
        log_line("Profile: " + err + " - using the built-in control list");
        prof = apc20::builtin_profile();
    } else {
        log_line("Profile: " + prof->path + " (" + std::to_string(prof->count()) + " controls)");
    }
    const auto map_path = map_arg.empty() ? find_repo_file("apc20_map.json") : std::filesystem::path(map_arg);
    apc20::Map map;
    if (auto m = apc20::load_map(map_path, *prof, &err)) {
        map = *m;
        log_line("Map: " + map_path.string() + (map.verified ? " (every control seen)" : " (partly learned)"));
    } else {
        map = apc20::default_map(*prof);
        log_line("Map: " + err + " - using the protocol table (not yet checked on the unit)");
    }

    SharedMemory shm;
    if (slot < 1 || slot > kMaxSlots || !shm.create(slot, apc20::make_header(*prof, args.sim))) {
        log_line("Could not create shared memory for slot " + std::to_string(slot));
        return 1;
    }
    shm.get()->status.map_verified.store(map.verified ? 1 : 0);
    log_line("Shared memory ready: Local\\HimmelWeapon" + std::to_string(slot) + " (APC20, " +
             std::to_string(sizeof(SharedBlock)) + " bytes)");

    auto io = args.sim ? make_udp(sim_port) : make_winmm({"APC20", "APC 20"}, args.port);
    if (args.sim) log_line("Simulated APC20 on udp 127.0.0.1:" + std::to_string(sim_port));
    run_driver(
        shm, *io, [&] { return std::make_unique<apc20::Apc20Session>(map, shm); }, g_stop, "APC20");

    log_line("Stopped");
    timeEndPeriod(1);
    return 0;
}
