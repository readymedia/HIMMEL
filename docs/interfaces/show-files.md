# Show files, the show block and the show commands (`himmel-show/1`)

Status: **version 1, written by SHOW 2026-11-17 (M3)**. Owner: SHOW track
(`src/show/`). Code: [`src/show/src/files.cpp`](../../src/show/src/files.cpp)
(formats), [`src/protocol/include/himmel/protocol/show.h`](../../src/protocol/include/himmel/protocol/show.h)
(block + enums, the source of truth for field layout),
[`src/show/include/himmel/show/model.h`](../../src/show/include/himmel/show/model.h)
(the model in memory). Design: [`../design/projects.md`](../design/projects.md),
[`../design/perform.md`](../design/perform.md) §2–3,
[`../design/presets-and-shuffle.md`](../design/presets-and-shuffle.md).
Commands: [`command-protocol.md`](command-protocol.md) §4b. The block:
[`show-block.md`](show-block.md).

## 1. Who does what

```
GUI / MK2 Perform / OSC / tools ──commands 25..36──► himmel_render
                                                     ├─ ShowRuntime (frame side): scenes, recall, morph,
                                                     │  undo 64, dice, kits, macros, songs
                                                     │    └─► DeckModel::submit(slot 4 "Show", ordinary commands)
                                                     └─ save worker (own thread): every file
                                                          ▲ copies via preallocated buffers + SPSC rings
Local\HimmelShow ◄── status (frame) · files (worker) · text boxes (producers)
```

- The render process is still the only consumer of commands. Show commands
  (types 25–36) go to the show runtime instead of the deck model; the runtime
  changes the decks **only** by sending ordinary commands (LayerClip, FxSet,
  LayerSet, …) into `DeckModel::submit` on producer slot 4 (`Show`), so they
  pass the same validation and the same rate limit (the cap below).
- Only what differs is sent, and it is sent in the same frame: a recall
  between two full 4-layer scenes is ~50 commands and costs ~3 µs (measured, §8).
- **The cap (LEAD, 2026-11-17).** Slot 4 has the same validation and rate
  limit (4000/s) as every producer. The runtime sends at most
  `kShowCommandsPerFrame` = 384 commands per frame and keeps its own count
  of the second's budget; what does not fit waits in its outbox (1024) and goes
  out first in the next frame. One deck's full look is at most 352 commands
  (`kLookCommandsMax`, static_assert), so **a recall's look always lands in
  one frame**. Knob reactions (`FxReact`, at most 2 per knob, 160 per deck,
  `kLookReactCommandsMax`) go after every other command of the recall, so in
  the extreme case (a full look and dozens of custom reactions) the rest of
  the reactions follow one frame later. An undo of both decks in the worst
  case takes three frames. A new recall /
  undo / dice / reset drops what is still waiting (it is computed against the
  model as it is), and a morph waits a frame instead of piling up.
- Only the save worker touches disk. The frame hands it copies (the whole
  `ShowData`, ~1.7 MB, only when a save or a coalesced autosave needs it; the live
  state, ~10 KB, at most 10 times a second). JSON is parsed and written in the
  worker; a loaded show comes back as a pointer swap at a frame boundary.

## 2. The model (what a show holds)

