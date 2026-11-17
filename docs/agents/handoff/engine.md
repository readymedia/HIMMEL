# ENGINE handoff
Updated: 2026-11-17 by the ENGINE M6 session · Branch: `claude/engine-m6-spout` (M6 itself merged on main) · Last commit: see `git log` on the branch

## State in one paragraph
**M6 outputs (2026-11-17, this branch)** on top of S1 + M2 (below). Output
slots (1..8) are windows onto one canvas, made by a pure `OutputModel`:
resolution presets 720p / 1080p / 1440p / 4K / Custom, span (with an edge
blend overlap) or mirror, layer places (FILL ALL / OUT n / FIT / FREE),
black per output, IDENTIFY. In full screen each slot belongs to its display
**by identity** (`DisplayWatch` worker: QueryDisplayConfig + EDID from the
registry): a display that goes away hides its window and leaves the wait
set. The others are not touched. It comes back to its slot on any connector.
The refresh (60 / 50 / 59.94 / 30 / 25) is switched by the worker
(`CDS_FULLSCREEN`). Status out: the new block `Local\HimmelOutputs`
(`docs/interfaces/outputs-block.md`); commands 70–73 (`command-protocol.md` §4c).
The supervisor holds **curtains** (black windows just below each output
window), keeps the outputs / state / commands / show blocks open (the show
survives a renderer restart without a GUI), and restarts the renderer with
at most 0.5 s back-off. Measured on the dev laptop (3 windows): kill the
renderer → black at once (0 ms), show back after 1.0–1.6 s (6 / 6 runs,
`src/engine/tools/curtain_check.py`). Simulated cable pull (`simlost`) on
OUT 2: 60 cycles, 1 late frame, the others never stuttered. **Spout out**
(`--spout NAME`, LEAD yes 2026-11-17): MAIN goes out as a Spout 2 DX11 sender;
SpoutDX (2.007h, pinned by commit + SHA256, only SpoutDX + its SpoutGL helpers
compiled) runs on its own thread and D3D11 device, the frame loop only copies
MAIN into one of two shared textures. Checked with `spout_probe.exe`: 60 fps,
size follows the canvas, black follows master black. A held IDENTIFY is let go
when its producer goes quiet (LEAD condition). The first M6 commit (3e8d15b) is
merged on main (dcc01f9); Spout + the IDENTIFY release are the follow-up.

## Done (with commits)
- **M6 (this branch, `claude/engine-m6-outputs`):**
  - `protocol/outputs.h` (new block, enums), commands 70–73 appended to
    `commands.h` (`Count` = 74); `protocol_tests` asserts the wave-3 ranges.
  - `output_model.h/.cpp` (pure, in `himmel_engine_model`): layout, blend,
    places → `l_xform`, IDENTIFY fades, black, SimLost / Adopt, restore,
    `same_display` / `match_displays`. `DeckModel::set_outputs()` routes
    types 70–73 to it (same rate limit / validation / ack).
  - `display_watch.h/.cpp` (in `himmel_engine_shared`): the display worker
    (500 ms + poke on WM_DISPLAYCHANGE), seqlock snapshot, refresh switching.
  - `render_main.cpp`: output slots with state / display / identity; full-
    screen windows refuse foreign moves (`WM_WINDOWPOSCHANGING`); no DWM
    transitions on output windows; per-frame wait set = shown outputs only;
    `set_display()` (hide / move + resize only that swap chain / show);
    canvas from the model (rebuilt only on a setup command); blit with black
    per output, edge blend, IDENTIFY picture (t3); layer xform from the place
    (placeholder colour too); outputs block written on change / 100 ms;
    start-up time logged; windowed default stays 3 windows; `M` = Mode command.
  - `slice_shaders.h`: `b_edge`, `b_ident`, `tex3`, `edge_gain()`; the
    placeholder respects the place.
  - Supervisor: `Curtains` thread, keeps the blocks open, writes the command
    block header when it creates it, render back-off ≤ 0.5 s, 50 ms poll,
    `--no-curtains`.
  - `himmel_cmd`: `outputs`, `outblack`, `identify`, `place`, `outres`,
    `refresh`, `blendw`, `blendgamma`, `outmode`, `adopt`, `simlost`.
  - `tests/engine/output_tests.cpp` → `output_tests.exe` (111 checks).
  - `src/engine/tools/curtain_check.py`: the kill → black → back measurement.
