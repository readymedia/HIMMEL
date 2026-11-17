# Outputs block (`Local\HimmelOutputs`)

Status: **version 1, written by ENGINE 2026-11-17** (M6; `build-v1.md`,
`design/operations.md` §1–3, `design/projects.md` §5, `design/engine.md` §2.0).
Header: [`src/protocol/include/himmel/protocol/outputs.h`](../../src/protocol/include/himmel/protocol/outputs.h).
Writer: `himmel_render` only. Readers: the supervisor (curtains), the GUI, the
weapons, the next renderer. The commands that change it are
[`command-protocol.md`](command-protocol.md) §4c (types 70–73).

## 1. What it is

One seqlock section, `OutputsSection` (~3 KB), written by the renderer **on
change, while IDENTIFY runs, and every 100 ms**:

- the canvas: output count, span / mirror, canvas size, edge-blend overlap and
  gamma, the refresh asked for and the frame rate measured, the number of
  displays Windows has;
- per output slot (`OutputInfo`, up to 8):
  - `state`: `Ok` (full screen on its display), `Window` (windowed test
    mode), `Lost` (its display is gone), `Other` (its display is gone and a
    different one is free: the GUI / MK2 asks "Use the new display as OUT n?",
    then sends `OutputConfig` `Adopt`), `Off` (slot not used);
  - the display it belongs to, **by identity** (`DisplayId`: EDID maker,
    product, serial number / serial text, plus the connector), and its name;
  - its part of the canvas (`canvas_x`, `width` × `height` = the preset),
    its swap-chain size, the display's refresh;
  - `black`, `identify`, the resolution preset, the blend ramps;
  - the output window: desktop `rect` (visible bounds) and `hwnd`;
  - `lost_count` and `returned_ms` (how long the last return took);
- per layer slot (9: deck × 4 + layer, then the HIT layer): its place
  (`FillAll`, `Out` n, `Fit`, `Free`) and the computed rect on the canvas.

`OutputsStatus` holds the renderer's `pid` (0 after a clean stop) and its
heartbeat.

## 2. Lifetime

- The supervisor (`himmel.exe`) opens the block at start and keeps it open
  all night. The renderer creates it if nobody did, and writes the header.
- **A restarted renderer restores from it:** black per output and the layer
  places always; the display identities of full-screen slots; and the canvas
  settings (presets, mode, blend, refresh) only when a command changed them
  (`config_by_command`). Otherwise its arguments win.
- The supervisor also keeps `Local\HimmelState`, `Local\HimmelCommands` and
  `Local\HimmelShow` open, so the show survives a renderer restart even when
  no GUI is running. It writes the command block's header when it creates it
  (command-protocol.md §2). It leaves the state block without a header, for
  the renderer to write.

## 3. Curtains (supervisor)

The supervisor's curtain thread reads the block every 50 ms. For every used
slot it holds a black window (`HimmelCurtain`: popup, tool window, no
activation, no DWM animations) at the output's `rect`, **just below the
output window** (`hwnd`), topmost when the outputs are full screen. A curtain
only moves when the renderer's window moved. When Windows tries to move a
curtain (its display went away), it refuses, so a curtain never lands on
another output. When the renderer dies, its windows vanish and the curtains
show black until the new renderer's windows come back on top.
`himmel.exe --no-curtains` turns them off.

## 4. Displays that come and go (renderer)

- A worker (`DisplayWatch`, `src/engine/src/display_watch.cpp`) scans
  `QueryDisplayConfig`, plus the EDID from the registry, every 500 ms and at
  once on `WM_DISPLAYCHANGE`. The frame loop only copies its snapshot when
  the generation changed.
- Slots are matched to displays by identity (`match_displays`): a known
  slot takes its own display on any connector; a slot that never had one
  takes the free displays left to right; a known slot whose display is
  missing while another one is free becomes `Other`, and is never taken over
  silently.
- A display that goes away: its window is hidden. Full-screen output windows
  refuse every move we did not make (`WM_WINDOWPOSCHANGING`), so Windows
  cannot push it onto another output. It leaves the wait set and is not
  presented. Its part of the canvas keeps rendering (MAIN, previews). **The
  other outputs are not touched**: no swap-chain rebuild, no mode change.
- When it comes back, the window is moved to the display's rect and shown.
  Only its own swap chain is resized, and only if the size changed. The
  watcher then asks for the refresh again.
- Refresh (operations.md §3): in full screen the watcher's worker switches
  each used display to the asked rate at its current size
  (`ChangeDisplaySettingsEx` with `CDS_FULLSCREEN`, which is temporary:
  Windows puts the user's mode back when the renderer exits). In windowed
  mode the rate is only noted. Clips always play at their own fps: the
  playhead maps clip time to output time.

## 5. Canvas geometry (`OutputModel`, pure, `output_tests`)

- Presets: 720p 1280×720, 1080p 1920×1080 (the full-screen default),
  1440p 2560×1440, 4K 3840×2160, Custom = `--size` (the windowed default).
  A preset can be set for all outputs or for one.
- Span: the outputs side by side, neighbours overlapping by the blend width
  O. The canvas width is ΣW − (N−1)·O; the overlap is capped at half the
  narrowest output. Mirror: every output shows the whole canvas, with no
  blend.
- Edge blend (in `ps_blit`): a smoothstep ramp over the overlap on the inner
  edges, raised to 1/γ (γ = `blend_gamma`, 2.2 by default).
- Layer places become the layer shader's transform (canvas uv → clip uv).
  They are computed on a command or a canvas change, never per frame:
  - FILL ALL and OUT n crop the clip (cover);
  - FIT shows the whole clip, and the rest of the canvas is transparent;
  - FREE covers its rect.

  The clip's placeholder colour keeps the same place.
- IDENTIFY: every shown output darkens the show by 60 % and draws its number,
  its "OUT n W×H Hz" line and the display name (5 s, or held). It fades in
  and out over 0.5 s. It is drawn over the show, so the GUI asks before it
  sends it during a show (operations.md §2).
- Black per output is a cut (GUESS: no fade). Master black still applies to
  every output.

## 5a. Spout out (`--spout NAME`)

MAIN (the whole canvas, black when master black) goes out as a Spout 2 DX11
sender named NAME. SpoutDX waits on Spout's named mutex, so it never runs on
the render thread:
- the frame loop copies MAIN into one of two shared textures (legacy share
  handles) and signals;
- a worker with its own D3D11 device on the same adapter opens the textures
  and calls `SendTexture`.

A texture the worker is still reading is skipped that frame. Any Spout
failure is logged once, and Spout stops; the outputs go on. Off without
`--spout`. Checked with `spout_probe.exe NAME` (a SpoutDX receiver).

## 6. Rules

- The frame loop only reads the model and the watcher's snapshot. Window
  moves, hide / show and the one swap-chain resize happen only on a display
  event or a test command. A preset, mode or blend change rebuilds the canvas
  at the next frame start: a setup action with one hitch, like a window
  resize.
- Append-only: new fields go at the end of the section, with a version bump
  when the size changes.
