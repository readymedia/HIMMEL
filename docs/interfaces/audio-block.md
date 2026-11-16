# Audio block (`Local\HimmelAudio`)

Status: **version 1, AUDIO track, 2026-11-17** (M8). Header (source of truth
for the layout): [`src/audio/include/himmel/audio/block.h`](../../src/audio/include/himmel/audio/block.h).
Design: [`design/music.md`](../design/music.md) §1–5. Change rules as in
[`command-protocol.md`](command-protocol.md): append only, a version bump
when the size changes, readers check magic + version + size.

## 1. Who writes what

| Section | Seqlock | Writer | Rate | Holds |
|---|---|---|---|---|
| `header` | — | whoever creates the block (himmel_audio writes it last, after a fence) | once | magic `HAUD`, version 1, size |
| `status` | atomics | himmel_audio | every hop (~5 ms), at least every 100 ms | `heartbeat_us`, `pid`, `config_gen_applied`; `stop_request` is set by the supervisor |
| `signals` | `signals_seq` | himmel_audio | every analysis hop (~5 ms at 48 kHz) | the signals below |
| `input` | `input_seq` | himmel_audio | on change and every 1 s | device, mode, channels, rate, state, opens, glitches |
| `clock[4]` | `clock_seq` | himmel_audio | every MIDI tick batch, at least every 100 ms | external clock sources; [0] = MIDI clock in v1 |
| `config` | `config_seq` | **one** controller (GUI bridge or a tool), never himmel_audio | on change | the requested input setup |

The block is open-or-create (the OS zero-fills it): the engine may map it
before himmel_audio runs and keeps its mapping for its whole life, so a
restarted himmel_audio writes into the same block. A zero `heartbeat_us` or
a wrong header = no audio.

## 2. Signals (`Signals`)

All values 0..1, smoothed with their own attack / release, auto-gained.
`v[]` is indexed by `Sig`:

| Index | Signal | How (v1) |
|---|---|---|
| 0 | `level` | RMS of the gained input over the hop; attack 5 ms, release 120 ms; 0 when gated |
| 1 | `bass` | Band < 150 Hz (biquads), each band normalised by its own slow peak (~30 s release) |
| 2 | `mid` | 150 Hz – 2.5 kHz, as bass |
| 3 | `high` | > 2.5 kHz, as bass |
| 4 | `kick` | Onset envelope: jumps to the hit's strength at an onset in the kick band (40–120 Hz), releases over ~150 ms |
| 5 | `snare` | Same, snare band (150–400 Hz body + 2–6 kHz noise, both must rise) |
| 6 | `hat` | Same, > 7 kHz, release ~60 ms |
| 7 | `onset` | max(kick, snare, hat) |
| 8 | `energy` | Slow loudness (one-pole, ~16 s ≈ 8 bars at 120 BPM), against the 30 s peak |
| 9, 10 | `build`, `drop` | 0 in version 1 (0.2) |
| 11–15 | — | spare (append only) |

- `spectrum[32]`: log-spaced bands 40 Hz – 16 kHz from a 1024-point FFT every
  hop, normalised together by one slow peak.
- `waveform[256]`: the newest 256 samples after gain (mono), for scope
  generators.
- `kicks`, `snares`, `hats`: onset counters, so a reader that polls once per
  frame never misses a hit between two frames.
