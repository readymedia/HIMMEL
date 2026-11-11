# Himmel roadmap

Updated 2026-11-11. This file is the plan; `CLAUDE.md` keeps the facts and
decisions. When a step is done, strike it through here and note the details
in `CLAUDE.md`.

## Decisions this plan builds on

- **Goals, in order:** show-safe, low latency, easy to use, light on the GPU.
- **Main instrument = the MK2 and the Lightpad Block together** (owner,
  2026-11-11).
  - The other controllers are **integrations**: LUMI, Entrancer, NuVJ, KF4,
    V-4 and Ohm64. They plug into the same weapon layer and get a simulator
    and a map. They do not steer the design, and they come after the main
    instrument.
- **Weapons:** a controller = profile + map + driver + simulator
  (`weapons/README.md`). Anything we don't own can be simulated.
- **Visual tools first**, not terminal tables: live view, click-then-press
  learning, mirrored screens and LEDs.
- **Browser first** for the table view; Dear ImGui comes later (owner,
  2026-11-10).
- **Effect set after the engine's effect chain exists** (owner, 2026-11-10).
- **Open source (MIT).** Credit everything. Copyleft parts (Ableton Link,
  beat-link) go in separate bridge processes.

## Where we are (main + branches, 2026-11-11)

| Area | State | Where |
|---|---|---|
| MK2 | Map verified; C++ controller with shared memory, LEDs, screens, reconnect; screen pages; games; simulator that plays the engine | `main` |
| Engine | Supervisor with restarts; D3D11 with 1–8 outputs (span/mirror); HAP player (own MOV reader); camera layer; latency test (not run yet) | `main` |
| Lightpad (no. 2) | Simulator, WinMM driver in Python, our own LittleFoot LED renderer on the real block (~55 fps), pressure set to 127, games | branch `weapon-2-sim` (also in the branches below) |
| Touch FX | Gesture logic + one shader pass, 9 effects (Hue Turn, Duotone, Grid, Kaleido, Bulge, Slide, Waves, Strobe, Trails), 0.05–0.5 ms; played from MK2 page 4 and the mouse | branch `entrancer-touch-fx` |
| LUMI Keys (no. 3) | Simulator; keys, buttons and LEDs checked on the device; key map not verified | `weapon-2-sim` |
| Entrancer (no. 4) | Simulator that plays Touch FX in the engine, Lightpad as its pad; real unit over MIDI | `entrancer-touch-fx` |
| NuVJ (no. 5) | Simulator, protocol read from the old software, effect catalogue, **classic two-deck view** | `weapon-5-nuvj` |
| KF4 (no. 6) | Simulator only (the unit has no data port) | `weapon-6-kf4` |
| V-4 (no. 7) | Simulator drawn like the real one, full effect catalogue | `weapon-roland-v4` |
| Ohm64 (no. 8) | Simulator, probe, map readback; hardware test planned for 2026-11-11 | `weapon-ohm64` |
| VJ table | Design proposal + mockup: patch the boxes on a table, the room is the output | `rack-design` |

Merge check (2026-11-11):
- `rack-design` and `entrancer-touch-fx` merge into `main` without conflicts.
- The NuVJ, KF4, V-4 and Ohm64 branches conflict only in docs and config
  (`CLAUDE.md`, `weapons/README.md`, `THIRD_PARTY_NOTICES.md`,
  `.claude/launch.json`), not in code.

---

## Phase 0: gather everything in `main` (small, do first)

1. Merge in this order:
   1. `rack-design`, which brings in `weapon-2-sim` (Lightpad and LUMI);
   2. `entrancer-touch-fx`;
   3. `weapon-5-nuvj`;
   4. `weapon-6-kf4`;
   5. `weapon-roland-v4`;
   6. `weapon-ohm64`.

   Resolve the doc conflicts by keeping all sections, and give every live
   view its own port in `launch.json`. After each merge, build and run
   `mk2_tests`, `media_tests`, `touch_fx_tests`, `entrancer_tests` and the
   Python `--selftest`s.
2. **One numbering list** of the weapons in `weapons/README.md`, matching
   `CLAUDE.md`:
   1 MK2, 2 Lightpad, 3 LUMI, 4 Entrancer, 5 NuVJ, 6 KF4, 7 V-4, 8 Ohm64.
   Mark each one as *main* (1–2) or *integration* (3–8).
