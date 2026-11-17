# SHOW handoff
Updated: 2026-11-17 by the SHOW track agent (wave 2) · Branch: `claude/show-m3-projects` · Last commit: see `git log`

## State in one paragraph
M3 is built: projects (folder format `himmel-show/1`, new / open / recent /
save / save as / `.himmelshow` export + open, revert, an atomic save worker,
the autosave journal + `show.json` + 5-minute snapshots, restore at start,
relink search), show files (banks of 16 scenes, variations a–h, kits with pad
targets, 4 macros per scene, songs, setlist), save / recall / morph, undo 64 +
redo + reset to scene, dice v1 (roll / mutate / back / forward / rotate,
scopes, 8 locks). It all runs inside `himmel_render` (`ShowRuntime`, hooked
into the drain loop), reachable through commands 25–36 and the new block
`Local\HimmelShow`. Contracts: `docs/interfaces/show-files.md`,
`command-protocol.md` §4b. `show_tests` (778 checks) and `show_crash_test`
(6 kills, 3 of them during a write) pass.

## Done (with commits)
- `src/protocol/include/himmel/protocol/show.h` (new): enums + `ShowBlock`.
  `commands.h`: types 25–36, `Producer::Show` (= slot 4, was `Spare4`),
  flags `kVariation`, `kClear`. Append-only, `kCommandVersion` stays 1 (as `FxPress`).
- `src/show/`: `model` (PODs), `look_ops` (diff → commands, morph lerp,
  clip binding by name), `dice`, `files` (JSON, atomic writes, CRC
  manifest, journal, recents, stored zip), `runtime` (frame side + worker),
  `tools/show_cmd`.
- Engine hooks (agreed with the LEAD by message, 2026-11-17):
  `render_main.cpp` routes types ≥ 25 to `ShowRuntime::submit`, calls
  `show->tick` after `model->tick`, logs show messages, maps
  `Local\HimmelShow`; args `--project`, `--no-show-files`, `--show-root`.
  `deck_model.cpp`: command names for logs only (slot 4 is rate limited like
  everyone; the runtime caps itself at 384 commands per frame, LEAD 2026-11-17).
- Wave 8 (2026-11-17, branch `claude/show-w8-libids`): clips by library id
  (D210). `ClipIndex` (`clips.h` / `clips.cpp`): the show's append-only copy
  of the engine's clip directory (8192 ids, library id, hash from the clip
  table's `hap/<hash>.mov`, state), synced every tick and before each
  command; the worker reads it. Looks (`clip_lib`, `clip_hash`), kit pads
  (`lib`, `hash`) and the journal (version 3) carry the reference; files
  version 2 (`library_id`, `hash`; version 1 loads by name and is upgraded).
  Recall binds the scene in place (library id → hash → name), loads Ready
  library clips with `LayerClipLib`, and names missing clips in the recall
  message. render_main hook: `RuntimeConfig::clip_dir` / `clip_hash` in
  SHOW hook 1/4. Tests: `test_library_clips`.
- Wave 6 (2026-11-17, branch `claude/show-reactions`): looks keep the knob
  reactions (D131). `ReactKnob` / `ReactRow` in `model.h` (canonical:
  Default and Off zero the rest), `diff_react` in `look_ops.cpp` (FxReact
  after every FxSet, Set + Range), `"react"` per FX slot in the files, old
  files = Default, journal version 2, dice resets a re-rolled slot's
  reactions. `show_tests` `test_reactions`.
- LEAD conditions (2026-11-17) done: ranges 25–39 / slot 4 / D109–D119,
  `show-block.md` with the text-id handshake, layout `static_assert`s in
  `protocol_tests`, the four render_main hooks marked `SHOW (M3) hook n/4`.
- Measured (`show_tests`, dev laptop, 2026-11-17): a recall between two full
  4-layer scenes = up to 51 commands, mean 2.7 µs, worst 21 µs.

## In progress
Nothing half-done.

## Next up (ordered)
1. CONTROL M4 uses the commands (pads LOOKS → `SceneRecall` with the focus
   deck, `REC` → `SceneSave` (+ `kVariation` in PLAY focus), `ERASE` →
   `Undo` / `SceneReset`, knob 2 / `TRANSPORT < >` → `Dice`, Top 1–4 →
   `Locks`, knobs 5–8 → `MacroValue`, `ENTER` → `SceneRecall kSceneNext`).
   The kit pads are data only so far: playing a kit pad (its target layer,
   effect hits, actions) is CONTROL's mapper reading `ShowStatus` + the kit.
   **A gap:** the kit itself is not in the show block yet (only names of the
   page). M4 needs the kit of the focus deck's scene: add a `KitInfo kit[2][16]`
   section to `ShowStatus` (append, version bump) when CONTROL asks.
