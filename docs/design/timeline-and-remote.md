# Timeline and remote control (OSC, MIDI, phone)

Status: decided by the owner, 2026-11-11 (D85–D87). OSC in for scenes is in
0.1; the timeline and the phone remote come in 0.2. Related:
[`perform.md`](perform.md) (songs and scenes),
[`clips-and-playback.md`](clips-and-playback.md),
[`music.md`](music.md) (clock and timecode), [`gui.md`](gui.md) §19.

## 1. Why a timeline

Live play (pads, scenes, `ENTER`) is the heart. A timeline adds three things:
1. **Programmed songs:** a song where the scenes, clips and moves are placed
   on time (bars or timecode), for shows that must look the same every night
   (a band with a click track, a product launch, an installation loop).
2. **Remote-triggered shows:** a lighting desk, a playback rig or a show
   controller fires the timeline or its markers over OSC / MIDI / timecode.
3. **Record and keep:** record a live performance onto the timeline, then
   tidy it and play it back.

**A timeline is just another way to drive the same scenes.** Anything you
touch live takes over from the timeline on that layer or macro, until you
press **RETURN** (or hold `ERASE` + group), like Autopilot.

## 2. What is on a timeline

Each song can have one timeline. A song is then "live", "timeline" or
"both" (timeline runs, you play on top).

```
 bars  | 1      | 5      | 9      | 13     | 17     | 21
 MARK  ▼Intro           ▼Verse            ▼Build   ▼Drop        (cue markers)
 SCENE [1 Intro  ][2 Verse A        ][4 Build ][5 Drop       ]  (scene blocks)
 L1    [Ocean ~~~~~~~][Neon tunnel ~~~~~~~~~~~~~~~~~~~~~~~~~~]  (clip blocks)
 L3          [Logo α]                       [Particles α ~~~~]
 FX    ........................../‾‾‾‾‾‾‾‾\....|strobe|......  (automation)
 MACRO INTENSITY  ____________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾   (macro curves)
 OSC   ◆/light/scene 3            ◆/light/strobe on             (OSC out events)
 AUDIO ▁▂▃▅▇▆▅▃▂▁▂▃▅▇█▇▅▃▂▁ (optional reference audio, for programming)
```

| Track | Holds | Notes |
|---|---|---|
| **MARK** (markers) | Named cue points | Jump targets for `ENTER`, OSC, MIDI notes; can **wait** ("hold here until GO") |
| **SCENE** | Scene blocks | A block takes its scene at its start, with the scene's transition (or a crossfade drawn as the block overlap) |
| **L1–L4** | Clip blocks per layer | Each block = clip + in / out + transport mode + clip FX; overlapping blocks crossfade |
| **FX / PARAM** lanes | Automation curves for any parameter (added on demand) | Drawn, or recorded from the knobs |
| **MACRO** lanes | Curves for the 4 live macros | The most useful lane: "intensity rises over the build" |
| **HIT** | Pad hits, effect hits | Recorded from the pads with velocity and pressure |
| **OSC / MIDI OUT** | Events sent at a time | Drive lights, a second Himmel, a lighting desk |
| **AUDIO** (reference) | A WAV / MP3 of the song, shown as a waveform, never played to the PA by default | To place things on the music when programming at home |

## 3. Time base and playback

- **Bars / beats** (default): the timeline follows the show clock (DJ tempo,
  Link, MIDI clock, tap). If the DJ speeds up, the timeline speeds up.
- **Timecode:** the timeline chases LTC / MTC (or an OSC time message);
  jumps in the timecode locate the timeline (with a 1-frame pre-roll).
- **Seconds:** free running, for installations.
- **Start:** `ENTER` on a timeline song, a marker, an OSC / MIDI trigger, a
  timecode start, or the schedule (installation mode).
- **Wait markers** let a programmed song wait for the band ("hold the verse
  loop until GO").
- **Loop region:** a part of the timeline can loop (installations, or "loop
  the chorus until GO").
- The timeline runs in the **show worker**, which sends ordinary commands
  (take scene, load clip, set value) on the beat; the render loop is
  unchanged. Clip blocks are pre-warmed 2 bars ahead.

## 4. Recording a performance

- **Arm** (`REC` held + `PLAY`, or the GUI button), then play: scene changes,
  clip loads, pad hits (with velocity / pressure), macro and knob moves and
  `ENTER` presses are written to the song's timeline, quantised to the grid
  you chose (or not).
- **Overdub** or **replace** per lane. Undo works on recordings.
- Afterwards: drag blocks, thin out curves, turn a recorded section into a
  loop region.

## 5. On the weapons

- **MK2:** `STEP` = the CUES / timeline view (already the CUES view). Left
  screen: the lanes around the playhead; right screen: markers and what is
  next. Pads = the first 16 markers (tap = jump on the bar, `SHIFT` = jump
  now); `PLAY` = play / stop the timeline (in this view only); `RESTART` =
  back to the start; the wheel = locate (scrub) while stopped, and stays TAKE
  while playing; `REC` held + `PLAY` = record.
- **NuVJ:** `BANK` push held + wheel A = locate; `BPM TAP` held + pad = jump
  to marker 1–9.
- **GUI:** Build › Timeline (§19 in `gui.md`).

## 6. OSC (in: 0.1 basics, out + the rest: 0.2)

- **In** on UDP port **7000** (`GUESS:` the common default; change in
  Settings), **out** to a list of targets. Only from allowed addresses
  (Settings › Remote); every message goes through the same validated command
  ring as the weapons, so OSC can never crash the engine.
- **Address space (`/himmel/…`), 0.1 set:**

  | Address | Args | Does |
  |---|---|---|
  | `/himmel/go` | — | `ENTER`: the next scene of the song, live, on the beat |
  | `/himmel/scene` | int n | Take scene n of the current song live (on the beat) |
  | `/himmel/scene/preview` | int n | Load scene n into PREVIEW |
  | `/himmel/song` | int n or string name | Select the song |
  | `/himmel/take` | — | TAKE |
  | `/himmel/cut` | — | CUT |
  | `/himmel/black` | int 0 / 1 | BLACK hold |
  | `/himmel/macro/1…4` | float 0–1 | The live macros |
  | `/himmel/master` | float 0–1 | Master dimmer |
  | `/himmel/bpm` | float | Set BPM (as a tap source) |
  | `/himmel/beat` | — | A beat pulse (phase source) |

- **0.2 additions:** `/himmel/timeline/play | stop | locate <bars> |
  marker <name or n>`, `/himmel/layer/<1-4>/clip <id>`, `/himmel/fx/...`
  (any parameter by path), `/himmel/pad/<n> <velocity>`; **OSC out** of the
  state (`/himmel/state/scene`, `/state/beat`, `/state/bpm`) for Bitfocus
  Companion / Stream Deck and lighting desks; OSC out events on the timeline.
- **Learn:** in the GUI, right-click any control → "OSC address" shows (and
  copies) its address.

## 7. MIDI and the phone remote (0.2)

- **Generic MIDI learn:** right-click a control → Learn → move a knob or hit a
  note on any MIDI device. Notes can fire scenes, markers and GO. Saved per
  project (as a "generic weapon").
- **MIDI timecode / clock in:** already in `music.md`.
- **Phone / tablet remote:** the web GUI serves a small **Remote** page on the
  local network (PIN protected): song and scene list, GO, BLACK, the 4
  macros, master dimmer and a status line. Big buttons, works in a dark room.
  It is a client of the same command ring; losing Wi-Fi changes nothing on the
  outputs.