3. **Slim down `CLAUDE.md`:** move the per-weapon details to
   `weapons/README.md` and keep a short status table plus a pointer to this
   roadmap.
4. Put the full design spec (the "Himmel" doc, exported as Markdown) in
   `docs/design/`, so every session works from the same text.
5. Delete the merged branches.

## Phase 1: the main instrument, MK2 + Lightpad as one rig

Goal: the owner plays the engine with both controllers at once, the real
devices or their simulators, and a pulled cable never stops the output.

1. **Generic weapon layer in C++** (the plan in `weapons/README.md`):
   - shared block `Local\HimmelWeapon<N>` with the same layout as
     `HimmelMk2`, plus a header with the weapon id;
   - new events `TouchStart` / `TouchMove` / `TouchEnd`;
   - the surface can be split into zones that send `PadHit` /
     `PadPressure`;
   - MIDI CC to `KnobDelta`.
2. **`weapon_blocks.exe`**, the Lightpad driver in C++. It uses WinMM, so no
   new dependency.
   - API mode, LittleFoot upload, LED frames through HeapSync, pings on
     their own timer.
   - It restores the pressure setting when it exits.
   - `--sim` over UDP 8767.
   - The supervisor starts it next to `mk2_controller.exe`; heartbeat and
     restart work the same way.
3. **Split the roles between the two** (proposal for the owner; see the open
   decisions):
   - **MK2 = the brain.** Browse and select, scenes and cues on the pads,
     knob pages, the screens, Shift menus, transport.
   - **Lightpad = the hand.** Touch FX (X = effect, Y = amount,
     pressure = depth, more fingers = more parameters), Hold on the Mode
     button, motion loop. Alternatively a 4×4 scene surface with velocity.
   - **Together:** the MK2 Touch FX page with a 15×15 mirror of the Lightpad
     LEDs (sketch: `docs/touch-fx-entrancer.md` §8). Knobs 1–4 choose the
     effect and amount; knobs 5–8 set Hold, Loop, Beat and Target.
4. **The LEDs show state on the Lightpad:** cursor, held cross, loop path and
   beat pulse, in the colour of the chosen effect.
5. **Touch FX step 2:** beat-locked motion loop, a second finger as the
   third parameter, scrub on clips, then Stutter / Echo Grid (history ring).
6. **Combined live view:** both weapons on one page, simulated or real, with
   learning and mirrors.
7. Check on the hardware: latency from touch to picture, a cable pulled from
   each one, an 8-hour soak with both connected.

## Phase 2: a show-safe engine core

1. Black "curtain" windows held by the supervisor, so a renderer restart
   shows black instead of the desktop.
2. Run the latency test with an external webcam and log the figures in
   `CLAUDE.md`.
3. **Effect chain / render graph = VJ table milestone M1**
   (`docs/vj-table-design.md` §10).
   - `src/table/` with the patch worker, value block, and unit tests without
     a GPU.
   - `--table file.json` with hot reload.
   - Measurements and failure tests: a bad shader, a missing clip, a camera
     pulled out, a loop in the file, the renderer killed.
   - Today's fixed layers (test card, clip, camera, Touch FX) become the first
     ready-made table.
4. Decks / layers: either as groups on the table, or as a fixed deck model
   if the owner chooses that (open decision 1).

## Phase 3: content (clips and effects)

1. **Import pipeline:**
   - ffmpeg.exe in a worker turns clips into HAP (keeping the resolution,
     rounded to a multiple of 4);
   - analysis: colours, motion, energy, loop quality, flicker warning;
   - SQLite library;
   - thumbnails and preview loops;
   - Hap Q Alpha.
2. **Effect set**, one cheap pass each, with two named main parameters and
   speed parameters:
   - start: Hue, Mirror, Kaleido, Tiling, RGB split, BPM strobe, Tunnel,
     RotoZoom, Ripple, Edge, Posterize, Threshold, Scroller;
   - feedback: Motion Blur, Larsen / Twirl, Frame Delay;
   - audio: Bass Zoom, EarthQuake;
   - V-4 pedals: negative, colorize, monocolor, posterize, colorpass,
     mirror, multi, strobe, still, shake;
   - mixer: copy modes, luma / chroma key, crossfade curves, mask wipes.

   Sources: `docs/nuvj-effects.md`, `docs/v4-effects.md`,
   `docs/touch-fx-entrancer.md` and the 42-effect catalogue in the design
   spec.