- **M2 (merged 2026-11-16):**
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
0. **M6 leftovers:** Spout per output (only MAIN now) and Spout in; check a real cable pull and a 50 Hz switch on the show PC (only simulated
   / one display here); save display identities + places in the project
   (SHOW); per-output test pattern / crosshair (mapping, later); GUI Outputs
   view on the new block and commands (GUI).
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
- **M6 (D150–D154 in `decisions.md`):** windowed outputs report the first
  display's Hz; black per output is a cut; IDENTIFY darkens by 60 %; the
  blend is smoothstep ^ (1/γ); render restart back-off cap 0.5 s; identity
  rule (serial on any connector, else connector); a slot whose display is
  gone keeps its swap chain at the old size until it returns. Not checked on
  hardware: a real cable pull, two identical projectors without serials, a
  50 Hz mode switch, Windows reordering displays (all covered by
  `output_tests` / `simlost`, not by a real unplug).
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
build\bin\Release\output_tests.exe          # 111 checks, no GPU
build\bin\Release\himmel.exe --no-controller --no-weapons --no-ui --no-audio -- --outputs 3 --size 640x360 --no-show-files
build\bin\Release\himmel_cmd.exe clip a 1 0 -- outputs
python src\engine\tools\curtain_check.py --runs 6 --gap 6   # kill -> black -> back
build\bin\Release\himmel_cmd.exe identify -- blendw 120 -- place a 2 out 3 -- outblack 3 on -- simlost 2 on
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
- M6 Spout: `SpoutDX::SendTexture` waits on Spout's named mutex - never call it
  on the render thread. `LogRing` has one writer (the render thread): the Spout
  worker prints its lines straight to stdout.
- M6: a killed renderer's windows fade out (DWM close animation) over the
  curtain unless `DWMWA_TRANSITIONS_FORCEDISABLED` is set: it is now.
- M6: a supervisor that creates `Local\HimmelCommands` must write its header,
  or the renderer waits forever ("another magic / version / size").
- M6: screen-grab probes (BitBlt of the desktop at 200 Hz) can stall DWM and
  the renderer's Present; `curtain_check.py` samples every 20 ms.
- M6: an output preset of 4K on a 3-wide span makes a ~900 MB texture pool:
  fine on the 5090, watch the GTX 1070.
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
- M6: new `outputs-block.md` (`Local\HimmelOutputs` v1); `command-protocol.md`
  §4c (types 70–73, additive, `kCommandVersion` stays 1);
  `interfaces/README.md` row. State block unchanged (v2).
- `command-protocol.md` §4a (append-only): `LayerSet`, `LayerTrigger`,
  `LayerCue`, `LayerBeatLoop`, `LayerScratch`, `Clock`; `PlayAction` Release /
  Stop; `kHitDeck`, `kRelease`; enums `LayerParam`, `PlayMode`, `TimeBase`,
  `TriggerStyle`, `StartPos`, `ClipTarget`, `AutopilotMode`, `TransitionType`,
  `GridDiv`, `CueAction`, `ClockParam`. `kCommandVersion` unchanged.
- `engine-state.md` → **version 2**: `TransportSection` (clock, per-layer
  `LayerTransport`, HIT `LayerState` + transport) after the catalog with its own
  seqlock. GUI / CONTROL: rebuild against the new header; the HIT layer and the
  transport status are ready to show.
