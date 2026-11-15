# ENGINE handoff
Updated: 2026-11-15 by the S1 ENGINE session · Branch: `claude/engine-slice-decks` · Last commit: see `git log` on the branch

## State in one paragraph
`himmel_render.exe` is now the **slice engine** (roadmap §0a, task S1): a pure
deck model (2 decks × 4 layers, FX slots on layer / deck / master, crossfader,
TAKE / CUT / black / focus) fed only by validated commands from
`Local\HimmelCommands` (slots 1–7) and the in-process MK2 Perform mapper (slot 0);
the show on air goes out in `Local\HimmelState` (live + catalog, restored by clip
name after a restart) and small pictures in `Local\HimmelPreview`. Rendering:
one ClipPlayer worker per layer (8), blend modes + opacity, an 8-effect
uber-shader usable in any slot, A/B linear crossfade into MAIN, master FX, black,
span / mirror outputs. The old renderer (test card, camera + latency test, Touch
FX, Entrancer, weapon knob pages) is **parked** as `himmel_render_legacy.exe`
(`himmel.exe --legacy`). Tested with `himmel_cmd`, the MK2 simulator and the S3
GUI bridge (which was already sending real commands to this engine on the dev
laptop); 30-minute soak in `docs/measurements/2026-11-15-slice-engine-soak.md`:
60 fps, 0 crashes, CPU 1.7 ms / GPU 1.3 ms per frame, but **6 late frames**
(single missed vsyncs); an idle control run with no commands had 7 in 10 min,
so they come from the windowed laptop setup, not the engine. Not yet proven:
0 late frames fullscreen on a quiet machine.

## Done (with commits)
- `src/engine/include/himmel/engine/deck_model.h` + `src/deck_model.cpp`: the
  model, `validate()` / `submit()` exactly as command-protocol.md §4–5 (refuse:
  unknown type, bad target, bad id, NaN/inf/enum out of range, rate > 4000/s;
  clamp: values in range-ish), per-producer `acked_seq` / `rejected` /
  `last_reject`, one log line per producer per second, `restore()` by name.
  Library `himmel_engine_model` (no Windows, no GPU).
- `tests/engine/deck_tests.cpp` → `deck_tests.exe` (143 checks).
- `fx_catalog.h/.cpp`: the 8 effects (Hue/Sat, Invert, Mirror, Kaleido, RGB
  Split, RotoZoom, Strobe, Pixelate), up to 4 params each, defaults.
- `clip_catalog.h/.cpp`: `media/` playlist → `Catalog.clip` (size, fps, length,
  codec, placeholder = average colour of frame 1 from the BC end points).
- `shm_block.h/.cpp`: open-or-create named blocks, header written last.
  Library `himmel_engine_ipc`.
- `log_ring.h`: frame-loop-safe logger (fixed-size lines in an SPSC ring, a
  thread prints them).
- `render/render_main.cpp` (new), `render/slice_shaders.h` (ps_source,
  ps_layer, ps_fx, ps_mix, ps_blit, ps_down), `render/legacy_main.cpp` (old).
- `tools/himmel_cmd_main.cpp` → `himmel_cmd.exe` (slot 3, every command, chains
  with `--`, waits for the ack, prints the state; `flood N` = rate test).
- `tools/preview_dump_main.cpp` → `preview_dump.exe [dir] [--rate]` (the reader
  side of the preview block, PNGs + pictures/s).
- Supervisor: starts `himmel_ui.exe` if it is next to it (`--no-ui`), restarts
  it on exit, never waits on it; `--legacy`; children without a heartbeat block
  are only watched for exit.

## How the frame loop stays clean (rule 1)
- Everything allocated at start-up: catalog, model, 8 ClipPlayers (+ worker
  threads), Perform ring, shared blocks, texture pool (8 canvas targets +
  per-preview targets + 3 staging textures per stream), shaders (compiled
  once), queries. A window resize re-creates canvas targets (same as before;
  never happens in a fullscreen show).
- Per frame: ring pops (bounded: 1024 MK2 events, 256 commands per producer),
  model updates (POD), `UpdateSubresource` of ready BC frames, `Map(DISCARD)` of
  constant buffers, draws, `Map(DO_NOT_WAIT)` of staging textures (skipped when
  busy), `GetData(DONOTFLUSH)` for GPU timestamps, seqlock memcpy into the
  shared blocks. No `new`, no locks, no files.
- Logging only through `LogRing::logf` (snprintf into a fixed slot; a full ring
  drops the line and counts it). Startup logging before the loop is the same.
- Known exception (old, unchanged): the supervisor's stdout pipe is written by
  the log thread, not the render thread.

## In progress
Nothing half-done. Next step for the engine is in "Next up".

