# Library DB, sidecars and the library block

Status: **version 1, written by MEDIA 2026-11-17 (M7)**, before the code.
**Version 2 (2026-11-17, wave 5): commands to the ingest worker (§10)**, the
`removed` state, `source_missing`, stored watch folders. All additive.
Writers: `himmel_ingest.exe` only. Readers: the render process (its library
feed worker), the GUI bridge, SHOW (relink by name), tools. Change rules as
in [`command-protocol.md`](command-protocol.md): append-only; a layout or
meaning change bumps the version. Headers (source of truth for layouts):
`src/library/include/himmel/library/` (`library_db.h`, `sidecar.h`,
`library_block.h`). Design: [`design/library-and-analysis.md`](../design/library-and-analysis.md),
[`design/operations.md`](../design/operations.md) §3–4.

## 1. The library folder

```
<library>/                     default: Documents\Himmel\Library\  (--library DIR)
  himmel-library.db            SQLite index (WAL); rebuildable from meta/
  originals/                   the default watch folder (drop files here)
  hap/<hash>.mov               what the engine plays (HAP / HAP Alpha / HAP Q)
  stills/<hash>.png            stills and rasterised SVGs, kept as images
  meta/<hash>.json             sidecar: probe + analysis + tags (himmel-clip/1)
  thumbs/<hash>.jpg            thumbnail (320 px wide)
  thumbs/<hash>-strip.jpg      strip of 8 frames (8 × 160 px)
  work/                        temp output; a file here is never "ready"
```

- More watch folders: `himmel_ingest --watch DIR` (repeatable). Originals
  are **never** changed, moved or deleted (design §1.3).
- `<hash>` = 16 lowercase hex digits, see §4. Moving or renaming an original
  never triggers a new import: the hash finds the existing entry and only
  `source` is updated.
- Tests and tools pass `--library <temp dir>`, so they never touch the
  owner's library.

## 2. What gets imported

| Input | Kind | Result | Notes |
|---|---|---|---|
| `.mov .mp4 .m4v .mkv .avi .webm .mxf .mpg .wmv` | `video` | `hap/<hash>.mov` | HAP if no alpha, HAP Alpha if the pixel format has alpha. A source that already is HAP with a size divisible by 4 is copied, not re-encoded (still analysed) |
| `.png .jpg .jpeg .webp .tif .tiff .bmp` | `still` | `stills/<hash>.png` + a 1-frame `hap/<hash>.mov` | scaled down to fit 2 × the canvas (default canvas 1920 × 1080, so ≤ 3840 × 2160); HAP Alpha when the PNG has alpha |
| `.svg` | `vector` | as a still | rasterised with Windows Direct2D at 2 × canvas height (D142) |
| `.gif .apng` (and animated `.webp`, see §9) | `animation` | `hap/<hash>.mov`, HAP Alpha | GIF frame timing → fps rounded to the GIF's average |
| a folder of ≥ 2 numbered `.png .jpg .tga .tif` | `sequence` | `hap/<hash>.mov` | 25 fps (`--seq-fps N`); HAP Alpha when the PNGs have alpha; the hash is over the folder's sorted names + sizes + first / last file samples |

- Sizes are rounded **down** to a multiple of 4 (scale, not crop); chunks =
  `min(cores, 8)`; no audio. ffmpeg / ffprobe are external tools found on
  `PATH` or with `--ffmpeg DIR`; they are never linked (D-2026-11-10).
- A file is picked up when its size and time stamp did not change for 2 s
  (files still being copied are left alone).

## 3. The SQLite schema (`PRAGMA user_version = 1`)

Opened in WAL mode with `busy_timeout = 2000`, so readers never block the
writer. Only `himmel_ingest` writes.

