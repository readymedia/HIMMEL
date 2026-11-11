# Engine foundations

Status: agreed direction (owner, voice call 2026-11-11), with details proposed
here. This page covers the render model, cost, latency, stability,
inputs / outputs, tracking, add-ons and Overlay mode. What the engine
skeleton already does is in `docs/architecture.md` and `CLAUDE.md`. This page
says where it is going.

## 1. Targets

| Goal | Target | How we know |
|---|---|---|
| Controller → light | ≤ 1 frame + scan-out (measured 8–12 ms input → present today) | Pad-to-photodiode test |
| Camera in → screen out (glass to glass) | **2–3 frames** (35–50 ms at 60 Hz) on USB/HDMI capture; better with GPU-direct capture | The built-in latency test (Sampling → now in the system menu) and a phone slow-motion check |
| Frame time | ≤ 4 ms of GPU per frame at 1080p on the dev GTX 1070 (warning line); the RTX 5090 has 5–10× headroom | The GPU meter per layer and effect |
| Stability | 8 h soak with 3 cameras, all outputs and Autopilot, with no missed frame above 0.1 % and flat memory | The soak test before every release |
| Dropped frames | **Never skip a frame to save latency.** Stability wins | Frame-timing log per show |

What others do (from the Engine foundations research):
- Disguise buffers 2 frames by default, 1 in low-latency mode and 0 in
  ultra-low mode.
- A typical capture-card path through the CPU measures 4–5 frames
  (about 100 ms at 1080p50).
- Smode builds its own optimised capture paths.

## 2. The render model: decks, layers, levels

```
sources ──► LAYER (clip FX ▸ transform ▸ layer FX ▸ blend/opacity)
              ×4 per deck
           ──► DECK A / DECK B (deck FX = "group" level)
           ──► CROSSFADER (A↔B, curve) ──► MASTER FX ──► MAIN
MAIN / A / B / OWN / TABLE ──► OUTPUT n (output FX ▸ warp/blend ▸ swap chain)
```

- **The levels where effects can sit:** clip (saved with the clip), layer,
  deck, master and output. Every slot works the same way: 4 per level, and
  the knob layout is "1–4 parameters, 5 mix, 6 audio, 7 sync, 8 variation".
- **Blending** is part of the layer, not an effect: normal, add, screen,
  multiply, difference, lighten, luma key and chroma key, with crossfade
  curves and wipe shapes in the mixer.
- **Effects run on every source the same way.** Clips, generators, USB
  cameras, capture cards, Spout / NDI and the TABLE output are all just
  textures to the chain.
- **Off-air decks are cheap.** A deck with crossfader weight 0 that no output
  shows is rendered **only at thumbnail size** (for the GUI and the MK2
  preview), at up to 30 fps.
  - When a TAKE starts, it renders at full size from that frame on.
  - Stateful effects (feedback, trails, echo) keep a full-size buffer, so
    they do not jump on TAKE. Only stateless passes shrink.
- **Nothing that nobody sees is rendered:**
  - a layer at opacity 0 is skipped;
  - a muted layer is skipped;
  - a clip that is not visible keeps one decoded frame warm and stops
    decoding;
  - the TABLE graph runs only if something uses its output.
- **Fusion:** consecutive single-pass effects in one slot chain may be fused
  into one shader at Prepare time (phase 4+). They are never compiled in the
  frame loop.
- **Canvas format:** RGBA8 by default; RGBA16F only where an effect asks for
  it (feedback with long trails, glow).
- **HAP:** the BC textures (BC1 / BC3, HAP Q as YCoCg in BC3) are uploaded
  straight away; decode is a CPU Snappy decompress in a worker (0.2–0.9 ms
  per 1080p frame, measured). 4–8 HAP layers at 1080p are limited by disk
  speed before the GPU.

### 2.1 The render graph (built off the frame loop)

- The **patch / graph worker** turns the model (decks, layers, slots,
  outputs, table) into a flat, sorted list of passes with fixed textures.
- Graphs are **double-buffered:** the worker builds the next one and the
  renderer swaps it in at a frame boundary. Value changes (knob turns) go
  through a lock-free value block and never rebuild the graph.
- Textures come from a **pool sized at start-up** (Prepare Show sets the
  budget), so nothing is allocated in the frame loop.
- The same graph core serves both views: Perform (fixed deck model) and the
  VJ table (free patching). This is milestone M1 of `docs/vj-table-design.md`
  with the deck model as the first "ready-made table".

