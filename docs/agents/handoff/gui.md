# GUI handoff
Updated: 2026-11-17 by the GUI agent (wave 4, M11 part 2) · Branch: claude/gui-m11-panels · Last commit: see `git log claude/gui-m11-panels`

## State in one paragraph
M11 part 1 (on main) made every wave-1 engine feature reachable from the browser: transport, the CLIP editor, the
effect browser, PRESS, presets, generators, HIT, clock, the MK2 picture and a settings stub. Part 2 adds a panel for
every block of waves 2–3. Each block is **optional**: missing or the wrong version gives `{"present": false}` and the
panel hides itself; nothing fails.
- **SHOW** (`Local\HimmelShow`): the project bar in the top bar and the SCENES tab.
- **OUTPUTS** (`Local\HimmelOutputs`): the OUTPUTS view.
- **LIBRARY** (`Local\HimmelLibrary` + the library DB, read-only): the LIBRARY view.
- **AUDIO** (`Local\HimmelAudio`): the top-bar meter and Settings → Audio.
- **OSC** (`Local\HimmelOsc`): Settings → Remote / OSC.
- **Weapon blocks** (`Local\HimmelWeapon1..16`): "weapons found".

The start wizard's steps 1–4 use these blocks. The page is plain HTML / JS / CSS with no build step and is dark only:
`index.html`, `app.css` + `m11.css` + `panels.css`, `app.js` + `panels.js` + `views.js`.

## Done (with commits)
- **Bridge** (`src/ui_bridge/`):
  - `panels_json.{h,cpp}` (pure, tested): block → JSON, and the SHOW / OUTPUTS commands (25–36, 70–73) with enum names.
    Commands that carry a path or name have a `text` key.
  - `panels_link.{h,cpp}` (Windows): opens the optional blocks, with a heartbeat check per block.
  - SHOW text-box handshake: box id = the command's seq. The bridge writes the box under the push lock, then sends.
  - Writes the AUDIO `config` section (the "one controller").
  - Runs `himmel_audio --list` for the device picker (cached 10 s, 5 s limit).
  - Reads the library DB read-only and serves thumbnails (`<16 hex>.jpg` / `-strip.jpg` only).
  - New SSE events: `show`, `outputs`, `audio`, `osc`, `library`, `weapons`.
  - New routes: `GET /audio/devices`, `POST /audio/config`, `GET /library/clips`, `GET /library/thumb/<file>`;
    `/names` gains `panels`.
  - New args: `--library DIR | --show-root DIR | --no-library`.
  - `ui_bridge_tests`: 358 checks (112 new).
- **Supervisor hook** (marked `GUI (M11 part 2) hook`, in the MEDIA hook 1/3 block of
  `src/engine/supervisor/supervisor_main.cpp`): `himmel_ui` gets the same `--library` as `himmel_ingest`, and
  `--no-library` in test runs (`--no-show-files` / `--no-library`), so a test never reads the owner's library.
- **Page**:
  - `panels.js` (new): the panels listed above.
  - `views.js`: wizard steps 1–4 (import status + drop folder; MK2 + the weapons found; output cards + resolution +
    refresh + IDENTIFY; health + the sound-in picker with a live meter). Settings Audio / Remote are live pages,
    Engine points to OUTPUTS, Controllers lists the weapon drivers.
  - `app.js`: the new events, views OUTPUTS / LIBRARY, the SCENES tab.
- **Docs**: `getting-started.md` "Project, scenes, outputs, library, audio, OSC (M11 part 2)"; D121 PROPOSED; launch
  config `himmel-sim-test` (`himmel.exe --sim -- --show-root build/test-show-root`).
