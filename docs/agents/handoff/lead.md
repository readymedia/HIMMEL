# LEAD handoff
Updated: 2026-11-16 by the LEAD session (Claude Code, main checkout) · Branch: main

## State in one paragraph
M0 is done (merged, built, tested, pushed; remote branches deleted). The owner's
answers of 2026-11-15 are D89–D92 (`docs/decisions.md`): the LEAD merges and
pushes on its own when build + tests + self-tests are green; a playable slice
comes first (D90, `build-v1.md` §3a, `roadmap.md` §0a); 2–3 track agents at a
time; dev laptop until M12. The slice contracts are written and committed:
`docs/interfaces/command-protocol.md`, `engine-state.md`, `preview-block.md`,
headers in `src/protocol/` (`himmel_protocol`, `protocol_tests`), and the MK2
mapper contract `src/perform/` (`himmel_perform`, a LEAD stub CONTROL replaces).
Three agents run the slice: S1 ENGINE, S2 CONTROL, S3 GUI.

## Done (with commits)
- a36f078 M0: merged rack-design, entrancer-touch-fx, weapon-6-kf4,
  weapon-roland-v4, weapon-ohm64; one weapon list + port table in
  `weapons/README.md`; CLAUDE.md slimmed (per-weapon notes moved verbatim to
  `weapons/README.md` "Notes per weapon"); D89–D93.
- Next commit: slice contracts (protocol headers + docs + perform stub).
- Tests green at M0: mk2_tests 4286, media_tests 39, weapon_tests 163,
  touch_fx_tests 77, entrancer_tests 238 checks; Python `--selftest` of
  apc20, entrancer, kf4, lightpad, lightpad_games, lightpad_littlefoot, lumi,
  maschine_test, nuvj, nuvj_games, ohm64, v4; `mk2_fun_check`.

## In progress
- The slice is merged into main (2026-11-15): S1 ENGINE b2003f6, S2 CONTROL
  681900d, S3 GUI 1ea2217. Build green; 9 test programs pass (deck 143,
  entrancer 238, media 39, mk2 4286, perform 120, protocol 1111, touch_fx 77,
  ui_bridge 122, weapon 163). Checked together: `himmel.exe --sim` starts
  `himmel_ui`; `himmel_cmd` loads clips / FX; the web page on 8790 shows
  previews and TAKE flips PROGRAM; the MK2 sim shows the engine's Perform
  screens. MK2 sim input: fixed by CONTROL (bba57af, merged 2026-11-15:
  newest simulator wins, resets ignored, no flapping; mk2_tests 4306). CONTROL
  checked groups / wheel / TAKE reaching the engine; the LEAD has not re-run it.
- D94–D100: PROPOSED, defaults in use (LEAD 2026-11-16); the owner can still change them.
- Soak: 6 late frames in 30 min windowed on the laptop (an idle run had 7 in
  10 min); a fullscreen soak on a quiet machine is still owed (S1 handoff).

## Autonomous waves (owner 2026-11-16, `roadmap.md` §0b)
The owner's session (local_84946746…) starts each wave's sessions; the LEAD
merges each branch when green and writes the next wave here. **Standing
approval:** the owner told the LEAD session on 2026-11-16 "Yes, merge when
green" (build + every test + every `--selftest`), for every wave branch.

### Wave 1 (running, started 2026-11-16)
| Track · branch | Task | Merged |
|---|---|---|
| CONTROL · `claude/control-mk2-input` | MK2 sim + real MK2 drive the engine; e2e check; owner steps for the real MK2 | **merged 2026-11-16** (d43ac94): one sim works end to end (`tools/mk2_e2e.py` PASS, `mk2_chain_tests` 18); an ignored second sim now says so (red pill + Take over); `logs/mk2-<date>.log`. Real MK2 not yet checked by the owner |
| ENGINE · `claude/engine-m2-transport` | M2 clip transport (`design/clips-and-playback.md` §2–3, §10); supervisor log read sharing | — |
| FX · `claude/fx-m5-effects` | M5: metadata + default reaction, 16 tier-1 effects, 6 generators, PRESS / scopes, optional cap | — |

Merge order: CONTROL first (unblocks playing), then ENGINE, then FX (FX
likely touches the uber-shader / effect catalog that ENGINE also touches:
expect conflicts in `src/engine/`; resolve keeping both, rebuild, retest).

### Wave 2 (next, at most 3; start when wave 1 is merged)
1. **SHOW · `claude/show-m3-projects`.** M3 (`build-v1.md`): projects
   (`design/projects.md`: folder format, new / open / recent / save / save as,
   `.himmelshow` export, atomic save worker, autosave journal + restore,
   relink), show files (scenes, variations, kits with pad targets, macros,
   songs, setlist), save / recall / morph, undo stack 64 + reset-to-scene,
   dice / mutate / rotate v1 with locks. First write
   `docs/interfaces/show-files.md` and append the needed commands (Save,
   Recall, Undo, …) to `command-protocol.md` as a PR the LEAD approves.
   *Done when:* round-trip tests; a 4-layer scene recall hitches < 1 frame;
   kill the engine mid-save → autosave restores. *Owns:* `src/show/`,
   `tests/show/`, `docs/interfaces/show-files.md`, `docs/agents/handoff/show.md`.
