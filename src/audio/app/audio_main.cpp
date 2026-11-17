// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// himmel_audio.exe - audio in, analysis and MIDI clock (docs/interfaces/audio-block.md).
//
//   himmel_audio [--device NAME | --loopback [NAME]] [--channels 1|2|N|N-M]
//                [--gain DB] [--no-auto-gain] [--gate DB] [--sensitivity 0..1]
//                [--midi-clock NAME] [--monitor]
//   himmel_audio --list        endpoints and MIDI ports
//   himmel_audio --selftest    synthetic drums through the analysis (no device), exit 0 / 1
//   himmel_audio --play-drums [DEVICE] [--bpm N] [--seconds S] [--level L]   test player (a render endpoint)
//
// Threads: the capture worker (WASAPI read -> analysis -> `signals`, MMCSS "Pro Audio"),
// and the main thread (heartbeat, `input`, `clock`, `config`, stop). The MIDI port has
// its own watcher and WinMM's callback thread. A lost device is retried every second
// and reopened by name; while it is away the signals glide to rest.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "himmel/audio/analyzer.h"
#include "himmel/audio/block.h"
#include "himmel/audio/midi_clock.h"
#include "himmel/audio/midi_port.h"
#include "himmel/audio/synth.h"
#include "himmel/audio/wasapi.h"

using namespace himmel::audio;
namespace proto = himmel::protocol;

namespace {

std::atomic<bool> g_stop{false};
BOOL WINAPI on_console(DWORD) {
    g_stop = true;
    return TRUE;
}

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void log(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[512];
    const int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d  audio  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    std::printf("%s\n", line);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- settings
struct Settings {
    InputRequest input;
    AnalyzerSettings analysis;
    std::string midi_port;
};

bool same_input(const InputRequest& a, const InputRequest& b) {
    return a.mode == b.mode && a.device == b.device && a.channel_first == b.channel_first && a.channel_count == b.channel_count;
}

// ---------------------------------------------------------------- the block
struct Block {
    HANDLE mapping = nullptr;
    AudioBlock* b = nullptr;
    bool open() {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(AudioBlock), kBlockName);
        if (!mapping) return false;
        b = static_cast<AudioBlock*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioBlock)));
        if (!b) return false;
        // Header last (after a fence): a reader that sees the magic sees the rest.
        b->header.version = kVersion;
        b->header.size = sizeof(AudioBlock);
        b->header.reserved = 0;
        std::atomic_thread_fence(std::memory_order_release);
        reinterpret_cast<std::atomic<uint32_t>&>(b->header.magic).store(kMagic, std::memory_order_release);
        b->status.pid.store(GetCurrentProcessId());
        b->status.stop_request.store(0);
        return true;
    }
    ~Block() {
        if (b) UnmapViewOfFile(b);
        if (mapping) CloseHandle(mapping);
    }
};

// ---------------------------------------------------------------- capture worker
class Worker {
public:
    Worker(AudioBlock* b, const Settings& s) : b_(b), settings_(s) {}
    // --record PATH [SECONDS]: the first seconds of mono input as raw float32 (start-up only).
    void record(const std::string& path, double seconds) {
        if (fopen_s(&record_, path.c_str(), "wb") != 0) record_ = nullptr;
        record_left_ = static_cast<int64_t>(seconds * 48000);  // GUESS: 48 kHz; the log says the real rate
    }
    void start() { thread_ = std::thread([this] { run(); }); }
    void join() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    void set(const Settings& s) {
        std::lock_guard<std::mutex> lk(m_);
        settings_ = s;
        ++gen_;
    }
    InputStatus status() {
        std::lock_guard<std::mutex> lk(m_);
        return status_;
    }

private:
    void set_state(InputState st, const WasapiInput* in) {
        std::lock_guard<std::mutex> lk(m_);
        if (status_.state != static_cast<uint8_t>(st)) status_.since_us = now_us();
        status_.state = static_cast<uint8_t>(st);
        status_.mode = static_cast<uint8_t>(settings_.input.mode);
        if (in && in->is_open()) {
            status_.sample_rate = static_cast<uint32_t>(in->sample_rate());
            status_.device_channels = static_cast<uint32_t>(in->device_channels());
            status_.channel_first = static_cast<uint8_t>(in->channel_first());
            status_.channel_count = static_cast<uint8_t>(in->channel_count());
            status_.glitches = in->glitches();
            std::snprintf(status_.device, sizeof(status_.device), "%s", in->name().c_str());
        }
    }

