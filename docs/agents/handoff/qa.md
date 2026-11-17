# QA handoff
Updated: 2026-11-17 by the CONTROL wave-3 session (M12 failure rig) · Branch: claude/qa-m12-rig · Last commit: see git log

## State in one paragraph
**M12: `tools/qa_rig.py`** runs the whole stack in `--sim` with a temporary show root and checks that a failing part never takes the outputs down: it kills every process in turn (render, ui, audio, ingest, osc, mk2_controller, weapon_blocks, weapon_nuvj), pulls and replugs each simulated weapon (MK2, Lightpad, NuVJ), makes a clip file disappear, and runs a soak (`--minutes` / `--hours`) that writes fps, late frames, frame time, input → present and per-process memory + handles every minute into `docs/qa/`. The recipe for the owner's 8 h soak and the show-PC run is **`docs/qa/README.md`**. A 20-minute soak on the dev PC is in `docs/qa/` (see "Done").

## Done (with commits)
- 2026-11-17 (`claude/qa-m12-rig`): `tools/qa_rig.py`, `docs/qa/README.md`. Results on the dev PC: `docs/qa/rig-20261117-0942.md` **PASS** (every kill back in < 2 s, the renderer in 1.40 s with black curtains and the show restored; MK2 / Lightpad / NuVJ unplug + replug; missing clip = placeholder) and `docs/qa/soak-20261117-0920.md` (20 min: **0 late frames**, 60 fps, render 124 → 127 MB, handles flat 448–456).

## In progress
- Nothing.

## Next up (ordered)
1. The owner runs the 8 h soak (`docs/qa/README.md` §2.1) and the show-PC run (§2.2).
2. A release checklist (build-v1 M12 gates the release): build, tests, `mk2_e2e.py`, `qa_rig.py --minutes 20`, the 8 h soak, the show-PC run.
3. Rig gaps (README §5): audio device pull in the rig, the supervisor itself dying, all four weapons + an open GUI during a long soak.
4. The camera latency test (needs an external webcam; parked in `himmel_render_legacy.exe`).

## GUESSES
- `GUESS:` "back" for the renderer = a new epoch drawing ≥ 3 frames (its fps figure is a 5 s average). The supervisor's own restart is 0.25–0.5 s; the measured total (kill → frames) is in the rig report.
- `GUESS:` the curtain check samples the desktop 0.3 s after the kill (after Windows' close animation) and calls < 8 / 255 mean brightness black. Another window on top of the output area makes it fail: do not use the PC during the rig.
- The GPU device-loss path is not forced (README §4); it shares the render-kill recovery.

## Open questions for the owner
- None blocking. The pass limits in README §2.1 (0 late frames, memory within ~10 % after the first hour, flat handles) are proposals.

## How to build / run / test this track
`python tools/qa_rig.py` (checks, ~3 min), `--minutes 20`, `--soak-only --hours 8`, `--out DIR`, `--bin DIR`. Needs a Release build and `media/test/*.mov`; refuses to start while another Himmel stack runs.

## Gotchas found
- The renderer's `LiveState.fps` is updated every 5 s (the STATS window): do not wait for fps > 0 right after a restart; count frames.
- Children print to the supervisor's stdout: start `himmel.exe` with stdout to a file to get the renderer's log ("Lightpad offline", "STATS ..."). `logs\himmel-*.log` has only the supervisor's own lines.
- `himmel_cmd quit` does not exist: stop the stack with `taskkill /T /F` on `himmel.exe` (the job object takes the children).
- The simulated Lightpad is silent until touched: send active sensing (0xFE) so `weapon_blocks --sim` finds it.

## Interfaces touched
- None (the rig only reads the existing blocks through `himmel_cmd` / `show_cmd` and the desktop).