2. **GUI · `claude/gui-m11-v1`.** M11 on what wave 1 delivered: clip editor
   drawer (transport modes, in / out, cue points), effect browser with the FX
   metadata, generators as sources, the MK2 picture with live feedback
   (`design/gui.md` §4, §16, §17, mockups `gui-mockups/v4/`), settings view
   stub, start wizard steps 1–4 against what exists. *Done when:* every new
   engine feature of wave 1 is reachable from the browser; killing / restarting
   `himmel_ui` mid-show changes nothing. *Owns:* `gui/`, `src/ui_bridge/`,
   `tests/ui_bridge/`, `docs/agents/handoff/gui.md`.
3. **AUDIO · `claude/audio-m8-reactions`.** M8 (`design/music.md` §3–5):
   `himmel_audio.exe` (WASAPI mic / line in / loopback, auto gain, gate;
   level, 3 bands, spectrum, kick / snare / hat onsets, energy; tap tempo, MIDI
   clock in, internal clock, beat / bar phase), `docs/interfaces/audio-block.md`
   first, reactions on every effect parameter in the engine (using FX's
   default `reaction` metadata), supervisor starts it. *Done when:* mic and a
   line in both drive zoom pulse on the kick on time; unplug / replug the input
   during a soak. *Owns:* `src/audio/`, `tests/audio/`,
   `docs/interfaces/audio-block.md`, `docs/agents/handoff/audio.md`; engine-side
   reaction code by agreement with ENGINE (small, in `src/engine/src/reactions*`).

### Wave 3 (after wave 2)
MEDIA M7 ingest · ENGINE M6 outputs / curtains / displays by identity ·
CONTROL M9 Lightpad (then M13 OSC). Then M10 NuVJ, M12 QA soak rig.

## Start here tomorrow (2026-11-15 evening)
- The owner reported: **neither the MK2 simulator nor the real MK2 drives the
  engine yet.** Real MK2: `himmel.exe --sim` never touches hardware; run
  `himmel.exe` without `--sim` with every MK2 Live page closed, and check.
  Simulator: with the sim link fix merged, only 1 of several group presses
  reached the engine (Perform ack 12 → 13), and the sim never showed the
  engine's screens. An orphaned `mk2_live.py --sim` from S1's soak was
  sending at the same time; it is closed now. Re-test with exactly one
  simulator. If it still fails, send to CONTROL: `SimPeerPolicy`
  (`src/controller/include/himmel/mk2/sim_link.h`) hands the peer back to an
  ignored sender after 1 s of silence from the current one, and an idle sim
  then stays dropped. Also check that the live view's sim sends anything when
  idle.
- Small bug: the supervisor opens `logs/himmel-*.log` without read sharing, so
  nobody can read the log while Himmel runs.
- Everything was shut down at the owner's request; nothing runs.

## Next up (ordered)
1. Owner plays the slice; collect feedback; settle D94–D100.
2. After owner feedback: M1 (rest), M2 transport, M3 projects/scenes,
   M5 effects — max 3 agents at a time.
3. A CI build (GitHub Actions on windows-latest) is not set up; M0 asked for
   it. Propose it to the owner if he wants CI (it is free for public repos).

## GUESSES
- GUESS: the slice's TAKE default is 2000 ms (command-protocol.md §4); the
  owner may want another default. Medium confidence.
- GUESS: `himmel_ui` port 8790 is free on the owner's PCs (all other Himmel
  ports are 8765–8787). High confidence.
- GUESS: the timeline weeks in `roadmap.md` §3 are estimates.

## Open questions for the owner
- None blocking. Ask after the slice: does the GUI layout fit, TAKE length,
  which effects matter most.

## How to build / run / test
- `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then
  `cmake --build build --config Release` (VS Build Tools 2022's bundled cmake:
  `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`).
- Run every `build/bin/Release/*_tests.exe`, every `python tools/<x>.py --selftest`.
- `python tools/make_test_clips.py` recreates the HAP test clips in `media/test/`.

## Gotchas found
- Git auto-merge of `weapons/README.md` interleaved sections from different
  branches without conflict markers. After a merge that touches it, check the
  `## ` headings order.
- Worktrees of finished sessions still hold their local branches; they go away
  when those sessions are archived (owner's choice).

## Interfaces touched
- New: command-protocol.md, engine-state.md, preview-block.md, the
  `himmel_perform` API. `docs/interfaces/README.md` updated.
