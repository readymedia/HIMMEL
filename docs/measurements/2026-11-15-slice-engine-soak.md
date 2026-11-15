# Slice engine soak, 30 minutes (S1 ENGINE)

- **Date:** 2026-11-15, 19:50:51–20:20:53
- **Machine:** the owner's dev laptop, NVIDIA GeForce GTX 1070, one screen, Windows 11
- **Build:** `claude/engine-slice-decks` @ 120ccc7, Release
- **Run:** `himmel.exe --sim -- --outputs 2 --size 640x360` (2 windowed outputs,
  span, canvas 1280 × 360), `python tools/mk2_live.py --sim`, all four weapon
  drivers in sim mode (no simulators attached), the S3 `himmel_ui` bridge
  running and sending real commands on slot 1.
- **Show:** 2 decks × 4 layers from `media/test/` (Hap 1080p chunked, Hap 716p,
  Hap Alpha 720p, Hap Q 1080p), blend modes, layer / deck / master FX.
- **Driver:** a script sending one random action every 0.3–2.5 s for 30 minutes
  (1214 actions, 0 unexpected refusals): TAKE (0 / 500 / 1000 / 3000 ms), CUT,
  clip changes (incl. empty), opacity, blend, speed (−2..2), FX set / param
  (all 8 effects, incl. strobe now and then, on all three levels), jumps, MK2 sim
  PLAY / RESTART / wheel turns. Before it: a flood of 9000 commands (4000
  applied, 5000 refused `RateLimited`, one log line).
- **Method:** the renderer's `STATS` line every 5 s (361 periods), summed with a
  script. Frame interval = time between frame starts (after the waitable
  object). Late = interval > 1.5 × 16.67 ms. CPU = frame start to after
  Present. GPU = timestamp queries from the first pass to after the previews,
  read back with DONOTFLUSH a few frames later. Preview GPU = the downsample +
  copy part of it.

## Results

| Metric | Value |
|---|---|
| Frames | ~108 000 at 59.99 fps average (lowest 5-s period 59.79) |
| Frame interval | avg 16.67 ms, worst 33.58 ms |
| **Late frames** | **6** (each exactly one missed vsync, 33.2–33.6 ms; ~1 per 5 min) |
| CPU per frame | avg 1.73 ms, worst 10.35 ms |
| GPU per frame | avg 1.31 ms, worst 13.20 ms (canvas 1280 × 360) |
| Preview path GPU | avg 0.154 ms, worst 0.339 ms (budget 0.3 ms: avg OK, worst over) |
| Preview path CPU | not separated; whole frame CPU avg 1.73 ms |
| Previews published | 181 680 (5 streams × ~20 fps); 34 841 readbacks skipped as still busy (never waited) |
| Clip frame misses | 172 (a frame not decoded in time: the previous one stays; mostly right after a clip change or jump) |
| Input → present (MK2 sim) | avg 9.24 ms (event read by the controller → Present returned) |
| Crashes / restarts | 0 / 0 |
| Log lines dropped | 0 |

## Reading it

- **0 late frames was not reached: 6 in 30 min.** Every late frame is a single
  missed vsync. In those periods CPU stayed low (worst 3.3–10.4 ms), so the
  frame was not CPU-bound. GPU spikes of 9–13 ms appear in many periods, with
  and without a late frame, so they are not the whole story either. Likely
  suspects, not yet separated: DWM composition of two windowed outputs (no
  independent flip in windows), other work on the laptop (other agent sessions
  were building C++ at the same time), and GPU spikes from clip changes (texture
  create + first BC upload). An idle control run is below.
- The preview path costs about 0.15 ms GPU on average; its worst period (0.34 ms)
  is just over the 0.3 ms budget in `preview-block.md`. Previews are spread
  over frames (one downsample per stream per 3 frames at 60 Hz).

## Control run without commands (same show, same processes)

10 minutes, 20:21:13–20:31:10, the same processes and the same show left on
air, **no commands at all** (driver stopped; the GUI producer went quiet):

| Metric | Value |
|---|---|
| Late frames | **7** in 10 min (worst interval 49.99 ms = two vsyncs) |
| CPU per frame | avg 1.57 ms, worst 28.85 ms |
| GPU per frame | avg 0.84 ms, worst 17.30 ms |
| Preview GPU | avg 0.146 ms, worst 0.375 ms |

More late frames without any commands than with them (7 in 10 min vs 6 in
30 min), and a 28.9 ms CPU spike with nothing to do: **the missed vsyncs come
from the machine (windowed outputs through DWM, other sessions' builds and
tools running on the same laptop), not from command handling, clip changes or
effects.** The engine's own work stays at ~1.6 ms CPU and ~1–2 ms GPU per frame.
The 0-late-frames criterion therefore cannot be judged on this laptop in
windowed mode; it needs a quiet machine and fullscreen outputs.

## Next

- Log each late frame with its own numbers (CPU parts, GPU of that frame when it
  comes back) so the cause can be named instead of guessed.
- Run the soak fullscreen on the show PC (RTX 5090) and on the laptop's own
  screen fullscreen (independent flip), where DWM is out of the path.
- Off-air deck at thumbnail size and no decode for invisible layers will cut
  the GPU / upload load (handoff "Next up" 1–2).
