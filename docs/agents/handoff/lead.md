# LEAD handoff
Updated: 2026-11-15 by the LEAD session (Claude Code, main checkout) · Branch: main

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
- Open owner choices: D94–D100 (PROPOSED).
- Soak: 6 late frames in 30 min windowed on the laptop (an idle run had 7 in
  10 min); a fullscreen soak on a quiet machine is still owed (S1 handoff).

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