    static void sink(const float* x, int n, uint64_t t_last, void* ctx) {
        auto* w = static_cast<Worker*>(ctx);
        if (w->record_ && w->record_left_ > 0) {  // --record: raw mono float32 for offline checks
            const int k = static_cast<int>(std::min<int64_t>(n, w->record_left_));
            std::fwrite(x, sizeof(float), static_cast<size_t>(k), w->record_);
            w->record_left_ -= k;
            if (w->record_left_ <= 0) {
                std::fclose(w->record_);
                w->record_ = nullptr;
                log("record: done");
            }
        }
        const float sr = w->an_.sample_rate();
        int left = n;
        const float* p = x;
        while (left > 0) {
            if (w->an_.feed(p, left)) {
                // time of the hop's newest sample
                const uint64_t t = t_last - static_cast<uint64_t>(static_cast<double>(left) * 1e6 / sr);
                w->publish(t);
            }
        }
    }

    void publish(uint64_t t) {
        Signals s = an_.out();
        s.time_us = t;
        proto::seq_write(b_->signals_seq, b_->signals, s);
        last_publish_us_.store(now_us(), std::memory_order_relaxed);
    }

public:
    uint64_t last_publish_us() const { return last_publish_us_.load(std::memory_order_relaxed); }

private:

    // Input away: let the signals release at their normal speed, one hop every 5 ms.
    void idle(int ms) {
        const uint64_t end = now_us() + static_cast<uint64_t>(ms) * 1000;
        while (!stop_ && now_us() < end) {
            an_.feed_silence_hop();
            publish(now_us());
            Sleep(5);
            if (gen_seen_ != gen_.load()) return;
        }
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        WasapiInput in;
        uint32_t opens = 0;
        bool logged_fail = false;
        while (!stop_) {
            Settings s;
            {
                std::lock_guard<std::mutex> lk(m_);
                s = settings_;
                gen_seen_ = gen_.load();
            }
            an_.set_settings(s.analysis);
            set_state(InputState::Opening, nullptr);
            std::string err;
            if (!in.open(s.input, &err)) {
                if (!logged_fail) log("input: cannot open %s%s: %s - retrying every second",
                                      s.input.mode == InputMode::Loopback ? "loopback " : "",
                                      s.input.device.empty() ? "(default device)" : s.input.device.c_str(), err.c_str());
                logged_fail = true;
                set_state(InputState::Lost, nullptr);
                idle(1000);
                continue;
            }
            logged_fail = false;
            ++opens;
            an_.reset(static_cast<float>(in.sample_rate()));
            an_.set_settings(s.analysis);
            {
                std::lock_guard<std::mutex> lk(m_);
                status_.opens = opens;
            }
            set_state(InputState::Running, &in);
            log("input: %s \"%s\", %d Hz, %d ch, using %d-%d", s.input.mode == InputMode::Loopback ? "loopback" : "capture",
                in.name().c_str(), in.sample_rate(), in.device_channels(), in.channel_first() + 1,
                in.channel_first() + in.channel_count());
            uint64_t last_data = now_us();
            bool lost = false;
            while (!stop_) {
                const auto r = in.read(50, &Worker::sink, this);
                const uint64_t t = now_us();
                if (r == WasapiInput::Read::Lost) {
                    lost = true;
                    break;
                }
                if (r == WasapiInput::Read::Data) last_data = t;
                if (t - last_data > 500000) {
                    if (s.input.mode == InputMode::Capture) {  // a capture device always delivers
                        lost = true;
                        break;
                    }
                }
                // Loopback with nothing playing delivers nothing: feed silence on time.
                if (s.input.mode == InputMode::Loopback && t - last_publish_us_.load() > 20000) {
                    an_.feed_silence_hop();
                    publish(t);
                }
                if (gen_seen_ != gen_.load()) {
                    Settings ns;
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        ns = settings_;
                    }
                    if (!same_input(ns.input, s.input)) break;  // reopen
                    gen_seen_ = gen_.load();
                    s = ns;
                    an_.set_settings(s.analysis);
                }
                set_state(InputState::Running, &in);
            }
            in.close();
            if (lost) {
                log("input: \"%s\" lost - retrying every second", s.input.device.empty() ? "default device" : s.input.device.c_str());
                set_state(InputState::Lost, nullptr);
                idle(1000);
            }
        }
        set_state(InputState::None, nullptr);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
    }

    AudioBlock* b_;
    Analyzer an_;
    std::mutex m_;
    Settings settings_;
    std::atomic<uint32_t> gen_{0};
    uint32_t gen_seen_ = 0;
    InputStatus status_{};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> last_publish_us_{0};
    std::FILE* record_ = nullptr;
    int64_t record_left_ = 0;
    std::thread thread_;
};

