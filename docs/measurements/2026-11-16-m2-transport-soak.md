# M2 clip transport: cue-jump latency and soak (ENGINE)

- **Date:** 2026-11-16, 18:20–18:57
- **Machine:** the owner's dev laptop, NVIDIA GeForce GTX 1070, one screen,
  Windows 11 (other agent sessions were active on the machine)
- **Build:** `claude/engine-m2-transport`, Release
- **Run:** `himmel_render.exe --outputs 2 --size 640x360` alone (no supervisor,
  no GUI bridge, no MK2), 2 windowed outputs, span, canvas 1280 × 360. Test
  clips from `python tools/make_test_clips.py` (Hap 1080p chunked, Hap 716p
  uncompressed 25 fps, Hap Alpha 720p, Hap Q 1080p).
- **Method:** the renderer's `STATS` line every 5 s (late = frame interval
  > 1.5 × 16.67 ms; `jumps / next-frame / worst` = cue jumps on a showing clip
  and how many frames after the command their frame was uploaded; 0 = in the
  next present). Summed with a small script.

## 1. Jump to a cue point: visible on the next frame

`himmel_cmd cuebench <layer> 40 120` on four layers (Hap 1080p ping-pong with a
cue point at 0.5, Hap Q 1080p, Hap Alpha 720p, Hap 716p), 160 random cue / slice
jumps, one every 120 ms:

| | Value |
|---|---|
| Jumps | 160 |
| **On screen in the next present** | **160 (100 %)** |
| Worst | 0 frames late |
| Clip misses | 0 |

How: the decode worker keeps the 16 jump targets of every layer decoded
(`ClipPlayer` warm slots), the jump is applied at the frame start, and the
playhead holds its landing frame for that frame (`Playhead::landed_`). Before
the hold, the playhead moved half a frame past the warm frame in the same tick
and half of the jumps were one frame late (32 / 60 in the first run).

## 2. Soak with every transport feature cycling

`himmel_cmd soak 900`: 8 layers loaded (blend add on one, kaleido on A1, a
master hue effect), then one transport command every ~170 ms on a random layer
for 15 minutes, in a fixed cycle: clip load, play mode (all 9), cue jump, beat
loop on (1/4–1 beat), beat loop off, speed (−2..2, glided), transition type
(cut / fade / black-dip), pad trigger, scratch hold to a random position,
scratch release, a HIT trigger, a HIT release. About 1.7 clip loads per second.

| Metric | Soak 1 (15 min, cold renderer) | Idle control (5 min, after) | Soak 2 (10 min, warm renderer) |
|---|---|---|---|
| Commands | 5180 (33 refused on purpose: cue jumps on empty layers) | 0 | 3448 (19 refused on purpose) |
| fps | avg 59.99, lowest 5-s period 59.40 | 60.00 | avg 59.99, lowest 59.79 |
| **Late frames** | **10** (in 5 periods; 6 of them in the first 2 min) | **0** | **2** |
| Worst interval | 34.3 ms | 24.6 ms | 32.0 ms |
| CPU per frame | avg 1.38 ms, worst 29.0 ms | avg 1.22, worst 5.8 ms | avg 1.44 ms, worst 16.2 ms |
| GPU per frame | avg 1.75 ms, worst 10.1 ms | avg 1.87, worst 8.3 ms | avg 1.89 ms, worst 8.7 ms |
| Cue jumps | 595, 573 next frame, worst 3 | — | 396, 374 next frame, worst 3 |
| Clip misses | 4047 | — | 2778 |
| Warm decodes | 23 209 | — | — |
| Crashes / log drops | 0 / 0 | 0 / 0 | 0 / 0 |
| Renderer RAM | ~385 MB (9 players + warm slots) | same | same |

Baseline (`2026-11-15-slice-engine-soak.md`): 6 late in 30 min with commands
(1 per 5 min, a much lower clip-change rate), 7 in 10 min idle.

## Reading it

- **Cue jumps meet the bar** when the clip has been on the layer for a moment.
  The late jumps in the soaks (22 + 22, worst 3 frames) are most likely jumps right after a clip
  load on that layer, before the worker had decoded its 16 warm frames (not proven per jump;
  the soak loads a clip every ~0.6 s; ~20–30 ms of decoding per clip).
- **Clip misses** are high by design in this soak: RANDOM jumps and absolute
  scratch moves land on frames that are not warm, so the last frame stays one
  more frame. Next step: pre-roll the next random target into a warm slot
  (handoff "Next up" 1).
- **Late frames:** soak 1 had 10 in 15 min, 6 of them in its first two minutes,
  in periods with 25–29 ms CPU frames (frame start → after Present; not
  separated further: first use of 9 clip players' textures and warm slots, or
  other work on the laptop). The idle control after it had 0 in 5 min, and
  **soak 2 on the warm renderer had 2 in 10 min (1 per 5 min), the same rate
  as the S1 baseline**, while driving ~3.5× more clip changes. So no new
  steady-state late frames from the transport; the cold-start burst is a
  candidate for the fullscreen late-frame proof (handoff "Next up").
