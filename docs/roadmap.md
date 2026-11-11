# Himmel roadmap (v2)

Updated 2026-11-11. This page is **the plan**. The *why* is in
[`design/`](design/vision.md), the decisions are in
[`decisions.md`](decisions.md), and the hardware facts are in
[`../CLAUDE.md`](../CLAUDE.md). Who builds what, and how, is in
[`agents/README.md`](agents/README.md). Strike items through when they are
done, and note the details in the track's handoff.

**The goal for mid-January 2027:** the owner plays a real gig with the MK2 +
Lightpad:
- 2 decks × 4 layers of HAP clips and cameras, with a crossfader;
- shuffle with locks, history and one-press save;
- scenes and cues;
- effects that react to the music;
- 3 outputs.

All of this at the lowest latency we can measure, from a 4-step wizard start,
and without one dropped frame in an 8-hour soak.

## 1. What we have to build on

| Exists | Where | Becomes |
|---|---|---|
| Verified MK2 map, C++ controller (events ~750 Hz, LEDs, screens, reconnect), `--sim` over UDP | `main`: `src/controller`, `maschine_mk2_map.json` | The MK2 driver, unchanged; the Perform mapping is added in the input layer |
| MK2 screen pages (pure drawing, knob-aligned) | `main`: `pages.cpp` | Every MK2 page in `design/hardware-mapping.md` §1.5 |
| Supervisor (restarts < 1 s, heartbeats, logs) | `main`: `src/engine/supervisor` | The supervisor of every process; curtains to add |
| D3D11 renderer, 1–8 outputs, span/mirror, waitable flip-model, input → present 8–12 ms | `main`: `src/engine/render` | The output stage of the render graph |
| Own MOV reader + HAP decoder, ClipPlayer (4 frames ahead, BC upload) | `main`: `src/media`, `clip_player.cpp` | The clip source for every layer |
| CameraCapture (Media Foundation, NV12, triple buffer, retry) + latency test | `main` | The camera / capture source; the latency test moves to the system menu |
| Touch FX: 9 one-pass effects + the gesture logic, tests, timing tool | branch `entrancer-touch-fx` | The first effects in the slot system; Lightpad TOUCH mode |
| MIDI input (WinMM), Entrancer link | branch `entrancer-touch-fx` | `weapon_midi.exe` |
| Lightpad: sim, WinMM driver, our own LED renderer (~55 fps), games | branch `weapon-2-sim` (in rack-design too) | Prototype for `weapon_blocks.exe` (C++) |
| LUMI, NuVJ (+ classic view model), KF4, V-4, Ohm64: profiles + sims | branches `weapon-*` | Integrations (phase 13) |
| Browser live view that draws any weapon from its profile, with learn / sim | `main` + branches: `tools/mk2_live.*` | The weapon renderer of the GUI (weapon select, weapon band, audience view) |
| VJ table design + mockup | branch `rack-design` | Phase 10 |
| Effect catalogues (NuVJ, V-4, Entrancer) | branches | `design/effects-and-generators.md` |

## 2. Tracks (they run in parallel)

| Track | Short | Owns | First job |
|---|---|---|---|
| LEAD | L | Plan, merges, interfaces, decisions | Phase 0 |
| ENGINE | E | Render graph, decks, outputs | Phase 1 |
| FX | F | Effects, generators, ISF, safety gate | Phase 4 |
| MEDIA | M | Ingest, analysis, library | Phase 3 |
| SHOW | S | Presets, shuffle, moods, cues | Phase 5 |
| CONTROL | C | Weapons, mapping, Lightpad driver | Phases 0–2 |
| AUDIO | A | Analysis, clocks, bridges | Phase 7 |
| GUI | G | Web GUI and bridge | Phase 6 (once D43 is decided) |
| QA | Q | Latency, soak, release checks | All along |

## 3. Timeline (weeks are estimates; ISO weeks 2026–27)

