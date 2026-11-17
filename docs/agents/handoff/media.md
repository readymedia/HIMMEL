# MEDIA handoff
Updated: 2026-11-17 by the MEDIA wave-3 agent (M7) · Branch: `claude/media-m7-ingest` · Last commit: see `git log` (M7 commits on top of main 89d5d1d)

## State in one paragraph
M7 ingest works end to end. `himmel_ingest.exe` watches folders, probes with
ffprobe, converts with ffmpeg.exe (external tool, never linked) to HAP / HAP
Alpha (×4 sizes, chunks = min(cores, 8)), keeps stills as PNG (+ a 1-frame HAP),
rasterises SVG with Direct2D, imports GIF / APNG and numbered image sequences,
stores fps + judder flags, makes thumbnails (+ an 8-frame strip), runs
analysis v1 on a small RGB proxy, writes `meta/<hash>.json` and the SQLite row,
and only then marks the clip READY. The renderer's library feed appends ready
clips to the catalog while the show runs (append-only ids). While a show runs
the import works at idle / background priority and suspends ffmpeg on any late
frame. **Measured: 100 mixed files imported during a 3-layer show, 0 late
frames** (`docs/measurements/2026-11-17-ingest-during-show.md`).

## Done (with commits)
- `docs/interfaces/library-db.md` v1 (library folder, kinds, SQLite schema,
  hash, sidecar `himmel-clip/1`, `Local\HimmelLibrary`, throttling, the feed).
- `src/library/` (`himmel_library`): `record.h`, `hash` (FNV-1a 64, D140),
  `analysis` (v1, D146), `sidecar`, `library_db` (SQLite, WAL, `ready_order`
  cursor), `library_block.h`, `clip_table.h` (append-only playlist + catalog
  entries), `feed` (`LibraryFeed`, `default_library_root`).
- `src/media/ingest/` (`himmel_ingest_core` + `himmel_ingest.exe`): `scan`
  (watch folders, sequences, settle 2 s), `ffmpeg` (find, command lines,
  `run_tool` with idle / I/O priority, suspend / resume, kill job, ffprobe
  JSON), `svg` (Direct2D), `throttle.h`, `ingest_main.cpp` (pipeline, resume
  via `stage_done`, `--once --rebuild --retry --status --mode --canvas
  --seq-fps --quality --watch --library --ffmpeg`).
- Engine hooks (fenced `==== MEDIA (M7) hook ====`): `render_main.cpp` (clip
  table + feed at start-up before the show restore, players share the table,
  catalog append in the frame loop, `late_frames` published every frame,
  `--library DIR` / `--no-library`); `ClipPlayer(device, const ClipTable*)`
  (the vector constructor stays for the legacy renderer); supervisor starts
  `himmel_ingest` (`--no-ingest`; off for `--no-show-files` test runs unless
  `--library`; `--show-root DIR` → `DIR\Library`).
- Tests: `tests/library/library_tests.cpp` (181 checks; one real ffmpeg + SVG
  round trip when ffmpeg is on PATH).
- Tools: `tools/make_ingest_test_set.py` (100 mixed items into
  `build/ingest_test`), `tools/ingest_measure.py` (the M7 measurement).
- Credits: SQLite in `THIRD_PARTY_NOTICES.md` + `licenses/sqlite-PUBLIC-DOMAIN.txt`;
  Direct2D / WIC listed; ffmpeg row updated. Decisions D140–D147.

## In progress
Nothing. Waiting for the LEAD's merge.

## Next up (ordered)
1. MK2 `BROWSE` (build-v1 M7 lists it; CONTROL owns Perform): needs a
   library query path for weapons (filters by mood / colour / NEW / FAV).
2. Commands to the ingest worker (CommandType 60–69): rescan, retry, set stars /
   tags, Prepare Show priority. Then the GUI library view (GUI track) can use them.
3. `library-rules.json` (tunable analysis thresholds) and tuning on the starter library.
4. Natural beats from motion periodicity; bars at the show BPM; hover loops.
5. Hap Q Alpha; "optimise for show" downscale; Prepare Show step 1–2.
6. Catalog beyond 256 clips: a later protocol change (catalog v3 / paging, with the LEAD; LEAD 2026-11-17: not now). Today a full catalog leaves the clip in the library, `LibraryBlock::catalog_full` counts them for the GUI, the renderer logs it once.
7. SHOW's worker keeps the start-up copy of the catalog: a project opened later
   does not see library clips that arrived after start (SHOW / LEAD to decide).

## GUESSES
- GUESS (D146, medium): every analysis threshold in `AnalysisRules`
  (cut_min 0.35, ×3 local median, motion gain 10, loopable ≥ 0.8, crossfade
  < 0.9, flashes warn ≥ 2 / bad > 3 per s, warmth weights, energy / mood
  weights). Verify: run on the starter library and look at the tags.
- GUESS (medium): flicker detection on a 12.5 fps proxy misses flicker faster
  than ~6 Hz; full-speed imports use up to 25 fps. A strobe clip at 30 Hz could
  read "ok". Verify with a known strobe clip; maybe analyse flicker at the
  clip's own rate.
- GUESS (high): SQLite readers in WAL mode never block the writer and vice
  versa; the measurement had the feed polling every second during the import.
- GUESS (medium): a GIF's average frame rate is good enough (variable GIF
  delays are evened out by `-fps_mode cfr`).
- GUESS: the SVG with no size at all is drawn square; Direct2D's SVG subset
  (no text-on-path, filters, CSS animation) is enough for logos.
- GUESS: the 2 s settle time is enough for files copied over a network share.

## Open questions for the owner
- D140–D147 are PROPOSED with the default in use (hash, SQLite, SVG, stills as
  1-frame HAP, library location, throttling, analysis rules, catalog append).
- Is `Documents\Himmel\Library\originals` the right default drop folder?

## How to build / run / test this track
- Build as usual (`cmake --build build --config Release`); SQLite comes via
  FetchContent (amalgamation 3.47.2).
- `build\bin\Release\library_tests.exe` (needs nothing; uses ffmpeg if on PATH).
- `himmel_ingest --library DIR --watch DIR --once` imports a folder and exits;
  `--status`, `--rebuild`, `--retry`.
- `python tools/make_ingest_test_set.py` then `python tools/ingest_measure.py`
  (refuses to run while another Himmel stack runs).

## Gotchas found
- The gyan.dev ffmpeg's mjpeg encoder refuses limited-range YUV from a PNG
  still: thumbnails need `format=yuvj420p`.
- `mandelbrot` (lavfi) has no `duration` option; use `trim`.
- Two byte-identical sources import once (same hash) — expected, but a test set
  needs different content per item.
- The supervisor starts children at HIGH priority; `himmel_ingest` lowers its
  own class at start and runs its work thread in background mode.
- Ids are given at queue time, so the feed's cursor is `ready_order`, not `id`.
- Git Bash turns `cmake --build ... -- /m` into `M:/`; use `--parallel`.

## Interfaces touched
- New: `docs/interfaces/library-db.md` (v1), `Local\HimmelLibrary` (v1).
- `engine-state.md`: one additive note (the catalog grows while running).
- `EngineStatus::late_frames` is now written every frame (same field, same
  meaning; was every 5 s).
- No change to `src/protocol/`.