```sql
CREATE TABLE clips (
  id            INTEGER PRIMARY KEY AUTOINCREMENT, -- library id: stable, never reused
  hash          TEXT NOT NULL UNIQUE,              -- 16 hex (§4)
  kind          TEXT NOT NULL,       -- video | still | vector | animation | sequence
  name          TEXT NOT NULL,       -- display name (file stem / folder name), unique in the library
  source        TEXT NOT NULL,       -- absolute UTF-8 path of the original (last seen)
  state         TEXT NOT NULL,       -- queued | probing | converting | analysing | ready | failed | removed (v2)
  stage_done    INTEGER NOT NULL DEFAULT 0,  -- bitmask: 1 probe, 2 convert, 4 analyse, 8 thumbs (resume)
  error         TEXT NOT NULL DEFAULT '',
  play_path     TEXT NOT NULL DEFAULT '',    -- relative to the library: hap/<hash>.mov
  still_path    TEXT NOT NULL DEFAULT '',    -- stills/<hash>.png (stills / vectors)
  thumb_path    TEXT NOT NULL DEFAULT '',
  src_codec     TEXT NOT NULL DEFAULT '',
  codec         TEXT NOT NULL DEFAULT '',    -- hap | hap_alpha | hap_q
  width INTEGER, height INTEGER,             -- of the HAP file
  fps REAL, frames INTEGER, seconds REAL,
  alpha INTEGER NOT NULL DEFAULT 0,
  judder TEXT NOT NULL DEFAULT '',           -- '' | '50' | '60' | '50,60': refreshes it judders on
  avg_rgb INTEGER, brightness REAL, contrast REAL, saturation REAL, warmth REAL,
  motion REAL, motion_dir REAL, steadiness REAL,
  cuts_per_min REAL, energy INTEGER,
  mood_chill REAL, mood_groove REAL, mood_techno REAL, mood_crazy REAL,
  loop_quality REAL, loop_in INTEGER, loop_out INTEGER, loop_score REAL,
  loop_style TEXT, natural_beats INTEGER, flicker TEXT, black_frames INTEGER,
  palette TEXT NOT NULL DEFAULT '[]',        -- JSON [[rgb as an integer 0xRRGGBB, weight], ...] up to 5
  slices  TEXT NOT NULL DEFAULT '[]',        -- JSON [frame, ...] up to 16
  analysis_version INTEGER NOT NULL DEFAULT 0,
  added_unix INTEGER NOT NULL, ready_unix INTEGER NOT NULL DEFAULT 0,
  stars INTEGER NOT NULL DEFAULT 0, favourite INTEGER NOT NULL DEFAULT 0,
  user_tags TEXT NOT NULL DEFAULT '[]',
  sidecar TEXT NOT NULL DEFAULT '',          -- the full sidecar JSON (motion curve, cuts, probe)
  ready_order INTEGER NOT NULL DEFAULT 0     -- 1, 2, 3 ... in the order rows FIRST turned ready; 0 = never ready.
                                             -- v2: kept while a ready row is re-analysed or removed (§10)
);
CREATE INDEX clips_state ON clips(state, id);
CREATE INDEX clips_ready ON clips(ready_order);
CREATE TABLE sources (       -- path -> hash cache, so a rescan does not re-hash
  path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, hash TEXT
);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);  -- 'schema', 'ready_seq', v2: 'watch' (JSON array of UTF-8 folders)
```

- **Ready means complete.** A row turns `ready` only after the HAP file,
  the thumbnail and the sidecar are on disk under their final names (written
  to `work/`, then renamed). Readers show only `ready` rows (plus the queue
  for the import view); a half-ready clip is never offered.
- `name` is made unique when the row is created (`Sunset`, `Sunset (2)`…),
  because shows find clips by name (`show-files.md` §2).
- `failed` rows keep `error` (one line) and are retried when the source
  changes or on `himmel_ingest --retry`.
- **Rebuild:** `himmel_ingest --rebuild` deletes the DB and recreates it from
  `meta/*.json` (every sidecar whose `hap/` file exists is `ready`; v2: a
  sidecar with `"removed": true` becomes a `removed` row).
