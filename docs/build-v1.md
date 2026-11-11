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
4. make **every effect react to the music** from a mic, a line in or the
   PC's own sound, with the reaction settings under their hands;
5. use the full clip transport (loop, ping-pong, once, random, chop, step,
   scratch, in / out, cue points, beat loops) on a tap / MIDI-clock tempo;
6. play pads as kits (targets, PRESS, SLICE, HIT, FX pads), in PREP or PLAY
   focus;
7. save scenes with kits and macros into songs, and run a setlist with
   `ENTER`;
8. undo or reset anything live;
9. save and open projects, and fire scenes from OSC;

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
| **Audio-reactive everything:** mic, line in and loopback (WASAPI), auto gain; level, 3 bands, spectrum, kick / snare / hat onsets, energy; **a reaction on any parameter of any effect** with full settings (signal, amount, range, mode, attack / release, threshold, curve, quantise); defaults per effect; reaction editor | ASIO, audio beat tracking (BPM from audio), build / drop / sections |
| Scenes, variations, kits, macros, songs, setlist, `ENTER` | Cue triggers on timecode / drop / DJ track |
| **Projects:** new / open / open recent / save / save as / one-file export, autosave, relink missing clips | Pack with media (0.2) |
| **Output resolution presets** 720p / 1080p / 1440p / 4K | Custom sizes beyond "Advanced" |
| Dice / mutate / rotate with locks (basic sampler v1) | Moods-driven smart roll beyond rules v1 |
| Ingest: drop folder → HAP, thumbnails, loop points, slices, bars, palette, brightness | Type tags / similarity (ONNX) |
| Outputs: 1–3 screens or projectors, one canvas, simple edge blend, curtains, per-output black; **displays that drop out and come back** (by identity), **IDENTIFY**, 60 / 50 Hz | Mesh warp, pen masks, 4–6 outputs |
| **Clips at their own frame rate** on any refresh (nearest frame, optional frame blend) | — |
| **Stills, image sequences, GIFs, SVG** in the library | — |
| **Settings view** (audio, tempo, weapons, outputs, library, projects, GPU, remote, start-up) | — |
| **OSC in, basic set** (`/himmel/go`, `/scene`, `/song`, `/take`, `/cut`, `/black`, `/macro/n`, `/master`, `/bpm`, `/beat`) | OSC out, full address space, MIDI learn, phone remote |
| — | **Timeline** (lanes, recording, timecode chase, OSC markers) |
| — | **Installation mode** (start with Windows, schedule, PIN lock), **send logs** |
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
| **M3** | SHOW | **Projects** (`design/projects.md`: folder format, new / open / recent / save / save as / `.himmelshow` export, atomic save worker, autosave journal + restore, relink) and show files: scenes, variations, kits (pad targets), macros, songs, setlist; save / recall / morph; undo stack (64) + reset-to-scene; dice / mutate / rotate v1 with locks | `design/perform.md` §2–3, `design/presets-and-shuffle.md` | Round-trip tests; recall of a 4-layer scene < 1 frame of hitch; kill the engine during a save → the autosave restores everything |
| **M4** | CONTROL | MK2 Perform v4: focus, wheel TAKE, Top buttons, pages PLAY / CLIP / LAYER / FX / MIX, pads CLIPS / SLICE / LOOKS / FX PADS, PRESS, targets, HIT, screens | `design/perform.md`, `design/hardware-mapping.md` §3 | The owner plays a song from the MK2 alone; input → glass ≤ 3 frames (measured) |
| **M5** | FX | Effect metadata (`press`, `scopes`, presets, **a default `reaction` for every effect**), 16 tier-1 effects (hue / sat, brightness / contrast, colorize, invert, posterize, mirror, kaleido, tile, rotozoom, zoom pulse, wave, shake, blur, glow, strobe, RGB split; plus trails and luma key if time allows), 6 generators, the optional strobe / flash cap (off by default, D65), PRESS + scope resolution in the engine | `design/effects-and-generators.md` | Each effect: PNG check + timing on the GTX 1070; with the optional cap on, strobe never exceeds it |
| **M6** | ENGINE + CONTROL | **Display drop-out / return by identity, IDENTIFY, 60 / 50 Hz, clips at their own fps** (`design/operations.md` §1–3); outputs: 1–3 screens / projectors as one canvas, **resolution presets 720p / 1080p / 1440p / 4K** (`design/projects.md` §5), simple blend, layer places (FILL ALL / OUT n / FIT / FREE), curtains in the supervisor, black per output; Spout | `design/engine.md` §2.0, §4, `design/gui.md` §12 | Kill the renderer: the outputs show black, not the desktop, and come back < 2 s; pull an output cable mid-show: the others never stutter and it returns to its slot |
| **M7** | MEDIA | Ingest: watch folder, ffmpeg → HAP (×4, chunks), **stills / image sequences / GIF / SVG** (`design/operations.md` §4), fps stored, thumbnails, analysis v1 (palette, brightness, motion, cuts, loop points, slices, bars, flicker), SQLite + sidecars, background throttling; MK2 `BROWSE` | `design/library-and-analysis.md` | 100 mixed clips import during a show without a late frame |
| **M8** | AUDIO + FX | **Audio-reactive in 0.1:** `himmel_audio.exe` with mic / line in / loopback (WASAPI), auto gain, gate; signals level, bass / mid / high, spectrum, kick / snare / hat, onset, energy; tap tempo, MIDI clock in, internal clock, beat / bar phase; **reactions on every parameter** with all settings; REACT macro; AV offset test; reaction editor on the MK2 | `design/music.md` §3–5 | Mic in a room and a line from a DJ mixer both drive zoom pulse on the kick, on time; unplug / replug the input during a soak |
| **M9** | CONTROL | Lightpad Block C++ driver (TOUCH, MIX = fader, SCRUB) with LED feedback, `--sim` | `design/hardware-mapping.md` §5 | Block MIX fades the decks with soft takeover against the wheel |
| **M10** | CONTROL | NuVJ as main weapon: driver from the branch, verified on the unit; wheel modes BEND / SCRATCH / FX WHEEL, trims, INSTA / auto fade, crossfader = PROGRAM | `design/perform.md` §5.2, `design/hardware-mapping.md` §4 | A song played from the NuVJ alone |
| **M11** | GUI (web app, D43) | `himmel_ui` bridge; wizard (with IDENTIFY, resolution, refresh, sound in); **Settings view**; reaction editor; Perform (MK2 and NuVJ layouts); clip editor drawer; scene / kit editor; library; outputs | `design/gui.md`, mockups `gui-mockups/v4/` | The GUI can be killed and restarted mid-show with no change on the outputs |
| **M13** | CONTROL | **OSC in (basic set)** on UDP 7000 through the command ring, allowed addresses, a "last messages" list | `design/timeline-and-remote.md` §6 | A lighting desk / Companion fires GO and scenes; garbage OSC never reaches the engine |
| **M12** | QA | Soak and failure rig: 8 h soak, kill each process, unplug each USB device, missing file, GPU device-loss path, latency rig | `design/engine.md` §3 | All pass on the owner's dev laptop **and** the show PC; report in `docs/qa/` |

M1–M3 can run in parallel once the command protocol is written (LEAD, first
day of M1). M5, M7, M8 and M13 run in parallel with M2–M4. M12 (QA) runs
last and gates the release.

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
