# Music in: inputs, signals and reactions

Status: proposal for the owner, 2026-11-11. Goal: effects that react *well*
to the music from whatever source the venue gives you, and a tempo that never
jumps.

## 1. Inputs

| Input | What it gives | How |
|---|---|---|
| **Line in from the mixer** | Audio for analysis | ASIO (lowest latency) or WASAPI, 48 kHz, 128–256-sample buffers |
| **Microphone** | Audio for analysis (room sound) | WASAPI, with auto gain |
| **Loopback** ("listen to an app") | Audio from Spotify, rekordbox, Traktor, … on the same PC, with no cables | WASAPI loopback, per device or per app |
| **Pro DJ Link** | BPM, beat, bar, track position and track info from CDJs / XDJs | The beat-link bridge (EPL) in its own process, sending OSC |
| **Ableton Link** | BPM and phase from rekordbox, Serato, Traktor, Live | The Link bridge (GPLv2+) in its own process |
| **MIDI clock** | BPM, beat | WinMM |
| **LTC / MTC** | Absolute time for timecoded shows | LTC from an audio input, MTC over MIDI |
| **Tap** | BPM, phase | MK2 `TEMPO`, NuVJ `BPM TAP`, keyboard |

## 2. Clock arbitration

There is one **show clock**: BPM, beat phase, bar and phrase (8 / 16 bars).
Sources are ranked by priority:

> Pro DJ Link > Link > MIDI clock > audio analysis > tap

The automatic choice can be overridden (`SHIFT + TEMPO`).
- If a source drops out, the clock **glides** to the next source over 2 beats
  and never jumps.
- A confidence value per source is shown as the dot next to the BPM.
- **Nudge:** `TEMPO + ◀ / ▶` moves the phase by 1/32 of a beat.

## 3. Signals (the audio block)

`himmel_audio.exe` writes a shared block every ~5 ms. Everything is
normalised to 0–1 and smoothed with its own attack and release.

| Signal | Description |
|---|---|
| `level` | Overall loudness (RMS, gated) |
| `bass`, `mid`, `high` | Three bands, each auto-gained over ~30 s |
| `spectrum[32]` | Log bands for spectrum generators |
| `kick`, `snare`, `hat` | Onset envelopes (attack-shaped) per drum |
| `onset` | Any onset |
| `beat`, `bar`, `phrase` | Pulses on the clock grid, plus phase 0–1 |
| `energy` | Slow energy (about 8 bars) |
| `build`, `drop` | Build-up level (rising energy and density) and the drop event |
| `section` | calm / groove / build / drop / break (best guess) |
| `waveform[256]` | For oscilloscope generators |

## 4. Reactions (how effects use the music)

- **Any parameter** can carry one **reaction**:

  > signal × amount × curve, with attack / release and optional
  > beat-quantised triggering

  Knob 6 on every FX page (AUDIO) sets the amount. `AUTO WRITE` + turn picks
  the signal.
- **Every effect has a default reaction** (from the effect metadata), for
  example kick → zoom for zoom pulse, bass → amount for displace, hi-hat →
  trigger for stutter. That way a fresh shuffle already moves with the music.
- **The REACT macro** scales every reaction in the deck at once (0 = still,
  100 % = hits hard). By default it is linked to `energy`, so the picture
  follows the night by itself.
- **Speed parameters** can lock to the clock: sync knob 7 sets free or
  1/16 … 4 bars.
- **AV offset:** one value per venue (ms), set with the flash-and-click test.
  Beat-triggered events are scheduled early by the measured output latency,
  so they land *with* the beat on the screen, not after it.

## 5. In the GUI and on the MK2

- **`SAMPLING` (music page):**
  - the screens show input levels, the 3 bands, onsets and BPM with source and
    confidence;
  - the knobs set input gain, band gains, onset sensitivity, AV offset and
    the clock source.
- The **knob ghosts** (`gui.md` §4.3) show each reaction live.
- The **top bar** shows BPM, the beat squares, the source and the
  confidence.