| Thing | Holds | Limits |
|---|---|---|
| **Show** | name, outputs (count, preset 720p / 1080p / 1440p / 4K / custom, span / mirror, refresh), start focus (PREP / PLAY, D75), default TAKE ms, locks, current bank / song / setlist mode | 256 scenes (variations included), 16 banks, 64 songs |
| **Scene** | name, **look** (one deck: 4 layers × clip, opacity, blend, mute, speed, 4 FX slots, every transport setting; 4 deck FX slots; each FX slot's **knob reactions**, D131), **kit** (16 pads), **4 macros**, transition (cut / morph / take, seconds, on beat) | variations a–h (8) |
| **Variation** | a scene with `parent` = its main scene; same pad; letters b, c, … (the main is a) | |
| **Kit pad** | kind (empty, clip, cue, effect, scene, action), target (SELECTED, LAYER 1–4, ALL, FREE, HIT, D71), id, slot, name | 16 |
| **Macro** | name, value 0..1, up to 8 bindings (layer or deck FX, slot, param, min, max) | 4 per scene |
| **Song** | name, BPM (0 = none), up to 16 scenes | in the setlist order |
| **Bank** | 16 scene pads | 16 banks |

- A scene lives in exactly one place: a bank pad **or** a song position.
- Without the setlist, pages (the LOOKS pads, the GUI scene grid) show the
  current bank; with it (`ShowSelect UseSetlist`), the current song's scenes.
- Cue points belong to the clip, not the scene (the engine keeps them per clip).
- Not in a look: playhead position, play / pause, the HIT layer, PRESS,
  the crossfader (these are live state, not a look).
- **Knob reactions (D131, 2026-11-17).** Every FX slot of a look (layer,
  deck and, in the live state / undo, master) keeps the reaction of its knobs
  1–4 as the engine's reaction table has it (`ReactionSection`,
  `engine-state.md`): **Default** (the effect's own reaction), **Custom**
  (signal, mode, curve, quantise, hold, amount, attack, release, threshold,
  and the range when it was set) or **Off**. Scenes, variations, undo steps,
  the journal and `live.json` all carry them; a scene is "edited" when a
  reaction differs too. Only the live values (`env`, `value`) are not kept.

## 3. On disk: the project folder

```
My Show.himmel/
  project.json          head + the CRC manifest (written last)
  banks/bank-01.json    one per non-empty bank (bank-01 .. bank-16)
  songs/song-01.json    one per song, in setlist order
  live.json             both decks + master FX + crossfader at save time
  autosave/
    show.json           the whole show as one document (every show change, ≤ 4/s)
    journal.bin         live-state records (every live change, ≤ 10/s)
    snap-YYYYMMDD-HHMMSS.json   a full snapshot every 5 min, the last 10 kept
    offer-show.json / offer-journal.bin   a newer autosave that was not used on open (§5)
```

- Default place `Documents\Himmel\Projects\`; untitled work lives in
  `%LOCALAPPDATA%\Himmel\Untitled.himmel\` until it is saved. Recent projects:
  `%LOCALAPPDATA%\Himmel\recent.json` (the last 10).
- Every JSON file starts with `"format": "himmel-show/1"` and a `"kind"`
  (`project`, `bank`, `song`, `live`, `document`, `recent`). Readers refuse
  another format and a `version` higher than theirs (forward migration: a
  newer reader upgrades older files on load). **Version 2** (wave 8, D210):
  clips carry `library_id` and `hash` (§3a). Version 1 files load (clips by
  name) and are written as version 2 by the next save. Changes are
  append-only: new keys, never a changed meaning.
- Tabs, one key per line, floats rounded to 4 decimals: the files diff well in
  git, and saving what was loaded gives the same bytes (tested).
- **Clips are stored by name** (`{"name": "clip04.mov", "w": 1920, "h": 1080,
  "seconds": 10}`), effects by their catalog name, enums mostly by name
  (`"blend": "add"`, `"mode": "pingpong"`). A clip the catalog does not have
  loads as an empty layer **with its name kept**, so it survives a save and is
  back when the clip is (relink, §6). `GUESS:` the short file hash of
  `projects.md` §1 needs the library (MEDIA M7); until then name + size + length.

`project.json`:

```json
{
	"format": "himmel-show/1", "kind": "project", "version": 2, "app": "Himmel 0.1",
	"name": "My Show", "created": 1763400000, "saved": 1763401234, "saved_ms": 1763401234567,
	"outputs": {"count": 3, "preset": "1080p", "span": true},
	"start_focus": "prep", "default_take_ms": 2000, "locks": ["clip", "colour"],
	"use_setlist": false, "bank": 0, "song": 0,
	"banks": ["banks/bank-01.json"], "setlist": ["songs/song-01.json"],
	"files": {"banks/bank-01.json": "3fa2c1d0", "songs/song-01.json": "...", "live.json": "..."}
}
```

A scene (inside `bank-NN.json` → `"pads": [scene | null × 16]`, or
`song-NN.json` → `"scenes": [...]`):

```json
{
	"name": "Scene 02",
	"transition": {"kind": "cut", "seconds": 2, "on_beat": false},
	"look": {
		"layers": [
			{"clip": {"name": "clip04.mov", "library_id": 412, "hash": "3fa2c1d09b8e7a61", "w": 1920, "h": 1080, "seconds": 10},
			 "opacity": 1, "blend": "normal", "speed": 1,
			 "fx": [{"effect": "Kaleido", "params": [0.5, 0.25], "mix": 1,
			         "react": [{"knob": 0, "source": "custom", "signal": "bass", "mode": "follow", "curve": "soft",
			                    "amount": 0.7, "attack_ms": 25, "release_ms": 300, "threshold": 0.2,
			                    "quantise": "1/4", "hold_beats": 0.5, "range": [0.2, 0.8]},
			                   {"knob": 1, "source": "off"}]}, null, null, null],
			 "transport": {"mode": "loop", "in": 0, "out": 1, "loop_in": 0.25, "loop_out": 0.75, "...": "every LayerParam"}},
			"... 4 layers"
		],
		"fx": [null, null, null, null]
	},
	"kit": [{"kind": "clip", "target": "layer2", "name": "clip04.mov", "library_id": 412, "hash": "3fa2c1d09b8e7a61"}, null, "... 16"],
	"macros": [{"name": "INTENSITY", "value": 0.5, "binds": [{"layer": 1, "slot": 0, "param": 1, "min": 0.2, "max": 0.9}]}, "... 4"],
	"variations": [{"name": "Scene 02", "look": {}, "kit": [], "macros": []}]
}
```

### 3a. Clips: library id, hash, name (version 2, wave 8, D210)

A clip in a look (`"clip"`), a kit pad (kind `clip` / `cue`) and the live
state (`live.json`, the journal) is remembered by three things, strongest first:

| Key | What | When |
|---|---|---|
| `library_id` | the library DB row id (`library-db.md`) | library clips; left out for `media/` and `--clip` clips |
| `hash` | the content hash, 16 lowercase hex (`library-db.md` §4; the file stem of `hap/<hash>.mov`) | library clips |
| `name` | the clip's name in the catalog | always |

Engine clip ids are never stored: they are a run-time binding (up to 8192,
catalog v3). Binding (`bind_clip`, `src/show/include/himmel/show/clips.h`):
1. **library id**: the newest Ready engine clip with that id (the same rule
   as `LayerClipLib`). A known hash that differs wins over the id (another
   library can reuse row ids).
2. **hash**: when the id is not found, or its clip is not Ready (removed,
   gone): the newest Ready clip with that hash. This is the **relink** of a
   clip that was removed and imported again (a new library id, the same
   content); the reference takes the new id.
3. **name**: case-insensitive; never across two different known hashes.
4. Nothing matches: the clip is **missing** (below and §6); the reference is
   kept as it is and saved back unchanged.

A found reference is brought up to date (library id, hash, the catalog's
name), so a version 1 file (names only) carries library ids and hashes after
its next save, and a relinked scene marks the show dirty.

**`"react"` in an FX slot** (D131, added 2026-11-17; same `himmel-show/1`
format, since it is an optional key): a list of the knobs that are not
Default, each `{"knob": 0..3, "source": "custom" | "off", ...}`. Custom
knobs carry `signal` (none, level, bass, mid, high, kick, snare, hat, onset,
beat, bar, energy, build, drop), `mode` (add, follow, trigger), `curve`
(linear, soft, hard, steps, exp), `amount` −1..1, `attack_ms` / `release_ms`
0..2000, `threshold` 0..1, and only when not the default: `quantise` (1/16,
1/8, 1/4, 1/2, bar), `hold_beats` (quarter steps, 0..4) and `range` `[lo, hi]`
(knob positions 0..1; without it the param's good range is used). The same
names and ranges as `FxReact` (`command-protocol.md` §4d).
- **Old files** (no `"react"` key) load with every knob Default: the
  effect's own reaction, as before D131.
- A knob past the effect's params, an unknown source or a broken entry is
  skipped (that knob stays Default); numbers are clamped like `FxReact` does.
  `"source": "default"` is accepted and means the same as leaving the knob out.
- The master FX slots in `live.json` (`"master_fx"`) carry `"react"` the same way.

## 4. Saving (atomic, never in the frame)

- Every file is written to `<file>.tmp`, flushed, then renamed over the old one
  (`MoveFileEx` `REPLACE_EXISTING | WRITE_THROUGH`): a crash leaves either the
  old or the new file, never half of one.
- `project.json` is written **last** and holds the CRC-32 of every other file.
  A loader that finds a file whose CRC does not match knows the save was cut
  off between files (`interrupted`), and takes the autosave instead.
- Save as writes a new folder (the old one stays) and moves the autosave
  there. An untitled show's `Save` = save as under its name in the projects
  folder (`Untitled.himmel`, `Untitled 2.himmel`, …).
- Export (`Save a copy as file…`) writes the project into a temp folder and
  zips it to `<name>.himmelshow` (zip, **stored**, UTF-8 names, CRC per
  entry, without `autosave/`). Open accepts it and unpacks it into the
  projects folder (`<name> (2).himmel` when the name is taken). No zip library:
  our own ~100 lines (stored entries only). "Pack with media" (with the HAP
  clips) is not in 0.1.

## 5. Autosave, journal, restore

- **Journal** (`autosave/journal.bin`): a header (journal version 2 since
  the knob reactions, D131; a version 1 journal is not restored, the show
  then comes from `show.json` / the last save; format version,
  `sizeof(LiveSnapshot)`, the effect names at write time) and records
  `{magic, size, unix ms, CRC-32, LiveSnapshot}`. A reader takes the newest
  record whose CRC is right, so a torn last record (the process died
  mid-write) is ignored. Effect ids are mapped back by name, clips by the names
  each look carries. A record is written within ~5 ms of a change on the model
  (at most every 100 ms); `FlushFileBuffers` once a second. A killed process
  loses nothing written (the OS holds it); a power cut loses ≤ 1 s.
  A save never holds the journal up: the worker writes waiting live records
  between the files of a save (wave 8; before that a 200-scene save could
  keep a live change out of the journal for longer than a save).
- `autosave/show.json` = the whole show as one document, after every show
  change (coalesced, ≤ 4 per second).
- **Snapshot** every 5 minutes: `snap-*.json` (show + live), the last 10;
  then the journal starts again (so it never grows past ~5 minutes of records).
- **On start** the worker opens the most recent project (or `--project`). If
  the autosave (`show.json` / newest journal record) is newer than the last
  save, or the save was cut off, it loads the autosave and says so
  (`FilesStatus.restored` = 1 autosave / 2 interrupted save; the message
  "Autosave 22:41 restored"). The live state is applied only if the decks are
  empty (a renderer restart restores the decks from the state block first,
  engine-state.md §2, which is fresher).
  `GUESS:` restore happens **automatically** at start (show-safe: never black,
  never a question on stage); the GUI offers **Revert** (the last save) when
  `restored != 0`. `projects.md` §3 says "offers"; the owner may want a question.
- **Opening a project by hand** loads the saved version; a newer autosave is
  renamed to `offer-*` and stays available through `RestoreAutosave`.
- Switching project mid-show: the new project's PROGRAM look goes onto
  **PREVIEW**; PROGRAM and the master are untouched (`projects.md` §3). New:
  the decks keep playing. Revert / RestoreAutosave apply the whole live state.

## 6. Missing clips and relink

On open every clip is bound as in §3a (library id, then hash, then name) against
every engine clip id (catalog v3, up to 8192). Missing ones:
`FilesStatus.missing_count` and the first 8 names; the layers stay empty and
the reference is kept. **On recall** the clips are bound again (a clip imported
since then, or imported again with a new library id, is found), and a layer
whose clip is missing, or found but not Ready (its file gone, removed), is
never silent: the recall message says so (`"Scene 02 a on B: clip
lib350.mov missing"`, or `"… 2 clips missing (name, ...)"`), in the show
block and the log. A found clip whose file is gone loads with `LayerClip` and
shows the placeholder (build-v1 §4.4). `Project Relink` with a folder in the text
box searches it (recursively) by file name and reports `relink_found`.
`GUESS:` adding the found files to the running catalog is ENGINE / MEDIA work
(M7); until then the operator copies them into `media/` and restarts — the
names in the project then bind again, nothing is lost.

## 7. The show block `Local\HimmelShow`

Specified in [`show-block.md`](show-block.md): the show status (written by the
render process), the file status (the save worker) and one text box per
producer for paths and names.

## 8. Recall, morph, undo, dice (what the commands do)

- **Recall** (`SceneRecall`): the scene's look → the addressed deck, in the
  frame the command is applied, by sending only the differences (settings
  before the clip, so the scene's transition / fades apply to its own clip).
  Measured 2026-11-17 on the dev laptop (`show_tests`, recall test: two full
  4-layer scenes, 2 FX per layer, deck FX, transport settings, 400 recalls):
  up to 51 commands, mean 2.7 µs, worst 21 µs — about 0.1 % of a 60 Hz frame
  (16.7 ms). Clip loads then open in the clip players' workers as with any
  `LayerClip` (the placeholder colour shows until the first frame is ready).
  **Library clips load by library id** (`LayerClipLib`, wave 8) when their
  clip is Ready; other clips (`media/`, `--clip`, or a library clip whose
  file is gone) with `LayerClip` and the bound engine id. Measured
  2026-11-17 (`show_tests`, library clips past id 256): 4 library clips,
  8 commands, ~6 µs.
  **Knob reactions** come last: after the look's `FxSet`s (which reset a
  slot to the new effect's Default reactions), one `FxReact` per knob that
  differs: `Default` or `Off`, or for a Custom knob `Set` and then `Range`
  (`f[0]` < 0 = back to the param's good range) when the range differs. A
  knob the effect does not have is never addressed. With `f[0] > 0` (or a scene transition of kind
  morph): a **morph** (numbers glide where both sides have the same thing,
  clips swap at the middle with a dip, the rest switches at 50 %; knob
  reactions switch at 50 % too).
- **Undo** (`Undo`): 64 steps of both decks + master FX (not the crossfader,
  TAKE / CUT / black). Edits closer than 400 ms are one step (a knob turn;
  `GUESS:` 400 ms). Recall, dice and reset are one step each. Redo until the
  next edit.
- **Reset** (`SceneReset`): back to the scene as it was recalled / saved,
  for one layer or the whole deck, gliding over `f[0]` s (default 1 beat).
- **Save** (`SceneSave`): the deck's look → a pad of the page (the next free
  one by default; an occupied pad gets the new look and keeps its kit, macros
  and variations). With `kVariation`: a new variation of the deck's scene
  (never overwrites, D72; the message says "3c saved").
- **Dice v1** (`Dice`, `presets-and-shuffle.md` §2): ROLL (seeded,
  deterministic), MUTATE (± amount, default 0.2), BACK / FORWARD through 64
  results per deck; a slot that gets a new effect gets that effect's Default
  reactions, a slot that keeps its effect keeps its reactions (FORWARD at the end = a new roll; a roll in the middle drops
  the forward part), ROTATE (the deck's scene's variations, then the next scene
  of the page). Scopes: deck / layer / FX slot. Locks: CLIP (clips), FX
  (effects), LAYOUT (opacity, blend), MOTION (speed, direction, params named
  speed / rate / spin / zoom …), COLOUR (params named hue / colour / sat …).
  SOURCE / REACT / MOOD are kept as bits but do nothing yet (no generators in
  the roll, the roll does not change reactions, no moods in 0.1's catalog). `GUESS:` the roll's
  weights (1–4 layers, 60 % an effect in slot 1, …) until moods and library
  analysis exist (M7).
- **Macros** (`MacroValue`): knobs 5–8 → the PROGRAM deck's scene's macros.
  Unbound macros fall back (`GUESS:` until "Fill macros"): INTENSITY = the mix
  of every layer effect, COLOUR / SPACE = the colour / motion params of the
  deck's effects, MASTER FX = the master effects' mix.

## 9. Limits and failure rules

- Every command is validated (range, ids, NaN) before it touches anything;
  refusals are counted per producer in the show block.
- A full page / show (`SceneSave` with no free pad, 256 scenes, 8 variations)
  is refused with `BadValue`; nothing is half-saved.
- The worker never blocks the frame: if it is busy, requests queue (64 deep;
  saves wait for the one stage buffer, at most 8 queued); the frame never
  waits for a file.
- If the worker cannot write (disk full, folder gone): `files.error`, the
  message "Save failed", `save_ok = 0`; the show plays on and the dirty dot stays.
