# GUI handoff
Updated: 2026-11-15 by the GUI agent (S3, session keen-haslett) · Branch: claude/gui-slice-web · Last commit: see `git log claude/gui-slice-web`

## State in one paragraph
The playable-slice GUI (roadmap §0a, S3) works against `fake_engine.exe` and was also seen working against S1's real
`himmel_render` (real catalog, real previews, MK2 producer alive) while S1 tested on the same PC. `himmel_ui.exe`
(`src/ui_bridge/`) serves `gui/` on 127.0.0.1:8790, streams the engine state as Server-Sent Events, turns JSON into
validated `Command`s on producer slot 1, and serves the preview pictures. The page (`gui/index.html`, `app.css`,
`app.js`, no build step) is the dark Perform view: PREVIEW (green) / PROGRAM (red) monitors with 4 layer strips each,
MAIN + TAKE ring + CUT + crossfader in the middle, clip list on the right, FX panel (layer / deck / master, 4 slots with
effect, bypass, mix and parameters) at the bottom.

## Done (with commits)
- `himmel_ui.exe`, `fake_engine.exe`, `ui_bridge_tests.exe` (122 checks), `gui/`, launch config `himmel-ui`,
  getting-started "Play the slice from the web GUI", D94 PROPOSED (layout). Commit on `claude/gui-slice-web`.
- Checked in the browser preview at 1366 × 768 against `fake_engine`: load clips (click-then-layer; drag and double-click
  wired the same way), effects + params on a layer and on master, opacity / mute / blend, TAKE (space, ring fills,
  decks swap sides, focus follows), CUT (Enter), BLACK, a local refusal toast (clip id 99).
- Kill / restart `himmel_ui` while the fake engine runs: the page says "BRIDGE NOT REACHABLE", reconnects by itself;
  the show model (everything but play positions) was identical before and after, same epoch, engine frames kept going.
- Kill the engine: the page says "ENGINE NOT RESPONDING"; a restarted engine restores the show and bumps the epoch
  (toast "Engine (re)started").
- Screenshots: `side-session-notes/local_5d59d0bb-…/gui-screens/` (outside the repo).

## Wire format (bridge ↔ page)
- `GET /events`: SSE events `bridge` (1 Hz + on change: `engine` = ok / absent / stale / starting / version,
  `commands`, `preview`, `epoch`, `fps`, `frame`, `seq`, `pushed`, `dropped`, `versions`), `catalog` (on change:
  `clips[] {id,name,width,height,fps,seconds,codec,thumb_rgb}`, `effects[] {id,name,params[],defaults[]}`), `live`
  (~30 Hz, only when changed; the `LiveState` fields minus frame / time / fps; empty FX slots are just `{effect:-1}`).
- `POST /cmd` (`Content-Type: application/json`): one command or an array (≤ 256), e.g.
  `{"type":"FxParam","deck":"master","slot":0,"param":1,"value":0.3}`. Names = `CommandType` names; `deck` 0/1/"A"/"B"
  (FX also "master"); `value` is the int or float argument (blend / play / toggle also take names / booleans);
  `"relative": true` only for float types. Answer `{"ok":true,"seq":N}` or `{"ok":false,"error":"…"}`.
  The page shows "on air #N" from `live.producer[1].acked`. Spec + checks: `src/ui_bridge/src/json_codec.cpp`.
- `GET /preview/N?since=P`: 16-byte header (`HPV1`, u16 w, u16 h, u32 0, u32 published) + w·h RGBA; 204 = nothing new
  / inactive. Streams 0 DeckA, 1 DeckB, 2 Main (3..5 outputs). Raw RGBA over loopback (~0.5 MB per picture, ~20 fps
  per stream): no encoder, no new dependency.
- Safety: only `Host` 127.0.0.1 / localhost / [::1] and a local or empty `Origin` are answered; `/cmd` needs
  `application/json` (so a foreign web page cannot post without a CORS preflight we never answer).

## In progress
Nothing. S1 (`claude/engine-slice-decks`) was not pushed when this was written, so the merge-and-try step is open.

## Next up (ordered)
1. When S1 is pushed: merge it locally, run `himmel.exe --sim` + `himmel_ui`, check load / FX / TAKE / CUT against the
   real engine (do not push S1's changes from this branch). ENGINE makes the supervisor start `himmel_ui.exe`.
2. The 4 × 4 clip pad grid of the v4 mockup (`gui.md` §17), pad 1 = bottom-left = first clip of a 16-clip page
   (matches S2's mapper, see the S2 note), with PREVIEW / PROGRAM tags; keep the list for browsing.
3. Knob tiles 1–4 / 5–8 under the monitors once the engine publishes pages / macros (needs a state-block addition).
4. Wizard, weapon select, library, outputs (M11).

## GUESSES
- `GUESS:` layer 1 (A / E) is the bottom of the stack in `fake_engine`'s compositing (the contract does not say).
  The GUI lists A at the top like the MK2 group row. Check with ENGINE.
- `GUESS:` a TAKE while one is travelling is ignored by `fake_engine`; the real engine decides.
- `GUESS:` the TAKE ring shows how far PREVIEW has travelled in, computed from `crossfader` and `program_deck`
  (so the wheel and a manual fader move fill it too), not only `take_progress`.
- `GUESS:` clicking a layer strip sends `Focus` (= pressing its group button); the strip's own controls do not.

## Open questions for the owner
- D94 (PROPOSED): the slice page layout (see `docs/decisions.md`).

## How to build / run / test this track
- Build: `cmake --build build --config Release --target himmel_ui fake_engine ui_bridge_tests`
- Test: `build\bin\Release\ui_bridge_tests.exe` (122 checks, also in ctest).
- Run: `build\bin\Release\fake_engine.exe` (or the real engine), then launch config `himmel-ui` / `himmel_ui.exe`
  (`--port`, `--gui DIR`; it finds `gui/` by walking up from the exe), open http://127.0.0.1:8790.

## Gotchas found
- **The block names are per Windows session, so every agent on this PC shares them.** S1's real engine and
  `fake_engine` use the same `Local\Himmel*` blocks. `fake_engine` refuses to start (exit 3) when another engine's
  heartbeat moves; a real engine started after it restores the fake show from the block (by design, engine-state §2).
- The bridge keeps the state and preview blocks open, so they survive an engine restart (wanted). If a new engine
  build changes a block's size, stop `himmel_ui` too, or the old mapping stays.
- Space / Enter are TAKE / CUT everywhere except in text fields; selects and sliders drop the keyboard focus after a
  change so the keys keep working.

## Interfaces touched
None changed. Uses `docs/interfaces/command-protocol.md`, `engine-state.md`, `preview-block.md` (v1) as written.
