# AUDIO handoff
Updated: 2026-11-17 by the AUDIO wave-5 agent · Branch: `claude/audio-fxreact` (from main e8e0e88) · M8 merged earlier from `claude/audio-m8-reactions`

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
children. **Wave 5 (D131, decided 2026-11-17):** `FxReact` = command 40 and a
per-knob reaction table in `Local\HimmelState` v3: any FX knob (layer, deck,
master slots) can carry its own reaction, or none; the renderer resolves
each knob on its own envelope and publishes env / value for the GUI's knob
ghosts. **Not done:** the REACT macro, the MK2 reaction / SAMPLING pages, the AV offset flash test, audio BPM
tracking (0.2), build / drop / section (0.2), ASIO, Link.

## Done (with commits)
- 1b33218: audio block v1 (`src/audio/include/himmel/audio/block.h`), the
  analysis (`analyzer.*`), MIDI clock tracker (`midi_clock.*`), WASAPI input
  (`wasapi.*`), WinMM MIDI clock port (`midi_port.*`), `himmel_audio.exe`,
  engine feed (`src/engine/src/reactions*.cpp`), supervisor child.
- 8925e23: snare / hat decided on a full hop (fixes a hop-alignment bug found
  with a real capture), `--play-drums`, `--record`, `--analyse`.
- Last commit: single-instance mutex, `audio_probe.exe`, docs, measurements
  (`docs/measurements/2026-11-17-audio-m8.md`), this handoff.

## Wave 5: D131 FxReact (branch `claude/audio-fxreact`)
- aa36c95 **contract** (pushed first for GUI): `commands.h` FxReact = 40,
  `ReactOp` (Set, Amount, Signal, Range, Default, Off), `ReactSignal / ReactMode /
  ReactCurve / ReactQuantise`, `ReactArgs` + `pack_react / unpack_react /
  react_args_ok / make_react`; `state.h` version 3 (`ReactionSection` after
  transport, 48-byte `ReactionEntry` per slot per knob 0..3, `gen`).
  Docs: `command-protocol.md` §4d, `engine-state.md` v3, `effect-metadata.md` §3.
