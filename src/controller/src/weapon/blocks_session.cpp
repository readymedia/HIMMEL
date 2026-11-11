// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/weapon/blocks_session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace himmel::weapon::blocks {

namespace {
// The block's own defaults (owner's Lightpad M; ROLI's defaultConfig agrees per blocksd).
const std::map<uint32_t, int32_t> kDefaults = {{kCfgVelocitySensitivity, 50}, {kCfgPressureSensitivity, 64}};
}  // namespace

DeviceSetup lightpad_setup() {
    // Pressure sensitivity 127: the owner's choice (2026-11-10), the longest span from light to full.
    return {Kind::Lightpad, "lightpad", "Lightpad", 2, kSimPortLightpad, {"Lightpad"},
            kGrid * kGrid, kGrid, kGrid, {{kCfgPressureSensitivity, 127}},
            {kCfgPressureSensitivity, kCfgVelocitySensitivity}};
}

DeviceSetup lumi_setup() {
    // With Windows' USBMidi2-ACX driver the LUMI's port is called "USBMidi2-ACX" (weapons/README.md).
    return {Kind::Lumi, "lumi", "LUMI Keys", 3, kSimPortLumi, {"LUMI", "USBMidi2-ACX"},
            kLumiKeys, kLumiKeys, 1, {},
            {kCfgPressureSensitivity, kCfgVelocitySensitivity, kCfgOctave}};
}

Header make_header(const DeviceSetup& d, const Profile& p, bool sim) {
    Header h{};
    set_text(h.weapon, d.weapon);
    set_text(h.name, d.name);
    set_text(h.profile, p.path);
    set_text(h.driver, "weapon_blocks");
    h.controls = static_cast<uint32_t>(p.count());
    h.pixel_w = static_cast<uint16_t>(d.pixel_w);
    h.pixel_h = static_cast<uint16_t>(d.pixel_h);
    h.simulated = sim ? 1 : 0;
    std::memset(h.role, kRoleNone, sizeof(h.role));
    std::memset(h.knob_slot, kNone, sizeof(h.knob_slot));
    std::memset(h.pad_control, kNone, sizeof(h.pad_control));
    auto role = [&](const char* name, Role r) {
        const int i = p.index(name);
        if (i >= 0 && i < kMaxControls) h.role[i] = r;
    };
    if (d.kind == Kind::Lightpad) {
        h.caps = kCapButtons | kCapPads | kCapTouch | kCapPixels;
        h.pads = 16;  // the surface as 4 x 4 zones, Pad 1 bottom-left (like the factory note grid)
        role("Mode", kRolePageNext);
    } else {
        h.caps = kCapButtons | kCapPads | kCapPixels;
        h.pads = kLumiKeys;
        role("Mode", kRolePageNext);
        role("Octave -", kRoleBrowsePrev);
        role("Octave +", kRoleBrowseNext);
        for (int k = 0; k < kLumiKeys; ++k) {
            const int i = p.index("Key " + std::to_string(k + 1));
            if (i >= 0) h.pad_control[k] = static_cast<uint8_t>(i);
        }
    }
    return h;
}

BlocksSession::BlocksSession(const DeviceSetup& d, const Profile& p, SharedMemory& shm) : d_(d), shm_(shm) {
    program_ = d.kind == Kind::Lightpad ? build_bitmap_program() : build_key_program(kLumiKeys);
    mode_idx_ = p.index("Mode");
    if (d.kind == Kind::Lightpad) {
        button_idx_ = {mode_idx_, -1, -1};
    } else {
        button_idx_ = {mode_idx_, p.index("Octave -"), p.index("Octave +")};
        for (int k = 0; k < kLumiKeys; ++k) key_idx_[k] = p.index("Key " + std::to_string(k + 1));
    }
}

