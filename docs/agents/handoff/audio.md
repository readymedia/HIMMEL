# AUDIO handoff
Updated: 2026-11-17 by the AUDIO wave-2 agent · Branch: `claude/audio-m8-reactions` · Last commit: see `git log` (after 950c92f)

## State in one paragraph
M8 core is built and tested without a renderer: `himmel_audio.exe` (WASAPI mic /
line in / loopback, auto gain, gate, level / bass / mid / high / spectrum[32] /
waveform / kick / snare / hat / onset / energy every 5 ms, MIDI clock in) writes
`Local\HimmelAudio` ([`docs/interfaces/audio-block.md`](../../interfaces/audio-block.md),
version 1). The renderer maps it at start-up, feeds every effect's default
`reaction` from it each frame (no audio → glide to rest over 1 s), takes the
beat / bar pulses from the show clock at now + `--av-offset`, and lets a
running MIDI clock lead the show clock (glides the phase over 2 beats, never
jumps). The supervisor starts and restarts `himmel_audio.exe` like the other
children. **Not done:** per-slot reaction editing (needs a command), the REACT
macro, the MK2 reaction / SAMPLING pages, the AV offset flash test, audio BPM
tracking (0.2), build / drop / section (0.2), ASIO, Link.

## Done (with commits)
- d67575f: audio block v1 (`src/audio/include/himmel/audio/block.h`), the
  analysis (`analyzer.*`), MIDI clock tracker (`midi_clock.*`), WASAPI input
  (`wasapi.*`), WinMM MIDI clock port (`midi_port.*`), `himmel_audio.exe`,
  engine feed (`src/engine/src/reactions*.cpp`), supervisor child.
- 950c92f: snare / hat decided on a full hop (fixes a hop-alignment bug found
  with a real capture), `--play-drums`, `--record`, `--analyse`.
- Last commit: single-instance mutex, `audio_probe.exe`, docs, measurements
  (`docs/measurements/2026-11-17-audio-m8.md`), this handoff.

## Engine files touched (tell ENGINE / LEAD)
- New: `src/engine/include/himmel/engine/reactions.h`, `src/engine/src/reactions.cpp`
  (pure, in `himmel_engine_model`), `src/engine/src/reactions_link.cpp` (in
  `himmel_engine_ipc`).
- `transport.h/.cpp`: `BeatClock::follow(bpm, beat, phase, now, dt)` (new method).
- `deck_model.h/.cpp`: `follow_clock()`, `clock_external()`; while MIDI leads,
  `Clock Tap` / `Clock Bpm` are acked but ignored; `ClockState::source` = 2 (MIDI)
  — a new **value** of an existing field (engine-state.md should list it; LEAD).
- `render/render_main.cpp`: `--av-offset MS`, `--no-audio`; AudioLink read +
  `follow_clock` before `model->tick`; `fc.sig` from `ReactionFeed` unless
  `--test-signal`. Frame loop: only seqlock copies (~1.5 KB) and arithmetic; the
  mapping is made at start-up and kept for life (no allocation / lock / I/O).
- `supervisor/supervisor_main.cpp`: child `audio` (`--no-audio`, `--audio "ARGS"`),
  heartbeat from the block, stop request on exit.
- `CMakeLists.txt` (root): `add_subdirectory(src/audio)` before `src/engine`.
- `src/protocol/`: **not touched**.

## In progress
Nothing half-done. Next step is on hardware with the owner (below).

## Next up (ordered)
1. **Owner test (the M8 "done when"):** mic in a room + a line from a DJ mixer,
   zoom pulse on screen on the kick, on time; unplug / replug the USB sound
   card during a soak. Run: `himmel.exe --audio "--device Focusrite --channels 1-2" -- ...`
   (or `--audio "--device BLUE"` for the mic). `himmel_audio --monitor` shows the meters.
   Tune the snare / hat thresholds with `--record` + `--analyse` on real music.
2. PROPOSED `FxReact` command (below) + reaction state in `FxSlotState`
   (LEAD: protocol), then per-slot reactions on any param, REACT macro, MK2
   reaction page, GUI ear "~".
3. AV offset flash-and-click test; SAMPLING page on the MK2 (input gain, band
   gains, onset sensitivity, AV offset, clock source) via `AudioConfig`.
