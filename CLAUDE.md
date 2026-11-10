# Himmel – project handoff for Claude Code

Himmel is a local Windows VJ and interactive-installation system played from a
**Native Instruments Maschine MK2 (white)**. It drives 1–6 screens or projectors
from one PC. The owner speaks Norwegian, so talk to him in Norwegian. Everything
inside the app (UI, help, logs, code comments) is in **English**.

## Goals (in priority order)
1. **Show-safe.** It must run a whole night without crashing, and a part that
   fails must never take down the output.
2. **Minimal latency** for video, camera capture and controller input.
3. **Easy to use**, also for someone who has never done VJ work: a show setup
   wizard, Smart Random / "next preset", and a help menu.
4. **Light on the GPU.** Prefer clip effects and cheap generators over heavy
   shaders.

## Hardware
- **PC:** Windows 11 with an NVIDIA RTX 50-series card (5090).
  - The card drives up to 4 displays. For 5–6 outputs you need a splitter
    (Datapath Fx4 / TripleHead2Go) or a second GPU.
  - NVIDIA Surround needs the same resolution, refresh rate and timing on every
    display.
- **Controller:** Maschine MK2, USB `17cc:1140`, vendor HID (more under *Status*).
- **Cameras:** 3 webcams.
- **Default rig:** 3 screens side by side, run either as one canvas or as 3
  separate outputs.

## Agreed stack
| Area | Choice |
|---|---|
| Render | C++20, Direct3D 11, DXGI flip model, waitable swap chain, max frame latency 1 |
| Processes | supervisor → render engine, Dear ImGui operator GUI, workers (decode, analysis, ffmpeg), bridges. A crash in a worker or the GUI must not stop output. |
| Clips | HAP / HAP Q / HAP Alpha via ffmpeg. Keep the source resolution by default (rounded to a multiple of 4), use chunks, offer optional downscaling. |
| Effects | ISF shaders, translated GLSL→HLSL with glslang and SPIRV-Cross; own HLSL for core effects |
| Cameras | Media Foundation (MJPEG/NV12), one capture thread per camera |
| Video I/O | Spout 2 (DX11) and NDI |
| Audio | WASAPI (input and loopback) and ASIO. Two levels: simple (levels, bands, onsets) and full (BPM, beat, phase, sections) |
| Sync | Ableton Link (GPLv2+ – check licensing before release), Pro DJ Link via a beat-link bridge, LTC/MTC timecode |
| Controller | hidapi; libusb/WinUSB (Zadig) as fallback |
| Data | SQLite (library, clip analysis incl. colors and mood) and JSON (shows, presets, themes, cues) |
| ML (optional) | ONNX Runtime + DirectML (segmentation and keying for cameras) |
| GUI | Dear ImGui with dark, light and night themes |

**TouchDesigner is not allowed.** Do not suggest it.

## Main concepts
- **Show:** outputs, mapping (keystone, mesh warp, edge blend), cameras, banks,
  audio and sync sources.
- **Bank:** clips and loops, with analysis (colors, motion, mood such as
  lounge ↔ techno).
- **Deck / layer:** clip or generator + effect chain + mixing.
- **Preset levels:** effect preset → deck preset → session preset. A **theme**
  is a palette and mood that filters Smart Random.
- **Setlist → song → cues → scenes** inside each cue. Cues can be fired by
  timecode or beat.
- **Smart Random:** picks clips, effects and parameters that fit together, from
  the theme, BPM and clip analysis.
- **Output modes:** span, mirror, triptych, chase.

## MK2 control layout (planned)
- **16 pads:** clips / scenes, velocity → intensity.
- **8 group buttons:** decks / layers. **8 knobs:** the parameters on the
  current page.
- **The two 1-bit screens (256×64 each)** show page names, values and VU meters.
- **Shift + Pattern:** theme picker. **Shift + Scene:** save a scene.
- **Note Repeat:** beat-synced stutter.

The details are in the mockup (`docs/vj-mockup.html`).