3. **Generators:** gradient, noise / fBm, plasma, lines and rings, shapes,
   particles, spectrum, text, starfield, Voronoi.
4. **ISF loader** (glslang + SPIRV-Cross) for user effects as "ISF pedals".

## Phase 4: music

1. Audio analysis via WASAPI (input + loopback): level, bands, onset, BPM,
   beat phase, drop detector. It replaces the fixed 120 BPM in Touch FX.
2. A clock item on the table: tap, MIDI clock, and an AV offset.
3. Bridges: Ableton Link (GPL bridge process), Pro DJ Link (beat-link
   bridge), LTC/MTC.
4. Audio signals on knobs (Auto Write + knob), shown as meters on the MK2
   and in the GUI.

## Phase 5: show structure

1. **Themes** (12 to start), **scenes** (16 per cue on the energy grid) and
   **macros** (8 knobs: Energy, Colour, Speed, Density, Chaos, Glow, Space,
   Reaction).
2. **Setlist → song → cues:** triggered by hand, on a bar, on timecode, on a
   drop, or from the DJ's track.
3. **Presets** at the effect, deck/item and session levels; morphing; undo.
4. **Smart Random / next preset**, following the rules in the design spec
   (fits the clip, the theme, the music and the GPU budget; never the same as
   the last 10).
5. **Assist / Autopilot** and the panic button (Shift + Mute = a safe scene).
6. **Prepare Show:** convert, read every clip, compile shaders, budget, warm
   up.

## Phase 6: operator GUI

1. **Table view in the browser** (M2): lay out and patch items, monitors,
   and play from both weapons.
2. Dear ImGui GUI: Setup / Prepare / Perform; dark, light and night themes;
   a controller picture with click-then-press learning.
3. Setup wizard in 7 steps, Help menu built from the same descriptions as
   the code, and Explain Mode on the MK2.
4. **NuVJ classic two-deck view** as an alternative Perform view (the
   owner's nostalgia wish), using the same model as everything else.

## Phase 7: projection and outputs

1. **Room view** on the table: projectors that are LIVE → output N, and the
   wall with screens.
2. Keystone, mesh warp, edge blending and masks, set from the MK2 or the
   mouse; the Setup Mapping mockup.
3. Up to 6 outputs (splitter or a second GPU), triptych and chase modes.
4. Spout 2 and NDI in and out.
5. Show mode in Windows (power, USB suspend, notifications, updates) and
   autostart.

## Phase 8: integrations (other weapons)

Each one gets the generic weapon layer from phase 1, a verified map where
we own the unit, and a box or controller on the table.

| Weapon | Next step |
|---|---|
| LUMI Keys (3) | Verify the key map key by key; keys as a scene or colour keyboard; BLOCKS touch per key |
| Entrancer (4) | The real unit over MIDI in the C++ layer; Touch FX shared with the Lightpad |
| NuVJ (5) | Test on the owner's unit: LEDs, LCD segments, map |
| KF4 (6) | No data port: capture its output as a camera, or use it as an analog mixer after Himmel |
| V-4 (7) | Simulator only (the owner has none); the V-4 box on the table is the worked example in M1/M3 |
| Ohm64 (8) | Hardware test with the owner, then the map |
| Generic MIDI | One profile + learning for any MIDI controller |

## Always

- Easter eggs and games on the screens and on the Lightpad, as long as they
  never get in the way of a show.
- Keep `README.md`, `docs/getting-started.md`, `THIRD_PARTY_NOTICES.md` and
  the licences up to date in the same commit as each change.
- Measure instead of guessing; test on the hardware early.

## Open decisions for the owner

1. **Is the table the main way to build a show?** In that case decks and
   layers become groups on the table. This changes the main concepts in
   `CLAUDE.md`. (Table design §11.1.)
2. **The roles of MK2 and Lightpad** as proposed in phase 1.3. Should the
   Lightpad be Touch FX by default, or the scene surface?
3. Names and format for the table (`himmel-table/1`, "item", "pedal"). They
   become public, so changes later mean migrations.
4. GPU budget: a warning at 4 ms per frame at 1080p on the GTX 1070.
5. Should the merge in phase 0 happen now, in the order given?
