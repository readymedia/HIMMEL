# Clips and playback: transport, loops, triggering, effect levels, pads

Status: v1, 2026-11-11. Written after a check of what Resolume, Modul8, VDMX
and media servers (Hippotizer, Modulo Player, Viz) give a VJ as basics. It
fills the gaps that check found. Related: [`engine.md`](engine.md) (the
player), [`effects-and-generators.md`](effects-and-generators.md),
[`presets-and-shuffle.md`](presets-and-shuffle.md),
[`hardware-mapping.md`](hardware-mapping.md) §3.4 (MK2 pads),
[`gui.md`](gui.md) §16 (the clip editor).

**The idea in one line:** every clip is an instrument. You decide *where* the
playhead goes (in / out, cue points, slices), *how* it moves (loop, ping-pong,
random, step, beat-locked), *how* it starts and stops (trigger style), and
*which* effects travel with it. All of it is tried on PREVIEW first.

## 1. Basics check (what other tools have, and where Himmel has it)

| Basic | Seen in | Himmel | Where |
|---|---|---|---|
| Clip grid in banks, thumbnails | All | Yes | `library-and-analysis.md`, pads |
| Layers, blend modes, opacity, mute / solo | All | Yes | `engine.md` §2 |
| A/B decks + crossfader | VDMX, NuVJ, Modul8 (via groups) | Yes | `engine.md` §2, wheel = TAKE |
| **Direction: forward / backward / pause** | All | **New here** | §2.1 |
| **Play modes: loop, ping-pong, once + hold, once + eject, random** | Resolume, VDMX, Hippotizer | **New here** | §2.2 |
| **Random with interval and distance** | Resolume | **New here** | §2.2 |
| **In and out points** (drag the range, keep length) | All | **New here** | §2.3 |
| **Inner loop (intro → loop → outro)** | Hippotizer, Viz ("4-point loop") | **New here** | §2.3 |
| **Cue points** (jump to marks) | Resolume, TouchDesigner palette, media servers | **New here** | §2.4 |
| **Scrub / scratch the playhead** | Resolume, VDMX | **New here** (wheel, jog, Block) | §2.5 |
| **Beat loop / loop roll with catch-up** | Resolume ("Beatloopr") | **New here** | §2.6 |
| **Speed modes: free, BPM sync, timecode** | Resolume, Hippotizer | **New here** (BPM partly in music.md) | §2.7 |
| **Trigger style: normal, toggle, piano (momentary)** | Resolume | **New here** | §3.1 |
| **Beat snap (quantised launch) per clip or global** | Resolume, VDMX | Partly (`GRID`) | §3.2 |
| **Clip target: own layer / any free layer** | Resolume | **New here** | §3.3 |
| **Autopilot: next / random / previous clip after N loops** | Resolume | Partly (auto-rotate is for presets) | §3.4 |
| **Restart vs continue (resume position)** | Resolume, VDMX | **New here** | §3.1 |
| **Per-clip fade in / fade out** (piano release fade) | Resolume (asked for often), Modulo | **New here** | §3.1 |
| **Layer transition type and time** | Resolume, Modulo | Partly (MIX page) | §3.5 |
| Per-clip defaults (fit / fill, blend, opacity) | Resolume, Modul8 | Yes (PLACE) | `gui.md` §12 |
| **Effects saved on the clip** | Resolume, Modul8 | Named in `engine.md`, now specified | §4 |
| **Parameter animation (LFO, envelope)** | Resolume, VDMX | Partly (REACT is audio only) | §5 |
| Effect presets, dashboard / macros | Resolume | Yes | `presets-and-shuffle.md` |
| **Live sampler (record a source into a new clip)** | Resolume, VDMX | **New here** | §6 |
| Output recording to file | Resolume | Later | `roadmap.md` phase 12 |
| Masks, crop, text, mapping, blend | All | Yes | `gui.md` §12–13 |
| MIDI / OSC learn, Link, timecode, DJ sync | All | Yes | `music.md`, weapons |
| DMX / Art-Net in (lighting desk drives Himmel) | Arena, media servers | Later, optional | `roadmap.md` phase 13 |
| Clip audio playback | Resolume | **Off by default**; later, optional (D66) | §7 |