### 2.2 Cost guide (GTX 1070, 1080p, measured or estimated)

| Thing | Cost |
|---|---|
| HAP layer upload + draw | < 0.2 ms |
| Simple effect (colour, mirror, kaleido, tile, RGB split) | 0.05–0.1 ms (Touch FX measured) |
| Geometry with big zoom-out (grid of 16) | 0.3–0.5 ms (needs mips or a half-size copy) |
| Blur / glow (separable, half-res) | 0.3–0.8 ms (estimate) |
| Feedback / trails | 0.1–0.2 ms + one buffer |
| Generators (noise, plasma, lines) | 0.1–0.5 ms; raymarched 3D look 1–3 ms (tier 3) |
| 4K | about 4× 1080p |

**Budget guard:** if a frame runs late, the most expensive *optional* effects
drop to half resolution before any frame is missed, and the GPU meter turns
yellow.

## 3. Stability rules (from day one)

1. **Nothing is prepared during the show:** shaders are compiled, textures and
   buffers allocated and clips opened before it starts (Prepare Show). The
   frame loop never allocates, locks or touches the disk.
2. **Separate processes:** renderer, controllers (one per weapon), capture,
   audio, ingest, GUI / bridge and copyleft bridges. The supervisor restarts
   what dies (in under 1 s today).
3. **The output never shows the desktop:**
   - black **curtain** windows owned by the supervisor;
   - the last good frame is held;
   - the safe scene is always loaded.
4. **Fail soft:**
   - a broken effect is bypassed;
   - a missing clip shows its neighbour in the bank, or a generator in the
     same colours;
   - a lost camera shows a still or the layer underneath;
   - lost sync glides to the next clock source.
5. **Budget guard** (above).
6. **Soak test** of 8+ hours with real gear before each release; logs and
   frame-timing graphs are saved per show.
7. **Windows show mode:** no updates, no sleep, no notifications, USB power
   saving off, high-performance power plan, a fixed refresh rate, and a
   separate Windows show user is recommended.

## 4. Inputs and outputs (one layer, many drivers)

Every source and destination is a driver behind one interface, so new
hardware never touches the render core. Each driver reports its own measured
latency, so the GUI can show the full glass-to-glass figure for the current
rig.

| Kind | First | Later |
|---|---|---|
| Outputs | GPU outputs (DisplayPort / HDMI) with flip-model swap chains, Spout, NDI | Blackmagic DeckLink / AJA SDI with GPU-direct transfer; SMPTE ST 2110 (NVIDIA Rivermax or similar) |
| Inputs | USB webcams and USB capture cards (Media Foundation), Spout, NDI | DeckLink / AJA / Deltacast capture with GPU-direct (GPUDirect for Video / DVP); ST 2110 receive |
| Sync | Display refresh | Genlock from house sync or the input signal; PTP for ST 2110 |

Where the frames go, and how we save them:

| Step | Typical cost | Himmel's way |
|---|---|---|
| Capture card buffers the input | 1 frame | A callback per frame; the newest frame wins; no queue |
| Copy card → CPU → GPU | 1 frame | GPU-direct transfer (the card writes into GPU memory) |
| YUV → RGB | small | One shader, fused with the first effect |
| Effects and layers | 0.05–0.5 ms per pass | Budget meter; heavy effects flagged |
| Render queue (present waiting) | 1–2 frames | Waitable swap chain, max 1 frame queued (done) |
| Output through an SDI card | 1 frame | Prefer GPU outputs; for SDI use GPU-direct output or a synced HDMI → SDI converter |
| Display / LED processor | 1+ frame | Outside our control; measure and document per venue |

**Genlock:** lock render timing to the input's frame clock (or house sync), so
a frame never waits a whole refresh. An optional **ultra-low** mode
(0 buffered frames) is shown with a warning.

## 5. Audio in the engine

The audio analysis is its own process (WASAPI and ASIO). It writes a small
shared block of signals every ~5 ms (see [`music.md`](music.md)). Effects read
the newest values at frame start, so audio never blocks rendering. Beat
predictions are corrected for the measured output latency (the AV offset).

## 6. Tracking: effects that follow the picture

- **Clips: analyse at import, not live.** Pose, person masks, face and hands,
  and optical flow are computed once by the ingest pipeline and stored as
  sidecars. Playback just reads the data for the frame shown, so tracking
  costs nothing live.
