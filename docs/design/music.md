# Music in: inputs, signals and reactions

Status: approved by the owner, 2026-11-11 (decisions D42–D76). Goal: effects that react *well*
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

**The rule (D88): every effect is audio-reactive.** Every parameter of every
effect, generator and layer (opacity, zoom, position, speed, the clip's
playhead, the macros, the master dimmer) can carry a reaction, and every
effect ships with a sensible default reaction so it moves with the music the
moment it is switched on. Nothing needs code to be made reactive.

- **Any parameter** can carry one **reaction** (plus one MOVE modulator,
  `clips-and-playback.md` §5):

  > signal × amount × curve, with attack / release and optional
  > beat-quantised triggering

- **The settings of a reaction** (all editable, saved with the effect slot;
  each can use the parameter scopes SLOT / LOOK / GLOBAL):

  | Setting | Range | What it does |
  |---|---|---|
  | **Signal** | level, bass, mid, high, a spectrum band, kick, snare, hat, onset, beat, bar, energy, build, drop | What it listens to |
  | **Amount** | −100 … +100 % | How far the parameter moves (negative = the other way) |
  | **Range** | low … high of the parameter | Where it moves between (default: the effect's `good` range) |
  | **Mode** | ADD (on top of the knob), FOLLOW (replaces it), TRIGGER (fires on onsets) | How it combines with the knob |
  | **Attack / release** | 0–2000 ms or in beats | How fast it rises and falls |
  | **Threshold** | 0–1 | Ignore the signal below this (a gate) |
  | **Curve** | linear, soft, hard, steps | The response shape |
  | **Quantise** | off, 1/16 … 1 bar | TRIGGER mode fires only on the grid |
  | **Hold** | 0–4 beats | Keeps a trigger up for a while |

- **Where you set it:**
  - **MK2:** on any FX page, knob 6 (AUDIO) = amount; `AUTO WRITE` + turn
    a knob = pick that parameter's signal; `SHIFT` + `AUTO WRITE` + knob 6
    opens the reaction page (knobs = signal, amount, attack, release,
    threshold, mode, quantise, curve).
  - **GUI:** every knob tile has a small "~" ear; click it for the reaction
    editor: signal list with live meters, a graph of input → output, and the
    settings above. "Copy reaction" / "Paste to all".
  - **Presets:** reactions are part of effect presets and scenes, and the
    dice can change them when the REACT lock is free.

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

## 5. Audio in 0.1 (a must: Himmel is tied to the music)

0.1 ships with audio-reactive effects working end to end (milestone M8 in
`build-v1.md`):

- **Inputs:** **microphone** and **line in** (any input of any sound card,
  mono or a stereo pair) through WASAPI, plus **loopback** (sound playing on
  the same PC). ASIO comes in 0.2.
- **Input setup:** device, channels, gain, **auto gain** (on by default, so a
  quiet mic and a hot line both work), noise gate, a live meter, and an "is
  it hearing music?" check in the wizard (step 2) and in Settings.
- **Signals in 0.1:** level, bass / mid / high, spectrum[32], kick / snare /
  hat onsets, onset, energy, and beat / bar / phase from the show clock
  (tap, MIDI clock or internal). Audio beat tracking (BPM from the audio) is
  0.2; until then the clock comes from tap or MIDI clock.
- **Reactions:** on every parameter with all the settings in §4, defaults per
  effect, the REACT macro, the AV offset test.
- **Failure:** if the input disappears, reactions glide to rest over 1 s and
  the picture keeps playing; the status line says "no audio".
- **Done when:** with a mic in a room and with a line from a DJ mixer, a kick
  visibly hits zoom pulse on time (AV offset tested), and the input can be
  unplugged and plugged back during a soak.

## 6. In the GUI and on the MK2

- **`SAMPLING` (music page):**
  - the screens show input levels, the 3 bands, onsets and BPM with source and
    confidence;
  - the knobs set input gain, band gains, onset sensitivity, AV offset and
    the clock source.
- The **knob ghosts** (`gui.md` §4.3) show each reaction live.
- The **top bar** shows BPM, the beat squares, the source and the
  confidence.
