# Operations: displays, frame rates, media types, settings, installations

Status: decided by the owner, 2026-11-11 (D79–D84). The "boring" basics that
decide whether a night or an installation survives. Related:
[`engine.md`](engine.md) §3–4, [`projects.md`](projects.md),
[`timeline-and-remote.md`](timeline-and-remote.md),
[`../build-v1.md`](../build-v1.md).

## 1. Displays that come and go (0.1)

The most common gig failure is not a crash but a projector that power-cycles
or an HDMI cable that drops.

- **Outputs are known by identity, not by Windows order:** the monitor's
  EDID (maker, model, serial) plus the connector. Saved in the project.
- **When a display disappears:**
  - the engine keeps rendering that output's part of the canvas (into an
    off-screen target), so nothing else changes;
  - the other outputs never stutter (no swap-chain rebuild on them);
  - the GUI and the MK2 status line show "OUT 2 lost" in red.
- **When it comes back** (same identity, any connector): it is re-attached to
  its slot within ~2 s, full screen, same mode, and the warning clears.
- **If Windows reorders displays** (it does after a hot-plug), Himmel follows
  identity and the order on the canvas never changes.
- **A different display in the slot:** Himmel asks once (GUI + MK2): "Use the
  new display as OUT 2?".
- Test in M12: pull each output cable during an 8 h soak.

## 2. Identify outputs (0.1)

- **IDENTIFY** (wizard step 3, Outputs view, MK2 system menu): every output
  shows a huge number 1, 2, 3 …, its name and resolution for 5 s (or while
  held), over the show if needed (a 0.5 s fade in / out, never in the middle
  of a show without asking).
- The wizard lets you drag the numbers into the physical order ("which screen
  is on the left?").

## 3. Frame rates (0.1)

- **Output refresh:** 60 Hz by default; **50 Hz** as a choice per show (for
  European cameras, LED walls and broadcast); all outputs of one canvas share
  it. 59.94 / 30 / 25 under Advanced.
- **Clips play at their own frame rate** (24, 25, 30, 50, 60 …): the clip
  player maps clip time to output time, so a 25 fps clip runs at the right
  speed on 60 Hz.
  - Default: nearest frame (sharp, no cost).
  - Optional per clip: **frame blend** (smooth, cost S) for slow footage.
- **Cameras:** the capture rate is chosen to match the output (50 Hz show →
  25 / 50 fps camera modes preferred) to avoid judder.
- **BPM-synced clips** ignore the clip's fps and use beats (as specified).
- The import notes the clip's fps; the library shows a small badge when it
  will judder on the show's refresh (e.g. 24 fps on 50 Hz).

## 4. Stills, image sequences, GIFs (0.1)

| Type | Import | Playback |
|---|---|---|
| **PNG / JPG / WebP / TIFF stills** (logos, overlays, backgrounds) | Kept as an image (PNG keeps alpha), plus thumbnail and palette tags; huge images are scaled to at most 2× the canvas | A one-frame source; all effects, places and transport (hold) work |
| **Image sequences** (numbered PNG / JPG / TGA folders) | Converted to HAP (HAP Alpha when the PNGs have alpha) at the frame rate you pick (default 25) | Like any clip |
| **Animated GIF / APNG / WebP** | Converted to HAP Alpha | Like any clip |
| **SVG** | Rasterised to PNG at 2× canvas height | As a still |

Drop them into the same watch folder as video; the library treats them all as
clips with a type badge.

## 5. Settings (0.1)

One **Settings** view (GUI top bar gear icon; the MK2 system menu holds the
most used ones). Grouped, each with a "Test" button where it makes sense:

| Group | Settings |
|---|---|
| **Audio in** | Device (mic / line / loopback), channels, gain, auto gain, noise gate, AV offset, test meter (`music.md`) |
| **Tempo** | Clock source and priority, tap behaviour, MIDI clock in port |
| **Weapons** | Main weapon, side arms, MIDI ports, Block / LUMI modes, pad sensitivity curve |
| **Outputs** | Default resolution and refresh, identify, flash cap (off by default) |
| **Library** | Watch folders, library location, import quality, background import limits |
| **Projects** | Projects folder, autosave interval, open last project on start |
| **GPU** | Which GPU renders (when there are two), budget warning level |
| **Remote** | OSC in / out ports and allowed addresses, phone remote on / off and PIN (`timeline-and-remote.md`) |
| **Start-up** | Splash, start focus, installation mode (§6) |
| **Help & logs** | Language of help (English), send logs (§7), version |

Settings live in `%APPDATA%\Himmel\settings.json` (per PC), not in the
project, except where the project must carry them (outputs, tempo, remote
addresses), which the project stores and the Settings view marks "saved with
the project".

## 6. Installation mode (0.2)

For a gallery, a shop window or a club that runs Himmel every night without a
VJ:

- **Start with Windows** (a scheduled task under the show user), **open the
  last project**, and **start playing** a chosen song, scene or timeline.
- **Schedule:** per weekday, on / off times and what to play
  (e.g. 18:00 scene "Evening", 22:00 timeline "Club", 02:00 fade to black and
  outputs off via DDC/CI or the projector's network control later).
- **Unattended safety:** no dialogs (everything answers itself with the safe
  choice and logs it), the mouse cursor hidden on outputs, the GUI locked
  behind a **PIN** (the MK2 and the phone remote can be locked too), auto
  restart after a crash (the supervisor) and after a power cut (start with
  Windows).
- **Health ping:** optional OSC / HTTP heartbeat to a monitoring tool, and a
  daily log rotation.
- **Content rotation:** Autopilot (`presets-and-shuffle.md` §7) can run all
  night inside a mood, so the picture never repeats exactly.

## 7. Send logs (0.2)

- **Help › Send logs** packs the logs of the last 3 shows, the frame-timing
  graphs, the settings, the project (without media) and system info (GPU,
  driver, displays, USB devices) into one zip on the desktop, ready to attach
  to an issue on GitHub. Nothing is uploaded by itself.