- **Live cameras: beside the render, never in front of it.**
  - A tracking worker (ONNX Runtime on the GPU, or MediaPipe) gets a small
    copy of each camera frame (256–384 px).
  - It returns points and masks a few ms later.
  - The renderer uses the newest result, predicted forward to the current
    frame.
  - The picture latency is unchanged; the tracking trails by about one frame
    (estimate, to be measured).
- **Cheap live helpers:** hardware optical flow and frame-difference motion
  masks.
- **Effects that use tracking:**
  - put graphics behind a person;
  - particles from the hands;
  - a body outline glow;
  - follow the face;
  - motion-driven distortion;
  - auto-crop to the action.
- Tracking is optional per layer, and everything works without it.

## 7. Built-in effects vs add-ons, and generators

- **Two shelves:**
  - **built-in** effects and generators are written and tested by us, ship in
    every release and are show-safe;
  - **add-ons** are ISF shaders, or generators described to Claude. They have
    their own badge, can be switched off in one press, and a show can be
    locked to "built-ins only".
- **Himmel generators** (our answer to Notch): small shader graphs (noise,
  shapes, particles, feedback, 3D-look raymarching, audio inputs) compiled
  into one or a few passes before the show. Each declares its cost, which
  Prepare Show checks. They have the same two main knobs plus the audio and
  sync knobs as effects. We build them in code first; later they can be
  patchable boxes on the VJ table.
- **Describe-a-generator** (no node editor needed):
  1. describe it ("slow purple smoke that pulses on the kick");
  2. Claude writes the shader with two main knobs plus audio / sync inputs;
  3. the **safety gate** compiles it, measures the GPU cost, checks for
     flicker and renders a preview loop;
  4. you tune and save presets;
  5. it lands on the add-on shelf. Promotion to built-in needs a soak test.

## 8. Overlay mode (separate show type)

For shows run through a production mixer or scaler, where musicians need
perfect lip sync, **the live picture never passes through Himmel.**

1. The production cameras go into the mixer as usual. The mixer's **program
   out** (or a clean feed) goes into Himmel through a capture input.
2. Himmel analyses that picture live (motion, people, masks, beat) and
   renders **only the effect layer** on a transparent background.
3. Himmel outputs **fill + key** (the colours, plus the alpha as a
   black-and-white picture) on two SDI outputs, genlocked to the mixer. NDI
   with alpha is the option where one or two more frames do not matter.
4. The mixer's **downstream keyer** (a linear key, the "PNG way") puts the
   effects on top. **Stage colours do not matter:** the mixer only follows
   our key signal. Chroma / luma keying is a fallback only.
5. If Himmel stops, the mixer simply shows the clean live picture.

What suits this mode:
- effects that tolerate 2–3 frames of lag (particles, glows, light leaks,
  frames, beat flashes, text, loose motion-followers);
- for effects locked to a person, use tracking prediction, or let the mixer
  delay its program by the same 2–3 frames;
- whole-picture effects (kaleido, colour) need Himmel's full picture as a
  normal mixer input instead.

Tracking in this mode stays soft and forgiving (haze, strobes and stage light
confuse it), with a one-press off switch.

Timeline: after inputs / outputs (roadmap phase 2), when an SDI card is
available.

## 9. Processes (target)

| Process | Job | State today |
|---|---|---|
| `himmel.exe` | Supervisor: starts, watches and restarts everything; owns the curtains | Exists (curtains to do) |
| `himmel_render.exe` | Render graph, outputs, previews to shared memory | Exists (fixed layers) |
| `mk2_controller.exe` | MK2 driver, `--sim` | Exists |
| `weapon_blocks.exe` | Lightpad / LUMI driver (BLOCKS over WinMM), `--sim` | Planned (Python prototype exists) |
| `weapon_midi.exe` | Generic MIDI weapons (NuVJ, Ohm64, Entrancer, V-4, …) | Planned (Python prototypes exist) |
| `himmel_audio.exe` | WASAPI / ASIO analysis, clock arbitration | Planned |
| `himmel_ingest.exe` + ffmpeg | Import, convert, analyse, index | Planned |
| `himmel_ui` | GUI bridge: serves the web GUI, relays commands / state / previews | Planned (decision pending) |
| `link_bridge.exe`, `prodj_bridge` | Ableton Link (GPL) and Pro DJ Link (beat-link, EPL) in their own processes | Planned |

All of them talk through named shared-memory blocks (SPSC rings for events and
commands, seqlock blocks for state) as `HimmelMk2` does today. Formats live in
`docs/interfaces/`.
