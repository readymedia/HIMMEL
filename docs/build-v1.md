# Build brief: Himmel 0.1 "Playback"

Status: 2026-11-11, written for Claude Code (all tracks). **Goal: fully
working, robust playback that is fun to play live.** Everything here is
already designed; this file says what is in 0.1, in what order, and how we
know it is done. When this file and an older doc disagree on scope, this file
wins; on behaviour, [`design/perform.md`](design/perform.md) wins.

Read first: `AGENTS.md` → `CLAUDE.md` → this file → `design/perform.md` →
the design file for your milestone. Rules: `agents/README.md` (branches,
handoffs, `GUESS:`).

## 1. What 0.1 is

A VJ can, from the MK2 (+ Lightpad Block) alone:
1. drop a folder of clips and get playable HAP clips with thumbnails;
2. pick outputs (1–3 screens or projectors, one canvas);
3. play two decks × 4 layers with clips, generators and cameras, with
   effects, through a TAKE wheel and live macros;
4. use the full clip transport (loop, ping-pong, once, random, chop, step,
   scratch, in / out, cue points, beat loops) on a tap / MIDI-clock tempo;
5. play pads as kits (targets, PRESS, SLICE, HIT, FX pads), in PREP or PLAY
   focus;
6. save scenes with kits and macros into songs, and run a setlist with
   `ENTER`;
7. undo or reset anything live;

…for 8 hours without a dropped frame, and survive any one process dying.

The NuVJ as the second main weapon comes in 0.1 **if** its driver checks out
on the owner's unit (milestone M10); otherwise it is the first thing in 0.2.

## 2. In and out

| In 0.1 | Not in 0.1 (later) |
|---|---|
| Decks A / B × 4 layers, crossfader / wheel TAKE, CUT, transitions | Layers per deck > 4 (D46) |
| Effect slots at clip, layer, deck, master, output; ~16 cheap tier-1 effects + strobe safety | Tier 2–3 effects, ISF add-ons |
| 6 generators (solid / gradient, noise, lines, rings, shapes, text ticker) | Full text generator, particles |
| Clip transport (all modes in `clips-and-playback.md` §2–3) | TIMECODE / TRACK time base |
| Tempo: tap, MIDI clock in, internal clock | Ableton Link and Pro DJ Link bridges (0.2), audio beat tracking (0.2) |
| Simple audio: input level + 3 bands + kick onset (WASAPI) for REACT | Full analysis (BPM, sections, drops) |
| Scenes, variations, kits, macros, songs, setlist, `ENTER` | Cue triggers on timecode / drop / DJ track |
| Dice / mutate / rotate with locks (basic sampler v1) | Moods-driven smart roll beyond rules v1 |
| Ingest: drop folder → HAP, thumbnails, loop points, slices, bars, palette, brightness | Type tags / similarity (ONNX) |
| Outputs: 1–3 screens or projectors, one canvas, simple edge blend, curtains, per-output black | Mesh warp, pen masks, 4–6 outputs |
| Cameras (exists), Spout in / out | NDI, capture cards beyond Media Foundation, DeckLink |
| MK2 full Perform mapping (v4), Lightpad Block (TOUCH / MIX / SCRUB) | LUMI, Ohm64, KPE1, generic MIDI learn |
| GUI v1: Perform, clip editor, scene / kit editor, library, outputs, wizard (web app, D43) | VJ table, two GUI screens, audience view |
| Live sampler | — (0.2) |

## 3. Milestones (in order; each ends merged to `main`)

Each milestone has an owner track (`agents/README.md`), the spec, and its
**done when**. Do not start a milestone's GUI before its engine side works
from the MK2.