bool BlocksSession::send(MidiTransport& io, const MidiMsg& m) {
    if (!io.send(m.b, m.len)) return false;
    shm_.get()->status.msgs_out.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BlocksSession::publish(EventType type, uint8_t id, int16_t value, uint16_t raw, uint16_t reserved, uint64_t t) {
    shm_.publish({t, type, id, value, raw, reserved});
}

bool BlocksSession::start(MidiTransport&, uint64_t) {
    api_ = false;  // the first tick does the handshake
    return true;
}

bool BlocksSession::handshake(MidiTransport& io, uint64_t t) {
    // Order checked on the owner's blocks: serial, topology, endAPIMode, beginAPIMode.
    if (!send(io, serial_request()) || !send(io, host_command(kCmdTopology)) || !send(io, host_command(kCmdEndApi)) ||
        !send(io, host_command(kCmdBeginApi)))
        return false;
    sync_.reset();
    stage_ = Stage::Clear;
    api_ = true;
    next_ping_ = t + kPingIntervalUs;
    last_rx_ = t;
    pixel_seen_ = 0xFFFFFFFF;
    for (uint32_t item : d_.config_read)  // learn the block's own values first
        if (!send(io, host_config_request(item))) return false;
    for (const auto& [item, v] : d_.want)
        if (!send(io, host_config_set(item, v)) || !send(io, host_config_request(item))) return false;
    return true;
}

bool BlocksSession::tick(MidiTransport& io, uint64_t t) {
    auto& st = shm_.get()->status;
    if (!api_ || t - last_rx_ > kNoAnswerUs) {
        if (api_) shm_.set_message("no answer for 3 s in API mode: handshake again");
        return handshake(io, t);
    }
    for (uint8_t f = 0; f < hit_due_.size(); ++f)  // a resting finger that sent no move in time
        if (hit_due_[f] && t >= hit_due_[f]) fire_hit(f, t);
    if (t >= next_ping_) {  // pings first: never behind LED traffic
        next_ping_ = t + kPingIntervalUs;
        if (!send(io, host_command(kCmdPing))) return false;
    }
    for (const auto& p : sync_.due(t))
        if (!send(io, p.msg)) return false;
    st.resends.store(sync_.resends, std::memory_order_relaxed);
    if (sync_.busy()) return true;

    if (stage_ == Stage::Clear) {  // like ROLI's host: wipe the program memory (the factory program stops)
        frame_.fill(0);
        for (const auto& p : sync_.start(frame_.data(), t))
            if (!send(io, p.msg)) return false;
        stage_ = Stage::Program;
    } else if (stage_ == Stage::Program) {
        std::copy(program_.begin(), program_.end(), frame_.begin());
        for (const auto& p : sync_.start(frame_.data(), t))
            if (!send(io, p.msg)) return false;
        stage_ = Stage::Frames;
        shm_.set_message(std::string(d_.name) + ": Himmel's LED renderer uploaded (" + std::to_string(program_.size()) +
                         " bytes)");
    } else {
        if (t - last_pic_ < 15000) return true;  // at most ~66 frames a second
        last_pic_ = t;
        build_pixels();
        pixels_to_bytes(reinterpret_cast<const uint8_t(*)[3]>(pixels_.data()), d_.pixels, frame_.data() + program_.size());
        if (std::memcmp(frame_.data(), sync_.target().data(), kDataBlock) != 0) {
            for (const auto& p : sync_.start(frame_.data(), t))
                if (!send(io, p.msg)) return false;
        }
    }
    st.led_frames.store(sync_.frames, std::memory_order_relaxed);
    st.sync_us.store(sync_.sync_us, std::memory_order_relaxed);
    return true;
}

// The engine's pixels; until the engine writes any, a quiet idle picture that still
// shows the fingers / keys (so a driver on its own is visibly alive).
void BlocksSession::build_pixels() {
    auto& out = shm_.get()->outputs;
    if (mk2::seq_peek(out.pixel_seq) != 0) {
        if (mk2::seq_peek(out.pixel_seq) == pixel_seen_) return;
        decltype(out.pixel) px;
        if (const uint32_t r = mk2::seq_read(out.pixel_seq, out.pixel, px)) {
            pixel_seen_ = r - 1;
            std::memcpy(pixels_.data(), px, sizeof(px));
        }
        return;
    }
    const int n = d_.pixels;
    for (int i = 0; i < n; ++i) {
        uint8_t* p = &pixels_[static_cast<size_t>(i) * 3];
        p[0] = 0, p[1] = 6, p[2] = 24;  // Himmel blue, dim
    }
    if (d_.kind == Kind::Lightpad) {
        for (const auto& f : fingers_) {
            if (!f[2]) continue;
            const float fx = f[0] / 4095.0f * kGrid - 0.5f, fy = f[1] / 4095.0f * kGrid - 0.5f;
            const float rad = 0.9f + 1.8f * (f[2] - 1) / 255.0f;
            for (int y = 0; y < kGrid; ++y)
                for (int x = 0; x < kGrid; ++x) {
                    const float dd = std::hypot(x - fx, y - fy);
                    const float k = std::max(0.0f, 1.0f - dd / rad);
                    uint8_t* p = &pixels_[static_cast<size_t>(y * kGrid + x) * 3];
                    p[0] = std::max(p[0], static_cast<uint8_t>(120 * k));
                    p[1] = std::max(p[1], static_cast<uint8_t>(200 * k));
                    p[2] = std::max(p[2], static_cast<uint8_t>(255 * k));
                }
        }
    } else {
        for (int k = 0; k < kLumiKeys; ++k) {
            uint8_t* p = &pixels_[static_cast<size_t>(k) * 3];
            if (key_vel_[k]) p[0] = p[1] = p[2] = 255;
            else if (k % 12 == 0) p[0] = 40, p[1] = 20, p[2] = 120;
        }
    }
}

void BlocksSession::on_message(const MidiMsg& m, uint64_t t) {
    if (!m.sysex()) {
        on_midi(m, t);
        return;
    }
    if (std::memcmp(m.b, kSerialHeader, 4) == 0) last_rx_ = t;  // any BLOCKS SysEx = the block is there
    DeviceEvent ev[32];
    const int n = decode_device(m.b, m.len, ev, 32);
    for (int i = 0; i < n; ++i) on_device(ev[i], t);
}

void BlocksSession::on_device(const DeviceEvent& e, uint64_t t) {
    auto& st = shm_.get()->status;
    switch (e.kind) {
        case DeviceEvent::Serial:
            set_text(st.device, e.text);
            log_line(std::string(d_.name) + " serial " + e.text);
            break;
        case DeviceEvent::Topology: st.battery.store(e.battery); break;
        case DeviceEvent::Ack:
            st.acks.fetch_add(1, std::memory_order_relaxed);
            sync_.on_ack(static_cast<uint16_t>(e.id), t);
            break;
        case DeviceEvent::Button: button(e.id, e.down, e.via_program, t); break;
        case DeviceEvent::Config: {
            if (e.id == 0xFFFFFFFF) break;
            config_[e.id] = {e.value, e.min, e.max};
            int32_t v = e.value;
            auto w = d_.want.find(e.id);
            auto def = kDefaults.find(e.id);
            if (w != d_.want.end() && v == w->second && def != kDefaults.end())
                v = def->second;  // our own leftover (Himmel was killed): use the block's default
            original_.emplace(e.id, v);
            break;
        }
        case DeviceEvent::Touch: {
            if (d_.kind != Kind::Lightpad || e.index >= 32) break;
            const uint8_t f = e.index;
            const auto reserved = static_cast<uint16_t>(e.z | (static_cast<uint8_t>(e.vz) << 8));
            const auto type = e.phase == 0 ? EventType::TouchStart : e.phase == 1 ? EventType::TouchMove : EventType::TouchEnd;
            publish(type, f, static_cast<int16_t>(e.x), e.y, reserved, t);
            if (e.phase == 0) {
                // The surface as 4 x 4 pads, Pad 1 bottom-left (y = 0 is the top edge).
                const int col = std::min(3, e.x * 4 / 4096), row = std::min(3, (4095 - e.y) * 4 / 4096);
                const int pad = 1 + col + 4 * row;
                finger_pad_[f] = static_cast<int8_t>(pad);
                hit_z_[f] = e.z;
                hit_due_[f] = t + kVelocityWaitUs;
                if (e.z >= kVelocityNowZ) fire_hit(f, t);  // a firm strike: no need to wait
                fingers_[f] = {e.x, e.y, static_cast<uint16_t>(e.z + 1)};
            } else if (e.phase == 1) {
                if (hit_due_[f]) {
                    hit_z_[f] = std::max(hit_z_[f], e.z);
                    fire_hit(f, t);
                }
                if (finger_pad_[f])
                    publish(EventType::PadPressure, static_cast<uint8_t>(finger_pad_[f]), static_cast<int16_t>(e.z * 16), e.z,
                            kNone, t);
                fingers_[f] = {e.x, e.y, static_cast<uint16_t>(e.z + 1)};
            } else {
                if (hit_due_[f]) fire_hit(f, t);  // a tap shorter than the wait still hits
                if (finger_pad_[f]) publish(EventType::PadRelease, static_cast<uint8_t>(finger_pad_[f]), 0, 0, kNone, t);
                finger_pad_[f] = 0;
                fingers_[f] = {0, 0, 0};
            }
            break;
        }
        default: break;
    }
}

void BlocksSession::fire_hit(uint8_t f, uint64_t t) {
    hit_due_[f] = 0;
    if (!finger_pad_[f]) return;
    const int vel = std::clamp(static_cast<int>(std::lround(127.0 * std::sqrt(hit_z_[f] / 255.0))), 1, 127);
    publish(EventType::PadHit, static_cast<uint8_t>(finger_pad_[f]), static_cast<int16_t>(vel), hit_z_[f], kNone, t);
}

void BlocksSession::button(uint32_t id, bool down, bool via_program, uint64_t t) {
    // The LUMI (firmware 1.3.8) sends every button twice: its own message and our renderer's
    // program event. Once a firmware button message has been seen, the program copies are
    // ignored (the Lightpad only sends the program ones).
    if (via_program) {
        if (firmware_buttons_) return;
    } else {
        firmware_buttons_ = true;
    }
    if (id >= button_idx_.size() || button_idx_[id] < 0) return;
    publish(down ? EventType::ButtonDown : EventType::ButtonUp, static_cast<uint8_t>(button_idx_[id]), 0, 0, 0, t);
}

void BlocksSession::on_midi(const MidiMsg& m, uint64_t t) {
    if (d_.kind != Kind::Lumi || m.len < 2) return;  // the Lightpad's MPE is not used (our program stops it)
    const uint8_t st = m.b[0] & 0xF0, ch = m.b[0] & 0x0F;
    auto oct = config_.find(kCfgOctave);
    const int octave = oct != config_.end() ? oct->second[0] : 0;
    if ((st == 0x90 || st == 0x80) && m.len >= 3) {
        const int k = m.b[1] - kLumiBaseNote - 12 * octave;
        if (k < 0 || k >= kLumiKeys) return;
        const auto res = static_cast<uint16_t>(key_idx_[k] >= 0 ? key_idx_[k] : kNone);
        if (st == 0x90 && m.b[2] > 0) {
            key_vel_[k] = m.b[2];
            chan_key_[ch] = static_cast<int8_t>(k + 1);
            publish(EventType::PadHit, static_cast<uint8_t>(k + 1), m.b[2], 0, res, t);
        } else if (key_vel_[k]) {
            key_vel_[k] = 0;
            if (chan_key_[ch] == k + 1) chan_key_[ch] = 0;
            publish(EventType::PadRelease, static_cast<uint8_t>(k + 1), 0, 0, res, t);
        }
    } else if (st == 0xD0) {
        const int k = chan_key_[ch] - 1;
        if (k < 0 || !key_vel_[k]) return;
        const auto res = static_cast<uint16_t>(key_idx_[k] >= 0 ? key_idx_[k] : kNone);
        publish(EventType::PadPressure, static_cast<uint8_t>(k + 1), static_cast<int16_t>(m.b[1] * 32), m.b[1], res, t);
    }
}

void BlocksSession::let_go(MidiTransport& io) {
    if (!api_) return;
    // Config values are KEPT by the block across sessions (checked on a Lightpad M): set each
    // one back and wait until the block reports it (max ~0.4 s per try, 3 tries).
    for (const auto& [item, v] : original_) {
        auto w = d_.want.find(item);
        if (w == d_.want.end() || w->second == v) continue;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (!send(io, host_config_set(item, v)) || !send(io, host_config_request(item))) return;
            const uint64_t t_end = now_us() + 400000;
            while (now_us() < t_end && !ok) {
                MidiMsg m;
                if (io.read(m, 10) > 0) on_message(m, now_us());
                auto c = config_.find(item);
                ok = c != config_.end() && c->second[0] == v;
            }
        }
        log_line(std::string(d_.name) + ": config " + std::to_string(item) + " back to " + std::to_string(v) +
                 (ok ? " (confirmed)" : " (NOT confirmed)"));
    }
    send(io, host_command(kCmdEndApi));  // the factory program and display come back (RAM only, nothing saved)
}

}  // namespace himmel::weapon::blocks
