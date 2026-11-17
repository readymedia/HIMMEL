# Wave 7: first-user flow in the browser (LEAD, 2026-11-17)

The wave 7 done-when, run by the LEAD on the merged wave 7 branch (e7f6f2a + GUI follow-up d6a4f57), dev laptop, simulated MK2. The stack was run under the stack lock:
`himmel.exe --sim --no-weapons --reserve-gb 2 -- --show-root %TEMP%\himmel_lead_walk\show --size 960x540`. The library and project were in that temp folder; nothing was written to Documents.
The test clips were made with `tools/make_ingest_test_set.py --count 8` (7 items: video, alpha, HAP, still, GIF, SVG, image sequence).

| Step | What the user does | Result |
|---|---|---|
| 0 Project | START → New project · 3 screens → Himmel's own confirm → OK | ok |
| 1 Clips | + IMPORT A FOLDER… → path → IMPORT THIS FOLDER | ok: "Imported 7 of 7", 11 clips ready, progress per file shown |
| 1 Clips | ESTIMATE DISK | **bug**: the worker answered (dry run, "needs ~104 MB for 7 clips … 7 fit"), but the dialog says "No estimate (the worker is busy); try again" |
| 5 Layers | choose a clip… on PREVIEW A and B (library clips) | ok: the engine plays both (B1 000_video, B2 001_alpha), gui ack 180, rej 0 |
| 6 TAKE + FX | TAKE ▶, then Kaleido | ok: both checks ticked; the effect goes to the on-air layer, and the text says so |
| 7 Save | SAVE SCENE (REC), SAVE PROJECT AS… "Lead walk" | ok: 1 scene; `show\Projects\Lead walk.himmel` |
| Quit | ⏻ QUIT → "Quit Himmel?" → QUIT HIMMEL | ok: every process stopped, exit 0 |

Automated on the same merge: build, 20 test programs, every selftest, `mk2_e2e.py` (94 checks), `quit_check.py`, and `tools/gui_flow_e2e.py` PASS (35 checks; it imports 7 of 7 in 25 s with `--reserve-gb 5`).

Found (sent to GUI):
- Perform layout: at 1600×900 and smaller, layer rows D and H draw over the bottom panel (CLIP / LAYER FX tabs), so a 4th layer cannot be reached.
- ESTIMATE DISK never shows the worker's plan (above).
- The Start screen's library line said "the library uses 0.0 GB" right after the import (small).
