// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// weapon_nuvj.exe - the driver process for the Numark NuVJ (weapon no. 5).
//
//   weapon_nuvj [--sim [--sim-port N]] [--slot N] [--port NAME|NUMBER] [--map nuvj_map.json] [--ports]
//
// Creates "Local\HimmelWeapon<slot>" (default 5), opens the NuVJ over WinMM, sends
// the init message and keeps the LEDs and LCD in step with the engine's outputs
// (changes at ~30 Hz, everything every 2 s). Only NuVJ feedback SysEx goes out
// (never 7B, which would store the contrast in the controller). Reconnects by itself.
// Ctrl+C: LEDs off, LCD back to "NuVJ".
//
// --sim: no hardware. Talks to the simulated NuVJ in tools/mk2_live.py --weapon nuvj
// over UDP 127.0.0.1:8774.
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

#include "himmel/weapon/driver.h"
#include "himmel/weapon/nuvj.h"

using namespace himmel::weapon;

namespace {
std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

// Used when weapons/nuvj.weapon.json cannot be found (show-safe: the driver still runs).
Profile builtin_profile() {
    Profile p;
    p.id = "nuvj";
    p.name = "NuVJ";
    p.path = "(built in)";
    for (const char* deck : {"A", "B"}) {
        const std::string d = deck;
        for (const char* n : {"Bank", "PMTR1", "PMTR2", "Latch", "Auto", "Sync"}) p.names.push_back(d + " " + n);
        for (int pad : {7, 8, 9, 4, 5, 6, 1, 2, 3}) p.names.push_back(d + " Pad " + std::to_string(pad));
        p.names.push_back(d + " Jog");
        p.names.push_back(d + " Scratch");
    }
    for (const char* n : {"Insta A", "Insta B", "FX Level", "Master FX", "Master FX Select", "Master PMTR1",
                          "Insta Black", "Contrast", "Master PMTR2", "BPM Tap", "Bright", "A Speed", "B Speed",
                          "Crossfader", "Fader Mode", "Auto Fade Speed", "Fader Start", "A Bank Push", "B Bank Push"})
        p.names.push_back(n);
    for (const auto& n : p.names) p.kinds.push_back(n.find(" Pad ") != std::string::npos ? "pad" : "button");
    return p;
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
    const int slot = args.slot > 0 ? args.slot : nuvj::kNumber;
    const uint16_t sim_port = args.sim_port ? args.sim_port : kSimPortNuvj;

    std::string err;
    auto prof = load_profile(find_repo_file("weapons/nuvj.weapon.json"), &err);
    if (!prof) {
        log_line("Profile: " + err + " - using the built-in control list");
        prof = builtin_profile();
    } else {
        log_line("Profile: " + prof->path + " (" + std::to_string(prof->count()) + " controls)");
    }
    const auto map_path = map_arg.empty() ? find_repo_file("nuvj_map.json") : std::filesystem::path(map_arg);
    nuvj::Map map;
    if (auto m = nuvj::load_map(map_path, *prof, &err)) {
        map = std::move(*m);
        log_line("Map: " + map_path.string() + (map.verified ? " (verified)" : ""));
    } else {
        map = nuvj::default_map(*prof);
        log_line("Map: " + err + " - using the starting table (LCD segments unknown)");
    }

    SharedMemory shm;
    if (slot < 1 || slot > kMaxSlots || !shm.create(slot, nuvj::make_header(*prof, args.sim))) {
        log_line("Could not create shared memory for slot " + std::to_string(slot));
        return 1;
    }
    shm.get()->status.map_verified.store(map.from_file ? 1 : 0);
    log_line("Shared memory ready: Local\\HimmelWeapon" + std::to_string(slot) + " (NuVJ, " +
             std::to_string(sizeof(SharedBlock)) + " bytes)");

    auto io = args.sim ? make_udp(sim_port) : make_winmm({"NuVJ", "Numark"}, args.port);
    if (args.sim) log_line("Simulated NuVJ on udp 127.0.0.1:" + std::to_string(sim_port));
    run_driver(
        shm, *io, [&] { return std::make_unique<nuvj::NuvjSession>(map, *prof, shm); }, g_stop, "NuVJ");

    log_line("Stopped");
    timeEndPeriod(1);
    return 0;
}
