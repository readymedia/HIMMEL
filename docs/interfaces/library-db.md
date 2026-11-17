# Library DB, sidecars and the library block

Status: **version 1, written by MEDIA 2026-11-17 (M7)**, before the code.
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
  state         TEXT NOT NULL,       -- queued | probing | converting | analysing | ready | failed
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
  palette TEXT NOT NULL DEFAULT '[]',        -- JSON [[rgb, weight] x5]
  slices  TEXT NOT NULL DEFAULT '[]',        -- JSON [frame, ...] up to 16
  analysis_version INTEGER NOT NULL DEFAULT 0,
  added_unix INTEGER NOT NULL, ready_unix INTEGER NOT NULL DEFAULT 0,
  stars INTEGER NOT NULL DEFAULT 0, favourite INTEGER NOT NULL DEFAULT 0,
  user_tags TEXT NOT NULL DEFAULT '[]'
);
CREATE INDEX clips_state ON clips(state, id);
CREATE TABLE sources (       -- path -> hash cache, so a rescan does not re-hash
  path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, hash TEXT
);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);  -- 'schema', 'ready_seq'
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
  `meta/*.json` (every sidecar whose `hap/` file exists is `ready`).

## 4. The content hash

`FNV-1a 64` over: the file size (8 bytes, little endian), then three 1 MB
samples at offsets 0, (size − 1 MB) / 2 and size − 1 MB (the whole file when
it is under 3 MB). Fast (~3 ms per file) and stable across renames (D140).

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
  "added": "2026-11-17T14:02:11Z", "ready": "2026-11-17T14:02:40Z"
}
```

Units: colours `#rrggbb`; 0..1 unless said; `motion_dir` degrees (0 = right,
90 = up); frame numbers are frames of the **HAP file**; `motion_curve` is one
value per proxy frame (`proxy.fps`). The rules and thresholds that write
`energy`, `mood`, `loop.style` and `flicker` are in
`src/library/analysis.cpp` (`AnalysisRules`, defaults listed in the handoff);
`library-rules.json` will make them tunable (later).

## 6. The library block (`Local\HimmelLibrary`, magic `HMLB`, version 1)

Written by `himmel_ingest` (creates it; header last), read by anyone.
Plain atomics plus one seqlock section.

| Field | Meaning |
|---|---|
| `pid`, `heartbeat_us` | the ingest worker is alive (supervisor timeout 2 s) |
| `stop_request` | supervisor → ingest: 1 = finish the current file write and exit |
| `ready_seq` | bumps every time a row turns `ready` (or a ready row's name / path changes). The engine's feed re-reads the DB when it changes |
| `mode` | 0 idle, 1 full speed (no show), 2 background (show running), 3 high (Prepare Show, later) |
| `paused` | 1 while throttled (§7) |
| `queued`, `working`, `ready_total`, `failed_total` | counts |
| `job` (seqlock) | the current file: name, stage, progress 0..1, `last_error` |

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
  every second (or when `ready_seq` changes) reads `ready` rows with
  `id > last_id`, probes the HAP header and first frame, and appends them
  to a fixed table (capacity `kMaxClips`, preallocated).
- The frame loop sees the published count rise (an atomic), copies the new
  `ClipInfo`s into its catalog and writes the catalog section once. **Ids are
  append-only**: existing clip ids never move, so commands and `LiveState`
  stay valid (`engine-state.md` §2). A library clip whose name is taken by a
  `media/` clip gets ` (lib)` appended.
- The same table is the clip players' playlist, so loading a new id works at
  once. The frame loop never reads the DB or a file.
- `--no-library` turns the feed off; `--library DIR` picks the folder.

## 9. Not in version 1 (append later)

- Hap Q Alpha, "optimise for show" downscale, keeping audio as a side file.
- Animated WebP: the bundled ffmpeg builds often cannot decode it; such a
  file imports as a still (first frame) and the log says so.
- Type tags / similarity (ONNX), tracking sidecars, generators' 10 s render.
- A `library-rules.json` file for the analysis thresholds.
- Hover loops (`thumbs/<hash>.webm`).
- Commands to the ingest worker (CommandType 60–69: rescan, retry, set
  stars / tags, Prepare Show priority) and MK2 `BROWSE`.
