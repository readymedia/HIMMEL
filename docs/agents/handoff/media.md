# MEDIA handoff
Updated: 2026-11-17 by the MEDIA wave-7 agent (import safety + options) · Branch: `claude/media-w7-import` (from main 522c0c7) · Green: build, 19 test programs, every selftest except `nuvj_sim.py` (not touched; fails here while another stack runs); `mk2_e2e.py` pending (another agent's stack was running)

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

**Wave 7 (2026-11-17): the import is safe on a full disk and has options.**
The owner's 89-clip import nearly filled C: (7.3 GB free, ~17 GB needed). Now:
a disk guard (estimate before queueing, reserve kept free, refuse with a
plain message, pause "disk low" while running, never a half file), the mode
is stored (Hold survives a restart), Clear queue (68), progress / ETA /
bytes / plain errors in the block, import options (720p, HAP Q, frame-rate
conform, skip known, a selection list, dry run, fit) and the library
location + move (69). Contract: `library-db.md` §11, block **v3**
(commit 131a896, pushed first for GUI). D180–D184 PROPOSED.

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

### Wave 7 (2026-11-17)
- 131a896 contract: `library_block.h` v3 (appended: `stored_mode`, `disk_*`,
  `library_bytes`, `queue_bytes`, `progress`, `plan`, `root`), commands 68
  `ClearQueue` / 69 `Library`, Import flags (HapQ, SkipKnown, List, DryRun,
  Fit, Move), `IngestCommand::reserved` -> `conform_hz`, statuses 10 NoSpace /
  11 Busy; `library-db.md` §11; D180–D184.
- 20691b2 + the next commit: the code.
  - `src/media/ingest/guard.{h,cpp}`: estimate (HAP 0.5, HAP Q / Alpha 1 byte
    per px per frame + 64 KB), `reserve_for` (clamp(5 %, 2 GB, 20 GB); was max(10 GB, 10 %)), `fits`,
    `output_shape` (max height, never up, multiple of 4, conform), `conform_fps`,
    `plain_error`, `BatchProgress` (n of m, ETA), `file_progress`.
  - `library_move.{h,cpp}`: plan / copy (CopyFileExW with progress + cancel,
    size check, undo) / delete the old copy (never `originals/`).
  - `src/library/location.{h,cpp}`: `Documents\Himmel\library-location.json`,
    `himmel-library-moved.json` redirect, `resolve_library_root`;
    `default_library_root()` reads the saved location; the feed follows
    `LibraryBlock::root` and resolves redirects; DB `meta.uid` (move vs switch).
  - `ingest_ops`: Import with options / list / dry / fit / skip and the plan;
    `enqueue_scanned` (the scan's guard); `queue_need`; `clear_queue`; stored
    mode; `Busy` while a library job runs.
  - `ingest_main`: guard before each file and every 0.5 s during a conversion
    (stop, delete `work/`, back to queued, `disk_low`); ffmpeg `-progress` for
    % of the file; plain errors in the `error` column (tech detail in the log);
    Library switch / move between files; old-copy cleanup once the renderer that
    loaded it is gone; `--reserve-gb N`; `--send` verbs `import ... dry fit skip
    hapq 720|max=H conform50|conform60`, `import-list FILE`, `clear`,
    `library DIR [move] [force]`, `progress`; test-only `HIMMEL_LIBRARY_SHM`.
- Tests: `himmel_ingest --selftest` v3 section (fake small disk 100 GB / 15 GB
  free: refusal, plan numbers, dry run, fit, queue counted, scan guard, unknown
  disk; stored mode across a reopen; Clear queue; skip known; options stored;
  list file; Busy); `library_tests` 402 checks (guard maths, conform, plain
  errors, ETA, `-progress` parser, convert args, sidecar options, location,
  move copy / cancel / redirect / delete old).
- Checked live on a temp library with its own block (another agent's stack was
  running; nothing of the owner's touched): `media/test` + 1080p30 h264 at 720
  (-> 1280x720) + 1080p24 h264 with conform60 (-> 60 fps, 180 frames), progress
  "5 of 6 ... 85 %, ETA 3 s"; the default reserve refused everything (see
  GUESSES); Hold survived a worker restart; a reserve just above free space ->
  "disk low: ... c1 needs ~93 MB - the import waits"; Clear queue cleared 3; a
  random-bytes .mp4 -> "This file is damaged or is not a video ffmpeg can
  read."; move 196 MB to another folder with progress, redirect written, the
  old copy kept while a renderer runs.

## In progress
Nothing. Waiting for the LEAD's merge (and the ENGINE answer on D181).

## Next up (ordered)
1. MK2 `BROWSE` (build-v1 M7 lists it; CONTROL owns Perform): needs a
   library query path for weapons (filters by mood / colour / NEW / FAV).
0. Wave 7 follow-ups: the GUI shows the v3 fields (disk bar, plan dialog,
   Clear queue, options, Move library); the GUI bridge follows `root`; the
   reserve default (D180) and judder (D181) once decided; a reserve setting
   the GUI can change (needs a command number; MEDIA's 60–69 are used up).
2. Stars / tags and Prepare Show priority (68–69 are now ClearQueue / Library,
   so these need new numbers from the LEAD) (the GUI library
   view uses 60–67 now). The GUI bridge's clip list should hide `removed` rows
   and show a badge for `source_missing` (GUI track).
3. `library-rules.json` (tunable analysis thresholds) and tuning on the starter library.
4. Natural beats from motion periodicity; bars at the show BPM; hover loops.
5. Hap Q Alpha; "optimise for show" downscale; Prepare Show step 1–2.
6. Catalog beyond 256 clips: a later protocol change (catalog v3 / paging, with the LEAD; LEAD 2026-11-17: not now). Today a full catalog leaves the clip in the library, `LibraryBlock::catalog_full` counts them for the GUI, the renderer logs it once.
7. SHOW's worker keeps the start-up copy of the catalog: a project opened later
   does not see library clips that arrived after start (SHOW / LEAD to decide).

## GUESSES
- **D180 (owner has the question):** the first rule, max(10 GB, 10 %), was
  102 GB on the owner's 1 TB C: and refused every import. The LEAD set the
  interim rule clamp(5 %, 2 GB, 20 GB) (20 GB there) plus `SetReserve` (100,
  stored in the DB; `--send reserve GB`), `--reserve-gb` for one run.
- GUESS (high): HAP ≈ 0.5 byte / px is on the safe side: the owner's 14 clips
  are 0.33 byte / px (3.0 GB for 14 × 1080p × 313 frames). HAP Q / Alpha
  1 byte / px likewise. Noisy footage compresses less (Snappy).
- GUESS (high): an unknown disk (GetDiskFreeSpaceEx fails, e.g. some network
  shares) is not guarded; the import goes on.
- GUESS (medium): the old copy after a move is deleted only once the renderer
  that ran at move time has a new pid or is gone; if the renderer never loaded
  that library (it ran on another one) the copy still waits. Harmless, it
  only delays freeing the space.
- GUESS (medium): the frame-rate conform uses ffmpeg's `framerate` filter
  (linear blend). Not judged on a projector yet; ENGINE may prefer a playback
  blend (D181).
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
- D180: is clamp(5 % of the disk, 2 GB, 20 GB) the right default reserve?
- D181 is settled by the LEAD: ENGINE's playback blend by default; the import
  conform stays an option, off.
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
- The renderer's feed must not follow a library block that belongs to another
  library (another agent's stack): `library_tests`' feed test failed that way.
  The feed now follows only a worker first seen on its own library.
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
- Wave 7: `library-db.md` §11 / `Local\HimmelLibrary` **v3** (appended after
  `channels`; old fields unchanged); `IngestCommand::reserved` renamed
  `conform_hz` (0 = off, what old producers send). New file
  `Documents\Himmel\library-location.json`. GUI bridge: should follow
  `LibraryBlock::root` for the DB / thumbnails (told the LEAD). No change to
  `src/protocol/`.
- New: `docs/interfaces/library-db.md` (v1), `Local\HimmelLibrary` (v1).
- Wave 5: `library-db.md` v2 / `Local\HimmelLibrary` v2 (channels appended, block
  size grows; `IngestMode::Hold` = 4 appended; `State::Removed` appended). No
  change to `src/protocol/` or `Local\HimmelState`.
- `engine-state.md`: one additive note (the catalog grows while running).
- `EngineStatus::late_frames` is now written every frame (same field, same
  meaning; was every 5 s).
- No change to `src/protocol/`.
