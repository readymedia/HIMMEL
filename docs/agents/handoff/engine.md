# ENGINE handoff
Updated: 2026-11-16 by the ENGINE M2 session · Branch: `claude/engine-m2-transport` · Last commit: see `git log` on the branch

## State in one paragraph
`himmel_render.exe` is the **slice engine** (S1, 2026-11-15) plus the **M2 clip
transport** (2026-11-16, `design/clips-and-playback.md` §2–3, §10). The pure
deck model (2 decks × 4 layers + a **HIT layer** on top of PROGRAM, FX slots on
layer / deck / master, crossfader, TAKE / CUT / black / focus) now owns one
`Playhead` per layer and an internal **beat clock** (tap / BPM / nudge / resync /
grid). Every play mode works (loop, ping-pong, once · hold / eject / next,
random, chop, step, scratch), with in / out, the 4-point inner loop, 16 cue
points per clip (equal slices when unset), beat loops with catch-up, scratch
hold (stay / snap back), REVERSE held with catch-up, FREE / BPM time base, speed
glide, trigger styles (normal / toggle / piano / legato), start position
(restart / continue), fade in / out, beat snap (PROGRAM waits, PREVIEW never),
clip targets (own / free / HIT), autopilot (next / previous / random / same,
ONCE · NEXT) and layer transitions (cut / fade / black-dip). All of it comes in
through six new commands (`command-protocol.md` §4a) and goes out in the new
transport section of the state block (`engine-state.md` v2). The decode worker
keeps the 16 jump targets of every layer **warm**, and a jump holds its landing
frame for that one frame, so a cue jump is on screen on the next present
(measured: 160 / 160). Soak (`docs/measurements/2026-11-16-m2-transport-soak.md`):
15 + 10 min with every transport feature cycling, 0 crashes; 10 late frames in
the first (6 in its first 2 min, cold start), 2 in 10 min on the warm renderer =
the S1 baseline rate. The supervisor's log file is now readable while Himmel
runs (`_wfsopen` + `_SH_DENYWR`).

## Done (with commits)
- **M2 (this branch):**
  - `transport.h/.cpp`: `BeatClock` (tap tempo over up to 8 taps, 2 s timeout,
    every tap lands on a beat; set BPM keeps the phase), `grid_beats()`,
    `Playhead` (main head = shadow playhead; overrides for beat loop / scratch /
    reverse; inner-loop phases; glide; seeded xorshift for random / chop /
    autopilot). Pure: no heap, no locks, no Windows.
  - `deck_model.h/.cpp`: slot model (0..7 = deck × 4 + layer, 8 = HIT), the six
    new commands validated + applied, `tick()` runs clock → waiting launches →
    fades → transition progress → playheads → events (loops → autopilot,
    ended → once rules) → status into `TransportSection`; cue points per clip
    id; resume positions per clip; `warm_frames()`; `transition_gen()`;
    `restore()` takes the old transport section (settings, cues, BPM, grid).
  - `clip_player.h/.cpp`: 16 warm slots per clip (`set_warm()`, filled by the
    worker after the play window, sized on first use in the worker),
    `acquire()` looks in them too; `decode_into()` shared.
  - `render_main.cpp`: 9 clip players (+ HIT), the playhead frame from the
    model (the old in-renderer position code is gone), warm frames sent on
    change, FADE transitions keep the old picture in a per-layer hold target
    (`ps_trans`), opacity × fade gain, HIT composited on the PROGRAM deck, FX
    `beats` from the beat clock (was a fake 120 BPM), transport section written
    with `live`, restore reads it, STATS adds `late worst`, `jumps / next-frame /
    worst`, `warm decodes`, `bpm`; a `JUMP` line for every late jump.
  - `slice_shaders.h`: `ps_trans` appended (nothing else touched).
  - `himmel_cmd`: `set`, `trigger`, `cue`, `beatloop`, `scratch`, `play
    release|stop`, `tap`, `bpm`, `grid`, `resync`, `phase`, `cuebench`, `soak`;
    `h` = the HIT layer; the state print shows the transport + clock.
  - `tests/engine/transport_tests.cpp` → `transport_tests.exe` (1440 checks, no
    GPU): clock, every mode, range, glide, BPM, ping-pong, once ×3, random
    (seeded, deterministic), chop, step, scratch both ways, reverse hold, inner
    loop, beat loop ± catch-up, cue points + warm frames == jump frames (also
    one tick later), trigger styles, start position, fades, beat snap, targets
    + HIT, autopilot (PREVIEW now, PROGRAM on the beat), transitions,
    validation, restore. `deck_tests` 143 (one check updated: `PlayAction` 4 is
    now `Release`).
  - Supervisor: `_wfsopen(..., _SH_DENYWR)` for `logs\himmel-*.log`.
- **S1 (2026-11-15, on main):** deck model, `fx_catalog`, `clip_catalog`,
  `shm_block`, `log_ring`, `render_main.cpp`, `slice_shaders.h`, `himmel_cmd`,
  `preview_dump`, supervisor starts `himmel_ui`, `--legacy`. 30-min soak in
  `docs/measurements/2026-11-15-slice-engine-soak.md`.

## How the frame loop stays clean (rule 1)
- **Transport:** all state is fixed-size POD inside `DeckModel` (9 playheads,
  9 pending launches, 9 fades, `cues_[256][16]`, `resume_[256]`), allocated once
  with the model. `tick()` is bounded arithmetic: one pass over 9 slots, at most
  4 step / random events per slot per frame, no loops over the catalog except
  autopilot's O(1) pick. No `new`, no locks, no files, no Windows calls.
- **Warm frames:** the render thread only does 16 atomic exchanges when the
  targets change (compared with a cached copy first). The worker sizes a warm
  slot's buffer on first use (allocation in the worker, never in the frame).