// ---------------------------------------------------------------- args
bool parse_channels(const std::string& v, InputRequest& r) {
    int a = 0, b = 0;
    if (sscanf_s(v.c_str(), "%d-%d", &a, &b) == 2 && a >= 1 && b == a + 1) {
        r.channel_first = a - 1, r.channel_count = 2;
        return true;
    }
    if (sscanf_s(v.c_str(), "%d", &a) == 1 && a >= 1) {
        // "1" = mono channel 1, "2" = the stereo pair 1-2 (the usual meaning), "N" > 2 = mono channel N
        if (a == 2) r.channel_first = 0, r.channel_count = 2;
        else r.channel_first = a - 1, r.channel_count = 1;
        return true;
    }
    return false;
}

Settings from_config(const AudioConfig& c) {
    Settings s;
    s.input.mode = c.mode == static_cast<uint8_t>(InputMode::Loopback) ? InputMode::Loopback : InputMode::Capture;
    s.input.device.assign(c.device, strnlen(c.device, sizeof(c.device)));
    s.input.channel_first = c.channel_first;
    s.input.channel_count = std::clamp<int>(c.channel_count, 1, 2);
    s.analysis.auto_gain = c.auto_gain != 0;
    s.analysis.gain_db = std::isfinite(c.gain_db) ? std::clamp(c.gain_db, -24.0f, 48.0f) : 0.0f;
    s.analysis.gate_db = std::isfinite(c.gate_db) ? std::clamp(c.gate_db, -96.0f, -20.0f) : -60.0f;
    s.analysis.onset_sensitivity = std::isfinite(c.onset_sensitivity) ? std::clamp(c.onset_sensitivity, 0.0f, 1.0f) : 0.5f;
    s.midi_port.assign(c.midi_clock_port, strnlen(c.midi_clock_port, sizeof(c.midi_clock_port)));
    return s;
}

