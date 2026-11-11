// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
#include "himmel/weapon/midi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "himmel/mk2/sim_link.h"
#include "himmel/mk2/types.h"

namespace himmel::weapon {

namespace {

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += (*w < 128) ? static_cast<char>(*w) : '?';
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Index of the first port whose name matches pick (a number or a name part) or one of the hints.
int choose(const std::vector<std::string>& names, const std::vector<std::string>& hints, const std::string& pick) {
    if (!pick.empty()) {
        if (std::all_of(pick.begin(), pick.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            const int i = std::atoi(pick.c_str());
            return i < static_cast<int>(names.size()) ? i : -1;
        }
        for (size_t i = 0; i < names.size(); ++i)
            if (lower(names[i]).find(lower(pick)) != std::string::npos) return static_cast<int>(i);
        return -1;
    }
    for (size_t i = 0; i < names.size(); ++i)
        for (const auto& h : hints)
            if (lower(names[i]).find(lower(h)) != std::string::npos) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------- WinMM
class WinMmTransport final : public MidiTransport {
public:
    WinMmTransport(std::vector<std::string> hints, std::string pick)
        : hints_(std::move(hints)), pick_(std::move(pick)) {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~WinMmTransport() override {
        close();
        CloseHandle(wake_);
    }

    bool open(std::string& why) override {
        close();
        const auto ins = list_midi_in(), outs = list_midi_out();
        const int i = choose(ins, hints_, pick_);
        int o = -1;
        if (i >= 0) {  // the out port with the same name, else by the hints
            for (size_t k = 0; k < outs.size() && o < 0; ++k)
                if (outs[k] == ins[static_cast<size_t>(i)]) o = static_cast<int>(k);
            if (o < 0) o = choose(outs, hints_, pick_);
        }
        if (i < 0 || o < 0) {
            why = "no MIDI port matching";
            for (const auto& h : hints_) why += " \"" + h + "\"";
            why += " (in:";
            for (const auto& n : ins) why += " [" + n + "]";
            why += ")";
            return false;
        }
        closing_ = false;
        MMRESULT r = midiInOpen(&hin_, static_cast<UINT>(i), reinterpret_cast<DWORD_PTR>(&WinMmTransport::callback),
                                reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            hin_ = nullptr;
            why = "midiInOpen(" + ins[static_cast<size_t>(i)] + ") failed (" + std::to_string(r) +
                  "): close other MIDI software, the port is exclusive";
            return false;
        }
        r = midiOutOpen(&hout_, static_cast<UINT>(o), 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            midiInClose(hin_);
            hin_ = nullptr;
            hout_ = nullptr;
            why = "midiOutOpen(" + outs[static_cast<size_t>(o)] + ") failed (" + std::to_string(r) + ")";
            return false;
        }
        for (size_t k = 0; k < kBuffers; ++k) {
            MIDIHDR& h = hdr_[k];
            std::memset(&h, 0, sizeof(h));
            h.lpData = reinterpret_cast<LPSTR>(buf_[k].data());
            h.dwBufferLength = static_cast<DWORD>(buf_[k].size());
            h.dwUser = k;
            midiInPrepareHeader(hin_, &h, sizeof(h));
            midiInAddBuffer(hin_, &h, sizeof(h));
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_head_ = q_tail_ = 0;
            readd_n_ = 0;
        }
        midiInStart(hin_);
        name_ = ins[static_cast<size_t>(i)];
        lost_ = false;
        return true;
    }

    void close() override {
        if (!hin_) return;
        closing_ = true;
        midiInStop(hin_);
        midiInReset(hin_);  // returns every buffer (MIM_LONGDATA with 0 bytes)
        for (auto& h : hdr_) midiInUnprepareHeader(hin_, &h, sizeof(h));
        midiInClose(hin_);
        hin_ = nullptr;
        if (hout_) {
            midiOutReset(hout_);
            midiOutClose(hout_);
            hout_ = nullptr;
        }
    }

    int read(MidiMsg& m, int timeout_ms) override {
        if (!hin_ || lost_) return -1;
        for (int pass = 0; pass < 2; ++pass) {
            readd();
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (q_tail_ != q_head_) {
                    m = q_[q_tail_ % kQueue];
                    ++q_tail_;
                    return 1;
                }
            }
            if (pass == 0 && timeout_ms > 0) WaitForSingleObject(wake_, static_cast<DWORD>(timeout_ms));
        }
        return 0;
    }

    bool send(const uint8_t* data, size_t len) override {
        if (!hout_ || len == 0) return false;
        MMRESULT r;
        if (data[0] != 0xF0 && len <= 3) {
            DWORD w = data[0];
            if (len > 1) w |= static_cast<DWORD>(data[1]) << 8;
            if (len > 2) w |= static_cast<DWORD>(data[2]) << 16;
            r = midiOutShortMsg(hout_, w);
        } else {
            if (len > out_buf_.size()) return true;  // never ours; drop
            std::memcpy(out_buf_.data(), data, len);
            MIDIHDR h{};
            h.lpData = reinterpret_cast<LPSTR>(out_buf_.data());
            h.dwBufferLength = h.dwBytesRecorded = static_cast<DWORD>(len);
            midiOutPrepareHeader(hout_, &h, sizeof(h));
            r = midiOutLongMsg(hout_, &h, sizeof(h));
            const uint64_t t_end = mk2::now_us() + 200000;
            while (r == MMSYSERR_NOERROR && !(h.dwFlags & MHDR_DONE) && mk2::now_us() < t_end)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            midiOutUnprepareHeader(hout_, &h, sizeof(h));
        }
        if (r != MMSYSERR_NOERROR) lost_ = true;
        return r == MMSYSERR_NOERROR;
    }

    bool present() override {
        if (lost_) return false;
        for (const auto& n : list_midi_in())
            if (n == name_) return true;
        return false;
    }
    std::string port_name() const override { return name_; }
    bool simulated() const override { return false; }

private:
    static constexpr size_t kBuffers = 16;
    static constexpr size_t kQueue = 256;

    static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
        // WinMM's thread: copy and return, no WinMM calls here.
        auto* self = reinterpret_cast<WinMmTransport*>(inst);
        if (msg == MIM_DATA) {
            MidiMsg m;
            const auto st = static_cast<uint8_t>(p1 & 0xFF);
            m.len = static_cast<uint16_t>(short_len(st));
            for (int k = 0; k < m.len; ++k) m.b[k] = static_cast<uint8_t>((p1 >> (8 * k)) & 0xFF);
            self->push(m);
        } else if (msg == MIM_LONGDATA || msg == MIM_LONGERROR) {
            auto* h = reinterpret_cast<MIDIHDR*>(p1);
            if (h->dwBytesRecorded)
                self->asm_.feed(reinterpret_cast<const uint8_t*>(h->lpData), h->dwBytesRecorded,
                                [&](const MidiMsg& m) { self->push(m); });
            if (!self->closing_) {
                std::lock_guard<std::mutex> lk(self->mu_);
                if (self->readd_n_ < kBuffers) self->readd_[self->readd_n_++] = h->dwUser;
            }
            SetEvent(self->wake_);
        }
    }

    void push(const MidiMsg& m) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_head_ - q_tail_ < kQueue) {  // full: drop (the reader is far behind)
                q_[q_head_ % kQueue] = m;
                ++q_head_;
            }
        }
        SetEvent(wake_);
    }