| # | Track | Build | Spec | Done when |
|---|---|---|---|---|
| **M0** | LEAD + CONTROL | Merge all branches (phase 0), one weapon list, CI build of every target | `roadmap.md` phase 0 | `main` builds, all tests pass, branches deleted |
| **M1** | ENGINE | Deck / layer model, crossfader + TAKE / CUT, effect slots at all levels, render graph built in a worker, texture pool, off-air thumbnails, command ring + state block + preview block | `design/engine.md` §2, `interfaces/` | 2 decks × 4 HAP layers + 8 effects play; 2 h soak, 0 missed frames |
| **M2** | ENGINE | Clip transport: playhead state machine, modes, in / out, inner loop, cue points, beat loop + catch-up, scratch, BPM time base, speed glide, trigger styles, beat snap, clip target incl. HIT layer, autopilot, layer transitions, warm cue frames | `design/clips-and-playback.md` §2–3, §10 | Unit tests per mode (no GPU); jump-to-cue visible on the next frame |
| **M3** | SHOW | Show files: scenes, variations, kits (pad targets), macros, songs, setlist; save / recall / morph; undo stack (64) + reset-to-scene; dice / mutate / rotate v1 with locks | `design/perform.md` §2–3, `design/presets-and-shuffle.md` | Round-trip tests; recall of a 4-layer scene < 1 frame of hitch |
| **M4** | CONTROL | MK2 Perform v4: focus, wheel TAKE, Top buttons, pages PLAY / CLIP / LAYER / FX / MIX, pads CLIPS / SLICE / LOOKS / FX PADS, PRESS, targets, HIT, screens | `design/perform.md`, `design/hardware-mapping.md` §3 | The owner plays a song from the MK2 alone; input → glass ≤ 3 frames (measured) |
| **M5** | FX | Effect metadata (`press`, `scopes`, presets), 16 tier-1 effects (hue / sat, brightness / contrast, colorize, invert, posterize, mirror, kaleido, tile, rotozoom, zoom pulse, wave, shake, blur, glow, strobe, RGB split; plus trails and luma key if time allows), 6 generators, the optional strobe / flash cap (off by default, D65), PRESS + scope resolution in the engine | `design/effects-and-generators.md` | Each effect: PNG check + timing on the GTX 1070; with the optional cap on, strobe never exceeds it |
| **M6** | ENGINE + CONTROL | Outputs: 1–3 screens / projectors as one canvas, simple blend, layer places (FILL ALL / OUT n / FIT / FREE), curtains in the supervisor, black per output; Spout | `design/engine.md` §2.0, §4, `design/gui.md` §12 | Kill the renderer: the outputs show black, not the desktop, and come back < 2 s |
| **M7** | MEDIA | Ingest: watch folder, ffmpeg → HAP (×4, chunks), thumbnails, analysis v1 (palette, brightness, motion, cuts, loop points, slices, bars, flicker), SQLite + sidecars, background throttling; MK2 `BROWSE` | `design/library-and-analysis.md` | 100 mixed clips import during a show without a late frame |
| **M8** | AUDIO | WASAPI in, level + 3 bands + kick onset; tap tempo, MIDI clock in, internal clock, beat / bar phase for the whole engine | `design/music.md` | REACT on kick is visibly on time (AV offset test) |
| **M9** | CONTROL | Lightpad Block C++ driver (TOUCH, MIX = fader, SCRUB) with LED feedback, `--sim` | `design/hardware-mapping.md` §5 | Block MIX fades the decks with soft takeover against the wheel |
| **M10** | CONTROL | NuVJ as main weapon: driver from the branch, verified on the unit; wheel modes BEND / SCRATCH / FX WHEEL, trims, INSTA / auto fade, crossfader = PROGRAM | `design/perform.md` §5.2, `design/hardware-mapping.md` §4 | A song played from the NuVJ alone |
| **M11** | GUI (web app, D43) | `himmel_ui` bridge; wizard; Perform (MK2 and NuVJ layouts); clip editor drawer; scene / kit editor; library; outputs | `design/gui.md`, mockups `gui-mockups/v4/` | The GUI can be killed and restarted mid-show with no change on the outputs |
| **M12** | QA | Soak and failure rig: 8 h soak, kill each process, unplug each USB device, missing file, GPU device-loss path, latency rig | `design/engine.md` §3 | All pass on the owner's dev laptop **and** the show PC; report in `docs/qa/` |

M1–M3 can run in parallel once the command protocol is written (LEAD, first
day of M1). M5, M7 and M8 run in parallel with M2–M4.

## 4. Robustness rules for every milestone (non-negotiable)

1. The render loop never allocates, locks, waits on I/O or compiles.
2. Every worker (decode, ingest, audio, weapons, GUI) can die; the supervisor
   restarts it; the output keeps the last good state.
3. Every new value from a weapon or the GUI goes through the command ring and
   is validated (range, rate) before the engine sees it.
4. A missing or broken clip shows its thumbnail-colour placeholder, never
   black and never a crash; the library marks it.
5. When a show switches on the optional flash cap (D65), strobe / flash never exceed it.
6. Each milestone adds its failure tests to the QA rig.
7. Every `GUESS:` is listed in the track handoff; every owner choice is a
   PROPOSED row in `decisions.md`.

## 5. Owner decisions that block or shape 0.1

**All decided by the owner on 2026-11-11** (D42–D76 in `decisions.md`):
- D43: the GUI is a **local web app** through the `himmel_ui` bridge.
- D47: merge all branches now (M0), then merge within 1–2 days.
- D46: 4 layers per deck.
- D76: this scope.
- D48–D75: built as written. D65: **no flash cap by default**, an optional
  per-show cap.
- Still open, not blocking 0.1: Ableton Link licensing (GPL bridge, 0.2).