## 2. The playhead

Every clip player (and every generator with a time base) has one transport.
All settings are saved **with the clip** in the library, and can be changed
per use (in a look) without touching the library default.

### 2.1 Direction

`▶` forward, `◀` backward, `‖` pause. A held **REVERSE** (momentary) flips the
direction while held and comes back where the playhead *would* have been
(catch-up, §2.6).

### 2.2 Play modes

| Mode | What it does | Good for |
|---|---|---|
| **LOOP** (default) | In → out, jump back to in | Seamless loops |
| **PING-PONG** | In → out → in … | Footage that does not loop; hides the cut |
| **ONCE · HOLD** | In → out, then hold the last frame | Endings, reveals |
| **ONCE · EJECT** | In → out, then the layer clears (fades out) | One-shot hits |
| **ONCE · NEXT** | In → out, then the next clip in the bank (autopilot) | Story order |
| **RANDOM** | Jumps to a random point every *interval*, within *distance* of where it is, then plays on (in the current direction) | Restless, glitchy beds |
| **CHOP** | The range is cut into *N* slices (4 / 8 / 16 or the analysed cuts); every *interval* it plays a random slice (or a pattern: 1-1-3-4) | Beat-chopped edits |
| **STEP** | Stop-motion: holds a frame and steps *k* frames on every beat division | Stutter / stop-motion look |
| **SCRATCH** | The playhead follows a controller (wheel, jog, Block X, LUMI slide) and nothing else | Hand scratching |

- *Interval* and *distance* are in **beats** when BPM sync is on, in seconds
  when not (as in Resolume).
- Random, chop and step use a **seeded** random source, so a saved look plays
  the same pattern again.
- The dice can change the play mode when the MOTION lock is free (§4.4).

### 2.3 In and out points, inner loop

- **In / out** bracket the part of the clip that plays. In the clip editor:
  drag the brackets; drag the range with `SHIFT` to move it and keep the
  length; double-click = snap to the nearest analysed cut or beat.
- **On the MK2:** in SLICE mode, hold one pad and tap another = in at the first
  slice, out at the end of the second (§8.3). On the CLIP knob page, knobs 1 /
  2 = in / out (§8.4).
- **Inner loop (4-point loop):** optional second pair inside in / out. The clip
  plays the intro (in → loop-in), loops inside, and on **RELEASE** (a tap of
  `PLAY` on that layer, or the cue) plays out the outro (loop-out → out). Used
  for logos, reveals and song-specific clips.
- **Lengths can snap to beats:** 1, 2, 4, 8, 16 beats; the clip speed is then
  set so the range lasts exactly that long (§2.7).

### 2.4 Cue points

- Up to **16 cue points** per clip (`GUESS:` 16 fits the pads; Resolume has
  fewer). They are saved with the clip.
- Set with the playhead (`REC` in SLICE mode on a pad), or drag markers in the
  editor.
- If a clip has no cue points, SLICE mode cuts it into 16 equal slices (or at
  the cuts found at import).
- Jumping to a cue point ends a beat loop (as in Resolume), so you can build up
  with loop rolls and land on a mark.

### 2.5 Scrub and scratch

- The playhead can always be grabbed: drag in the clip editor, the NuVJ jog,
  the Block in TOUCH (X = position), a LUMI key slide, and on the MK2 **hold
  `SELECT` + turn the wheel** (the wheel scratches the selected layer instead
  of taking).
- On release, the clip goes back to its mode: **snap back** (catch-up to where
  it would be) or **stay** (continue from here). Default: stay.

### 2.6 Beat loop (loop roll)

- Loops a short part (1/16, 1/8, 1/4, 1/2, 1, 2, 4 beats) from the current
  playhead, starting on the beat.
- **Catch-up on** (default): when you let go, the clip continues where it would
  have been without the loop, so it stays in phase with the music. Off: it
  continues from the loop.
- On the MK2 this is the pressure in SLICE mode and `NOTE REPEAT` (§8).

### 2.7 Speed and time base