4. GUI: write `AudioConfig` from the wizard step 2 / Settings (device list =
   `himmel_audio --list`; a JSON list in the block may be better — PROPOSED).
5. 0.2: audio BPM tracking (clock source "audio"), build / drop / section, ASIO,
   Link bridge (license decision), Pro DJ Link bridge.

## GUESSES
- GUESS: snare = noise within 8 dB of the body (`analyzer.cpp`, `snare_like`).
  Tuned on the synthetic kit only; a snare exactly on a kick without hats is
  still often missed (test prints it as KNOWN WEAK). Medium-low confidence.
  Verify: `--record` real music, `--analyse`, compare by ear.
- GUESS: band edges (bass < 150, mid < 2.5 k, kick 40–120, snare body 150–400,
  noise 2–6 k, hat > 7 k), onset threshold 3–9 dB, refractory 100 / 100 / 50 ms.
  Medium confidence; same check.
- GUESS: gate −60 dBFS default; a quiet room mic sits at about −68 here
  (gated), music in a room should be well above. Check in a venue.
- GUESS: the WASAPI QPC position / 10 = steady_clock µs (MSVC steady_clock is
  QPC). Used only for `Signals::time_us` (age); a wrong value is clamped to now.
- GUESS: MIDI clock confidence: jitter 5 % = still fine; leads at ≥ 0.5.
  Never tried with real gear.
- GUESS: no MIDI clock port by default (a port held by himmel_audio cannot be
  held by a weapon driver). Owner: is that right, or open "any port that sends clock"?
- GUESS: `--record` assumes 48 kHz for the length.

## Open questions for the owner (PROPOSED, need D numbers from the LEAD)
- **PROPOSED (AUDIO): `FxReact` command** (append after 24): deck / layer / slot,
  `param` = knob 0..3, `i` = signal | mode << 8 | curve << 16, `f[0]` amount,
  `f[1]` attack ms, `f[2]` release ms, `f[3]` threshold (quantise / hold / range
  in a second message or packed) — plus a per-slot reaction table in the state
  block, so every parameter can carry its own reaction (music.md §4). Until then
  only the effect's default reaction moves.
- **PROPOSED (AUDIO): MIDI clock leads tap.** While MIDI clock runs, Tap / set
  BPM are ignored (music.md §2 order); `SHIFT + TEMPO` override comes later.
- **PROPOSED (AUDIO): device list in the block** for the GUI (now only `--list`).

## How to build / run / test this track
- Build: as `docs/getting-started.md` (VS 2022 Build Tools CMake). Targets:
  `himmel_audio`, `audio_tests`, `audio_probe`.
- Tests: `build\bin\Release\audio_tests.exe` (129 checks), `himmel_audio --selftest`
  (also in ctest).
- No-cable line-in check (VB-Audio Virtual Cable installed on the dev laptop):
  `himmel_audio --device "CABLE Output" --channels 1-2 --monitor` and, in another
  shell, `himmel_audio --play-drums "CABLE Input" --bpm 120 --seconds 30 --level 0.3`.
- Renderer path without a renderer: `audio_probe --seconds 30`.

## Gotchas found
- A noise onset's first hop may hold only a few samples of the hit: deciding
  snare vs kick / hat on that hop misclassifies depending on where the beat
  falls in the hop (found only with a real capture; now tested at 10 offsets).
- `seq_read` copies into its target even when it fails: read into a scratch
  copy (reactions_link.cpp).
- `himmel::engine::fx` (the catalog enum) shadows `himmel::fx` inside
  `himmel::engine`: write `::himmel::fx::` there.
- Loopback endpoints deliver no packets while nothing plays: the worker feeds
  silence hops on time; only a capture endpoint is "lost" after 500 ms of no data.
- Two himmel_audio processes would both write the block: now a named mutex.

## Interfaces touched
- `docs/interfaces/audio-block.md`: new, version 1 (and the row in
  `docs/interfaces/README.md`).
- `ClockState::source` = 2 (MIDI): new value; `engine-state.md` not updated (LEAD owns it).
- `docs/architecture.md`: the himmel_audio row and diagram node.