2. GUI M11: project menu (`Project` + text box), scene grid from
   `ShowStatus.pad`, recent projects from `FilesStatus.recent`, "Restore the
   autosave from 22:41?" from `FilesStatus.restored` / `autosave_unix`
   (D109), the dirty dot.
3. Relink into the running catalog (needs ENGINE / MEDIA: add files to the
   catalog at run time, M7).
4. Moods (16 files), mood-aware dice (library analysis, M7), effect presets
   for ROTATE at FX level, cues / timeline (0.2), "Pack with media".
5. `thumb.png` per project (needs a preview grab from the engine).

## GUESSES
- GUESS (D109, PROPOSED): the autosave is restored automatically at start;
  the GUI offers Revert. Medium confidence the owner prefers this on stage;
  ask him.
- GUESS (D110, PROPOSED): undo coalesces edits < 400 ms; undo leaves the
  mixer alone; GO past the last scene goes to the next song / a bank wraps;
  REC on an occupied pad replaces the look only; scene names "Scene 07";
  dice weights; macro fallbacks; effect-hit pads use FX slot 4; opening a
  project mid-show lands on PREVIEW. All easy to change in `runtime.cpp` /
  `dice.cpp`.
- GUESS: the clip identity on disk is name + size + length; the short hash of
  `projects.md` §1 needs the library (M7).
- GUESS: param classes for the COLOUR / MOTION locks come from param
  **names** (`dice.cpp` `param_lock`); better: a `class` per param in
  `effect.json` (FX track).

## Open questions for the owner
- D109: restore the autosave by itself at start, or ask first?
- D110: the defaults above.

## How to build / run / test this track
- Build as usual (`cmake --build build --config Release`), then
  `build\bin\Release\show_tests.exe` and `build\bin\Release\show_crash_test.exe`
  (it starts itself as a child and kills it 6 times; ~30 s). Both write only
  under `%TEMP%\himmel_show_tests`.
- In the engine: `himmel_render --show-root D:\tmp\showtest` keeps projects out
  of `Documents\Himmel`; `show_cmd status`, `show_cmd save a next`,
  `show_cmd recall a 1`, `show_cmd project saveas "My Show"`, … (usage at
  the top of `src/show/tools/show_cmd_main.cpp`). `show_cmd` uses producer
  slot 3 like `himmel_cmd`: do not run both at once.

## Not verified yet
- **No live engine run.** When this was finished another agent's full stack
  (`himmel.exe`, `himmel_render.exe`, `himmel_ui.exe`, drivers) was running,
  so by the one-stack rule no second renderer was started. The criteria are
  shown with the real `DeckModel` in `show_tests` (recall cost, one-frame
  landing) and with real process kills in `show_crash_test`; the render hook
  itself compiles and links but has not run. Next free moment:
  `himmel_render --show-root %TEMP%\showlive`, load layers with `himmel_cmd`,
  `show_cmd save a next` / `recall a 1`, check the per-second late-frame log,
  then `taskkill /F` the renderer during `show_cmd project save` and restart.

## Gotchas found
- **Text boxes keep their text.** A command that reads the box only
  sometimes must say so (Export `i = 1`, Open `i = −1`, SongEdit New
  `i = 1`); the first version picked up an old Save-as name and exported
  into the working folder.
- **Before the start-up project is loaded, the frame's show is empty.** An
  autosave or journal record from those first frames overwrote the real
  autosave. Fixed with load epochs (`Request.epoch`, `LiveRec.epoch` vs the
  worker's `handed`) and a `ready` gate. Keep that rule for any new request
  that carries show data.
- The journal is read as "the newest record by time"; records from before a
  load are dropped by epoch, not by position.
- Git Bash: `cmake --build ... -- /m` is turned into a path; use `--parallel`.
  Heredocs with an apostrophe break the shell wrapper; write patch scripts
  to a file.

## Interfaces touched (and whether docs/interfaces/ is updated)
- `command-protocol.md` §3 (slot 4), §4b (types 25–36), header note: updated.
- `show-files.md` and `show-block.md`: new (version 1). SHOW's ranges (LEAD
  b236400): command types 25–39 (36 used), slot 4, decisions D109–D119.
- `docs/interfaces/README.md`, `docs/architecture.md`, `docs/decisions.md`
  (D109, D110): updated.
- No change to `state.h` (the state block) or the preview block.