| Week | L | E | F | M | S | C | A | G | Q |
|---|---|---|---|---|---|---|---|---|---|
| 46 (now) | **P0 merge**, interfaces | P1 design | — | — | — | P0 merge help | — | mockups v2 ✓ | latency test |
| 47 | P1 command protocol | **P1 decks / layers / slots** | metadata | **P3 ingest** | model + formats | P1 mapping layer | — | wait for D43 | soak rig |
| 48 | merges | P1 graph + previews, **P1c clip transport**, **P2 I/O** | **P4 tier 1** | P3 analysis | **P5 scenes / save** | P2 weapon block | **P7 audio** | **P6 bridge + wizard** | soak 2 h |
| 49 | merges | P2 outputs, curtains, sampler | P4 tier 1 (+ time FX) | P3 DB + browse, loop analysis | P5 shuffle v1, variants | **P1c pads: SLICE, PRESS**, P8 Lightpad C++ | P7 clocks | P6 Perform v1 | soak 8 h |
| 50 | | budget guard | P4 tier 2, ISF | starter library | P5 moods ×16 | P8 modes | Link / ProDJ bridges | P6 library + shuffle UI | latency rigs |
| 51 | release 0.1 | polish | safety gate | background throttling | **P9 cues / songs** | roles / NuVJ | reactions | P6 show view | release check |
| 52–1 | holidays / buffer | | | | P9 Assist / Autopilot | | | P6 audience view | |
| 2–3 | **gig test** | fixes | tier 2 | tracking v1 | P9 Prepare Show | | | polish | 8 h soak, gig |
| 3–6 | | **P10 table core** | add-ons | | | P13 integrations | | **P10 table view** | |
| later | | P11 mapping, P12 overlay / SDI | tier 3 | | | P13 | | | |

## 4. Phases, step by step

### Phase 0: gather everything (L + C, week 46, 1–2 days)

1. Merge into `main`, in this order:
   1. `rack-design` (brings in Lightpad + LUMI);
   2. `entrancer-touch-fx`;
   3. `weapon-5-nuvj`;
   4. `weapon-6-kf4`;
   5. `weapon-roland-v4`;
   6. `weapon-ohm64`;
   7. `roadmap` (this plan).

   The only conflicts are in docs and config (checked 2026-11-11). Keep all
   sections, and give each live view its own port in `.claude/launch.json`.
2. Build, then run `mk2_tests`, `media_tests`, `touch_fx_tests`,
   `entrancer_tests` and every Python `--selftest`.
3. One weapon list (1 MK2, 2 Lightpad = **main**; 3–8 = **integrations**).
   Slim `CLAUDE.md`: move the per-weapon detail to `weapons/README.md`.
4. Delete the merged branches. **From now on: merge within 1–2 days**
   (`agents/README.md` §3).

### Phase 1: engine core, decks and layers (E, weeks 46–48)

1. **The deck model:** Deck A / B × 4 layers. A layer is a source + transform
   + blend + opacity + 4 FX slots + reactions + clip FX.
2. **The crossfader** A ↔ B into MAIN, with curves, TAKE (timed,
   quantisable) and CUT, and the decks swapping roles after a TAKE.
2b. **The canvas model:** canvas size from the outputs (screens butt,
   projectors overlap); layer places (FILL ALL, OUT n, FIT, REPEAT, FREE)
   (`design/engine.md` §2.0).
3. **Effect slots at every level** (clip, layer, deck, master, output).
   Touch FX moves into a slot.
4. **The render graph** is built in a worker and swapped at frame boundaries;
   the texture pool is sized at start-up; value changes go through a
   lock-free block. Unit tests run without a GPU.
5. **Off-air deck at thumbnail size;** layers that nobody sees are skipped.
6. **The command ring + state block + preview block** (with L:
   `interfaces/command-protocol.md`, `preview-block.md`).
7. **The MK2 Perform mapping** (with C): groups → layers, Play / Restart =
   TAKE / CUT, pages MACRO, LAYER, FX1–4, MIX; this replaces the skeleton's
   temporary pages.
8. **GPU meter** per layer and effect, on the MK2 screen and in the status
   block.
9. **Done when:** 2 decks × 4 HAP layers + 8 effects play from the MK2, TAKE
   and CUT work, and a 2-hour soak has no missed frame.

### Phase 1c: clip transport and the pads as an instrument (E + C, weeks 48–49)

Spec: `design/clips-and-playback.md`.

1. **The playhead state machine** in the clip player's worker: direction,
   play modes (LOOP, PING-PONG, ONCE hold / eject / next, RANDOM, CHOP, STEP,
   SCRATCH), in / out, inner loop, 16 cue points, seeded randomness.
2. **Time base:** FREE / BPM (range = n beats) / TIMECODE; speed glide;
   beat loop with catch-up (shadow playhead).
