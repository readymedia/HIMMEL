#include "himmel/mk2/controller.h"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#include <hidapi.h>

namespace himmel::mk2 {

namespace {

constexpr unsigned short kVid = 0x17CC;
constexpr unsigned short kPid = 0x1140;
constexpr int kHidInterface = 2;

hid_device* open_device(std::string& why) {
    hid_device_info* list = hid_enumerate(kVid, kPid);
    hid_device* dev = nullptr;
    for (auto* d = list; d && !dev; d = d->next) {
        if (d->interface_number != kHidInterface && d->interface_number != -1) continue;
        dev = hid_open_path(d->path);
        if (!dev) why = "hid_open_path failed (NI software running?)";
    }
    if (!list) why = "MK2 not found";
    hid_free_enumeration(list);
    if (dev) hid_set_nonblocking(dev, 0);
    return dev;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

struct Controller::Session {
    Session(const Map& m, const PadConfig& pc) : controls(m), pads(m, pc) {}
    hid_device* dev = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> io_error{false};
    std::atomic<uint64_t> last_report_us{0};
    ControlsParser controls;  // reader thread only
    PadTracker pads;          // reader thread only
};

Controller::Controller(const Map& map, ControllerConfig cfg, SharedBlock* shm, LogFn log)
    : map_(map), cfg_(cfg), shm_(shm), log_(std::move(log)) {}

void Controller::publish(const Event& e) {
    for (auto& ring : shm_->rings)
        if (ring.attached.load(std::memory_order_acquire)) ring.push(e);
}

void Controller::set_message(const std::string& m) {
    auto& msg = shm_->status.message;
    strncpy_s(msg, sizeof(msg), m.c_str(), _TRUNCATE);
    if (log_) log_(m);
}

// ---------------------------------------------------------------- reader
// Blocking reads only; parsing is allocation-free; events go straight to the rings.
void Controller::reader_loop(Session& s) {
    std::array<uint8_t, 128> buf{};
    std::array<uint16_t, kPads> pressures{};
    auto& st = shm_->status;
    while (!s.stop.load(std::memory_order_acquire)) {
        const int n = hid_read_timeout(s.dev, buf.data(), buf.size(), 50);
        if (n < 0) {
            s.io_error.store(true);
            return;
        }
        if (n == 0) continue;
        const uint64_t t = now_us();
        s.last_report_us.store(t, std::memory_order_release);
        EventBatch batch;
        if (buf[0] == 0x20 && n >= 1 + kPadsPayload) {
            parse_pad_report(std::span<const uint8_t>(buf.data() + 1, kPadsPayload), pressures);
            s.pads.process(pressures, t, batch);
            st.pad_reports.fetch_add(1, std::memory_order_relaxed);
        } else if (buf[0] == 0x01 && n >= 1 + kControlsPayload) {
            s.controls.parse(std::span<const uint8_t>(buf.data() + 1, kControlsPayload), t, batch);
            st.control_reports.fetch_add(1, std::memory_order_relaxed);
        }
        for (int i = 0; i < batch.count; ++i) publish(batch.items[i]);
    }
}

// ---------------------------------------------------------------- writer
// The only thread that writes to the device. Sends a report only when its bytes
// changed, LEDs at most every led_interval_us, screens (changed strips) at most
// every screen_interval_us. A new session resends everything.
void Controller::writer_loop(Session& s) {
    auto& out = shm_->outputs;
    auto& st = shm_->status;
    LedState leds{};
    std::array<uint8_t, 49> pad_r{}, pad_last{};
    std::array<uint8_t, 57> grp_r{}, grp_last{};
    std::array<uint8_t, 32> btn_r{}, btn_last{};
    bool led_force = true;
    uint32_t led_seen = 0xFFFFFFFF;
    std::array<std::array<uint8_t, kScreenBytes>, 2> screen{}, sent{};
    bool strip_force[2] = {true, true};
    uint32_t scr_seen[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    std::array<uint8_t, kStripReportSize> strip{};
    uint64_t next_led = 0, next_scr = 0;

    auto write = [&](const uint8_t* data, size_t len) {
        if (hid_write(s.dev, data, len) < 0) {
            s.io_error.store(true);
            return false;
        }
        return true;
    };

    while (!s.stop.load(std::memory_order_acquire) && !s.io_error.load()) {
        const uint64_t t = now_us();
        if (t >= next_led && (led_force || seq_peek(out.led_seq) != led_seen)) {
            if (const uint32_t r = seq_read(out.led_seq, out.leds, leds)) {
                led_seen = r - 1;
                build_pad_led_report(leds, map_, pad_r);
                build_group_led_report(leds, grp_r);
                build_button_led_report(leds, btn_r);
                auto send = [&](auto& report, auto& last) {
                    if (!led_force && report == last) return true;
                    if (!write(report.data(), report.size())) return false;
                    last = report;
                    st.led_writes.fetch_add(1, std::memory_order_relaxed);
                    return true;
                };
                if (!send(pad_r, pad_last) || !send(grp_r, grp_last) || !send(btn_r, btn_last)) return;
                led_force = false;
                next_led = t + cfg_.led_interval_us;
            }
        }
        if (t >= next_scr) {
            for (int side = 0; side < 2; ++side) {
                if (!strip_force[side] && seq_peek(out.screen_seq[side]) == scr_seen[side]) continue;
                const uint32_t r = seq_read_bytes(out.screen_seq[side], out.screen[side], screen[side].data(), kScreenBytes);
                if (!r) continue;
                scr_seen[side] = r - 1;
                for (int k = 0; k < kStrips; ++k) {
                    const uint8_t* a = screen[side].data() + k * 256;
                    uint8_t* b = sent[side].data() + k * 256;
                    if (!strip_force[side] && std::memcmp(a, b, 256) == 0) continue;
                    build_strip_report(side, k, screen[side].data(), strip);
                    if (!write(strip.data(), strip.size())) return;
                    std::memcpy(b, a, 256);
                    st.strip_writes.fetch_add(1, std::memory_order_relaxed);
                }
                strip_force[side] = false;
            }
            next_scr = t + cfg_.screen_interval_us;
        }
        sleep_ms(2);
    }
}

// ---------------------------------------------------------------- run loop
void Controller::run(const std::atomic<bool>& stop) {
    auto& st = shm_->status;
    if (hid_init() != 0) set_message("hid_init failed");
    std::string last_why;
    while (!stop.load()) {
        st.heartbeat_us.store(now_us());
        std::string why;
        hid_device* dev = open_device(why);
        if (!dev) {
            if (why != last_why) set_message("Waiting for the MK2: " + why);
            last_why = why;
            sleep_ms(static_cast<int>(cfg_.reopen_interval_ms));
            continue;
        }
        last_why.clear();
        Session s(map_, cfg_.pads);
        s.dev = dev;
        s.last_report_us.store(now_us());
        st.connected.store(1);
        publish({now_us(), EventType::Connected, 0, 0, 0, 0});
        set_message("MK2 connected");
        std::thread reader([&] { reader_loop(s); });
        std::thread writer([&] { writer_loop(s); });

        std::string reason;
        while (!stop.load()) {
            sleep_ms(20);
            const uint64_t t = now_us();
            st.heartbeat_us.store(t);
            if (s.io_error.load()) {
                reason = "USB read/write error";
                break;
            }
            // The pad report streams at ~750 Hz: silence means the device is gone.
            // Signed: the reader may store a stamp newer than t between our two reads.
            const int64_t silent = static_cast<int64_t>(t - s.last_report_us.load(std::memory_order_acquire));
            if (silent > static_cast<int64_t>(cfg_.heartbeat_timeout_us)) {
                reason = "no reports for " + std::to_string(cfg_.heartbeat_timeout_us / 1000) + " ms";
                break;
            }
        }
        s.stop.store(true);
        reader.join();  // hid_read_timeout returns within 50 ms
        writer.join();
        hid_close(dev);
        st.connected.store(0);
        publish({now_us(), EventType::Disconnected, 0, 0, 0, 0});
        if (!stop.load()) {
            st.reconnects.fetch_add(1);
            set_message("MK2 lost (" + reason + "), reopening");
        }
    }
    hid_exit();
}

}  // namespace himmel::mk2