- Engine apply (next commit):
  - `src/engine/fx/` (FX's code, the resolver part only): `SlotRuntime` envelopes /
    TRIGGER state are now **per knob** (`react_env[4]` …), plus `out[4]` and
    `frame`; `resolve(..., const Reaction* reacts = nullptr)` runs one reaction per
    knob in knob order (nullptr = the old path: only `m.reaction`);
    `reactions_from()` (table row -> resolver) and `default_entry()` (effect.json ->
    Default entry); `FxGpu::set_reactions()` + `frame_count()`, `run_chain` passes
    the slot's row (HIT chain: defaults).
  - `reactions.h/.cpp`: `react_defaults`, `react_empty_row`, `react_check`, `react_apply`.
  - `deck_model.h/.cpp`: the table (`reactions()`), `set_react_defaults()`, FxReact
    validation (empty slot / knob >= param_count / kNone with Set = `BadTarget`;
    bad packed field / NaN = `BadValue`) and apply; **FxSet resets the slot's row to
    the effect's defaults**; `restore(..., old_react)` brings Custom / Off back.
  - `render_main.cpp` (marked `AUDIO (D131)` blocks): the defaults table at start-up
    (in AUDIO hook 1), restore from a v3 block, `g_fx->set_reactions`, and the
    `react` section written every frame while any FX slot holds an effect (else
    with `live`), env / value 0 for slots not drawn this frame.
  - `himmel_cmd react T S K|all SIGNAL [AMOUNT] [add|follow|trigger] [ATT REL THR]`
    and `react T S K|all default|off`.
- Tests: `protocol_tests` (packing, ranges, layout), `audio_tests` `test_fx_react`
  (the Default row resolves bit-identically to the old path for **every** effect;
  validation; Set / Amount (relative) / Signal / Range / Default / Off incl. `all`;
  master and deck slots; restore; FxSet reset; two knobs on independent
  envelopes in the resolver). All `*_tests.exe` green, ctest 21/21.
- **e2e pending:** another Himmel process (`himmel_ingest.exe`) was running, so
  no stack was started. LEAD check: `himmel.exe --sim`, put zoom pulse on A1
  slot 1 (`himmel_cmd fx a1 1 zoom_pulse`), then `himmel_cmd react a1 1 3 bass 0.8`
  (CENTER X follows the bass) and watch `react` in the state block / the picture;
  `himmel_cmd react a1 1 all off` stops all of them.
- For SHOW: scenes do not save reactions yet; a recall that sends `FxSet`
  resets the slot's row to Default. Saving the table in looks / presets is a
  SHOW follow-up (append to `himmel-show/1`).
- For GUI: `fake_engine` and `json_codec` do not know `FxReact` yet; the
  fake engine's v3 block has a zero `react` section.

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
2. ~~`FxReact` + the table~~ (wave 5, above). Still: REACT macro (a new op or
   type in 41–49), MK2 reaction page (knob 6 = Amount, `AUTO WRITE` + turn =
   Signal, `SHIFT + AUTO WRITE` = Set from the state row), reactions on layer
   params (opacity, speed) and macros, spectrum-band signals.
3. AV offset flash-and-click test; SAMPLING page on the MK2 (input gain, band
   gains, onset sensitivity, AV offset, clock source) via `AudioConfig`.
4. GUI: write `AudioConfig` from the wizard step 2 / Settings (device list =
   `himmel_audio --list`; a JSON list in the block may be better — PROPOSED).
5. 0.2: audio BPM tracking (clock source "audio"), build / drop / section, ASIO,
   Link bridge (license decision), Pro DJ Link bridge.

## GUESSES
- GUESS (wave 5): `FxReact Signal` on a knob with no reaction yet starts it at
  amount 0.5, ADD, 10 / 200 ms, linear (`react_apply`). Medium confidence.
- GUESS (wave 5): reactions belong to the slot + effect: `FxSet` (even the same
  effect) resets them to Default. High confidence for 0.1.
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
- GUESS (D132): no MIDI clock port by default (a port held by himmel_audio cannot be
  held by a weapon driver). Owner: is that right, or open "any port that sends clock"?
- GUESS: `--record` assumes 48 kHz for the length.

## Open questions for the owner (PROPOSED rows D130–D132 in docs/decisions.md)
- **D131 PROPOSED: `FxReact` = command 40** (AUDIO range 40–49): deck / layer / slot,
  `param` = knob 0..3, `i` = signal | mode << 8 | curve << 16, `f[0]` amount,
  `f[1]` attack ms, `f[2]` release ms, `f[3]` threshold (quantise / hold / range
  in a second message or packed) — plus a per-slot reaction table in the state
  block, so every parameter can carry its own reaction (music.md §4). Until then
  only the effect's default reaction moves.
- **D130 PROPOSED: MIDI clock leads tap** (and the own audio block). While MIDI clock runs, Tap / set
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
- Wave 5: `commands.h` (FxReact 40 + enums / helpers), `state.h` v3
  (`ReactionSection`), `command-protocol.md` §4d, `engine-state.md` v3,
  `effect-metadata.md` §3, `docs/interfaces/README.md`.
- `docs/interfaces/audio-block.md`: new, version 1 (and the row in
  `docs/interfaces/README.md`).
- `ClockState::source` = 2 (MIDI): new value; `engine-state.md` not updated (LEAD owns it).
- `docs/architecture.md`: the himmel_audio row and diagram node.

## Not verified (one-stack rule)
Another Himmel stack ran on the machine the whole session, so `himmel_render`
and `himmel.exe` were **not started** with these changes: the render-loop
wiring, `--av-offset`, the MIDI lead in a live renderer and the supervisor's
`audio` child are compiled and unit-tested only. First thing to run when the
machine is free: `himmel.exe --audio "--device \"CABLE Output\" --channels 1-2"`
with `--play-drums "CABLE Input"` and zoom pulse on a layer.
