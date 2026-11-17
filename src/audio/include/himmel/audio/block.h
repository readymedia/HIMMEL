// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// The audio block `Local\HimmelAudio` (docs/interfaces/audio-block.md, version 1).
//
// Writer of signals / input / clock / status: himmel_audio.exe only.
// Writer of `config`: one controller at a time (the GUI bridge, or a tool);
// himmel_audio reads it. Readers: himmel_render (reactions, clock), the GUI.
//
// Header-only, no Windows headers, no heap. Same rules as the other protocol
// blocks (himmel/protocol/shm.h): fixed-size PODs, one writer per seqlock
// section, readers copy a section and work on the copy.
//
// Times are steady_clock microseconds (QueryPerformanceCounter on Windows),
// the same clock as every other Himmel process (mk2::now_us).
#pragma once

#include <atomic>
#include <cstdint>

#include "himmel/protocol/shm.h"

namespace himmel::audio {

inline constexpr wchar_t kBlockName[] = L"Local\\HimmelAudio";
inline constexpr uint32_t kMagic = 0x44554148;  // "HAUD"
inline constexpr uint32_t kVersion = 1;

inline constexpr int kSpectrumBands = 32;
inline constexpr int kWaveform = 256;
inline constexpr int kNameLen = 128;
inline constexpr int kClockSources = 4;

// Index into Signals::v. Append only. Each 0..1, smoothed, auto-gained.
enum class Sig : uint8_t {
    Level = 0,   // overall loudness (RMS, gated)
    Bass,        // < 150 Hz
    Mid,         // 150 Hz .. 2.5 kHz
    High,        // > 2.5 kHz
    Kick,        // onset envelopes (1 at the hit, then release)
    Snare,
    Hat,
    Onset,       // any onset (max of the three)
    Energy,      // slow loudness, ~16 s
    Build,       // 0 in version 1 (0.2)
    Drop,        // 0 in version 1 (0.2)
    Count
};
inline constexpr int kSigSlots = 16;  // room to append
static_assert(static_cast<int>(Sig::Count) <= kSigSlots);

// Written after every analysis hop (~5 ms).
struct Signals {
    uint64_t time_us;            // when the newest sample of this hop arrived (steady clock)
    uint64_t hop;                // hops since the input opened (0 = none yet)
    float v[kSigSlots];          // see Sig
    float spectrum[kSpectrumBands];  // log bands 40 Hz .. 16 kHz, 0..1 (auto-gained together)
    float waveform[kWaveform];   // the newest 256 samples after gain (~5 ms at 48 kHz), -1..1
    uint32_t kicks, snares, hats;    // onset counters (wrap): a reader sees every hit, even between frames
    float rms_db;                // input RMS before gain, dBFS
    float peak_db;               // input peak before gain over the last hop, dBFS
    float gain_db;               // gain in use (manual + auto)
    uint8_t gated;               // 1 = below the gate: level / bands are held at 0
    uint8_t clipping;            // 1 = the input touched full scale in the last 0.5 s
    uint8_t reserved[6];
};

enum class InputState : uint8_t { None = 0, Opening = 1, Running = 2, Lost = 3 };
enum class InputMode : uint8_t { Capture = 0, Loopback = 1 };

// Written when it changes (and every 1 s).
struct InputStatus {
    uint8_t state;               // InputState
    uint8_t mode;                // InputMode
    uint8_t channel_first;       // 0-based first channel used
    uint8_t channel_count;       // 1 = mono, 2 = a stereo pair (summed to mono)
    uint32_t sample_rate;
    uint32_t device_channels;    // channels the device delivers
    uint32_t opens;              // times the input was (re)opened
    uint32_t glitches;           // WASAPI discontinuities / lost packets
    uint32_t reserved;
    uint64_t since_us;           // when `state` last changed
    char device[kNameLen];       // friendly name (UTF-8), "" = none
};

enum class ClockKind : uint8_t { None = 0, Midi = 1, ProDjLink = 2, Link = 3 };  // 2, 3: later bridges
enum class ClockRun : uint8_t { Absent = 0, Stopped = 1, Running = 2 };

// One external clock source. Index 0 = MIDI clock in version 1.
struct ClockSource {
    uint8_t kind;                // ClockKind
    uint8_t run;                 // ClockRun
    uint8_t phase_valid;         // 1 = `beat` is a real song position (a Start or SPP was seen)
    uint8_t reserved;
    float bpm;                   // smoothed tempo, 0 = unknown
    float confidence;            // 0..1 (tick jitter, how long it has been steady)
    uint32_t ticks;              // ticks seen since start / continue
    double beat;                 // beat position at time_us (beat 0 = MIDI Start / song position)
    uint64_t time_us;            // when `beat` was sampled
    uint64_t last_tick_us;       // the last tick (0 = never)
    char port[64];               // the port's name
};

// Requested settings (written by the GUI / a tool, read by himmel_audio).
// `gen` = 0: nothing requested, himmel_audio uses its command line.
// A writer bumps `gen` (inside the seqlock) for every change.
struct AudioConfig {
    uint32_t gen;
    uint8_t mode;                // InputMode
    uint8_t channel_first;       // 0-based
    uint8_t channel_count;       // 1 or 2
    uint8_t auto_gain;           // 1 = on (default)
    float gain_db;               // manual gain, -24..+48 dB
    float gate_db;               // gate, -96..-20 dBFS (default -60)
    float onset_sensitivity;     // 0..1 (default 0.5)
    char device[kNameLen];       // part of the device's name, case-insensitive; "" = the default device
    char midi_clock_port[64];    // part of a MIDI input name; "" = no MIDI clock
};

struct Status {
    std::atomic<uint64_t> heartbeat_us;  // himmel_audio is alive (written every ~5 ms, at least every 100 ms)
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> stop_request;  // the supervisor sets it: exit cleanly
    std::atomic<uint32_t> config_gen_applied;  // the AudioConfig::gen in use
    std::atomic<uint32_t> reserved[3];
};

struct AudioBlock {
    protocol::BlockHeader header;
    Status status;
    std::atomic<uint32_t> signals_seq;
    uint32_t pad0;
    Signals signals;
    std::atomic<uint32_t> input_seq;
    uint32_t pad1;
    InputStatus input;
    std::atomic<uint32_t> clock_seq;
    uint32_t pad2;
    ClockSource clock[kClockSources];
    std::atomic<uint32_t> config_seq;
    uint32_t pad3;
    AudioConfig config;
};

inline float sig(const Signals& s, Sig k) { return s.v[static_cast<int>(k)]; }

}  // namespace himmel::audio
