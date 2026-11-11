# Library, import and analysis

Status: proposal for the owner, 2026-11-11. Goal: drop in clips and forget
about formats. Everything is made show-ready and tagged in the background,
including during a show, and the tags drive shuffle, moods and reactions
([`presets-and-shuffle.md`](presets-and-shuffle.md)).

## 1. Rules

1. **Optimise before the show, play during it.** Playback only ever reads
   ready HAP files. Conversion and analysis never run in the render process.
2. **Import during a show is allowed, but in the background:**
   - the import worker runs at idle priority, with I/O throttling;
   - GPU analysis uses its own low-priority device, or the CPU;
   - the worker pauses when the renderer's frame time gets close to the
     budget.

   New clips appear as **NEW** when they are ready, never half-ready.
3. **Originals are never touched.** Converted files, thumbnails and analysis
   are kept next to the library, keyed by a hash of the content, so moving or
   renaming a file never triggers a new analysis.
4. **Everything is rebuildable:** each clip has a JSON sidecar with its
   analysis, and the SQLite DB is an index that can be rebuilt from the
   sidecars.

## 2. The pipeline

```
watch / drop
  → probe       (ffprobe: codec, size, fps, duration, alpha, audio)
  → convert     (ffmpeg → HAP | HAP Q | HAP Alpha; size rounded to ×4; chunks = min(cores, 8);
                 keep resolution by default, optional "optimise for show" downscale;
                 audio kept as a side file only if asked)
  → analyse     (see §3; low-res proxies, sampled frames)
  → tag         (rules → mood scores; optional local model → type tags)
  → previews    (thumbnail strip, 2 s hover loop for the GUI)
  → index       (SQLite + sidecar JSON)
  → READY       (shows in the library; usable live)
```

- Each stage is resumable: a crash or reboot continues where it stopped.
- **Stages run as separate worker processes** (`himmel_ingest.exe` plus
  ffmpeg), supervised like everything else.
- The **priority** follows the show state:

  | Show state | Import |
  |---|---|
  | No show running | Full speed |
  | Show running | Idle priority; analysis at low resolution; at most 1 conversion at a time |
  | Prepare Show | High priority |

- **Clips that already are HAP** skip conversion but are still analysed.
- **Generators** get the same analysis on a 10-second render, so shuffle
  treats them like clips.
- **Cameras and capture inputs** get live, cheap tags (brightness, motion,
  main colours), updated a few times a second.

## 3. What is measured (the tags)

| Tag | How | Range / values | Used by |
|---|---|---|---|
| **Palette** | k-means on sampled frames (Lab colour), plus a weight per colour | 5 colours + weights | Colour search, mood fit, palette mapping |
| Average colour, brightness, contrast, saturation | Frame statistics | 0–1 | Fit; rejecting rolls that are too dark or too bright |
| **Warm / cool** | From the palette | −1 … +1 | Mood style axis |
| **Motion** | Optical flow on a 1/4-size proxy (NVIDIA hardware optical flow when available; Farneback otherwise) | Amount 0–1, main direction, steadiness | Calm vs fast; chase direction |
| **Cut rate** | Shot-change detection | Cuts per minute | Calm takes vs fast montage |
| **Energy** | Combined from motion, cuts, contrast and saturation | 1–5 | The energy grid, Assist, mood fit |
| **Mood scores** | Rules on the tags above, tuned on the starter library | chill / groove / techno / crazy, each 0–1 | Mood filters, shuffle |
| **Type** | Optional local image model (ONNX, a CLIP-style zero-shot) over a set of words | abstract, nature, water, city, people, dance, text, particles, geometry, fire, space, retro, … | Type filters, the mood clip filter |
| **Similarity fingerprint** | An embedding vector from the same model, or a colour + motion histogram if there is no model | Vector | "Find similar" |
| **Loop quality** | SSIM between the last and first frames | 0–1, plus a "needs crossfade" flag | Seamless loops |
| **Bars** | Duration at the show BPM | 1, 2, 4, 8, … or "free" | Beat-synced playback, rolls |
| **Flicker warning** | Flash and red-flash frequency check against photosensitivity guidelines (3 flashes / s) | ok / warn / bad | Safe mode excludes it; a GUI badge |
| **Tracking** *(optional)* | Pose / person segmentation once, stored as a low-res sidecar per frame | Masks, keypoints | Tracking effects at zero live cost (`engine.md` §6) |
| **Technical** | Resolution, fps, duration, alpha, black frames | — | Warnings before the show |
| **User** | Stars, own tags, favourite, "used in show X" | — | Weighting in shuffle |

The thresholds and rules live in a config file (`library-rules.json`), so they
can be tuned without code. Every rule names the tag it writes, so a wrong tag
can be traced back to the rule that made it.

## 4. Storage

```
<library>/
  himmel-library.db        SQLite index (rebuildable)
  originals/ …             (or references to the user's own folders)
  hap/<hash>.mov           converted clips
  meta/<hash>.json         sidecar: probe + analysis + tags
  thumbs/<hash>.jpg|webm   thumbnail strip + hover loop
  track/<hash>.bin         optional tracking data
```

- **Hash:** xxHash3 over the size plus 3 × 1 MB samples (fast, and stable
  across renames).
- **Show folder (Prepare Show → Pack):** copies exactly the clips, LUTs,
  shaders and moods used by the setlist, so the show runs from one folder on
  a backup PC.

## 5. In the GUI and on the MK2

- **Library view:** see `gui.md` §6.
- **MK2 `BROWSE`:**
  - the encoder scrolls;
  - Top 1–8 are filters (CHILL, GROOVE, TECHNO, CRAZY, COLOUR, MOTION, NEW,
    FAV);
  - pads show 16 results in each clip's main colour;
  - push loads into the selected layer on PREVIEW;
  - `SHIFT + pad` = find similar;
  - knob 1 = hue filter, knob 2 = energy filter.
- **Wizard step 1:** the import queue with stages and tags (`gui.md` §3).

## 6. The starter library

About 50 clips, plus recorded generator looks, spread over the mood map, so a
new user can play on day one. They must be **free to redistribute** (CC0 or
made by us) and credited in `THIRD_PARTY_NOTICES.md`. The starter library is
kept outside git (as a download) because of its size.

## 7. Prepare Show (before the night)

One button. It runs these steps and gives a green, yellow or red report:

1. Convert and analyse whatever is not ready.
2. Read every clip end to end (finds broken files).
3. Check that everything the setlist uses is present.
4. Compile all shaders and effect chains for this GPU.
5. Measure the GPU cost of every effect and every scene.
6. Report the memory and disk read of the heaviest cue.
7. Pre-load the first cue of each song.
8. Optionally play through all cues fast and measure the fps.
9. Pack the show folder.