| Time base | Speed means | Notes |
|---|---|---|
| **FREE** | ×0.0–×4, also negative | Knob 4 SPEED on the PLAY page |
| **BPM** | The range lasts *n* beats (n from analysis, 1–64) | Follows the clock (Link, Pro DJ Link, tap). Default for loops tagged "loopable" |
| **TIMECODE** | The playhead follows LTC / MTC (with an offset) | Cues and song-locked clips |
| **TRACK** | The clip is linked to a DJ track and follows its position | Pro DJ Link bridge; later |

- **Frame blending** (optional, cost S) smooths slow speeds below ×0.5.
- Speed changes glide over a set time (default 1 beat), so a speed knob never
  jumps.

## 3. Triggering

### 3.1 Trigger style (per clip; a global default)

| Style | Press | Press again | Release |
|---|---|---|---|
| **NORMAL** (default) | Start (or restart) | Restart | — |
| **TOGGLE** | Start | Stop (fade out) | — |
| **PIANO** (momentary) | Start | — | Stop (fade out) |
| **LEGATO** | Start at the position of the clip it replaces (same layer) | — | — |

- **Start position:** RESTART (from in) or CONTINUE (from where it was last
  stopped). Per clip.
- **Fade in / fade out per clip:** separate times (0 = cut). A common wish in
  Resolume (snap in, fade out on release) is just *in 0, out 1 s* here.
- **Velocity** (any pad weapon) sets the start opacity or the PRESS target
  (§8.2). A soft hit = a soft start.

### 3.2 Beat snap (quantised launch)

- Global `GRID` on / off, with a division: off, 1/4 beat, 1 beat, 1 bar, 2 /
  4 bars. A clip can override it.
- **PREVIEW never waits.** Only launches on PROGRAM wait for the snap.
- The waiting pad blinks on the beat; the MK2 screen shows a countdown.

### 3.3 Clip target

- **OWN LAYER** (default): into the selected layer.
- **FREE LAYER**: into the lowest free layer on the same deck, so piano
  triggers can stack (with PIANO this plays like a keyboard of clips).
- **HIT LAYER**: a fifth layer on top of the PROGRAM deck, used only by
  momentary hits (§8.1). It has its own blend (add by default) and is never
  saved in a look.

### 3.4 Autopilot (per layer)

- After *N* loops (or *N* bars): **NEXT**, **PREVIOUS**, **RANDOM** (from the
  bank, weighted by the mood fit), or **SAME** (stay).
- Uses the layer's transition (§3.5).
- Works on PREVIEW and PROGRAM; on PROGRAM the change is on the beat.
- This is clip-level autopilot. Preset auto-rotate (`presets-and-shuffle.md`)
  is the same idea one level up.

### 3.5 Layer transition (when a layer gets a new clip)

