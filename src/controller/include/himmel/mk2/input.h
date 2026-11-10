// Maschine MK2 controller: turning input reports into events (spec 2 and 5.2-5.4).
// Pure logic, no I/O, no allocation: unit-tested without hardware.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "himmel/mk2/map.h"
#include "himmel/mk2/types.h"

namespace himmel::mk2 {

// Fixed-size output buffer for one report's worth of events.
struct EventBatch {
    std::array<Event, 64> items{};
    int count = 0;
    void push(const Event& e) {
        if (count < static_cast<int>(items.size())) items[count++] = e;
    }
};

// Report 0x01: button edges, knob and encoder deltas (wrap-corrected).
class ControlsParser {
public:
    explicit ControlsParser(const Map& map) : map_(map) {}
    void reset() { valid_ = false; }
    // payload = the report without its id byte (24 bytes expected).
    void parse(std::span<const uint8_t> payload, uint64_t t_us, EventBatch& out);

private:
    int knob_value(std::span<const uint8_t> p, const KnobPos& k) const;

    Map map_;
    std::array<uint8_t, kControlsPayload> last_{};
    bool valid_ = false;
    std::array<int, kKnobs> knob_max_seen_{};
};

// Report 0x20 -> pressure per hardware id (0..4095).
void parse_pad_report(std::span<const uint8_t> payload, std::array<uint16_t, kPads>& by_hw_id);

struct PadConfig {
    int onset = 60;            // above the pad's noise floor: a touch starts
    int release = 30;          // below floor + release: the pad is let go
    int confirm = 120;         // a hit needs floor + confirm at some point (no ghost touches)
    uint32_t window_us = 4000; // velocity window after onset (spec 5.4, measured)
    int full = 3300;           // pressure that gives velocity 127
    float gamma = 0.8f;
    int aftertouch_step = 16;  // send PadPressure when it moved this much
    uint32_t aftertouch_interval_us = 4000;
    // After a release the pad rebounds: seen on the MK2 as a second, weak hit
    // 2-9 ms after the release (2026-11-10). No new onset on that pad for this long.
    uint32_t retrigger_holdoff_us = 25000;
};

// Velocity from the highest pressure in the window (spec 5.4).
uint8_t pad_velocity(int max_pressure_in_window, const PadConfig& cfg);

// Per-pad state machine: idle -> onset (window) -> held (aftertouch) -> release.
class PadTracker {
public:
    PadTracker(const Map& map, PadConfig cfg) : map_(map), cfg_(cfg) {}
    void reset() { state_.fill({}); }
    void process(const std::array<uint16_t, kPads>& by_hw_id, uint64_t t_us, EventBatch& out);

private:
    enum class Phase : uint8_t { Idle, Window, WaitConfirm, Held };
    struct Pad {
        Phase phase = Phase::Idle;
        uint64_t t_onset = 0;
        int window_max = 0;
        int last_sent = 0;
        uint64_t t_last_sent = 0;
        uint64_t t_release = 0;  // kept across hits (retrigger hold-off)
    };
    Map map_;
    PadConfig cfg_;
    std::array<Pad, kPads> state_{};
};

}  // namespace himmel::mk2