- v2: `removed` rows are tombstones: the clip is out of the library, its
  derived files are deleted, and the row keeps the hash so the watch-folder
  scan does not import the same original again. **Readers hide `removed`
  rows.** An explicit Import of the same file brings it back (same id).
- v2: `source_missing` (in the sidecar JSON, so no new column; the schema
  stays `user_version = 1`) means the original is gone. A `ready` clip with a
  missing original still plays, because the HAP file is in the library. The
  GUI shows an "original missing" badge and offers Relink.

## 4. The content hash

`FNV-1a 64` over: the file size (8 bytes, little endian), then three 1 MB
samples at offsets 0, (size − 1 MB) / 2 and size − 1 MB (the whole file when
it is under 3 MB). Fast (~3 ms per file) and stable across renames (D140). It is
an identity hint: the `sources` cache (path, size, mtime) decides when a file
is hashed again.

## 5. The sidecar `meta/<hash>.json` (`"format": "himmel-clip/1"`)

The sidecar is the source of truth for one clip; the DB row is a copy of it
plus user fields. Readers ignore keys they do not know.

```json
{
  "format": "himmel-clip/1",
  "hash": "9f2c0a1b33d4e5f6", "kind": "video", "name": "Sunset",
  "source": "C:\\Users\\me\\Videos\\sunset.mp4",
  "play": "hap/9f2c0a1b33d4e5f6.mov", "still": "", "thumb": "thumbs/9f2c0a1b33d4e5f6.jpg",
  "probe": {"codec": "h264", "width": 1920, "height": 1080, "fps": 25.0,
            "frames": 750, "seconds": 30.0, "alpha": false},
  "hap": {"codec": "hap", "width": 1920, "height": 1080, "chunks": 8},
  "judder": [50],
  "analysis": {
    "version": 1,
    "avg_rgb": "#d0703a", "brightness": 0.55, "contrast": 0.31, "saturation": 0.62,
    "warmth": 0.7,
    "palette": [["#e07a3a", 0.34], ["#2a1a40", 0.22], ...],
    "motion": 0.18, "motion_dir": 90.0, "steadiness": 0.8,
    "motion_curve": [0.1, 0.12, ...],
    "cuts": [212, 480], "cuts_per_min": 4.0,
    "energy": 2, "mood": {"chill": 0.7, "groove": 0.4, "techno": 0.1, "crazy": 0.0},
    "loop": {"quality": 0.42, "in": 3, "out": 744, "score": 0.91, "style": "loopable",
             "needs_crossfade": true},
    "slices": [0, 47, 94, ...], "natural_beats": 16, "bars_at_120": 4,
    "flicker": "ok", "black_frames": 0,
    "proxy": {"width": 64, "height": 36, "fps": 12.5, "frames": 375}
  },
  "added": "2026-11-17T14:02:11Z", "ready": "2026-11-17T14:02:40Z",
  "source_missing": false, "removed": false
}
```

`source_missing` and `removed` are v2 keys (absent = false).

Units: colours `#rrggbb`; 0..1 unless said; `motion_dir` degrees (0 = right,
90 = up); frame numbers are frames of the **HAP file**; `motion_curve` is one
value per proxy frame (`proxy.fps`). The rules and thresholds that write
`energy`, `mood`, `loop.style` and `flicker` are in
`src/library/analysis.cpp` (`AnalysisRules`, defaults listed in the handoff);
`library-rules.json` will make them tunable (later).

## 6. The library block (`Local\HimmelLibrary`, magic `HMLB`, version 2)

Written by `himmel_ingest` (creates it; header last), read by anyone.
Plain atomics plus one seqlock section.

