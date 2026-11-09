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
- `tools/maschine_mk2_test.py`: a hardware test and **mapper**. It learns the
  byte and bit of every button, knob and pad, tests all LEDs, and saves the
  result to `maschine_mk2_map.json`.
  - It has **not been run against the real device yet**. That is step 1.
  - Known protocol facts:
    - Input report `0x01` carries buttons and knobs (25 bytes).
    - Input report `0x20` carries the pads (65 bytes, 16-bit words: 4-bit id +
      12-bit pressure).
    - LED report `0x80` sets the 16 pads (RGB each).
    - LED report `0x81` sets the 8 group buttons (2 RGB LEDs each), then 8
      transport LEDs.
    - LED report `0x82` sets 31 single-color buttons.
    - LED values are 0–127.
    - The screens use reports `0xE0` / `0xE1`, 1-bit (see github.com/abique/ni-mk2).
  - LED names marked `*` in the script are guesses. The chase test (key 4) shows
    the truth.
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
1. **Run `python tools/maschine_mk2_test.py`** together with the owner.
   - Fix any device-access problem (hidapi → Zadig/WinUSB).
   - Go through all the steps, then check the LED chase.
   - Commit `maschine_mk2_map.json` and correct the LED tables.
2. **Write the `controller` module** (its own process or thread):
   - read the MK2 at ≥ 500 Hz;
   - publish events (button, knob delta, pad velocity / aftertouch) over a
     lock-free queue or shared memory;
   - accept LED and screen commands;
   - reconnect on its own if the USB cable is pulled.
3. **Screen driver for the 1-bit displays**: render text and meters, dither
   images, invert highlighted areas.
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
