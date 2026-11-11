# Projects: save, open, files, output resolution

Status: decided by the owner, 2026-11-11 (D77, D78). Simple on purpose.
Related: [`presets-and-shuffle.md`](presets-and-shuffle.md) §8 (the JSON
inside), [`gui.md`](gui.md) §3 and §18, [`../build-v1.md`](../build-v1.md) M3.

## 1. What a project is

A **project** (a "show") is everything needed to play a night:
- the setlist, songs, scenes (with kits, macros, variations);
- banks and the clip settings (in / out, cue points, modes, variants, clip FX);
- the outputs (screens or projectors, how many, **resolution**, blend);
- the weapons and their settings (main + side arms);
- moods, macros, master and output effects, audio / tempo sources.

The clips themselves stay in the library (they are big, and shared between
projects). The project refers to them by library id **and** by file name + size
+ a short hash, so it can find them again on another PC.

## 2. On disk

- **One project = one folder**, named `<name>.himmel/`, holding plain JSON
  (`himmel-show/1`, one file per bank / song / mood, as in
  `presets-and-shuffle.md` §8) plus `project.json` (name, version, outputs,
  weapons) and a small `thumb.png`.
- Windows Explorer shows the folder with the Himmel icon; double-clicking
  `project.json` (or the folder, via the file association) opens it.
- **Default place:** `Documents\Himmel\Projects\`. Any folder works.
- **Export as one file:** `Save a copy as file…` writes `<name>.himmelshow`
  (a zip of the folder). **Open file…** accepts both the folder and the
  `.himmelshow` file (it is unpacked into the projects folder).
- **Pack with media** (in Prepare Show): the same zip **with** the HAP clips
  it uses, to move a whole show to the show PC on a USB stick.

## 3. Saving (never lose work)

| Action | Keys / where | What happens |
|---|---|---|
| **New project** | `Ctrl+N`, start screen | Empty project from a template (1 / 3 screens), opens the wizard at step 1 |
| **Open…** | `Ctrl+O`, start screen | File dialog for a project folder or `.himmelshow` |
| **Open recent** | Start screen, File menu, MK2 system menu (`SHIFT` + wheel) | The last 10, with thumbnail, date and output count |
| **Save** | `Ctrl+S`, MK2 system menu | Writes the project folder |
| **Save as…** | `Ctrl+Shift+S` | A new name / folder (the old one stays) |
| **Save a copy as file…** | File menu | One `.himmelshow` file |
| **Revert** | File menu | Back to the last save (asks first) |

- **Autosave:** every change is journalled at once to `autosave/` inside the
  project (a worker, never the render loop), and a full snapshot is kept
  every 5 minutes (the last 10). A crash or power cut loses nothing; on the
  next start Himmel offers "Restore the autosave from 22:41?".
- **Saving never interrupts the show:** the engine hands a state copy to the
  save worker; the write is atomic (write to a temp file, then rename).
- The title bar and the MK2 status line show the project name and a dot when
  there are unsaved changes.
- **Switching project during a show** loads the new one into PREVIEW-side
  memory first (clips warmed), then asks to TAKE; the outputs never go black
  unless you choose it.

## 4. Missing files (relink)

On open, every clip is checked. Missing ones show their thumbnail colour as a
placeholder (never black, never a crash) and a "**Find missing clips**"
dialog searches a folder you pick by name, size and hash, and relinks them all
at once.

## 5. Output resolution (kept simple)

Each output gets one of **four presets**:

| Preset | Size | When |
|---|---|---|
| **720p** | 1280 × 720 | Old projectors, weak laptops, test windows |
| **1080p** (default) | 1920 × 1080 | Most screens and projectors. **Recommended.** |
| **1440p** | 2560 × 1440 | Sharp large LED walls or monitors. **Recommended for bigger rigs** on the 5090 |
| **4K** | 3840 × 2160 | 4K projectors and TVs. **Recommended** only with 4K clips; costs about 4× of 1080p |

- The wizard (step 3) proposes the preset that matches the display's native
  mode, and warns when outputs on one canvas differ (they must match for a
  span; NVIDIA Surround needs identical modes).
- The **canvas** follows: e.g. 3 × 1080p screens = 5760 × 1080; 3 × 4K =
  11520 × 2160 (the wizard shows the GPU cost estimate next to it).
- **Custom** size exists only under "Advanced" (for LED processors with odd
  sizes); it is not offered by default.
- Clips are never re-encoded to match; they are scaled on the GPU.
- The refresh rate follows the display (60 Hz default); it must be the same on
  every output of one canvas.

## 6. Engine and files

- The command protocol has `project.new / open / save / save_as / export /
  revert`; only the SHOW track's worker touches disk.
- Opening a project builds the render graph in a worker and swaps it at a
  frame boundary (as with scenes).
- Formats in `docs/interfaces/show-files.md` (M3), with a `version` field and
  forward migration.