## Next up (ordered)
0. **Late-frame proof:** log each late frame with its own numbers, then soak
   fullscreen on a quiet machine (laptop screen fullscreen, later the 5090).
1. **Off-air deck at thumbnail size** (design/engine.md §2): today both decks
   render at canvas size every frame. Plan: per-deck viewport scale + uv scale in
   ps_layer / ps_fx / ps_mix; full size from the frame a TAKE starts.
2. Skip decoding for layers nobody sees (keep one warm frame), per engine.md §2.
3. GPU meter per layer / effect, budget guard (half-res optional effects).
4. Bring Touch FX + Entrancer back as a master-level pass in the deck engine
   (it is self-contained: `touch_fx.cpp`, `entrancer_link.cpp`), then the
   camera as a layer source; then drop `himmel_render_legacy.exe`.
5. Generic weapons (Lightpad, NuVJ, LUMI, APC20) are **not read** by the deck
   engine: their mappers should become `src/perform/` mappers like Mk2Perform
   (CONTROL). Until then they only work in the legacy renderer.
6. Crossfade curves, luma / chroma key blends, more effects (FX track, M5).
7. The render graph built in a worker (design/engine.md §2.1) once there are more
   than fixed decks; today the pass list is a fixed loop over the model.

## GUESSES
- **Program flip rule** (deck_model.cpp `set_crossfader`): a manual fader flips
  PROGRAM only at the ends. Medium confidence. PROPOSED D9x-S1a.
- **TAKE during TAKE finishes it now.** Low confidence. PROPOSED D9x-S1b.
- **Layer 1 = bottom**, TAKE clamped to 60 s. PROPOSED D9x-S1c.
- **Effect defaults** (fx_catalog.cpp) chosen to be visible but tasteful. Medium;
  owner tunes after playing.
- **Strobe / RotoZoom spin run on a fake 120 BPM clock** until the audio clock
  (M8). Certain it is wrong later; harmless now.
- **Grey (#404040) placeholder** for a file we cannot read at all. Low impact.
- **A hung `himmel_ui` is not detected** by the supervisor (it has no heartbeat
  block yet); a dead one is restarted. It cannot affect the outputs either way.
- **Clip names must be unique** for restore-by-name; two files with the same
  name in different folders restore to the first. Low impact for the slice.
- Start-up probes every clip (reads frame 1). With hundreds of 4K clips this
  could approach the supervisor's 8 s start grace. Not measured beyond 4 clips;
  move the probe to a worker (or the ingest DB, M7) before big libraries.

## Open questions for the owner
- D9x-S1a / b / c in `docs/decisions.md` (PROGRAM flip rule, TAKE during TAKE,
  layer order). LEAD to give them numbers.

## How to build / run / test this track
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 8
build\bin\Release\deck_tests.exe            # 143 checks, no GPU
build\bin\Release\protocol_tests.exe
python tools\make_test_clips.py             # media\test\*.mov (needs ffmpeg)
python tools\mk2_live.py --sim              # MK2 simulator, http://127.0.0.1:8765
build\bin\Release\himmel.exe --sim -- --outputs 2 --size 640x360
build\bin\Release\himmel_cmd.exe clip a 1 0 -- clip b 1 3 -- fx m 1 kaleido -- take 1000
build\bin\Release\himmel_cmd.exe            # the state; "catalog" lists ids
build\bin\Release\preview_dump.exe tools\_screens\preview --rate
```
MK2 stub mapping (the LEAD's stub until S2 lands): wheel = crossfader,
PLAY = TAKE, RESTART = CUT. Output windows: `M` = span / mirror, `O` = overlay,
`Esc` = quit (windowed). Every 5 s the renderer logs a `STATS` line (fps,
interval, CPU, GPU, preview GPU, late frames, commands, previews, clip misses).

## Gotchas found
- `small` is a macro in the Windows headers (rpcndr.h): never name a member
  `small`.
- The state block is kept alive by any reader (e.g. a running `himmel_ui`), so a
  fresh renderer restores whatever was there — including a `fake_engine` show
  (its clip names do not match, so those layers come back empty; logged).
- `himmel_cmd` continues its `seq` from the engine's last ack for slot 3, so the
  ack check is unambiguous across runs.
- Git Bash turns `/m` into a path: build with `--parallel 8`, not `-- /m`.

## Interfaces touched
None changed. The engine implements `command-protocol.md`, `engine-state.md`,
`preview-block.md` as written (v1). Notes for the LEAD, no change requested:
`PreviewStreamBlock.published` is never reset by the engine (keeps counting
across restarts, readers only compare it); output streams 2–3 are inactive when
there are fewer outputs.