    void readd() {
        size_t list[kBuffers];
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            n = readd_n_;
            std::copy(readd_, readd_ + n, list);
            readd_n_ = 0;
        }
        for (size_t k = 0; k < n && hin_ && !closing_; ++k) {
            MIDIHDR& h = hdr_[list[k]];
            h.dwBytesRecorded = 0;
            midiInAddBuffer(hin_, &h, sizeof(h));
        }
    }

    std::vector<std::string> hints_;
    std::string pick_, name_;
    HMIDIIN hin_ = nullptr;
    HMIDIOUT hout_ = nullptr;
    HANDLE wake_ = nullptr;
    volatile bool closing_ = false;
    bool lost_ = false;
    MIDIHDR hdr_[kBuffers]{};
    std::array<std::array<uint8_t, 1024>, kBuffers> buf_{};
    std::array<uint8_t, MidiMsg::kCap> out_buf_{};
    SysexAssembler asm_;  // callback thread only
    std::mutex mu_;
    MidiMsg q_[kQueue];
    size_t q_head_ = 0, q_tail_ = 0;
    size_t readd_[kBuffers]{};
    size_t readd_n_ = 0;
};

// ---------------------------------------------------------------- UDP (simulators)
class UdpTransport final : public MidiTransport {
public:
    UdpTransport(uint16_t port, uint32_t silent_ms) : port_(port), silent_us_(uint64_t{silent_ms} * 1000) {}

    bool open(std::string& why) override {
        if (!link_.bind(port_, why)) return false;
        // The first datagram means "a simulator is there" and tells us where it is.
        MidiMsg m;
        const uint64_t t_end = mk2::now_us() + 500000;
        while (mk2::now_us() < t_end) {
            const int n = link_.recv(m.b, MidiMsg::kCap, 100);
            if (n > 0) {
                link_.take_peer_changed();
                pending_ = m;
                pending_.len = static_cast<uint16_t>(n);
                has_pending_ = true;
                last_rx_ = mk2::now_us();
                return true;
            }
        }
        why = "no simulator on udp 127.0.0.1:" + std::to_string(port_);
        return false;
    }
    void close() override {}
    int read(MidiMsg& m, int timeout_ms) override {
        if (has_pending_) {
            m = pending_;
            has_pending_ = false;
            return 1;
        }
        const int n = link_.recv(m.b, MidiMsg::kCap, timeout_ms);
        if (n < 0) return -1;
        if (link_.take_peer_changed()) return -1;  // a new simulator: new session
        if (n == 0) return 0;
        m.len = static_cast<uint16_t>(n);
        last_rx_ = mk2::now_us();
        return 1;
    }
    bool send(const uint8_t* data, size_t len) override {
        link_.send(data, len);
        return true;
    }
    bool present() override { return mk2::now_us() - last_rx_ < silent_us_; }
    std::string port_name() const override { return "udp 127.0.0.1:" + std::to_string(port_); }
    bool simulated() const override { return true; }

private:
    mk2::SimLink link_;
    uint16_t port_;
    uint64_t silent_us_;
    uint64_t last_rx_ = 0;
    MidiMsg pending_;
    bool has_pending_ = false;
};

}  // namespace

std::vector<std::string> list_midi_in() {
    std::vector<std::string> out;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        out.push_back(midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname) : "?");
    }
    return out;
}

std::vector<std::string> list_midi_out() {
    std::vector<std::string> out;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        out.push_back(midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? narrow(caps.szPname) : "?");
    }
    return out;
}

std::unique_ptr<MidiTransport> make_winmm(std::vector<std::string> hints, std::string pick) {
    return std::make_unique<WinMmTransport>(std::move(hints), std::move(pick));
}

std::unique_ptr<MidiTransport> make_udp(uint16_t port, uint32_t silent_ms) {
    return std::make_unique<UdpTransport>(port, silent_ms);
}

}  // namespace himmel::weapon
