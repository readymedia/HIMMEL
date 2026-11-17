# MK2 input → glass (CONTROL M4), 2026-11-17

**Result: median 32.0 ms, p90 37.8 ms, max 40.2 ms over 20 runs** (to black
26–39 ms, back to the picture 24–40 ms). M4's target is ≤ 3 frames (50 ms at
60 Hz): met, with every sample under 2.5 frames.

## Method
- `python tools/mk2_e2e.py` (the "input -> glass" stage, after the song).
  Stack: `himmel.exe --sim --no-ui --no-weapons -- --no-show-files`, one
  output window, a test clip live on PROGRAM.
- Input: the simulated MK2 (`tools/mk2_sim.py`) holds / lets go of **Top 7**
  (BLACK while held). t0 = the moment the changed controls report (0x01) left
  the simulator for `mk2_controller --sim` (UDP 127.0.0.1:8766).
- Glass: the output window's client area read from the **composed desktop**
  (GDI `StretchBlt` of the screen DC down to 32 × 18 samples), polled in a
  loop. t1 = the first poll whose mean brightness crossed 25 % (to black) /
  60 % (to picture) of the way between the two levels. One poll takes
  16.5 ms: the screen DC waits for the next DWM composition, so the polls are
  one per 60 Hz desktop frame.
- Path covered: UDP → controller (report parse, ring 0) → `himmel_render`
  frame start → `Mk2Perform` → command → `DeckModel` → render → `Present`
  (flip model, max latency 1) → DWM composition → desktop.

## What it does not cover
- The real MK2's USB (pads stream every 1.33 ms, controls ~10 ms apart,
  measured 2026-11-09) instead of UDP: expect +0–10 ms for buttons / knobs.
- The monitor's own scan-out and panel response (one refresh + the panel),
  which a camera on the screen would see. The camera rig (`Sampling`,
  parked in `himmel_render_legacy.exe`) needs an external webcam.
- Fullscreen independent flip (no DWM): a fullscreen output skips the
  composition step, so it should be the same or shorter.
- The polling quantum (16.5 ms) adds 0–16.5 ms to each sample, so the true
  value is on average ~8 ms lower than measured.

## Machine
The owner's dev PC: NVIDIA GTX 1070 (WMI reports a 4500 px wide desktop at
59 Hz), output as a window, release build of `claude/control-m4-perform`.

## Raw samples (ms)
to black: 31.2 36.8 26.8 33.4 39.1 30.2 37.8 26.3 32.5 25.5
to picture: 33.9 40.2 31.0 35.9 28.6 31.5 23.5 33.1 35.0 24.2
