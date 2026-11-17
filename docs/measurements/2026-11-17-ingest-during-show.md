# M7: 100 mixed files imported during a running show

Date: 2026-11-17 · Machine: the owner's dev laptop (Intel i7-7820HQ, 4 cores /
8 threads, 32 GB, GeForce GTX 1070, Windows 11) · Branch
`claude/media-m7-ingest` · Method: `python tools/ingest_measure.py`
(test set from `python tools/make_ingest_test_set.py`).

## Set-up

- `himmel.exe --no-controller --no-weapons --no-ui --no-audio -- --show-root <temp> --size 960x540`:
  3 windowed outputs at 60 Hz, the supervisor runs `himmel_render` and
  `himmel_ingest` (library `<temp>\Library`, so the owner's files are never touched).
- The show: deck A layer 1 = `hap_1080p_chunks4` (1080p Hap), layer 2 =
  `hap_716p_nocompress_25fps` at 60 % opacity, deck B layer 1 = `hapalpha_720p`,
  crossfader at 0.5 (both decks on MAIN) — three HAP layers decoding.
- The set (100 items, all made by ffmpeg's test sources, 247 MB): 40 H.264
  videos (24 / 25 / 30 / 50 / 60 fps, 360p … 1080p, odd sizes such as
  1366 × 768), 6 PNG-in-MOV videos with alpha, 4 files that already are HAP,
  20 stills (PNG with / without alpha, JPG, WebP, BMP), 10 GIFs, 10 SVGs,
  10 image sequences (20 PNGs each).
- "Late frame" = the renderer's own count: a frame interval over 1.5 × 1/60 s
  (`EngineStatus::late_frames`, now published every frame).

## Result

| | Frames | Late | Notes |
|---|---|---|---|
| Baseline, 30 s, nothing to import | 1800 | **0** | 60.0 fps |
| Import of all 100 items, 347 s | 20 826 | **0** | 60.0 fps; frame interval max per minute 16.8–18.0 ms (supervisor status lines) |

- **100 ready, 0 failed** in 347 s (≈ 3.5 s per item in background mode:
  one file at a time, ffmpeg `-threads 2` at idle priority).
- The throttle never had to pause (0 pauses: no late frame to react to).
- The catalog grew from 4 to 104 clips while the show ran. Half-way, a
  freshly imported clip (id 53, `049_hap_already_hap`) was loaded on deck B
  layer 2 with `himmel_cmd clip b 2 53`: it played at once (state `play`, not
  missing).
- The supervisor started and stopped `himmel_ingest` with the rest of the
  stack.

**PASS** — the M7 done-criterion ("100 mixed clips import during a show
without a late frame") holds on the dev laptop.

## Full speed (no show), for comparison

`himmel_ingest --once` on the same kind of set with no renderer (all cores):
99 + 1 duplicate in about 2 minutes. (The first set had two byte-identical GIFs;
the content hash correctly imported them once. The generator now makes every
item different.)

## Not measured yet

- The show PC (RTX 5090) and 4K sources; long sources (minutes) and a library
  of hundreds of clips (the catalog holds 256, D147).
- Disk-bound cases (a slow USB drive as the watch folder).
- Fullscreen outputs (here: windows).