## Status right now
- **`maschine_mk2_map.json` is verified on the owner's MK2 (2026-11-09).** All
  73 controls (48 buttons, 8 knobs, master encoder turn + push, 16 pads) were
  touched and checked, and all LED groups were checked. This file is the source
  of truth for the controller module. Do not regenerate it; fix single entries
  with the live view below.
  - Device access: hidapi opens HID interface 2 directly on Windows. No Zadig
    needed. Close NI software first.
  - Report `0x01` payload (after the id): bytes 0–5 are buttons, bit 0 first
    (0 Top 1–8, 1 Control…Auto Write, 2 Volume/Swing/Tempo/Master </Master >/
    Enter/Note Repeat/encoder push, 3 Groups A–H, 4 transport, 5 Scene…Mute),
    byte 7 low nibble = master encoder (0–15), bytes 8–23 = 8 knobs as 16-bit
    little-endian values (about 0–1000 per ni-mk2). Knobs and encoder are absolute and wrap: the
    controller must turn them into deltas.
  - **Pads are flipped**: hardware pad id 0 is top-left, so Pad 1 (bottom-left)
    is id 12 and Pad 16 is id 3. Pad LEDs (report `0x80`) follow the hardware id
    order. The pad report streams all the time (~750 Hz).
  - **Pad 12 is worn**: it reads ~120–136 of 4095 when untouched. The map has
    `pad_noise_floor` per pad; the controller must use a per-pad dead zone.
