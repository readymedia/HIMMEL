# Decision log

Owner decisions and the big technical choices, newest first. **Only the owner
makes owner decisions.** An agent that needs one writes it here as
**PROPOSED**, with the options, and asks. Then it either waits or works on
something else. Each entry has a date, a status and a link to where the
details live.

Status values:
- **DECIDED**: the owner decided;
- **PROPOSED**: waiting for the owner;
- **TECH**: a technical choice by an agent, which can be reversed;
- **REPLACED**: superseded by a later entry.

## 2026-11-11

| # | Decision | Status | Details |
|---|---|---|---|
| D68 | **Two flows, one surface:** PREP → TAKE and PLAY both always available; **focus = the side of the last group pressed**; pads, knobs 1–4 and pages act on the focus; **knobs 5–8 always PROGRAM**; "LIVE EDIT" renamed PLAY focus | DECIDED (owner: both flows, super flow); the mapping is PROPOSED | `design/perform.md` §1 |
| D69 | **Scene = look + kit + 4 macros + transition + variations**; a song holds up to 16 scenes; LOOKS pads show the song's scenes | PROPOSED | `design/perform.md` §2 |
| D70 | **Scenes land where the focus is**; `ENTER` = next scene live on the beat; `SHIFT` + `ENTER` = into PREVIEW | PROPOSED | `design/perform.md` §2 |
| D71 | **Pad targets** SELECTED / LAYER 1–4 / ALL / FREE / HIT (hold pad + group); BANK kit vs SCENE kit (`SHIFT` + `PATTERN`); moods move to `SHIFT` + `SCENE` | DECIDED (owner: pads choose their layer, one or all); the mapping is PROPOSED | `design/perform.md` §4 |
| D72 | **Live undo / reset:** `ERASE` = undo (64), hold `ERASE` + group = reset the layer to the scene; `REC` in PLAY focus = a new variation, never an overwrite | PROPOSED | `design/perform.md` §3 |
| D73 | **NuVJ:** the crossfader decides PROGRAM (no focus); wheel modes BEND / SCRATCH / FX WHEEL on `SCRATCH`; hold pad + wheel = trim; hold `SYNC` + wheel = beat loop; `INSTA` = auto fade | PROPOSED | `design/perform.md` §5.2 |
| D74 | Hits and slices in PLAY focus are instant; clip and scene launches wait for `GRID` | PROPOSED | `design/perform.md` §4.3 |
| D75 | The start focus is a show setting (PREP default) | PROPOSED | `design/perform.md` §1 |
| D76 | **0.1 "Playback" scope and milestones M0–M12** | PROPOSED | `build-v1.md` |
| D61 | **Every clip is an instrument:** full clip transport (direction; LOOP, PING-PONG, ONCE hold / eject / next, RANDOM, CHOP, STEP, SCRATCH; in / out + inner loop; 16 cue points; beat loop with catch-up; FREE / BPM / TIMECODE time base) | DECIDED (owner asked for it); the details are PROPOSED | `design/clips-and-playback.md` §2 |
| D62 | **Trigger styles** NORMAL / TOGGLE / PIANO / LEGATO, restart / continue, fade in / out per clip, beat snap only on PROGRAM, clip target own / free, HIT layer, autopilot per layer | PROPOSED | `design/clips-and-playback.md` §3 |
| D63 | **Effect levels** clip / layer / deck / master / output, and **parameter scopes** SLOT / LOOK / GLOBAL / MOVE; clip variants A–D; all edits tried on PREVIEW first | DECIDED (owner: per clip, global or per scene); the model is PROPOSED | `design/clips-and-playback.md` §4 |
| D64 | **MK2 pads as an instrument:** pads follow the selected group (PREVIEW / PROGRAM); **PRESS** = velocity starts and pressure moves one target per clip / effect (a strobe gets faster and brighter the harder you press); **SLICE** mode = `PATTERN` twice; `SOLO` + pad = HIT on PROGRAM; FX-pad latch and velocity variation | DECIDED (owner: pressure drives effects on preview and live); the mapping is PROPOSED | `design/clips-and-playback.md` §8, `design/hardware-mapping.md` §3.4 |
| D65 | **Photosensitivity cap** on strobe / flash PRESS ranges (`GUESS:` 3 full flashes / s by default, a show setting) | PROPOSED | `design/clips-and-playback.md` §8.2 |
| D66 | **Clip audio** ignored by default; optional playback later for installations | PROPOSED | `design/clips-and-playback.md` §7 |
| D67 | **Live sampler** (`REC` held + empty pad): N beats of a camera / input / PROGRAM into a looping clip, in GPU memory, saved as HAP in the background | PROPOSED | `design/clips-and-playback.md` §6 |
| D54 | **One canvas over the outputs** (Modul8 style). Screens butt; projectors overlap by the blend, and the canvas width follows. **The GUI monitors take the canvas shape** (1 output: 16:9 side by side; 2–3: full-width PREVIEW and PROGRAM strips with output lines and hatched blend zones) | DECIDED (owner); the details are PROPOSED | `design/gui.md` §4.2, §12 |
| D55 | **Outputs step: Screens or Projectors first**; screens need no mapping; then 1 / 2 / 3 side by side, then the blend overlap with a live canvas size | DECIDED (owner) | `design/gui.md` §3 |
| D56 | **Layer places that snap:** FILL ALL (zoom + crop), OUT 1–3, FIT, REPEAT, FREE; LAYER page knob 1 = PLACE | PROPOSED | `design/gui.md` §12 |
| D57 | **Pen-tool masks** (click the corners, one or many per output) with **a crosshair projected on the real output**; the Block = a trackpad; later camera-assisted masks | DECIDED (owner); the details are PROPOSED | `design/gui.md` §13 |
| D58 | **Text generator** with many sources and styles | DECIDED (owner) | `design/effects-and-generators.md` §5 |
| D59 | **ROTATE** saved presets on every level (`SHIFT` + knob 2, etc.); `REC` saves at the current level; auto-rotate every n bars | DECIDED (owner); the mapping is PROPOSED | `design/presets-and-shuffle.md` §2 |
| D60 | **Two GUI screens optional:** screen 1 = the main layout (unchanged), screen 2 = the room + side arms; one screen must always work | DECIDED (owner) | `design/gui.md` §14 |
| D48 | **The MK2 master encoder = the TAKE wheel** (an endless T-bar: one turn = PREVIEW fully into PROGRAM, then the roles swap; push = auto TAKE). It replaces "encoder = browse" (D11) in PERFORM; browsing moves to knob 1 and `BROWSE`. `SHIFT` + wheel = system menu, as before | PROPOSED | `design/hardware-mapping.md` §3.1 |
| D49 | **Main weapon + side arms**: the main (MK2 or NuVJ) decides the GUI layout; side arms (Block, LUMI, Ohm64, KPE1, MIDI learn) only add control, with soft takeover. The Ohm64 is a side arm for now | DECIDED (owner); the details are PROPOSED | `design/hardware-mapping.md` §1 |
| D50 | **The GUI mimics the main weapon** (no drawn knobs or pads; knob tiles show what turning will do; the pads are clip thumbnails) | DECIDED (owner); the layout is PROPOSED | `design/gui.md` §1, §4 |
| D51 | **MK2: left screen + knobs 1–4 + groups A–D = PREVIEW; right screen + knobs 5–8 + groups E–H = PROGRAM** (roles that swap after a TAKE); choosing a program layer = LIVE EDIT with a red bar | PROPOSED | `design/hardware-mapping.md` §2 |
| D52 | **MK2 pads default to CLIPS** (16 clip thumbnails of the selected layer's bank; pressure = squeeze the layer's FX; LED = state) | PROPOSED | `design/hardware-mapping.md` §3.4 |
| D53 | MK2 Top 1–4 = dice locks, Top 5–8 = live holds (FREEZE, STROBE, BLACK, ASSIST) | PROPOSED | `design/hardware-mapping.md` §3.2 |
| D27 | The main instrument is the MK2 + Lightpad Block together; the other weapons are integrations (refined by D49: main weapon + side arms) | DECIDED | `design/hardware-mapping.md` |
| D28 | The GUI is dark only; the light theme is dropped; a brightness control replaces the "Night" theme | DECIDED | `design/gui.md` §2 |
| D29 | Startup is a 4-step wizard: 1 Add clips, 2 Choose weapon, 3 Set outputs, 4 Play | DECIDED | `design/gui.md` §3 |
| D30 | Two decks (A / B) with a crossfader into MAIN (the NuVJ idea). PREVIEW vs LIVE must always be clear on every output | DECIDED | `design/gui.md` §4.2 |
| D31 | Shuffle on the whole deck, a layer, an effect or a knob; save in one press to scenes / cues; trigger back and forth | DECIDED | `design/presets-and-shuffle.md` |
| D32 | Every imported clip is analysed and tagged (speed, theme / mood, colours, …). Import may run during a show, in the background | DECIDED | `design/library-and-analysis.md` |
| D33 | Optimise files before the show; playback only reads ready HAP | DECIDED | `design/library-and-analysis.md` §1 |
| D34 | The VJ table is a secondary view on the same engine, going to the same outputs. Its output can be routed into MAIN, and you can switch between the views | DECIDED | `design/gui.md` §9 |
| D35 | The GUI adapts to the chosen weapon ("Choose your weapon" at start-up) | DECIDED | `design/gui.md` §8 |
| D36 | Overlay mode (fill + key over a live mixer, for lip sync) is a separate show type | DECIDED | `design/engine.md` §8 |
| D37 | Built-in effects vs add-ons on separate shelves; describe-a-generator workflow with a safety gate; no Notch patches | DECIDED | `design/engine.md` §7 |
| D38 | Naming rule: product-specific effect names get Himmel names (for example Spiral feedback, Halftone dots, Bass punch) | DECIDED | `design/effects-and-generators.md` |
| D39 | Effects apply to USB inputs (cameras, capture) exactly as to clips | DECIDED | `design/engine.md` §2 |
| D40 | The NuVJ can run a simple show alone (classic two-deck setup) | DECIDED | `design/hardware-mapping.md` §4 |
| D41 | Development is done by agents, each owning a track, with handoff files and documentation all the way, so any human or agent can take over | DECIDED | `agents/README.md` |
| D42 | (Partly REPLACED by D48–D53) Mapping changes from v1: `TRANSPORT < >` = shuffle history / roll, `PLAY` = TAKE, `RESTART` = CUT, `SAMPLING` = music page, groups A–D / E–H = deck A / B layers | PROPOSED | `design/hardware-mapping.md` |
| D43 | GUI technology: a local web app through a `himmel_ui` bridge (instead of Dear ImGui), with previews over shared memory | PROPOSED | `design/gui.md` §10 |
| D44 | Block (side arm) modes: TOUCH (default), MIX, LOOKS, MOOD | PROPOSED | `design/hardware-mapping.md` §2 |
| D45 | 16 moods on a 4 × 4 energy × style map (12 old themes + 4 new) | PROPOSED | `design/presets-and-shuffle.md` §4 |
| D46 | 4 layers per deck to start (8 later if the budget allows) | PROPOSED | `design/engine.md` §2 |
| D47 | Merge all open branches into `main`, and from now on merge each branch within 1–2 days | PROPOSED | `roadmap.md` phase 0 |