3. **Triggering:** NORMAL / TOGGLE / PIANO / LEGATO, restart / continue,
   fade in / out per clip, beat snap (PROGRAM only), clip target (own / free),
   the HIT layer, autopilot per layer, layer transitions.
4. **Zero-latency jumps:** the first frame of each cue / slice kept warm for
   the selected clip and for PROGRAM clips.
5. **Effect levels + parameter scopes** (SLOT / LOOK / GLOBAL / MOVE) and
   clip variants A–D in the model; MOVE modulators (LFO, envelope, steps).
6. **MK2 (C):** SLICE mode (`PATTERN` twice), PRESS (velocity start,
   aftertouch move, release), `SOLO` + pad = HIT, `SHIFT` + pad = load and
   TAKE, FX-pad latch and velocity variation, the CLIP knob page with the
   timeline on the left screen, `SELECT` + wheel = scratch.
7. **Done when:** on the MK2 alone you can set in / out on a clip, chop it
   from the pads on the beat, squeeze a strobe harder and faster, and fire a
   hit over PROGRAM, all with input → glass under 3 frames.

### Phase 2: inputs and outputs (E + C, weeks 48–49)

1. USB cameras and USB capture cards as layer sources (3 cameras, a fallback
   picture when one is unplugged); effects work on them exactly as on clips.
2. Spout in / out; NDI later.
3. **The source per output:** MAIN / A / B / OWN / TABLE; per-output FX;
   the outputs wizard (screens vs projectors, 1 / 2 / 3, blend overlap → canvas);
   black-out per output.
4. **Curtain windows** in the supervisor; the safe scene is always loaded.
5. The latency test is run and logged (Q); the driver latency is reported per
   output.
6. **The generic weapon block** `Local\HimmelWeapon<N>` (C), with touch
   events and roles.
7. **The live sampler:** record N beats of a camera / input / PROGRAM into a
   looping clip (`REC` held + empty pad), written to HAP in the background.

### Phase 3: ingest pipeline and library (M, weeks 47–50)

1. `himmel_ingest.exe`: probe → ffmpeg HAP (keep resolution, ×4, chunks) →
   sidecar → SQLite. Resumable stages.
2. Analysis v1:
   - palette;
   - brightness / contrast / saturation;
   - warm / cool;
   - motion;
   - cuts;
   - loop quality, best loop points, loop style, slices, natural beats,
     motion curve (`design/library-and-analysis.md` §3);
   - flicker;
   - bars;
   - energy;
   - mood scores (rules in `library-rules.json`).
3. Background mode: idle priority and I/O throttling, paused near the frame
   budget (it reads the engine status block).
4. Thumbnails + hover loops; MK2 `BROWSE` with filters; load to a PREVIEW
   layer.
5. Optional type tags + similarity (ONNX, a CLIP-style model). Ask before
   adding the dependency (PROPOSED).
6. The starter library (~50 free clips + generator looks).
7. **Done when:** a dropped folder of 100 mixed clips becomes ready, tagged
   and browsable, while a show plays without one late frame.

### Phase 4: the first effect and generator set (F, weeks 48–51)

1. The effect metadata format (`interfaces/effect-metadata.md`), with `press`,
   `scopes` and `subtle` / `extreme` presets.
2. Tier 1 effects (~30) and tier 1 generators (~12) from
   `design/effects-and-generators.md`, each with a PNG + timing check,
   including the **text generator** (§5 there: sources, styles, SDF glyphs).
3. FX pads set; default reactions.
4. The ISF loader (glslang + SPIRV-Cross) on the **add-on shelf**, behind the
   safety gate: compile, cost, flicker check, preview loop.
5. Tier 2 after that.

### Phase 5: presets, shuffle, scenes (S, weeks 47–50)

1. Show file formats (`interfaces/show-files.md`).
2. Scenes / banks: save (`REC`), recall to PREVIEW / LIVE, morph, copy.
3. The **shuffle sampler v1:**
   - scopes (DECK / LAYER / FX SLOT / KNOB);
   - 8 locks;
   - a seeded history of 64;
   - ROLL / BACK / MUTATE;
   - constraint rules (library fit, effect compatibility, GPU budget,
     brightness).
4. 16 mood files on the 4 × 4 map; 8 macros per mood.
4b. **ROTATE** saved presets on every level, `REC` saves at the level,
   auto-rotate every n bars (`design/presets-and-shuffle.md` §2).
