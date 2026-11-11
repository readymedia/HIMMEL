// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// weapon_blocks.exe - the driver process for ROLI BLOCKS weapons (Lightpad Block, LUMI Keys).
//
//   weapon_blocks lightpad|lumi [--sim [--sim-port N]] [--slot N] [--port NAME|NUMBER] [--ports]
//
// Creates "Local\HimmelWeapon<slot>" (default slot = the weapon's number: Lightpad 2,
// LUMI 3), opens the block over WinMM, puts it in API mode, uploads Himmel's LED
// renderer (RAM only) and streams the engine's pixels to it. Events go to the
// shared block's rings. Reconnects by itself. Ctrl+C puts the block's settings back
// and ends API mode (the factory display returns).
//
// --sim: no hardware. Talks to the simulated block in tools/mk2_live.py
// (--weapon lightpad / lumi) over UDP 127.0.0.1:8767 (Lightpad) or :8769 (LUMI).
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

#include "himmel/weapon/blocks_session.h"
#include "himmel/weapon/driver.h"

using namespace himmel::weapon;

namespace {
std::atomic<bool> g_stop{false};
BOOL WINAPI on_console_event(DWORD) {
    g_stop.store(true);
    return TRUE;
}

// Used when weapons/<id>.weapon.json cannot be found (show-safe: the driver still runs).
Profile builtin_profile(const blocks::DeviceSetup& d) {
    Profile p;
    p.id = d.weapon;
    p.name = d.name;
    p.path = "(built in)";
    if (d.kind == blocks::Kind::Lightpad) {
        p.names = {"Surface", "Mode"};
    } else {
        p.names = {"Mode", "Octave -", "Octave +"};
        for (int k : {1, 3, 5, 6, 8, 10, 12, 13, 15, 17, 18, 20, 22, 24, 2, 4, 7, 9, 11, 14, 16, 19, 21, 23})
            p.names.push_back("Key " + std::to_string(k));
    }
    p.kinds.assign(p.names.size(), "button");
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
    const std::string which = args.rest.empty() ? "" : args.rest[0];
    if (which != "lightpad" && which != "lumi") {
        std::printf("usage: weapon_blocks lightpad|lumi [--sim [--sim-port N]] [--slot N] [--port NAME] [--ports]\n");
        return 2;
    }
    timeBeginPeriod(1);
    const blocks::DeviceSetup setup = which == "lightpad" ? blocks::lightpad_setup() : blocks::lumi_setup();
    const int slot = args.slot > 0 ? args.slot : setup.number;
    const uint16_t sim_port = args.sim_port ? args.sim_port : setup.sim_port;

    std::string err;
    auto prof = load_profile(find_repo_file(std::string("weapons/") + setup.weapon + ".weapon.json"), &err);
    if (!prof) {
        log_line("Profile: " + err + " - using the built-in control list");
        prof = builtin_profile(setup);
    } else {
        log_line("Profile: " + prof->path + " (" + std::to_string(prof->count()) + " controls)");
    }

    SharedMemory shm;
    if (slot < 1 || slot > kMaxSlots || !shm.create(slot, blocks::make_header(setup, *prof, args.sim))) {
        log_line("Could not create shared memory for slot " + std::to_string(slot));
        return 1;
    }
    log_line("Shared memory ready: Local\\HimmelWeapon" + std::to_string(slot) + " (" + setup.name + ", " +
             std::to_string(sizeof(SharedBlock)) + " bytes)");

    auto io = args.sim ? make_udp(sim_port) : make_winmm(setup.port_hints, args.port);
    if (args.sim) log_line(std::string("Simulated ") + setup.name + " on udp 127.0.0.1:" + std::to_string(sim_port));
    run_driver(
        shm, *io, [&] { return std::make_unique<blocks::BlocksSession>(setup, *prof, shm); }, g_stop, setup.name);

    log_line("Stopped");
    timeEndPeriod(1);
    return 0;
}