- **Visual mapping is the way to work with the controller. Keep it and build on
  it** (the owner wants graphical tools, not terminal tables):
  - `tools/mk2_live.py` + `tools/mk2_live.html`: a browser live view of the MK2
    front panel (stdlib HTTP server + Server-Sent Events, `.claude/launch.json`
    config `mk2-live`, http://127.0.0.1:8765). Controls light up when touched,
    knobs turn, pads glow by pressure. Click a control on screen and then press
    the real one to relearn it; the map is saved (with a `.bak.json` backup).
    It ticks off every control as "seen" and has LED tests and an on/off chase.
  - Later the operator GUI should have the same idea: a picture of the
    controller, live feedback, and click-then-press learning (MIDI-learn style).
  - The two screens on the page are a 1:1 mirror of what is sent to the MK2
    screens. Modes: Status, Test pattern, Knob page, and the fun modes Demo,
    Snake, 3D maze, Star Swarm (shooter), Munch Maze (maze chase), Sky Shield
    (missile defence: each knob aims the turret under it, Shift + knob = range)
    and the animated "Made by ReadyMedia.no" splash (played at start-up).
    On the device: **Shift + turn the master encoder opens the mode menu, turn
    to choose, push to start.** Without Shift the master encoder is the
    browse/select wheel (owner's decision 2026-11-09); the 8 knobs belong to the
    current mode. Code: `tools/mk2_fun.py`,
    one 512 × 64 image across both screens at ~30 fps. How they work and a
    step-by-step recipe for new modes: `docs/mk2-screen-fun.md`. Check modes
    without hardware with `python tools/mk2_fun_check.py --png`.
- **MK2 screens work (checked on the device 2026-11-09)**, prototype driver in
  `tools/mk2_screens.py`:
  - Two screens, 256 × 64 each, 1 bit per pixel, white-on-blue backlight.
    Report `0xE0` = left, `0xE1` = right.
  - One report draws an 8-row strip: `[id] 00 00 Y 00 20 00 08 00` + 256 bytes
    (8 rows × 32 bytes, row-major), 265 bytes in all. Y = 0, 8 … 56. A full
    screen is 8 strips.
  - Format confirmed with the test pattern: **bit 7 = leftmost pixel, bit set =
    lit pixel, no inversion**, strips in top-to-bottom order. Saved in the map as
    `"screen": {"msb_left": true, "invert": false, "checked": true}`.
  - Speed: one strip write takes ~0.7–0.8 ms over hidapi, so a full frame of both
    screens is ~12 ms. Send only changed strips; ~30 Hz is plenty.
  - Physical fit: the 8 knobs sit under the screens, 4 per screen, but they are
    spaced **wider than 64 px**. From a photo, knob centres are roughly at
    x ≈ 10, 87, 163, 240 on each screen (knob 1 near the left edge, knob 4 near
    the right edge). Place knob labels over those x positions, not in the middle
    of even 64-px columns. Re-measure with a test pattern before final layout.
  - Recommendations: a 5×7 or 6×8 bitmap font (about 42 × 8 characters per
    screen); a top line for the page name; invert the active knob or selected
    item; VU meters as simple bars. Screen drawing belongs in the controller
    module (never the render loop) and must never block reading buttons/pads.
    Use one write lock for LEDs and screens (hidapi on Windows is not safe for
    parallel writes on one handle).
- **Measured 2026-11-09** (MK2 Live → Measure card): pads stream every 1.33 ms,
  knob reports ~10 ms apart, one knob turn ≈ 1008 units, USB high speed. Velocity =
  max pressure in the first 4 ms after onset (the peak does not work). Master
  encoder: 30 detents per turn. Reconnect after a cable pull works. Decided: the master encoder is the
  browse/select wheel in Himmel; Shift + encoder = system menu.
- `tools/maschine_mk2_test.py`: the first terminal mapper (step-by-step learn,
  `--verify`, `--list`). `mk2_live.py` imports its `Transport` and `Mapper`.
  - Known protocol facts:
    - Input report `0x01` carries buttons and knobs (25 bytes).
    - Input report `0x20` carries the pads (65 bytes, 16-bit words: 4-bit id +
      12-bit pressure).
    - LED report `0x80` sets the 16 pads (RGB each).
    - LED report `0x81` sets the 8 group buttons (2 RGB LEDs each), then 8
      transport LEDs.
    - LED report `0x82` sets 31 single-color buttons.
    - LED values are 0–127.
    - The screens use reports `0xE0` / `0xE1`, 1-bit (details above).
  - The LED tables follow ni-mk2 and were checked with the LED tests; names
    marked `*` in the script are kept from before but behaved as expected.
- `tools/maschine_test.py`: an old MK1 test. **Not relevant** (wrong model);
  kept only for reference.
- `docs/vj-mockup.html`: an interactive mockup of the MK2 and the 3-screen wall.
- `docs/gui-mockups/`: GUI sketches (Perform in dark, light and night; Setup
  Outputs; Setup Mapping; Library; Setlist; Effect Browser; Help).
  - These are design-canvas files. Read them for content and layout only; they
    are not production code.
- Full design document (Norwegian): the Claude Doc "Himmel".
  - Its tabs: foundation/stack, effects/themes/library, inspiration
    (Resolume, VDMX, Modul8).
  - Ask the owner for an export if you need it.

## Next steps
1. ~~Run the MK2 mapper with the owner.~~ **Done 2026-11-09**: map verified and
   committed.
2. ~~Write the `controller` module.~~ **Done 2026-11-10, checked on the MK2**
   (pads/velocity colours, aftertouch, Pad 12 quiet, button LEDs, knobs, encoder,
   screens, cable pull + reconnect). Spec: `docs/mk2-controller-spec.md`.
   - Code: `src/controller/` (library `himmel_mk2`, `mk2_controller.exe`, test
     client `mk2_monitor.exe`), unit tests `tests/controller/mk2_tests.cpp`
     (no hardware needed).
   - Shared memory `Local\HimmelMk2`: 4 SPSC event rings (one per consumer),
     seqlock LED/screen outputs, status. Pad stream = heartbeat (250 ms).
   - Build (VS Build Tools 2022, its bundled CMake):
     `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then
     `cmake --build build --config Release`; run `build\bin\Release\mk2_tests.exe`.
     hidapi 0.14.0 and nlohmann/json 3.11.3 come via FetchContent.
   - Close MK2 Live before running `mk2_controller` (both open the device).
3. ~~Screen pages for the 1-bit displays.~~ **Done 2026-11-10, checked on the MK2.**
   - `src/controller/include/himmel/mk2/pages.h` + `src/pages.cpp`: pure drawing
     (no Windows, no heap) of small model structs onto a 512x64 canvas:
     Perform (8 knob slots over the physical knobs, `kKnobX` / `kKnobColumn`),
     list (browser, system menu), detail with dithered thumbnail, audio meters +
     spectrum, big message, knob-align calibration page.
   - Label room: knobs 1 and 5 fit 8 characters, the others 9+.
   - `mk2_pages_check.exe` renders every page to PNG in `tools/_screens/cpp`
     (no hardware); `mk2_pages_demo.exe` drives the pages live from the MK2
     (stand-in for the engine; run `mk2_controller.exe` first).
   - The app renders pages and writes the screens via the shared block; the
     controller only sends strips that changed.
4. **Engine skeleton**:
   - the supervisor starts the render process;
   - the render process opens 1–N outputs;
   - one test clip (HAP) and one camera are mixed and controlled from the MK2;
   - measure glass-to-glass latency.
5. Then: the ffmpeg/HAP import pipeline, audio analysis, the cue system, Smart
   Random and the GUI.

## Working rules
- Keep the render loop free of allocation, locks and disk I/O. Do all
  loading in workers.
- Every module must survive another module dying. Use watchdogs and
  heartbeats.
- Test on real hardware early, and measure latency instead of guessing.
- Ask the owner before you make choices that are hard to undo, such as
  licenses (Ableton Link, NDI) or big dependencies.