| Field | Meaning |
|---|---|
| `pid`, `heartbeat_us` | the ingest worker is alive (supervisor timeout 2 s) |
| `stop_request` | supervisor → ingest: 1 = finish the current file write and exit |
| `ready_seq` | bumps every time a row turns `ready` (or a ready row's name / path changes; v2: also after every command that changed rows). The engine's feed and the GUI re-read the DB when it changes |
| `mode` | 0 idle, 1 full speed (no show), 2 background (show running), 3 high (Prepare Show, later), 4 hold (v2: command `Mode` = Hold) |
| `paused` | 1 while throttled (§7) |
| `queued`, `working`, `ready_total`, `failed_total` | counts |
| `job` (seqlock) | the current file: name, stage, progress 0..1, `last_error` |
| `catalog_full` | written by the renderer's feed (its only field): ready clips left out of the catalog because it holds 256 (`kMaxClips`). They stay in the library; the GUI shows "catalog full"; the renderer logs it once |

## 7. Throttling (show-safe)

`himmel_ingest` watches `Local\HimmelEngine`. **Show running** = its
heartbeat is younger than 1 s. Then:
- the worker and its ffmpeg run at idle CPU priority and background I/O
  priority, one conversion at a time, ffmpeg `-threads 2`;
- analysis uses a 64 × 36 proxy at ≤ 12.5 fps (full speed: 96 × 54 at the
  clip's fps up to 25);
- **pause** (ffmpeg suspended, analysis waits) when the renderer's
  `late_frames` rose in the last second or its average frame time is above
  80 % of the frame budget (1 / `fps`); resume after 2 s without either.

With no show running the import uses every core (normal priority).

## 8. How the engine sees new clips (catalog refresh)

- The render process starts a **library feed** worker (`himmel_library`,
  `LibraryFeed`), never the frame loop. It opens the DB **read-only** and
  every second (or when `ready_seq` changes) reads `ready` rows with a
  `ready_order` above the last one it saw (ids are given at queue time, so a
  clip queued early can turn ready late), and appends them to a fixed table
  (capacity `kMaxCatalogClips` = 8192 since ENGINE wave 7, was `kMaxClips` =
  256; preallocated). A row whose library id and HAP path are already in the
  table (it turned ready again) is not appended twice. The catalog entry comes from the row
  (size, fps, length, codec; the placeholder colour is the clip's average
  colour); the feed never opens the HAP file.
- The frame loop sees the published count rise (an atomic), copies the new
  `ClipInfo`s into its catalog and writes the catalog section once. **Ids are
  append-only**: existing clip ids never move, so commands and `LiveState`
  stay valid (`engine-state.md` §2). A library clip whose name is taken by a
  `media/` clip gets ` (lib)` appended.
- The same table is the clip players' playlist, so loading a new id works at
  once. The frame loop never reads the DB or a file.
- `--no-library` turns the feed off; `--library DIR` picks the folder; with
  `--no-show-files` (test runs) it is off unless `--library` is given, and
  `--show-root DIR` means `DIR\Library` (D144).
- At start-up the renderer reads the ready library clips **before** it
  restores the show from a surviving state block, so a restarted renderer
  finds its library clips by name.
- Readers of `Local\HimmelState` must re-read the catalog whenever
  `catalog_seq` changes; it now grows while the show runs.
- **Wave 7 (catalog v3):** clips past id 255 are in the state block's clip
  pages (`engine-state.md` version 4), with their library id; the GUI and
  the MK2 load a library clip with `LayerClipLib` (command 75, by library id)
  and need no catalog index at all. The renderer's file watch marks a clip
  whose HAP file was deleted (`Remove`) as `Missing`.

## 9. Not in version 1 (append later)

- Hap Q Alpha, "optimise for show" downscale, keeping audio as a side file.
- Animated WebP: the bundled ffmpeg builds often cannot decode it; such a
  file imports as a still (first frame) and the log says so.
- Type tags / similarity (ONNX), tracking sidecars, generators' 10 s render.
- A `library-rules.json` file for the analysis thresholds.
- Hover loops (`thumbs/<hash>.webm`).
- Commands 68–69 (set stars / tags, Prepare Show priority) and MK2 `BROWSE`.
- A paged catalog beyond 256 clips (catalog v3, with the LEAD).

## 10. Commands to the ingest worker (version 2, CommandType 60–69)

The GUI, tools and later OSC can ask the ingest worker to import, rescan,
remove, relink and re-analyse. These commands use MEDIA's CommandType range
60–69, but they travel **in the library block**, not in
`Local\HimmelCommands`. The ingest worker is their only consumer, and paths
do not fit in a 32-byte `Command`. The engine still refuses 60–69 in its own
rings (`UnknownType`).
Header (source of truth): `src/library/include/himmel/library/library_block.h`.

### 10.1 Transport

`LibraryBlock::channels[8]` holds one `IngestChannel` per producer slot. The
slots are numbered like `protocol::Producer`: Perform 0, **Gui 1**, Osc 2,
**Tool 3**, Show 4. Exactly one process writes a slot's `commands` ring.

| Field | Writer | Meaning |
|---|---|---|
| `pid`, `heartbeat_us` | producer | who uses the slot, and when it last pushed |
| `commands` | producer → ingest | SPSC ring of 16 `IngestCommand` (1024 bytes each). When it is full, `push` returns false (resend later) |
| `replies` | ingest → producer | SPSC ring of 32 `IngestReply` (128 bytes each). When it is full, the reply is dropped and counted; `done_seq` still moves |
| `applying_seq` | ingest | the seq being applied, 0 = none |
| `done_seq` | ingest | the last seq replied to |
| `rejected` | ingest | replies with status ≥ `UnknownType` |

- `himmel_ingest` creates the block. **No ingest running = no block**, or a
  stale block with an old `heartbeat_us`. The GUI greys out the library
  buttons when the heartbeat is older than 2 s.
- Producers use `make_ingest()` + `push_ingest()` from the header (no
  allocation). `seq` runs 1, 2, 3 … per producer.
- The worker drains every ring every 100 ms. `Mode` is applied at once. The
  other commands are applied by the worker between files. A running
  conversion is never cut, except by a `Remove` or `Relink` of that very
  clip, which cancels it.
- **Restart safety:** each command is popped, then `applying_seq` is set,
  then the command is applied, then the reply is sent. If the worker dies in
  between, the next worker finds `applying_seq != 0`, replies `Interrupted`
  for that seq, and does not apply it again. So a poison command cannot
  crash-loop the worker. Commands still in the ring when the worker restarts
  are applied.
- If the block layout does not match (an older worker), the producer must
  not write. Check `header.magic`, `header.version == 2` and `header.size`.

### 10.2 `IngestCommand` (1024 bytes)

| Field | Type | Meaning |
|---|---|---|
| `type` | u16 | `IngestCommandType` (60–67) |
| `flags` | u16 | `kIngestWatch` 1, `kIngestForce` 2, `kIngestThumbs` 4 |
| `seq` | u32 | producer's running number; echoed in the reply |
| `clip` | i64 | **library id**: `clips.id` in the DB, not the engine catalog index. 0 = all / none |
| `i` | i32 | integer argument (`Mode`) |
| `path` | char[1000] | UTF-8, NUL-terminated, absolute; "" when unused |

### 10.3 The commands

| # | Name | Arguments | What it does | Reply |
|---|---|---|---|---|
| 60 | `Import` | `path` = a file or a folder; `kIngestWatch` | Hashes and queues every importable thing under `path`, recursively (an image sequence becomes one clip). The originals stay where they are: they are referenced, never copied or changed. Content already in the library is not imported again, but a `removed` clip with that content comes back with the same id. With `kIngestWatch` (folders only), the folder is stored in `meta.watch` and scanned every 2 s from then on, also after a restart. | `Accepted`, `count` = rows queued (0 = all already known), `clip` = the first one; `BadPath` |
| 61 | `Rescan` | `clip` = id, or 0 | With `clip` = 0: scan every watch folder now (new files still wait out the 2 s settle) and check every clip's original. With `clip` = id: check that one original. If the original is gone, `source_missing` is set (a ready clip keeps playing); if it is found again, the flag is cleared. If the same path now holds **other content**, it is imported as a new clip, and the old clip keeps its files. | `Ok`, `count` = rows whose `source_missing` changed, plus rows queued; `BadId` |
| 62 | `Remove` | `clip` = id | The clip leaves the library: the row turns `removed` (a tombstone), the sidecar keeps `"removed": true`, and the HAP file, still and thumbnails are deleted. **The original is never touched.** A running import of that clip is cancelled. If the engine still has the HAP file open (a layer is playing it), the file cannot be deleted yet; the delete is retried on every rescan and at start-up, and the layer keeps playing until it lets go. The clip stays in the engine catalog until the engine restarts (append-only, §8); loading it after that shows "missing". | `Ok`, `count` 1; `BadId` (no such clip, or already removed) |
| 63 | `Relink` | `clip` = id + `path` = a file; or `clip` = 0 + `path` = a folder; `kIngestForce` | **One clip:** hashes `path`. Same content → `source` = `path` and `source_missing` is cleared; nothing is imported again. Other content → `Mismatch`, unless `kIngestForce` is set: then the clip is imported again from `path` under the same id and name, with a new hash, a new HAP file and a new `ready_order`, so the engine catalog gets a new entry and the old entry shows "missing". **A folder:** for every clip with `source_missing`, searches the folder recursively for a file with the same name, then the same size, and relinks it when the hash matches ("Find missing clips", projects.md §4). | `Ok`, `count` = clips relinked; `Mismatch`; `Duplicate` (the forced content already is clip `clip`); `BadId`, `BadPath` |
| 64 | `Reanalyse` | `clip` = id, or 0 = every ready clip; `kIngestThumbs` | Runs the analysis again, for example after new rules. The clip **stays playable** and keeps its `ready_order`, so there is no duplicate catalog entry. Meanwhile the row shows `analysing`; then it turns `ready` with the new tags and `ready_seq` bumps. The HAP file is not converted again. | `Accepted`, `count` = rows queued; `BadId` |
| 65 | `Retry` | `clip` = id, or 0 = every failed clip | Queues a `failed` clip again from the start. | `Accepted`, `count`; `BadId` (not failed) |
| 66 | `Mode` | `i` = `IngestModeRequest`: 0 auto, 1 full, 2 background, 3 hold | Auto (the default) = background while a show runs. Full = full speed even during a show; the GUI should warn that this is not show-safe. Hold = start no new work and suspend the running ffmpeg; any other mode resumes. The mode is not stored, so a restarted worker is in Auto. `LibraryBlock::mode` shows the result (4 = hold). | `Ok`; `BadValue` |
| 67 | `Unwatch` | `path` = a folder | Removes a folder that was added with `Import` + `kIngestWatch`. Its clips stay in the library. The default `originals/` folder and `--watch` folders cannot be removed. | `Ok`; `BadPath` (not in the list) |
| 68–69 | free | | (stars / tags, Prepare Show priority) | |

### 10.4 `IngestReply` (128 bytes)

| Field | Meaning |
|---|---|
| `seq`, `type` | the command's |
| `status` | `IngestStatus`: 0 `Ok` (done), 1 `Accepted` (queued; follow the rows in the DB and `job`), 2 `UnknownType`, 3 `BadId`, 4 `BadPath`, 5 `Mismatch`, 6 `Duplicate`, 7 `Failed`, 8 `Interrupted`, 9 `BadValue` |
| `count` | rows changed / queued / relinked |
| `clip` | the clip it is about (`Duplicate`: the clip that already has this content) |
| `message` | one English line for a toast and the log, e.g. `relinked 3 of 5 missing clips` |

Every command gets exactly one reply. After a command that changed rows,
`ready_seq` bumps, so a GUI that re-reads the DB on `ready_seq` sees the result.

### 10.5 Testing

`himmel_ingest --selftest` runs every command against a temporary library
under `%TEMP%` (deleted afterwards; never `Documents`). It uses an in-memory
block (not the shared one), needs no ffmpeg, and prints `SELFTEST PASS`. `library_tests.exe`
covers the command logic (`ingest_ops`) for each command.