// ---------------------------------------------------------------- selftest
int selftest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    };
    std::printf("himmel_audio selftest\n");
    for (const float level : {1.0f, 0.02f}) {  // a line (hot) and a far mic (-34 dB)
        DrumSynth d;
        d.level = level;
        d.noise_gain = 0.002f;
        Analyzer an(48000.0f);
        int kicks = 0, late = 0;
        uint32_t seen = 0;
        double level_sum = 0;
        int level_n = 0;
        const int total = 48000 * 24;
        float buf[480];
        for (int done = 0; done < total; done += 480) {
            for (float& v : buf) v = d.next();
            const float* p = buf;
            int n = 480;
            while (n > 0) {
                if (!an.feed(p, n)) continue;
                const double t = an.seconds();
                if (an.out().kicks != seen) {
                    seen = an.out().kicks;
                    if (t > 8.0) {  // after auto gain has settled
                        ++kicks;
                        const double since = t - d.beat_time(static_cast<int>(std::floor(t * d.bpm / 60.0)));
                        if (since > 0.020) ++late;
                    }
                }
                if (t > 20.0) level_sum += sig(an.out(), Sig::Level), ++level_n;
            }
        }
        char what[128];
        std::snprintf(what, sizeof(what), "level %.2f: 32 kicks in 16 s at 120 BPM (got %d), none later than 20 ms (late %d)",
                      level, kicks, late);
        check(kicks == 32 && late == 0, what);
        std::snprintf(what, sizeof(what), "level %.2f: auto gain lifts the mean level over 0.4 (got %.2f, gain %+.1f dB)", level,
                      level_sum / std::max(1, level_n), an.out().gain_db);
        check(level_sum / std::max(1, level_n) > 0.4, what);
    }
    {
        MidiClock mc;
        uint64_t t = 1000000;
        mc.message(0xFA, 0, 0, t);
        const double tick = 60e6 / (128.0 * 24);
        for (int i = 0; i < 24 * 8; ++i) mc.message(0xF8, 0, 0, t + static_cast<uint64_t>(i * tick));
        const ClockSource s = mc.sample(t + static_cast<uint64_t>((24 * 8 - 1) * tick));
        char what[128];
        std::snprintf(what, sizeof(what), "MIDI clock 128 BPM: bpm %.2f, beat %.3f, confidence %.2f", s.bpm, s.beat, s.confidence);
        check(std::fabs(s.bpm - 128.0f) < 0.1f && std::fabs(s.beat - (24 * 8 - 1) / 24.0) < 1e-6 && s.confidence > 0.9f, what);
    }
    {
        static AudioBlock blk{};
        Signals a{};
        a.hop = 42;
        a.v[1] = 0.5f;
        proto::seq_write(blk.signals_seq, blk.signals, a);
        Signals b{};
        check(proto::seq_read(blk.signals_seq, blk.signals, b) != 0 && b.hop == 42 && b.v[1] == 0.5f, "block seqlock round trip");
    }
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}

void list() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("Capture endpoints (mic / line in):\n");
    for (const auto& e : list_endpoints(InputMode::Capture))
        std::printf("  %s %s  (%d ch, %d Hz)\n", e.is_default ? "*" : " ", e.name.c_str(), e.channels, e.sample_rate);
    std::printf("Render endpoints (for --loopback):\n");
    for (const auto& e : list_endpoints(InputMode::Loopback))
        std::printf("  %s %s  (%d ch, %d Hz)\n", e.is_default ? "*" : " ", e.name.c_str(), e.channels, e.sample_rate);
    std::printf("MIDI inputs (for --midi-clock):\n");
    for (const auto& n : MidiClockPort::list()) std::printf("    %s\n", n.c_str());
    CoUninitialize();
}

void bar(char* out, float v, int width) {
    const int n = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * width + 0.5f);
    for (int i = 0; i < width; ++i) out[i] = i < n ? '#' : '.';
    out[width] = 0;
}

}  // namespace

