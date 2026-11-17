# Wave 8: first-user flow at 1600×900 (LEAD, 2026-11-17)

This was the wave 8 done-when, run by the LEAD on the merged wave 8 branch (GUI 35cdfcb, CONTROL e1609a0, SHOW 41f5738), under the stack lock.
- Command: `himmel.exe --sim --no-weapons -- --show-root %TEMP%\himmel_lead_walk8\show --size 960x540`, with the **default reserve** (20 GB on this 1 TB disk with 22 GB free).
- Browser viewport: 1600×900.

| Step | Result |
|---|---|
| START → 1 CLIPS → IMPORT A FOLDER → ESTIMATE | ok: the plan is shown ("needs ~104 MB for 7 clips …") |
| IMPORT THIS FOLDER | ok: "Imported 7 of 7", "the library uses 29 MB · Himmel keeps 20 GB free", 11 clips ready with thumbnails |
| 5 LAYERS: choose a clip… on PREVIEW A and B | ok: both on PREVIEW; toasts confirm |
| 6 TAKE ▶, then Kaleido | ok: PROGRAM (deck B) shows 004_gif with Kaleido |
| 7 SAVE SCENE, SAVE PROJECT AS "Lead walk 8" | ok: 1 scene; `show\Projects\Lead walk 8.himmel` |
| PERFORM at 1600×900 | ok: all 8 layer rows (A–D, E–H) sit above the bottom panel; the clip editor shows a filmstrip |
| ⏻ QUIT → QUIT HIMMEL | ok: every process stopped |

The wave 7 run at the default size is in `2026-11-17-wave7-first-user-flow.md`. `gui_flow_e2e.py` (46 checks) covers the same flow plus the owner's disk case (NoSpace → Keep free on disk → import).

Automated on the merge, after a clean build:
- 20 test programs and every selftest;
- `mk2_e2e.py` PASS, 99 checks (incl. 300+ library clips: BROWSE past 256 loads clip 275);
- `quit_check.py` (1.60 s);
- `gui_flow_e2e.py` PASS, 46 checks.

Note for anyone checking by hand: the in-app browser pane does not repaint while it is hidden. Changing only the URL hash (`#start` → `#perform`) keeps the old page, so reload before you judge the layout. The bridge already sends `Cache-Control: no-store`.