- **Checked live 2026-11-17** with `himmel-sim-test` (my stack only, stopped afterwards; screenshots in the session):
  - All 5 blocks present and alive; 4 weapon drivers found (sim, not connected).
  - SHOW:
    - LayerClip, then REC → "1 saved".
    - Rename through the text box → "Opening".
    - Recall into B: on both decks.
    - Lock Clip, Dice Roll (1/1), Undo (undo 2 / redo 1), bank +1.
    - Save As "GuiTest" → `Projects\GuiTest.himmel`, then edit → ●, then SAVE → clean.
    - 0 refused.
  - OUTPUTS: IDENTIFY (5 s countdown), BLACK OUT 1, SIM: PULL → LOST, blend 64, A2 FIT, A3 OUT 3, 50 Hz. 0 refused.
  - LIBRARY: a HAP clip copied into `originals` went analysing → ready in ~2 s. The thumbnail, analysis and IN SHOW
    show, and the engine catalog has it.
  - AUDIO: the picker lists 14 inputs. USE THIS INPUT → `himmel_audio` switched to "CABLE Output", stereo
    (gen 1 applied). Meters, spectrum and 25 default reactions show.
  - OSC: `/himmel/scene 1` APPLIED, `/himmel/nonsense` REFUSED, newest first.
  - Kill `himmel_ui` mid-show: the supervisor restarted it; the renderer PID stayed the same. `/state` (live, show,
    outputs) was identical apart from the GUI producer's ack counter (and the OSC producer's own Hellos). The page
    reconnected by itself, and a text-box rename and OutputBlack worked through the new bridge.
  - Done-check: build clean, ctest 21/21, 7 Python selftests, `himmel_audio --selftest`, `mk2_e2e.py` PASS.

## In progress
- Nothing. Ready to merge.

## Next up (ordered)
1. `FxReact` editor per FX slot once D131 (command 40 + the reaction table) is in the protocol; the AUDIO page
   already lists every effect's default reaction with its live signal.
2. AV offset in the GUI. It is only a renderer argument today (`--av-offset`), with no block field and no command.
3. Kit editor (KitSet, the 16 pad targets per scene) and macro binding (MacroSet) in the SCENES tab; the setlist editor
   (SongEdit New / AddScene / Move / Bpm). The codec and tests already cover these commands.
4. 4 × 4 pad grid (CLIPS / SLICE / FX PADS) of the v4 mockup; knob tiles 1–4 / 5–8.
5. Effect browser with live thumbnails (needs per-effect previews from the engine), scope badges (FxScope, D106).

## GUESSES
- `GUESS:` the bridge counts a block's writer as alive when its heartbeat moved in the last 1.5 s. For SHOW the
  heartbeat is `ShowStatus.time_us` (written at least every 250 ms).
- `GUESS:` a weapon driver is "alive" when its heartbeat moved since the last check, 1 s earlier.
- `GUESS:` the first `POST /audio/config` starts from what himmel_audio uses now (device, mode, channels) plus the doc
  defaults (auto gain on, gate −60, sensitivity 0.5), because `config.gen` = 0 means "use the command line".
- `GUESS:` a SCENES pad click recalls into the **focused** deck with the scene's own transition (`f = −1`); Alt = cut.
  REC saves the focused deck on the selected pad (or the next free one, `i = −1`).
- `GUESS:` the OUTPUTS view shows output 0's preset as "the" resolution and sends `Resolution` for all outputs.
- Older ones: MK2 consumer ring 3 = the GUI bridge; the clock beat is extrapolated in the page; mouse PRESS = 1.0;
  a generator goes into the first free slot (D107); `fake_engine` drops a released HIT / STOPped layer at once.

## Open questions for the owner
- D100 (slice layout), D120 (M11 layout) and D121 (where the part-2 panels live) are PROPOSED; the defaults are in use.

## How to build / run / test this track
- Build: `cmake --build build --config Release --target himmel_ui fake_engine ui_bridge_tests`
- Test: `build\bin\Release\ui_bridge_tests.exe` (358 checks, also in ctest); `node --check gui/app.js gui/panels.js gui/views.js`.
- Run (test data only): launch config `himmel-sim-test`. It runs `himmel.exe --sim -- --show-root build/test-show-root`;
  the children run in `build/bin/Release`, so the root lands in `build/bin/Release/build/test-show-root`. Then open
  http://127.0.0.1:8790. Read-only second view: launch config `himmel-ui-watch`.

## Wire format (bridge ↔ page), additions in M11 part 2
- Every panel event: `{"present": false, "state": "absent"|"version"|"starting", "panel": name}` when its block is
  not usable, else `present: true` + `alive` + the fields below.
