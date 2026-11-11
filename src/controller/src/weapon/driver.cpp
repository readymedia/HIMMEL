// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/weapon/driver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace himmel::weapon {

int Profile::index(std::string_view control) const {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == control) return static_cast<int>(i);
    return -1;
}

std::optional<Profile> load_profile(const std::filesystem::path& path, std::string* err) {
    try {
        std::ifstream f(path);
        if (!f) {
            if (err) *err = "cannot open " + path.string();
            return std::nullopt;
        }
        const auto j = nlohmann::json::parse(f);
        Profile p;
        p.id = j.value("id", "");
        p.name = j.value("name", p.id);
        p.path = path.string();
        for (const auto& c : j.at("controls")) {
            p.names.push_back(c.value("name", ""));
            p.kinds.push_back(c.value("kind", ""));
        }
        return p;
    } catch (const std::exception& e) {
        if (err) *err = path.string() + ": " + e.what();
        return std::nullopt;
    }
}

std::filesystem::path find_repo_file(const std::string& rel) {
    namespace fs = std::filesystem;
    std::vector<fs::path> starts = {fs::current_path()};
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) starts.push_back(fs::path(self).parent_path());
    for (auto dir : starts) {
        for (; !dir.empty(); dir = dir.parent_path()) {
            std::error_code ec;
            if (fs::exists(dir / rel, ec)) return dir / rel;
            if (dir == dir.parent_path()) break;
        }
    }
    return rel;
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

DriverArgs parse_args(int argc, char** argv) {
    DriverArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--sim") a.sim = true;
        else if (s == "--sim-port") a.sim_port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (s == "--slot") a.slot = std::atoi(next().c_str());
        else if (s == "--port") a.port = next();
        else if (s == "--ports") a.list_ports = true;
        else a.rest.push_back(s);
    }
    return a;
}

void print_ports() {
    const auto ins = list_midi_in(), outs = list_midi_out();
    std::printf("MIDI in:\n");
    for (size_t i = 0; i < ins.size(); ++i) std::printf("  %zu  %s\n", i, ins[i].c_str());
    std::printf("MIDI out:\n");
    for (size_t i = 0; i < outs.size(); ++i) std::printf("  %zu  %s\n", i, outs[i].c_str());
}

namespace {
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

void run_driver(SharedMemory& shm, MidiTransport& io, const std::function<std::unique_ptr<Session>()>& make,
                const std::atomic<bool>& stop, const std::string& label) {
    auto& st = shm.get()->status;
    // Ctrl+C, or the supervisor's stop request in the shared block (works without a console).
    auto stopping = [&] { return stop.load() || st.stop_request.load() != 0; };
    std::string last_why;
    bool first = true;
    while (!stopping()) {
        st.heartbeat_us.store(now_us());
        std::string why;
        if (!io.open(why)) {
            if (why != last_why) {
                log_line("Waiting for the " + label + ": " + why);
                shm.set_message("Waiting: " + why);
            }
            last_why = why;
            for (int k = 0; k < 10 && !stopping(); ++k) {  // retry every 0.5 s, heartbeat meanwhile
                st.heartbeat_us.store(now_us());
                sleep_ms(50);
            }
            continue;
        }
        last_why.clear();
        if (!first) st.reconnects.fetch_add(1);
        first = false;
        set_text(st.device, io.port_name());
        auto s = make();
        uint64_t t = now_us();
        if (!s->start(io, t)) {
            io.close();
            continue;
        }
        st.connected.store(1);
        shm.publish({now_us(), EventType::Connected, 0, 0, 0, 0});
        log_line(label + " connected on " + io.port_name());
        shm.set_message(label + " connected (" + io.port_name() + ")");
        std::string reason;
        uint64_t next_present = t + 1000000, last_tick = 0;
        while (!stopping()) {
            MidiMsg m;
            const int r = io.read(m, 1);
            t = now_us();
            st.heartbeat_us.store(t);
            if (r < 0) {
                reason = io.simulated() ? "simulator restarted or gone" : "MIDI link lost";
                break;
            }
            if (r > 0) {
                st.msgs_in.fetch_add(1, std::memory_order_relaxed);
                s->on_message(m, t);
                // Drain what is waiting first, but the timers (pings!) never wait more than 1 ms.
                if (t - last_tick < 1000) continue;
            }
            last_tick = t;
            if (!s->tick(io, t)) {
                reason = "the device stopped answering";
                break;
            }
            if (t >= next_present) {
                next_present = t + 1000000;
                if (!io.present()) {
                    reason = io.simulated() ? "no datagrams from the simulator" : "port gone (cable pulled?)";
                    break;
                }
            }
        }
        if (stopping()) {
            log_line("Letting go of the " + label);
            s->let_go(io);
        }
        io.close();
        st.connected.store(0);
        shm.publish({now_us(), EventType::Disconnected, 0, 0, 0, 0});
        if (!stopping()) {
            log_line(label + " lost (" + reason + "), reopening");
            shm.set_message(label + " lost (" + reason + ")");
        }
    }
}

}  // namespace himmel::weapon