Per layer: **type** + **time** (beats or seconds). Types: cut, fade, add-fade
(through white-ish), black-dip, wipe (shapes from the V-4 catalogue), push /
slide, zoom-through, luma wipe (follows the new clip's brightness), noise
dissolve, blur-through, flash-through. The MIX page sets the selected layer's
transition. TAKE between decks uses the crossfader curve, not these.

## 4. Effect control levels

### 4.1 Where an effect can sit

| Level | Saved with | Travels when | Typical use |
|---|---|---|---|
| **CLIP** | The clip (library) | The clip is loaded anywhere | "This clip always gets a soft glow" |
| **LAYER** | The look | The look is recalled | The layer's character tonight |
| **DECK** (group) | The look | The look is recalled | One treatment for all 4 layers |
| **MASTER** | The show (and master presets) | Always on MAIN | Global colour, safety limiter |
| **OUTPUT** | The show (output setup) | Always on that output | Per-projector colour match, masks |

All levels have the same 4 slots and the same knob layout ("1–4 parameters,
5 mix, 6 audio, 7 sync, 8 variation").

### 4.2 Where a parameter's value comes from

Every parameter of every effect instance has a **scope**. The GUI shows it as
a small badge on the knob tile. To change it: right-click the tile in the
GUI, or hold `SHIFT` + `SELECT` and turn the knob on the MK2.

| Scope | Badge | The value is | Example |
|---|---|---|---|
| **SLOT** (default) | none | Stored in the slot (clip, look, show) | Kaleido segments on this clip |
| **LOOK** | `L` | Stored in each look / scene separately | The same strobe, a different rate per scene |
| **GLOBAL** | `G` | One value shared by every instance of this effect in the show | "All strobes follow one rate" |
| **MOVE** | `~` | Driven by a modulator (§5) or a REACT link | Hue drifts on an LFO |

This answers "per clip, per layer, global or per scene" without extra levels:
place the effect where it should travel (§4.1), and choose for each value
whether it is fixed, per look, or shared (§4.2).

### 4.3 Clip variants (fast, different looks of the same clip)

- A clip can have **up to 4 variants** (A–D): each is a full clip-level setup
  (transport + clip FX + PRESS target).
- `SHIFT` + knob 1 on the PLAY page (or `SHIFT` + tap the clip pad) rotates
  the variant. The dice can pick a variant when CLIP is free.
- New variants are made by DICE / MUTATE on the clip level and saved with
  `REC` while the CLIP level is selected.

### 4.4 Presets and randomise on every level

The three verbs (DICE, MUTATE, ROTATE) and `REC` work on the **selected
level**, as in `presets-and-shuffle.md`:

| Selected | DICE changes | ROTATE goes through |
|---|---|---|
| An FX slot (FX page) | That effect's parameters within `good` ranges | The effect's presets |
| A clip (CLIP page) | Transport mode, in / out, speed, clip FX | The clip's variants A–D |
| A layer (group button) | Clip, layer FX, place, blend | Layer presets |
| The look (`ALL`) | Everything not locked | Saved looks in the bank |
| Master (knob 8) | Master FX | Master presets |

The MOTION lock (Top 4) also holds the transport (play mode, speed, in / out).

### 4.5 Test on PREVIEW (the rule)

- Every edit, dice, rotate and clip load happens on **PREVIEW** unless a
  PROGRAM layer is selected (PLAY focus, red frame).
- **Try a program look:** `DUPLICATE` + a PROGRAM group copies that layer
  (or `DUPLICATE` + `ALL` the whole look) into PREVIEW. Tweak, then TAKE.
- **Audition a clip:** in LIBRARY, the wheel scrolls and the PREVIEW monitor
  plays the hovered clip with its variant A, without loading it.
- **A/B compare:** `SOLO` held = PREVIEW shows only the selected layer;
  `SHIFT` + `SOLO` = PREVIEW flips between the edit and what it was when you
  started (undo point), so you see the difference.

## 5. Modulators (parameter animation)

REACT links values to audio. **MOVE** adds simple modulators, cheap and
beat-locked:

- **LFO:** sine, triangle, saw up / down, square, random (smooth / steps);
  rate in beats (1/16 … 16 bars) or Hz; depth; phase.
- **Envelope on trigger:** attack / hold / release in beats, fired by a clip
  start, a pad hit or a beat division.
- **Sequencer:** 4–16 steps, one value per step, on the beat.
- **Follow:** follow another value (the crossfader, the TAKE %, a Block axis).

Every knob can have one MOVE and one REACT. `AUTO WRITE` + knob = REACT link
(already in the mapping); `SHIFT` + `AUTO WRITE` + knob = MOVE (LFO, the
first time; turning sets depth). The dice may add or remove a MOVE when
MOTION is free.

## 6. Live sampler

- Records N beats (1, 2, 4, 8) of a **source** (a camera, a capture input,
  NDI) or of **PROGRAM** into a new clip that loops at once.
- On the MK2: **hold `REC` + tap an empty pad** in CLIPS mode = record the
  selected layer's source into that pad, starting on the next beat, for 4
  beats (turn the wheel while holding `REC` to change the length).
- The sample lives in GPU memory at half resolution by default (`GUESS:`
  about 340 MB for 4 beats at 128 BPM, 1080p NV12; the 5090 has 32 GB), and
  is written to disk as HAP by a worker in the background, so it survives a
  restart and turns up in the library tagged "sampled".
- All the transport modes work on it (ping-pong a dancer, chop the crowd).

## 7. Clip audio

Himmel is visual. Clip audio is **ignored by default**. Optional later
(D66, decided): play clip audio to a chosen output for installations, with
volume per clip and per layer. Not in the gig path.

## 8. The MK2 pads as an instrument

The MK2's best feature is **16 pressure pads with velocity and aftertouch**
(~750 Hz, velocity = peak in the first 4 ms). No other weapon in the set has
this on 16 keys. So the pads do more than trigger:

### 8.1 Where pads act: PREVIEW and PROGRAM

- **The pads follow the selected group.** A–D selected → pads act on PREVIEW.
  E–H selected (PLAY focus) → pads act on PROGRAM (on the beat when GRID is
  on).
- **`SHIFT` + pad** (CLIPS mode, a PREVIEW group selected) = load into PREVIEW
  *and* TAKE that layer at once: "fire it live" without leaving prep.
- **Hit from prep:** in CLIPS mode, **hold `SOLO` + pad** = play that clip as a
  momentary **hit on PROGRAM** (the HIT LAYER, §3.3), whatever is selected.
  Velocity = start opacity, pressure = opacity / PRESS target, release = fade
  out (the clip's fade-out time). This is the "flash a clip on the beat" move.
- LEDs: green-tinted = loaded on a PREVIEW layer, red pulsing = on PROGRAM,
  white = your finger, the clip's colour dim = in the bank.

### 8.2 PRESS: every clip, look and effect has one pressure target

- **PRESS** is one parameter (or a macro) with a range (low → high) and a
  curve, defined in the effect metadata (`press` field) and overridable per
  clip / variant.
- **Velocity sets where it starts, aftertouch moves it while held, release
  brings it back** over the release time.
- Default PRESS targets (examples):

  | Effect / thing | PRESS drives |
  |---|---|
  | Strobe | Rate (1/4 → 1/32 beat) and brightness |
  | Flash | Strength |
  | Blur | Size |
  | RGB split | Distance |
  | Kaleido | Segments (3 → 12, stepped) |
  | Zoom pulse | Amount |
  | Shake | Amount |
  | Glitch blocks | Amount |
  | Pixelate | Block size |
  | Tunnel | Speed |
  | Echo / trails | Feedback |
  | A clip (no FX) | Opacity, then brightness above 100 % |
  | A look (LOOKS mode) | Morph speed |

- So **a strobe on a clip gets faster and brighter the harder you press**,
  a blur opens as you squeeze, and a soft tap is a gentle accent.
- **Flash rate (D65, owner):** no cap by default. A show can switch on an
  optional cap (for example 3 flashes / s) for venues that require it; then
  the strobe and flash PRESS ranges stop at that rate.
- **Pad 12 is worn:** the per-pad noise floor and dead zone apply before
  velocity and pressure (`maschine_mk2_map.json`).

### 8.3 SLICE mode: the playhead under your fingers

`PATTERN` pressed again while in CLIPS (the `PATTERN` LED blinks) = **SLICE**:
the 16 pads become the **cue points or 16 slices of the selected layer's
clip** (pad 1 = start, bottom-left, as on the panel).

| Gesture | Does |
|---|---|
| Tap | Jump to that slice (on the GRID division on PROGRAM; at once on PREVIEW) |
| Hold | Loop that slice (the slice length) |
| **Hold + press harder** | **Beat loop gets shorter:** 1 beat → 1/2 → 1/4 → 1/8 → 1/16 (stutter). Ease off = longer again. Release = catch-up |
| Hold one pad + tap another | Set **in** at the first slice and **out** at the end of the second (a live loop range) |
| Two pads held | Ping-pong between those two slices |
| `REC` + pad | Store the current playhead as that pad's cue point |
| `ERASE` + pad | Clear that cue point (back to an equal slice) |
| `SHIFT` + pad | Reverse from that slice |
| `NOTE REPEAT` held + pad | Retrigger the slice at the repeat rate (the wheel sets the rate); pressure = rate |
| Velocity | The PRESS target of the clip at the jump (a hard hit = a bright, punchy jump) |

Pad LEDs in SLICE: the slice under the playhead lights white and moves; the
in / out range is lit in the clip's colour; cue points are bright, equal
slices dim.

### 8.4 The CLIP knob page

A new knob page (after PLAY, `PAGE ▶`):

| Knob | PREVIEW side (1–4) | PROGRAM side (5–8) |
|---|---|---|
| 1 / 5 | **IN** (`SHIFT` = move the range) | Program layer: speed |
| 2 / 6 | **OUT** (`SHIFT` = length in beats) | Program layer: play mode |
| 3 / 7 | **MODE** (loop, ping-pong, once, random, chop, step, scratch) | Program layer: chop / random interval |
| 4 / 8 | **SPEED** / beats per loop | Program layer: beat-loop length |

The left screen draws the clip's timeline with the in / out brackets, the cue
marks and the playhead (the 1-bit thumbnail strip already exists in
`pages.cpp`).

### 8.5 FX PADS and latch

- FX PADS (`PAD MODE`): momentary effects on PROGRAM; velocity = punch,
  pressure = the effect's PRESS target.
- **Latch:** `SHIFT` + pad = the effect stays on (LED stays bright); tap again
  = off.
- **Velocity picks the variation** (`GUESS:` soft < 40 = the effect's "subtle"
  preset, hard > 100 = its "extreme" preset, in between = blend), so the same
  pad has three characters.
- Several pads at once = several effects stacked, in the order pressed.

### 8.6 More pad ideas (tier 3, for later)

- **Pressure XY:** in FX PADS with `SHIFT` + `SOLO`, a 2 × 2 block of pads
  acts as a small XY pad (the pressure-weighted centre = X / Y), for example
  the kaleido centre or the zoom centre.
- **Drum the edit:** CHOP mode with a pattern recorded from pad taps (16
  steps, `REC` held while tapping SLICE pads).
- **Velocity → colour:** in CLIPS mode, the hit's velocity picks a colour from
  the mood palette for the clip's tint.

## 9. Weapons: what each one is best at

| Weapon | Best at | Weak at | So in Himmel it does |
|---|---|---|---|
| **MK2** (main) | 16 velocity + pressure pads, 8 knobs, two screens, many buttons | No fader | Clips, slices, pressure FX, presets; the wheel is the T-bar |
| **NuVJ** (main) | A real crossfader, fixed A / B, one knob per job, a jog: **simple** | No pressure, small screen | Classic two-deck mixing; the jog scratches and scrubs |
| **Block** (side) | Continuous XY + pressure + glide on one surface | Few discrete controls | Scrub the playhead, XY on effects, crossfader (MIX), touch FX |
| **LUMI** (side) | 24 keys with velocity and per-key slide / pressure (MPE) | No screen | Slices across the keys (chromatic chop), FX keys, slide = speed bend |
| **Ohm64** (side) | Faders, a crossfader, an 8 × 8 button grid | No pressure | Layer opacities, the real crossfader, a big clip / look grid |
| **KPE1 / Entrancer** (side) | Touch gestures | Few controls | Touch FX |
| **Keyboard + mouse** | Always there | No feel | Everything, clickable; the safe fallback |

The rule stays: one main weapon sets the layout; side arms add what the main
one lacks (a fader for the MK2, pressure for the NuVJ).

## 10. Engine notes (for the ENGINE track)

- The playhead state machine runs in the clip player's **decode worker**,
  driven by the beat clock; the render loop only picks the ready frame
  (no change to the render-loop rules).
- HAP frames are all key frames, so random access, chop and slice jumps are a
  read + Snappy decompress (0.2–0.9 ms at 1080p, measured).
- **Zero-latency jumps:** for the clip on the selected layer (and every clip
  on PROGRAM), the worker keeps the first frame of each cue point / slice
  decoded and uploaded (`GUESS:` 16 × ~1–2 MB BC per 1080p clip). A pad jump
  then shows on the next frame.
- **Ping-pong and reverse** use the backward read-ahead that `ClipPlayer`
  already has.
- **Catch-up** keeps a shadow playhead running during beat loops and scratch.
- **Loop crossfade** (the "seam" option for loops that nearly loop) needs a
  second read head near the out point: blend the last *k* frames into the
  first *k*. Cost S.
- The sampler writes into a GPU ring; a worker encodes it to HAP afterwards.