- SSE `show`:
  - `{project, dirty, busy, writable, use_setlist, bank, song, song_count, song_name, song_bpm,
    pads[16]{used, name, variations, on:[a,b], rgb}, decks[2]{scene, pad, variation, edited, morphing, morph,
    dice_pos, dice_count}, locks:[names], undo, redo, macros[4]{name, value}, last_recall_us, scene_count,
    acked, rejected, last_reject` (the GUI slot's)`, message, message_seq, files{…}}`.
  - `files` = `{path, saved_unix, autosave_unix, restored, save_ok, saves, journal_records, relink_found, error,
    recent[]{name, path, saved_unix, outputs}, missing_count, missing[]}`.
- SSE `outputs`: `{count, mode, refresh, fullscreen, identify, identify_left_ms (250 ms steps), canvas[w,h], blend_px,
  blend_gamma, hz_target, hz_measured, displays, config_by_command, outputs[]{state, name, display{maker, product,
  serial, serial_text, connector}, black, preset, identify, size, canvas_x, swap, hz, rect, lost_count, returned_ms,
  blend}, places[9]{place, out, rect}}` (places 0..7 = deck × 4 + layer, 8 = HIT).
- SSE `audio` (~30 Hz):
  - `{sig{level, bass, mid, high, kick, snare, hat, onset, energy, build, drop}, spectrum[32], hits[3], rms_db,
    peak_db, gain_db, gated, clipping, hop, input{…}, clock[]{kind, run, bpm, confidence, phase_valid, beat, port},
    config{gen, applied, mode, device, …}, writable}`.
  - `POST /audio/config` takes the keys of `audio_config_from_json`.
- SSE `osc`: `{port, allow, counts{…}, total, last[]{unix_ms, from, verdict, reason, text}}` (newest first).
- SSE `library`: `{mode, paused, queued, working, ready_total, failed_total, pauses, ready_seq, catalog_full,
  job{name, stage, progress, last_error}, root}`.
- `GET /library/clips` → `{ok, root, clips[]{id, hash, name, kind, state, error, source, codec, size, fps, seconds,
  alpha, judder, thumb, stars, favourite, tags, analysis{…}}}`.
- SSE `weapons` (1 Hz): `[{slot, state, weapon, name, driver, simulated, connected, alive}]`.
- `POST /cmd`, show / output commands:
  - Keys: `deck`, `layer`, `slot`, `param` (an enum name or a number), `value` (or `i`), `f` (a number or up to 4),
    `relative` / `variation` / `clear`, and `text` (Project Open / SaveAs / Export / Relink, SongEdit New / Rename /
    RenameScene, MacroSet rename).
  - Spec + examples: `src/ui_bridge/include/himmel/ui/panels_json.h`; tests: `tests/ui_bridge/ui_bridge_tests.cpp`.

## Gotchas found
- **One stack per machine**: check `tasklist` in the same command before starting one, and stop it when you are done.
- **`himmel_ingest` has no `--selftest`.** Run with that flag it starts as a normal import worker on the *default*
  library (Documents\Himmel\Library). It did so once on 2026-11-17 at 10:15 and created an empty library there (no
  files imported). The owner may delete `Documents\Himmel` if it was not there before.
- Git Bash rewrites `/himmel/...` into a file path: send OSC tests with `MSYS_NO_PATHCONV=1`.
- The page draws on `requestAnimationFrame`, so a hidden tab (or a hidden Claude browser pane) draws nothing until it
  is shown. The data (`S`) keeps updating. Call `Panels.render()` / `Views.render()` by hand when you test in a hidden
  pane.
- The SHOW autosave (D109) brings back the last run's state at start: use `--show-root <temp>` for tests.
- The top bar must fit at 1440 px with BLACK visible. Below 1800 px the minor chips (sent / GUI / MK2, then fps) hide;
  they are all on Settings → Status.
- The Bash tool chokes on some heredocs (quotes inside); write a script file with the editor tools instead.
- Space / Enter are TAKE / CUT only in the PERFORM view and never in text / number fields.

## Interfaces touched
- No protocol change: no new CommandTypes, and no block version changed.
- Reads `show-block.md` v1, `outputs-block.md` v1, `audio-block.md` v1, `osc.md` (Local\HimmelOsc v1),
  `library-db.md` v1 (block + DB read-only) and the weapon blocks as written.
- Writes only its own parts: the SHOW text box of producer slot 1, and the AUDIO `config` section.
- Supervisor: the marked GUI hook above.
