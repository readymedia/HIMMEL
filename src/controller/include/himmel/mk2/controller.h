// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// Maschine MK2 controller: device I/O and the run loop (spec 5.1).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "himmel/mk2/input.h"
#include "himmel/mk2/map.h"
#include "himmel/mk2/shared.h"

namespace himmel::mk2 {

struct ControllerConfig {
    PadConfig pads;
    uint32_t led_interval_us = 16000;        // at most ~60 LED writes per second per report
    uint32_t screen_interval_us = 33000;     // screens at most ~30 Hz, changed strips only
    uint32_t heartbeat_timeout_us = 250000;  // no report for this long = device lost
    uint32_t reopen_interval_ms = 500;
    // Simulated MK2: read the reports from a simulator over UDP on loopback instead
    // of USB (see sim_link.h). Everything else is the same code.
    bool sim = false;
    uint16_t sim_port = 8766;
};

class SimLink;
namespace detail {
struct Device;
}

using LogFn = std::function<void(const std::string&)>;

class Controller {
public:
    Controller(const Map& map, ControllerConfig cfg, SharedBlock* shm, LogFn log);
    ~Controller();
    // Blocks until stop becomes true: open, run, watch the heartbeat, reopen.
    void run(const std::atomic<bool>& stop);

private:
    struct Session;
    std::unique_ptr<detail::Device> open(std::string& why);
    void reader_loop(Session& s);
    void writer_loop(Session& s);
    void publish(const Event& e);
    void set_message(const std::string& m);

    Map map_;
    ControllerConfig cfg_;
    SharedBlock* shm_;
    LogFn log_;
    std::unique_ptr<SimLink> sim_;  // bound once, kept across sessions
};

}  // namespace himmel::mk2