int main(int argc, char** argv) {
    Settings s;
    bool monitor = false;
    std::string record_path;
    double record_s = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto has_next = [&] { return i + 1 < argc && argv[i + 1][0] != '-'; };
        if (a == "--selftest") return selftest();
        else if (a == "--analyse" && i + 1 < argc) {  // offline: a --record file (raw mono float32, 48 kHz)
            std::FILE* f = nullptr;
            if (fopen_s(&f, argv[++i], "rb") != 0 || !f) return 1;
            Analyzer an(48000.0f, s.analysis);
            float buf[480];
            size_t got;
            uint32_t k = 0, sn = 0, h = 0;
            while ((got = std::fread(buf, sizeof(float), 480, f)) > 0) {
                const float* p = buf;
                int n = static_cast<int>(got);
                while (n > 0)
                    if (an.feed(p, n)) {
                        const Signals& o = an.out();
                        if (o.kicks != k || o.snares != sn || o.hats != h)
                            std::printf("%8.3f s  %s%s%s  rms %6.1f dB\n", an.seconds(), o.kicks != k ? "KICK " : "",
                                        o.snares != sn ? "SNARE " : "", o.hats != h ? "HAT" : "", o.rms_db);
                        k = o.kicks, sn = o.snares, h = o.hats;
                    }
            }
            std::fclose(f);
            std::printf("%.1f s: %u kicks, %u snares, %u hats\n", an.seconds(), k, sn, h);
            return 0;
        }
        else if (a == "--play-drums") {  // test player: --play-drums [DEVICE] [--bpm N] [--seconds S] [--level L]
            std::string dev;
            float bpm = 120, level = 0.5f;
            double secs = 30;
            if (has_next()) dev = argv[++i];
            for (++i; i < argc; ++i) {
                const std::string o = argv[i];
                if (o == "--bpm" && i + 1 < argc) bpm = std::clamp(static_cast<float>(std::atof(argv[++i])), 40.0f, 240.0f);
                else if (o == "--seconds" && i + 1 < argc) secs = std::clamp(std::atof(argv[++i]), 1.0, 86400.0);
                else if (o == "--level" && i + 1 < argc) level = static_cast<float>(std::atof(argv[++i]));
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::string err;
            const bool ok = play_drums(dev, bpm, secs, level, &err);
            if (!ok) std::fprintf(stderr, "play: %s\n", err.c_str());
            CoUninitialize();
            return ok ? 0 : 1;
        }
        else if (a == "--list") {
            list();
            return 0;
        } else if (a == "--device" && i + 1 < argc) s.input.device = argv[++i], s.input.mode = InputMode::Capture;
        else if (a == "--loopback") {
            s.input.mode = InputMode::Loopback;
            if (has_next()) s.input.device = argv[++i];
        } else if (a == "--channels" && i + 1 < argc) {
            if (!parse_channels(argv[++i], s.input)) std::fprintf(stderr, "bad --channels, using 1\n");
        } else if (a == "--gain" && i + 1 < argc) s.analysis.gain_db = std::clamp(static_cast<float>(std::atof(argv[++i])), -24.0f, 48.0f);
        else if (a == "--no-auto-gain") s.analysis.auto_gain = false;
        else if (a == "--gate" && i + 1 < argc) s.analysis.gate_db = std::clamp(static_cast<float>(std::atof(argv[++i])), -96.0f, -20.0f);
        else if (a == "--sensitivity" && i + 1 < argc) s.analysis.onset_sensitivity = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
        else if (a == "--midi-clock" && i + 1 < argc) s.midi_port = argv[++i];
        else if (a == "--monitor") monitor = true;
        else if (a == "--record" && i + 1 < argc) {
            record_path = argv[++i];
            if (has_next()) record_s = std::clamp(std::atof(argv[++i]), 1.0, 600.0);
        }
        else if (a == "--help" || a == "-h") {
            std::printf("himmel_audio [--device NAME | --loopback [NAME]] [--channels 1|2|N|N-M] [--gain DB]\n"
                        "             [--no-auto-gain] [--gate DB] [--sensitivity 0..1] [--midi-clock NAME] [--monitor]\n"
                        "himmel_audio --list | --selftest\n");
            return 0;
        }
    }
    // One writer per block: a second himmel_audio (a stray manual start next to the
    // supervisor's) exits at once instead of fighting over Local\HimmelAudio.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\HimmelAudioSingle");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        log("another himmel_audio is running - exiting");
        CloseHandle(single);
        return 3;
    }
    SetConsoleCtrlHandler(on_console, TRUE);
    timeBeginPeriod(1);

    Block blk;
    if (!blk.open()) {
        log("cannot open %ls", kBlockName);
        return 2;
    }
    AudioBlock* b = blk.b;

    // A config written by the GUI before we started wins over the command line.
    uint32_t cfg_gen = 0;
    {
        AudioConfig c{};
        if (proto::seq_read(b->config_seq, b->config, c) && c.gen != 0) {
            s = from_config(c);
            cfg_gen = c.gen;
            log("config %u from the block", c.gen);
        }
        b->status.config_gen_applied.store(cfg_gen);
    }

    log("himmel_audio up (pid %lu): %s %s, auto gain %s, gate %.0f dB, MIDI clock %s", GetCurrentProcessId(),
        s.input.mode == InputMode::Loopback ? "loopback" : "capture", s.input.device.empty() ? "(default)" : s.input.device.c_str(),
        s.analysis.auto_gain ? "on" : "off", s.analysis.gate_db, s.midi_port.empty() ? "off" : s.midi_port.c_str());

    MidiClockPort midi;
    midi.start(s.midi_port);
    Worker worker(b, s);
    if (!record_path.empty()) worker.record(record_path, record_s);
    worker.start();

    uint64_t next_input = 0, next_cfg = 0, next_mon = 0;
    const uint64_t started = now_us();
    InputStatus last_in{};
    while (!g_stop && b->status.stop_request.load() == 0) {
        const uint64_t t = now_us();
        // Heartbeat only while the capture worker is alive too (it publishes every hop,
        // even with the input lost): a stuck worker gets us killed and restarted.
        const uint64_t lp = worker.last_publish_us();
        if (lp == 0 ? t - started < 8000000 : t - std::min(t, lp) < 2000000) b->status.heartbeat_us.store(t);
        // MIDI clock: every 5 ms (the engine glides onto it; 5 ms is far below a tick's jitter).
        ClockSource cs = midi.sample(t);
        ClockSource all[kClockSources] = {};
        all[0] = cs;
        proto::seq_write(b->clock_seq, b->clock, all);
        if (t >= next_input) {
            const InputStatus in = worker.status();
            if (std::memcmp(&in, &last_in, sizeof(in)) != 0 || t >= next_input + 1000000) {
                proto::seq_write(b->input_seq, b->input, in);
                last_in = in;
            }
            next_input = t + 100000;
        }
        if (t >= next_cfg) {
            next_cfg = t + 100000;
            AudioConfig c{};
            if (proto::seq_read(b->config_seq, b->config, c) && c.gen != 0 && c.gen != cfg_gen) {
                const Settings ns = from_config(c);
                if (ns.midi_port != s.midi_port) midi.start(ns.midi_port);
                s = ns;
                worker.set(s);
                cfg_gen = c.gen;
                b->status.config_gen_applied.store(cfg_gen);
                log("config %u applied", cfg_gen);
            }
        }
        if (monitor && t >= next_mon) {
            next_mon = t + 200000;
            Signals sg{};
            proto::seq_read(b->signals_seq, b->signals, sg);
            char lv[21], ba[11], mi[11], hi[11];
            bar(lv, sig(sg, Sig::Level), 20);
            bar(ba, sig(sg, Sig::Bass), 10);
            bar(mi, sig(sg, Sig::Mid), 10);
            bar(hi, sig(sg, Sig::High), 10);
            std::printf("lvl %s  B %s M %s H %s  K%5u S%5u H%5u  %6.1f dBFS gain %+5.1f%s%s  midi %s %.1f\n", lv, ba, mi, hi,
                        sg.kicks, sg.snares, sg.hats, sg.rms_db, sg.gain_db, sg.gated ? " gated" : "",
                        sg.clipping ? " CLIP" : "", cs.run == 2 ? "run" : cs.run == 1 ? "stop" : "-", cs.bpm);
            std::fflush(stdout);
        }
        Sleep(5);
    }
    log("stopping");
    worker.join();
    midi.stop();
    // Tell readers we are gone: input None, heartbeat stays old (they glide to rest).
    InputStatus none{};
    proto::seq_write(b->input_seq, b->input, none);
    timeEndPeriod(1);
    log("bye");
    return 0;
}