- **Transitions:** the hold targets are made with the canvas (start-up / window
  resize only); a transition costs one `ps_source` pass at its start and one
  `ps_trans` pass per frame while it runs.
- Everything else as in S1: ring pops bounded, `UpdateSubresource` of ready BC
  frames, `Map(DISCARD)` of constant buffers, `DO_NOT_WAIT` / `DONOTFLUSH`
  readbacks, seqlock memcpy into the shared blocks (now + the ~2 KB transport
  section), logging through `LogRing` only.

## In progress
Nothing half-done.

## Next up (ordered)
1. **Warm the next random / scratch target:** RANDOM and absolute scratch moves
   land on frames that are not warm, so they show one frame late (counted as
   `clip misses`). Pre-roll the next random target from the seeded RNG into a
   warm slot; for chop the slice starts are already warm when slices divide 16.
2. **Clip-level transport:** move per-layer settings to per-clip defaults with
   per-use overrides once the library (M7) and show files (M3) exist (D104).
   Save cue points to disk (M3 / M7 sidecar).
3. **Phase-locked BPM loops:** today BPM time base is rate-based (the range lasts
   n beats); lock the phase to the bar when the audio clock exists (M8).
4. Off-air deck at thumbnail size; skip decoding of layers nobody sees (keep one
   warm frame) — S1 items, still open. The warm slots raise RAM (9 × 16 × 1–2 MB
   at 1080p); release them for layers nobody can jump on if RAM gets tight.
5. More transition types (wipes, push, zoom, luma, noise, blur, flash: append to
   `TransitionType`), chop patterns (1-1-3-4), loop crossfade (seam), frame
   blending below ×0.5, the live sampler (§6), TIMECODE / TRACK time bases.
6. S1 leftovers: late-frame proof fullscreen; Touch FX / Entrancer / camera
   back as passes or sources; generic weapons as `src/perform/` mappers; render
   graph in a worker.

## GUESSES
- **M2 (all listed in `decisions.md` D101–D104):** GRID off by default; plain
  clip loads never wait and start at in; defaults (cut 0.5 s, fades 0, HIT
  fade-out 0.2 s + ADD, chop 8, autopilot 4 loops, BPM range 4 beats, inner
  loop 25–75 %); beat loop starts at the playhead; after an outro the intro
  re-arms; unset cue pads = 16ths; random autopilot is uniform; velocity →
  opacity only on HIT; settings per layer, cue points per clip; playhead in the
  render thread. A stall > 250 ms is not played through (dt clamp).
- Warm-slot RAM: 16 × 1–2 MB per 1080p clip and layer (renderer ~380 MB with 8
  layers + HIT of 1080p test clips on the dev laptop). Fine on the show PC;
  measure with a big 4K library.
- S1 guesses (D94–D96, effect defaults, grey placeholder, hung `himmel_ui` not
  detected, unique clip names, start-up probe time) are unchanged.

## Open questions for the owner
- D101–D103 (PROPOSED, defaults in use): GRID default, transport defaults,
  behaviour guesses. Nothing blocks.

## How to build / run / test this track
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 8
build\bin\Release\deck_tests.exe            # 143 checks, no GPU
build\bin\Release\transport_tests.exe       # 1440 checks, no GPU
python tools\make_test_clips.py             # media\test\*.mov (needs ffmpeg)
build\bin\Release\himmel_render.exe --outputs 2 --size 640x360
build\bin\Release\himmel_cmd.exe clip a 1 0 -- set a 1 mode pingpong -- cue a 1 5 set 0.5
build\bin\Release\himmel_cmd.exe cuebench a 1 40 150   # STATS: jumps / next-frame / worst
build\bin\Release\himmel_cmd.exe trigger h 1 3 0.8 -- trigger h 1 3 1 up   # a HIT
build\bin\Release\himmel_cmd.exe soak 900              # transport soak driver
build\bin\Release\himmel_cmd.exe            # the state incl. transport + clock
```
Output windows: `M` = span / mirror, `O` = overlay, `Esc` = quit (windowed).

## Gotchas found
- The Bash tool's heredocs choke on some content (backticks); write helper
  scripts with the Write tool instead.
- A jump must not advance in the same tick, or it lands half a frame past the
  warm frame and shows one frame late (fixed: `Playhead::landed_`).
- `PlayAction` grew (`Release` 4, `Stop` 5): any test that used 4 as "out of
  range" must use `PlayAction::Count`.
- State block v2 is bigger: a still-running v1 `himmel_ui` keeps the old mapping
  alive, and the new renderer cannot map it at the new size (runs without a
  state block, logged). Restart everything after the upgrade.
- From S1: `small` is a macro in the Windows headers; the state block is kept
  alive by any reader; `himmel_cmd` continues its seq from the engine's ack; Git
  Bash turns `/m` into a path (use `--parallel 8`).

## Interfaces touched
- `command-protocol.md` §4a (append-only): `LayerSet`, `LayerTrigger`,
  `LayerCue`, `LayerBeatLoop`, `LayerScratch`, `Clock`; `PlayAction` Release /
  Stop; `kHitDeck`, `kRelease`; enums `LayerParam`, `PlayMode`, `TimeBase`,
  `TriggerStyle`, `StartPos`, `ClipTarget`, `AutopilotMode`, `TransitionType`,
  `GridDiv`, `CueAction`, `ClockParam`. `kCommandVersion` unchanged.
- `engine-state.md` → **version 2**: `TransportSection` (clock, per-layer
  `LayerTransport`, HIT `LayerState` + transport) after the catalog with its own
  seqlock. GUI / CONTROL: rebuild against the new header; the HIT layer and the
  transport status are ready to show.
