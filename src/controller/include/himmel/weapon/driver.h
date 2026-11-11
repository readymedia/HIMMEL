// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Generic weapons: what every driver process shares (profile, files, logging,
// the run loop with heartbeat and reconnect).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "himmel/weapon/midi.h"
#include "himmel/weapon/shared.h"

namespace himmel::weapon {

// weapons/<id>.weapon.json, only what a driver needs: control names and kinds, in order.
struct Profile {
    std::string id, name, path;
    std::vector<std::string> names, kinds;
    int index(std::string_view control) const;  // -1 if the profile has no such control
    int count() const { return static_cast<int>(names.size()); }
};
std::optional<Profile> load_profile(const std::filesystem::path& path, std::string* err = nullptr);

// Looks for rel (e.g. "weapons/lumi.weapon.json") in the working directory, the exe's
// directory and their parents. Returns rel itself if nothing is found.
std::filesystem::path find_repo_file(const std::string& rel);

void log_line(const std::string& s);  // "HH:MM:SS  text" to stdout

// Command line shared by the drivers: --sim [--sim-port N], --slot N, --port NAME, --ports.
struct DriverArgs {
    bool sim = false;
    uint16_t sim_port = 0;  // 0 = the weapon's default
    int slot = 0;           // 0 = the weapon's number
    std::string port;       // WinMM port number or name part
    bool list_ports = false;
    std::vector<std::string> rest;  // anything else
};
DriverArgs parse_args(int argc, char** argv);
void print_ports();

// One device session as the driver's run loop sees it.
class Session {
public:
    virtual ~Session() = default;
    // The link is open: say hello (handshake, init). False = give up this session.
    virtual bool start(MidiTransport& io, uint64_t t) = 0;
    // One message from the device.
    virtual void on_message(const MidiMsg& m, uint64_t t) = 0;
    // Called every loop (~1 ms): timers, outputs. False = the device is gone.
    virtual bool tick(MidiTransport& io, uint64_t t) = 0;
    // Ctrl+C: put the device back the way we found it (may read messages for ~1 s).
    virtual void let_go(MidiTransport& io) = 0;
};

// Opens the transport, runs a session per connection, reconnects, keeps the
// heartbeat. Returns when stop becomes true.
void run_driver(SharedMemory& shm, MidiTransport& io, const std::function<std::unique_ptr<Session>()>& make,
                const std::atomic<bool>& stop, const std::string& device_label);

}  // namespace himmel::weapon