## 2026-11-10

| # | Decision | Status | Details |
|---|---|---|---|
| D20 | Controllers are "weapons" (profile + map + driver + simulator); ones you do not own can be simulated | DECIDED | `weapons/README.md` |
| D21 | Weapon no. 2 = Lightpad Block M; no. 3 = LUMI Keys; no. 4 = Entrancer; no. 5 = NuVJ; no. 6 = KF4; no. 7 = V-4 (sim only); no. 8 = Ohm64 | DECIDED | `weapons/README.md` |
| D22 | Lightpad pressure sensitivity 127 (the block's setting is restored on exit) | DECIDED | `weapons/README.md` |
| D23 | VJ table instead of a rack; the boxes look like themselves; browser first | DECIDED | `docs/vj-table-design.md` |
| D24 | The effect set comes after the graphics engine's effect chain exists | DECIDED | `CLAUDE.md` (NuVJ branch) |
| D25 | NuVJ classic two-deck view in Himmel's own look (nostalgia) | DECIDED | NuVJ branch |
| D26 | Touch FX (the Entrancer gesture) on the Lightpad; do not clone the product | DECIDED | `docs/touch-fx-entrancer.md` |
| D14 | Open source, MIT; credit everything; copyleft parts in bridge processes | DECIDED | `CLAUDE.md` |
| D15 | HAP playback with our own MOV reader + Snappy; FFmpeg not linked (ffmpeg.exe as an external tool only) | DECIDED | `CLAUDE.md` |
| D16 | Supervisor + separate processes, shared-memory rings and blocks | TECH | `docs/architecture.md` |

## 2026-11-09

| # | Decision | Status | Details |
|---|---|---|---|
| D10 | The MK2 map, verified on the owner's unit, is the source of truth; fix single entries with the live view | DECIDED | `maschine_mk2_map.json` |
| D11 | The master encoder = browse / select wheel; `SHIFT` + encoder = system menu | DECIDED | `CLAUDE.md` |
| D12 | Visual mapping tools (browser live view), not terminal tables | DECIDED | `CLAUDE.md` |
| D13 | Velocity = max pressure in the first 4 ms after onset | TECH | `docs/mk2-controller-spec.md` |
| D1 | Stack: C++20, D3D11 flip model, waitable swap chain, max frame latency 1; no TouchDesigner | DECIDED | `CLAUDE.md` |
| D2 | Light theme (v1 mockups) | REPLACED by D28 | — |
