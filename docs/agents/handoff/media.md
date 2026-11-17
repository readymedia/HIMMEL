# MEDIA handoff
Updated: 2026-11-17 by the MEDIA wave-6 agent (CLI fixes) · Branch: `claude/media-cli-fixes` (from main 93b8640) · Green: build, test programs, himmel_ingest --selftest; e2e pending (LEAD runs it; agents run no stack this wave)

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

**Wave 5:** the worker takes commands 60–67 (Import, Rescan, Remove, Relink,
Reanalyse, Retry, Mode, Unwatch) from `Local\HimmelLibrary` v2
(`library-db.md` §10, D148–D149), each with one reply. The GUI builds its
library buttons on that contract (pushed first, commit 0e6375e).

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

### Wave 5 (2026-11-17)
- Contract first: `library-db.md` v2 §10 + `library_block.h` (`IngestCommand`
  1024 B, `IngestReply` 128 B, `IngestChannel` per producer, `make_ingest` /
  `push_ingest`), commit 0e6375e, pushed early for GUI.
- `src/media/ingest/ingest_ops.{h,cpp}`: `IngestOps` (each command, the
  enqueue moved here, watch list in `meta.watch`, pending deletes in
  `meta.pending_delete`), `CommandPump` (take / reply / recover), `touches`
  (a command for the running clip waits; Remove / forced Relink cancel it).
- Library: `State::Removed` (tombstone), `source_missing` + `removed` in the
  sidecar (no schema change), `ready_order` kept once given, `meta_get/set`,
  `source_info`, `--rebuild` keeps tombstones. Scan helper `candidate_from_path`.
- `himmel_ingest`: commands polled between files and from run_tool's poll
  (every 50 ms during a conversion), Mode = auto / full / background / hold,
  `--selftest` (temp library under %TEMP%, in-memory block, no ffmpeg, 56
  checks), `--send VERB ...` (one command as the Tool producer, prints the reply).
- Tests: `library_tests` runs the self-test + DB v2 rules (252 checks).
- Checked live on a temp library (no stack running): every verb through
  `--send` against the real worker; Remove during a 60 s 1080p conversion
  cancelled it in 0.1 s and cleaned `work/`; reanalysis kept `ready_order`
  and `ready_unix`; a removed clip came back with its id on Import.

## In progress
Nothing. Waiting for the LEAD's merge.

## Next up (ordered)
1. MK2 `BROWSE` (build-v1 M7 lists it; CONTROL owns Perform): needs a
   library query path for weapons (filters by mood / colour / NEW / FAV).
2. Commands 68–69: set stars / tags, Prepare Show priority (the GUI library
   view uses 60–67 now). The GUI bridge's clip list should hide `removed` rows
   and show a badge for `source_missing` (GUI track).
3. `library-rules.json` (tunable analysis thresholds) and tuning on the starter library.
4. Natural beats from motion periodicity; bars at the show BPM; hover loops.
5. Hap Q Alpha; "optimise for show" downscale; Prepare Show step 1–2.
6. Catalog beyond 256 clips: a later protocol change (catalog v3 / paging, with the LEAD; LEAD 2026-11-17: not now). Today a full catalog leaves the clip in the library, `LibraryBlock::catalog_full` counts them for the GUI, the renderer logs it once.
7. SHOW's worker keeps the start-up copy of the catalog: a project opened later
   does not see library clips that arrived after start (SHOW / LEAD to decide).

## GUESSES
- GUESS (medium): relink-folder search caps at 16 hash checks per missing
  clip (name matches first, then same size). Big folders of same-size files
  could miss; raise the cap if the owner's media hits it.
- GUESS (high): a HAP file the engine has open cannot be deleted (the clip
  player opens with FILE_SHARE_READ only), so Remove leaves it in
  `meta.pending_delete` and the next scan (every 2 s) retries. Checked with an
  open handle in the self-test, not with a running renderer.
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
- `himmel_ingest --selftest` (no files under Documents, no ffmpeg).
- `himmel_ingest --send import PATH [watch] | rescan [ID] | remove ID |
  relink ID|0 PATH [force] | reanalyse [ID] [thumbs] | retry [ID] |
  mode auto|full|background|hold | unwatch PATH` talks to the running worker.
- `python tools/make_ingest_test_set.py` then `python tools/ingest_measure.py`
  (refuses to run while another Himmel stack runs).
- `himmel_ingest --help` prints the usage. An unknown or incomplete argument
  (`--library` with no folder) prints it too and exits 2 before anything is
  opened, created or started (wave 6; `library_tests` checks it).

### Recipe: the whole of Himmel on a temp library (GUI library buttons, live)
Never with the owner's library, autosave or show files. **One stack per machine:**
the `Local\Himmel*` blocks are shared, so first check that `tasklist` shows no
`himmel*` process. Never start a second stack next to the owner's.

```bat
rem 1. A temp root with a few test clips in its drop folder (cmd.exe)
set T=%TEMP%\himmel_libtest
python tools\make_ingest_test_set.py --out %T%\Library\originals --count 8
rem    (under 10: one item of each kind, 7 in all: video, alpha, hap, still, gif, svg, sequence; needs ffmpeg on PATH)

rem 2. Start the stack on it: everything after "--" goes to the renderer, and the
rem    supervisor passes the same library to himmel_ingest and himmel_ui
build\bin\Release\himmel.exe --sim -- --show-root %T% --size 960x540
```

- `--show-root %T%` puts **both** the library (`%T%\Library`: DB, `hap\`,
  `meta\`, `thumbs\`) **and** the show / project / autosave files under `%T%`,
  so nothing of the owner's is read or written. `-- --library DIR` alone moves
  only the library (shows would still use the default folder: pair it with
  `--no-show-files`).
- The 8 items import in the background while the show runs (`himmel_ingest`
  log: `%T%\Library\ingest.log`). They appear in the GUI library and in the
  engine catalog as they turn READY.
- GUI library buttons = commands 60–67. To compare with the GUI, the same
  commands can be sent by hand to the running worker:
  `build\bin\Release\himmel_ingest.exe --send rescan`, `--send remove 3`,
  `--send import %TEMP%\some_folder watch`, `--send relink 0 %TEMP%\moved` …
  (`--send` alone lists the verbs). Each prints the reply.
- Quick checks: `himmel_ingest --library %T%\Library --status` (counts);
  `sqlite3`/Python on `%T%\Library\himmel-library.db` (`state` column).
- When done: stop the stack the normal way (Ctrl+C in its console / close the
  supervisor window), then `rmdir /s /q %T%`.

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
- The Bash tool's heredocs halve backslashes (`\\n` arrives as a newline):
  write patch scripts to a file first.
- mk2_e2e's first run of the day failed 21 checks ("the output shows a picture
  to black out", mean brightness 0); the next two runs passed. Unrelated to the
  library (e2e uses `--no-show-files`, no ingest).

## Interfaces touched
- New: `docs/interfaces/library-db.md` (v1), `Local\HimmelLibrary` (v1).
- Wave 5: `library-db.md` v2 / `Local\HimmelLibrary` v2 (channels appended, block
  size grows; `IngestMode::Hold` = 4 appended; `State::Removed` appended). No
  change to `src/protocol/` or `Local\HimmelState`.
- `engine-state.md`: one additive note (the catalog grows while running).
- `EngineStatus::late_frames` is now written every frame (same field, same
  meaning; was every 5 s).
- No change to `src/protocol/`.
