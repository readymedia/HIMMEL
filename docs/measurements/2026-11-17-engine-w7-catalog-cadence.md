# ENGINE wave 7: 300+ library clips, load by library id, frame cadence (2026-11-17)

Dev laptop (GTX 1070, one 60 Hz display), `himmel_render.exe --show-root %TEMP%\himmel_engine_w7
--outputs 3 --size 640x360` (3 windows), driven by `himmel_cmd`. Nothing of the owner's was read or
written. The stack was run under the wave 7 stack lock and stopped afterwards.

## Library
- 323 HAP sources: 320 copies of three 640x360 clips (24 / 25 / 30 fps, 3 s) plus 1080p clips at
  24, 25 and 30 fps (6 s). Each copy has a different trailing `free` atom, so each one has a
  different library hash. They were imported with `himmel_ingest --library ... --once` (HAP is
  copied as is): 323 ready in 5 min 40 s.
- The engine catalog had 327 clips (4 from `media/test`, 323 from the library). Ids 256..326
  are only in the clip pages; `himmel_cmd clips` lists all of them with their library ids.

## Load by library id
- 151 `LayerClipLib` loads, 120 ms apart, rotating over the 8 layers. That was 80 random library
  clips plus every clip with id >= 256.
- Result: **0 refused, 0 late frames** (7 STATS periods), **0 clip misses**, worst frame interval
  17.6 ms, GPU average 0.5–2.6 ms.
- An unknown library id (999999) is refused (`himmel_cmd` exit 3, `not ready`).

## Cadence (judder) before / after
These are CADENCE log lines: 5 s periods of 1080p clips on layers A1–A3 at 60 Hz.
- **judder** = the mean |cumulative hold error| in refreshes. The best a single frame can reach
  is 0 for 30 @ 60, 0.25 for 24 @ 60 (3-2) and ~0.28 for 25 @ 60.
- **irregular** = holds that are neither the floor nor the ceiling of the ideal.

| Mode | 24 fps (2.5 r/f) | 25 fps (2.4 r/f) | 30 fps (2.0 r/f) | GPU avg | late |
|---|---|---|---|---|---|
| Off (before wave 7) | 0.198–0.250, irr 0 | 0.217–0.279, irr 0 | 0.003–0.015, **irr 2** | 0.86–0.96 ms | 0 |
| Auto (default)      | 0.250, blend | 0.279, blend | 0.003, even, irr 0 | 0.88–0.93 ms | 0 (1 while switching modes) |
| Even                | 0.252–0.258 | 0.279–0.282 | 0.003 | 1.54–1.60 ms | 0–1 |
| Blend               | 0.250 | 0.279 | 0.003 | 1.67–1.84 ms | 0 |

Reading:
- On this laptop, in a window, the frame loop's wake-up jitter is small, so Off was already close
  to even. It still showed **irregular 30 fps holds** (1-3 instead of 2-2), which Even removes.
- The unit test models the owner's case: 30 fps on 60 Hz, 3 ms wake-up jitter, playhead on a frame
  boundary (`catalog_tests`). Off gets 18 of 298 holds wrong; Even gets 0 of 298. 24 @ 60 Even is
  a strict 3-2-3-2.
- 24 and 25 fps on 60 Hz can never be even with one frame per refresh: the 3-2 pulldown is the
  judder the owner sees. Blend mixes the two nearest frames by the playhead's fraction, so motion
  moves every refresh. The judder figure above measures the frame *index* stepping, so it does not
  change with Blend; the picture does.
- Blend cost: about +0.1–0.2 ms GPU for three blended 1080p layers. The GPU averages here vary with
  what else was on the layers at the time; the max spikes of 8–10 ms are the 1070 compositing
  three windows.

To repeat it, run `himmel_cmd smooth off|auto|even|blend` and `himmel_cmd playback`, and read the
`CADENCE` lines in the renderer's console every 5 s.