5. "Fill bank from mood".
5b. Clip variants A–D on the dice / ROTATE / `REC`; the MOTION lock holds the
   transport.
6. **Done when:** a beginner can make a good 16-scene bank in under
   2 minutes with `>` and `REC`.

### Phase 6: GUI v1 (G, weeks 48–52; needs D43)

1. The `himmel_ui` bridge (web app server + command / state relay + preview
   frames).
2. The wizard (4 steps) and the weapon select ("touch your weapon to choose
   it", loadouts, demo).
3. **Perform:**
   - the top bar;
   - three monitors with tallies;
   - the crossfader with TAKE / CUT;
   - the layer strips;
   - the pads;
   - the MK2 screen mirror + knob strip;
   - the shuffle panel;
   - the outputs band with "show next";
   - the weapon band;
   - **the clip editor drawer** (timeline, in / out, cues, transport chips,
     effect levels with scope badges, PRESS, variants; TRY / A-B / AUDITION;
     `design/gui.md` §16).
4. Library view, Show view, Outputs view.
5. The audience view (the weapon drawing on a second screen, with Explain).
6. The weapon-adaptive layouts: main weapon (MK2, NuVJ, keyboard) +
   side-arm cards; the monitors follow the canvas shape; optional second
   GUI screen (`design/gui.md` §8, §14).

### Phase 7: music (A, weeks 48–52)

1. `himmel_audio.exe`: WASAPI in + loopback, ASIO;
   - bands, onsets (kick / snare / hat), BPM / phase / bar / phrase;
   - energy, build / drop.
2. Clock arbitration with glide; tap; MIDI clock; LTC / MTC.
3. Link bridge (GPL) and Pro DJ Link bridge (beat-link), each in its own
   process.
4. Reactions on every parameter; the REACT macro; the AV offset test.

### Phase 8: Lightpad as the hand (C, weeks 49–50)

1. `weapon_blocks.exe` (C++, WinMM):
   - API mode;
   - LittleFoot upload;
   - HeapSync LED frames;
   - restoring the pressure setting on exit;
   - `--sim`;
   - supervised.
2. Modes TOUCH / MIX / SCENES / MOOD / **SCRUB** with LED feedback (red
   border on LIVE).
3. The MK2 Touch FX page with the 15 × 15 LED mirror.

### Phase 9: show structure and the first gig (S + Q, weeks 51–3)

1. Setlist → songs → cues; triggers (manual, bars, timecode, drop, DJ track).
2. Assist / Autopilot; panic; safe mode (strobe limits).
3. Prepare Show (checklist + report + pack).
4. An 8-hour soak and the **first real gig with the owner.**

### Phase 10: VJ table (E + G, weeks 3–6)

1. `src/table/` on the same graph core; `himmel-table/1`; hot reload.
2. The table view in the browser: boxes, cables, monitors, room view; the
   TABLE source routable into a layer, an output or MAIN; switching views
   without a gap.

### Phase 11: projection mapping

Keystone, mesh warp, edge blend and **pen-tool masks with a crosshair on
the real output** (the Block as a trackpad), per output; room view; up to
6 outputs (splitter / 2nd GPU). Simple edge blend for the 3-projector
canvas can come earlier if a gig needs it.

### Phase 12: Overlay mode and pro I/O

DeckLink / AJA with GPU-direct; fill + key out, genlocked; NDI with alpha;
live analysis of the program feed; soft tracking; **recording the output to
a file** (HAP / ProRes, a worker, never the render loop).

### Phase 13: integrations

| Weapon | Work |
|---|---|
| NuVJ | Standalone classic two-deck mode on the engine (hardware test first) |
| LUMI | Key map |
| Entrancer | Touch FX shared with the Lightpad |
| Ohm64 | Hardware test 2026-11-11, then the map |
| V-4 | The table box |
| KF4 | Capture as a camera |
| Generic MIDI | Learn |
| DMX / Art-Net in | A lighting desk drives looks, dimmer and FX (optional) |
| Clip audio | Optional playback for installations (D66) |

## 5. Open questions (see `decisions.md`)

D42 mapping changes · D43 GUI technology · D44 Lightpad modes · D45 the 16
moods · D46 layers per deck · D47 merge now · D61–D67 clip transport and
pads.