- `rms_db`, `peak_db`: the raw input before gain (meters, "is it hearing
  music?"); `gain_db`: the gain in use; `gated`, `clipping`.

**Beat, bar and phase are not in this block.** They come from the show clock
(below), which lives in the engine; the engine makes the `beat` / `bar`
reaction signals from it (AV offset applied).

**Latency:** one hop (5 ms) + the WASAPI buffer (10 ms shared-mode period)
+ the onset filter (~2–5 ms). `time_us` is when the hop's newest sample
arrived, so a reader can measure the age.

## 3. Input (`InputStatus`, `AudioConfig`)

- **Modes:** `Capture` (mic, line in: any WASAPI capture endpoint) and
  `Loopback` (what a render endpoint plays). Shared mode, event driven, the
  device's mix format (float or 16/24/32-bit PCM), any rate.
- **Channels:** `channel_first` + `channel_count` (1 = mono, 2 = a stereo
  pair summed). Out-of-range channels fall back to channel 0.
- **Device:** part of the friendly name, case-insensitive; empty = the
  Windows default (console role) for that mode.
- **Gain:** manual `gain_db` + **auto gain** (default on): the RMS is pulled
  towards −18 dBFS over ~10 s, between −12 and +36 dB of extra gain, never
  faster up than down. A quiet room mic and a hot line both work.
- **Gate:** below `gate_db` (default −60 dBFS raw RMS, with 3 dB hysteresis)
  `level` and the bands fall to 0, auto gain freezes, and onsets are ignored.
- **Unplug / replug:** a lost device (`AUDCLNT_E_DEVICE_INVALIDATED`, no
  packets for 500 ms, or a failed open) sets `state = Lost`; himmel_audio
  retries every second and reopens the same device by name when it comes
  back (`opens` counts). While lost, the signals glide to 0 over ~1 s
  (the engine does the same if the heartbeat stops).
- **Config:** a controller writes `AudioConfig` with `gen + 1` under the
  seqlock. himmel_audio applies a new `gen` within ~100 ms (reopening the
  device when the device / mode / channels changed) and reports it in
  `status.config_gen_applied`. `gen` = 0 = use the command line.

## 4. The show clock (tap, MIDI clock, internal)

There is one show clock (music.md §2), the engine's `BeatClock` (M2):
`Clock` commands give **tap**, **set BPM**, nudge, resync (**internal
clock**). himmel_audio adds the external sources:

- `clock[0]` = **MIDI clock in** (WinMM, 24 ticks per beat): Start = beat 0,
  Continue, Stop, Song Position Pointer. `bpm` = the tick rate smoothed over
  the last 24 ticks; `confidence` = 1 − tick jitter, rising over the first
  2 beats. `run = Absent` when no tick came for 500 ms. Port: `--midi-clock
  NAME` or `AudioConfig::midi_clock_port` (none by default, since a port
  held by himmel_audio cannot be held by a weapon driver).
- **Arbitration (engine, v1):** MIDI clock (running, confidence ≥ 0.5) >
  tap / internal. While MIDI leads, the engine's clock follows its BPM and
  glides its phase onto MIDI's beat over 2 beats (never jumps);
  `ClockState::source` = 2 (MIDI). A tap or set BPM is ignored while MIDI
  leads. When MIDI stops or drops out, the clock keeps the last tempo and
  phase (`source` back to 0 / 1).
- Later sources (Pro DJ Link, Ableton Link bridges) take `clock[1..3]`; the
  priority is Pro DJ Link > Link > MIDI > audio > tap.

## 5. How the engine uses it (reactions)

`src/engine/include/himmel/engine/reactions.h`:

- Once per frame the renderer copies `signals` (seqlock), checks the
  heartbeat (< 250 ms old) and the input state, and fills
  `himmel::fx::Signals` for the FX resolver: `level`, `bass`, `mid`, `high`,
  `kick`, `snare`, `hat`, `onset`, `energy` from the block; `beat` and `bar`
  from the show clock at *now + AV offset* (1 for the first 10 % of each beat
  / bar). No audio → the audio signals glide to 0 over 1 s (the picture keeps
  playing; the status says "no audio").
- Every effect's default `reaction` (effect-metadata.md §3) then moves its
  parameter. Per-slot reaction editing (signal, amount, …, on any parameter),
  the REACT macro and the reaction page on the MK2 need a command:
  PROPOSED in the AUDIO handoff (`FxReact`), not in version 1.
- `himmel_render --av-offset MS` (default 0, −200..200): the beat / bar
  signals are taken that much earlier, so they land with the beat on screen.
  `--no-audio` = silence; `--test-signal` still wins for checks.

## 6. Tools

- `himmel_audio.exe [--device NAME | --loopback [NAME]] [--channels 1|2|N-M]
  [--gain DB] [--no-auto-gain] [--gate DB] [--midi-clock NAME] [--monitor]`
  — `--list` prints the endpoints and MIDI ports; `--selftest` runs the
  analysis on synthetic drums and the block round trip (no device).
- `audio_tests.exe` (no hardware): filters, onset timing on synthetic kicks /
  snares / hats, auto gain and gate, the MIDI clock tracker, the block
  layout, the engine's signal feed (glide to rest, AV offset) and zoom pulse
  through the FX resolver.
