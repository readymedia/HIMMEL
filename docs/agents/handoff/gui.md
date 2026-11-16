# GUI handoff
Updated: 2026-11-17 by the GUI agent (wave 2, M11) · Branch: claude/gui-m11-v1 · Last commit: see `git log claude/gui-m11-v1`

## State in one paragraph
M11 on top of wave 1: every new engine feature of wave 1 is reachable from the browser. `himmel_ui` now speaks the
M2 transport commands (18–23), `FxPress` (24) and the HIT layer, streams the state v2 transport section and an MK2
mirror, and serves the FX metadata. The page (`gui/index.html`, `app.css` + `m11.css`, `app.js` + `views.js`, no build
step, dark only) has four views: START (wizard steps 1–4 against what 0.1 has), PERFORM (the slice page + clock, CLIP
editor tab, effect browser, PRESS, presets, generators, HIT), MK2 (the panel from its profile with live LEDs, both
screens, pressed buttons, knobs, pad pressure) and SETTINGS (stub: status, producers, versions, what comes when).

## Done (with commits)
- Bridge (`src/ui_bridge/`): codec for `LayerSet` (param by name or number, enum values by name), `LayerTrigger`
  (velocity, release), `LayerCue` (cue, Jump / Set / Clear, pos), `LayerBeatLoop`, `LayerScratch`, `Clock`,
  `LayerPlay` Release / Stop, `"deck":"hit"` on every layer command; `press` in the live FX slots; `kind` in the
  catalog; SSE `transport` (state v2 section, enums as names, beat extrapolated to now) and `mk2`; `GET /names`,
  `GET /fxmeta`, `GET /weapons/<id>.weapon.json`; `--watch` (read-only). `ui_bridge_tests` 246 checks.
- `fake_engine` keeps a transport section (settings, cues per clip, clock, HIT, PRESS) so the page works without the
  real engine.
- Page: see "State". Docs: `docs/getting-started.md` "The rest of the web GUI (M11)", D120 PROPOSED.
- Checked in the browser (1440 × 900) with `himmel_ui --watch` against the stack that was running on this PC
  (control-mk2-input build, state v1): the MK2 view mirrors the real MK2 (both screens, group and pad LEDs); START
  step 2 shows the MK2 as CONNECTED; PERFORM shows "ENGINE VERSION MISMATCH" as it should for a v1 engine.

## In progress
- **Not yet checked against a v2 engine** (wave-1 `himmel_render` or `fake_engine`): another Himmel stack was running
  (one-stack rule), so the CLIP editor, effect browser, PRESS, HIT and the kill / restart `himmel_ui` mid-show test
  wait for a free machine. Steps: stop every Himmel process (`tasklist`), `build\bin\Release\himmel.exe --sim`
  (or `fake_engine.exe`), launch config `himmel-ui` (or the supervisor's own), open http://127.0.0.1:8790, then kill
  `himmel_ui` mid-show and compare `/state` before / after.

## Next up (ordered)
1. The check above, with screenshots.
2. 4 × 4 pad grid (CLIPS / SLICE / FX PADS) of the v4 mockup once kits exist (SHOW M3).
3. Knob tiles 1–4 / 5–8 once the engine publishes pages / macros (state addition).
4. Effect browser with live thumbnails (needs per-effect previews from the engine), scope badges (FxScope, D106).
5. Wizard: real import (M7), outputs / IDENTIFY (M6), sound in (M8); Settings pages fill in as those land.

## GUESSES
- `GUESS:` MK2 consumer ring 3 belongs to the GUI bridge (ring 0 = engine; `mk2_monitor` / `mk2_pages_demo` take the
  first free one, normally 1). A crashed bridge leaves ring 3 "attached"; the controller counts drops, nothing else.
- `GUESS:` the clock beat between transport events is extrapolated in the page from BPM (the section's `beat` +
  `time_us`); the beat squares can be off by a frame.
- `GUESS:` PRESS with a mouse sends 1.0 while held (no pressure); pen / touch send `pointer.pressure`.
- `GUESS:` a generator from the GENERATORS tab goes into the first free slot of the focused layer (or the slot that
  already holds it), per D107.
- `GUESS:` `fake_engine` drops a released HIT clip and a STOPped layer at once (the engine fades them).
- Older ones (slice): layer 1 = bottom of the stack in `fake_engine`; a TAKE while one travels is ignored by
  `fake_engine`; the TAKE ring is computed from `crossfader` + `program_deck`; clicking a strip sends `Focus`.

## Open questions for the owner
- D100 (slice layout) and D120 (M11 layout: CLIP tab vs. a pop-out drawer, effect browser as cards) — PROPOSED.

## How to build / run / test this track
- Build: `cmake --build build --config Release --target himmel_ui fake_engine ui_bridge_tests`
- Test: `build\bin\Release\ui_bridge_tests.exe` (246 checks, also in ctest); `node --check gui/app.js gui/views.js`.
- Run: `himmel.exe --sim` (starts `himmel_ui`) or `fake_engine.exe` + launch config `himmel-ui`; open
  http://127.0.0.1:8790. Read-only second view: launch config `himmel-ui-watch` (port 8791, `--watch`).

## Wire format (bridge ↔ page), additions in M11
- SSE `transport`: `{clock:{bpm,grid,source,taps,beat}, layer:[deck][layer]{mode,time_base,trigger,start,target,
  autopilot,transition,snap ("Global" = follow the grid),inner_loop,catch_up,scratch_snap,slices,step_frames,
  autopilot_count,backward,reverse_hold,in,out,loop_in,loop_out,beats,interval,distance,step_div,glide,fade_in,
  fade_out,transition_time,seed,phase,beat_loop,scratching,pending,beat_loop_beats,speed_now,gain,
  transition_progress,pending_beats,velocity,loops,frame,cue[16] (-1 = none)}, hit:{LayerState without fx},
  hit_transport:{…}}`. Enum words = the `LayerSet` value names (`GET /names`).
- SSE `mk2`: `{present,connected,alive,map_verified,dropped,pressed:[button names],pads[16] 0..4095,knobs[8] 0..1 of a
  turn,last_knob,encoder,leds:{pad[16][rgb],group[8][rgb],button:{name:0..127}},screen: base64 of 2 × 2048 bytes}`.
- POST `/cmd` extra keys: `LayerSet {param, value}`, `LayerTrigger {value, velocity, release}`, `LayerCue {cue, value,
  pos}`, `LayerScratch {value, pos}`, `Clock {param, value}`; `"deck":"hit"`. Spec + checks:
  `src/ui_bridge/include/himmel/ui/json_codec.h`, `tests/ui_bridge/ui_bridge_tests.cpp`.
- POST `/cmd` takes an array (≤ 256): presets send all params in one request.

## Gotchas found
- **One stack per machine**: the `Local\Himmel*` names are per Windows session, so another agent's (or the owner's)
  stack shares them. `fake_engine` refuses to start when another engine's heartbeat moves; `himmel_ui --watch` is safe
  next to anything.
- The Bash tool on this PC chokes on some heredocs (quotes inside); write files with the editor tools instead.
- Space / Enter are TAKE / CUT only in the PERFORM view and never in text / number fields.

## Interfaces touched
None changed. Reads `command-protocol.md` (v1 + M2 + FxPress), `engine-state.md` v2, `preview-block.md` v1,
`effect-metadata.md` v1 and the MK2 block (`src/controller/include/himmel/mk2/shared.h`, ring 3) as written.
