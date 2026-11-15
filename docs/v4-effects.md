# The V-4's effects: a catalogue to rebuild in Himmel

The V-4 (weapon no. 7) is a small, cheap, very playable effects box. This page
lists **everything it can do**, so Himmel can rebuild the same effects as its
own GPU code, later perhaps as "devices" in a rack (see the end).

**Source.** The facts come from the menu list in the V-4 owner's manual, pp. 75-82,
plus pp. 23-39 for what the effects do. The manual says what each effect is and
how many variants it has; it does **not** say what each numbered variant looks
like (for example, how STROBE3 differs from STROBE4). Those details still need a
real V-4 or video of one. Nothing here is copied code; the effect ideas are
classic video-mixer effects. See `THIRD_PARTY_NOTICES.md`.

How the V-4 is organised:
- **Two channels, A and B.** Each has 4 effect buttons. Any button can be given
  any of the 96 effect patterns below, per memory (2-8; memory 1 is the
  factory set and cannot be changed).
- **Three transition buttons** (MIX, WIPE, EFX). Each can be given any of the
  248 transitions below, per memory.
- **The CONTROL dial** changes the parameter of the effect that is blinking.
  Effects marked ✱ in the manual can be changed with the dial; the copy we read
  lost those marks, so which ones they are is still open.
- Some effects cannot run at the same time (the manual does not list which).

## Effect patterns (effect buttons A1-A4, B1-B4)

| No. | Name in the menu | Variants | What it does | Rebuild idea (cheap on the GPU) |
|---|---|---|---|---|
| 01-02 | STILL1-2 | 2 | Freeze frame | Stop updating the channel texture |
| 03-12 | STROBE1-10 | 10 | Thins out frames: motion halts at intervals ("strobe motion"); dial = fast to slow | Hold every Nth frame; N from the dial or the BPM |
| 13-16 | SHAKE1-4 | 4 | Shakes the picture | Random UV offset per frame, amount per variant |
| 17-20 | NEGATIVE1-4 | 4 | Inverts brightness and colour | `1 - rgb`, or on luma only / chroma only |
| 21-29 | COLORIZE1-9 | 9 | Colours the image for a "paint-like impression"; dial changes the colouring | Luma → colour ramp (gradient map) |
| 30-38 | MONOCOLOR1-9 | 9 | Monochrome colour filter | Luma × a tint colour |
| 39-43 | POSTERIZE1-5 | 5 | Fewer brightness steps | `floor(luma * n) / n` |
| 44-52 | COLORPASS1-9 | 9 | Black and white except one colour | Keep pixels near a hue, desaturate the rest |
| 53-54 | W-LUMIKEY | 2 | Luminance key, cuts out white | Alpha from luma above a level, soft edge |
| 55-56 | B-LUMIKEY | 2 | Luminance key, cuts out black | Alpha from luma below a level, soft edge |
| 57-58 | CHROMAKEY | 2 | Chroma key | Alpha from distance to a key colour in CbCr |
| 59-63 | MULTI-H1-5 | 5 | Multiscreen, split horizontally; dial = number of parts | `fract(uv.x * n)` |
| 64-68 | MULTI-V1-5 | 5 | Multiscreen, split vertically | `fract(uv.y * n)` |
| 69-73 | MULTI-HV1-5 | 5 | Multiscreen, both ways | `fract(uv * n)` |
| 74-78 | MIRROR-H1-5 | 5 | Mirror left/right | `abs` fold of uv.x around a line |
| 79-83 | MIRROR-V1-5 | 5 | Mirror up/down | Fold uv.y |
| 84-88 | MIRROR-HV1-5 | 5 | Mirror both ways (kaleidoscope-like) | Fold both |
| 89-96 | PinP-1-8 | 8 | Picture in picture (8 stored boxes) | Second quad with border and shadow |

**Factory buttons (memory 1):** A = STROBE, NEGATIVE, COLORIZE, MULTI;
B = MIRROR, CHROMA KEY, LUMINANCE KEY, P in P.

### Effect settings

| Setting | Range | Notes |
|---|---|---|
| PinP1-8 HPosi / VPosi | 0-153 / 0-207 | Position of each of the 8 PinP boxes |
| PinP1-8 4:3 HVSize | 0-110 | Size, locked to 4:3 |
| PinP1-8 HSize / VSize | 0-110 / 0-220 | Free size |
| PinP1-8 Border / BColor | 0-15 / 0-15 | Border width and colour (16 colours) |
| PinP1-8 Shadow / SColor | 0-15 / 0-15 | Shadow distance and colour |
| White-LumiKey Level / Edge | 0-255 / 1-15 | Key level and edge softness |
| Black-LumiKey Level / Edge | 0-255 / 1-15 | |
| ChromaKey Color | Blue-Magenta … Yellow-Red | The key colour, as a hue |
| ChromaKey Level / Edge | 0-255 / 1-15 | |

## Transitions (MIX, WIPE and EFX buttons)

| No. | Family | Count | What it is | Rebuild idea |
|---|---|---|---|---|
| 001 | Mix | 1 | Dissolve | `mix(A, B, t)` |
| 002-073 | Wipe01-72 | 72 | Hard-edged wipe patterns | A pattern texture (or SDF) compared with `t` |
| 074-145 | SWipe01-72 | 72 | The same 72 patterns with a soft edge | `smoothstep` around the edge |
| 146-217 | MWipe01-72 | 72 | The same 72 patterns with a multi-colour border | A band around the edge, coloured |
| 218-221 | Key01-04 | 4 | Transitions that use key extraction | Luma/chroma key of B, threshold driven by `t` |
| 222-248 | Slide01-27 | 27 | Slide in / slide out | Move A out and/or B in, 27 directions/combinations |

So there are **72 wipe shapes**, each in three edge styles. The shapes are drawn
on the printed "Transition Effects patterns" sheet that came in the box; we have
not seen it, so the 72 shapes and the 27 slide directions are still open.
**Factory:** MIX = Mix; WIPE = a wipe; EFX = "special wipes and basic slides"
(differs per memory).

## The other buttons

**Transformer A/B** (36 settings per memory):

| No. | Name | What a press does |
|---|---|---|
| 01 | none-none | Nothing |
| 02 | Trans-Trans | Shows the pressed channel while held (factory) |
| 03-12 | A<->9-9<->B … A<->0-0<->B | Toggles between A and B; the digit is the transition time (9 = long, 0 = cut) |
| 13-22 | A<-9 - 9->B … A<-0 - 0->B | Goes to the pressed channel, with that time |
| 23-32 | A->9 - 9<-B … A->0 - 0<-B | Goes to the opposite channel, with that time |
| 33 | White-White | Both buttons fade to white |
| 34 | Black-Black | Both fade to black |
| 35 | White-Black | A fades to white, B to black |
| 36 | Black-White | A fades to black, B to white |

**Output fade:** No Control / Black-White (left = black, right = white,
factory) / AutoB/W (automatic fade). White and black levels 0-255 (Utility).

**BPM SYNC:** mode Direct A-B (cut) or TransitionA-B (using the transition);
speed BPM × 1/4, × 1/2, × 1, × 2. BPM comes from TAP, the BPM dial or MIDI clock.

**Video fader:** mode Normal / Quick, curve A / B / C, calibrate A and B ends.

**Utility menu** (useful ideas for Himmel's own per-input settings): brightness,
colour and hue per channel; sync threshold; picture position and display range;
black level 0 / 7.5 IRE; preview patterns (manual, always 1-4/out, auto cycling)
and display modes; no-signal blue back; colour bars; Presentation mode.

## What Himmel could take from this

- **Cheap effects first.** Almost all 96 patterns are one short pixel shader
  each (or no shader at all: STILL and STROBE only hold frames). That fits
  goal 4 in `CLAUDE.md` (light on the GPU).
- **One wipe engine, many shapes.** 72 shapes × 3 edges = 216 transitions from one
  shader plus a set of pattern functions.
- **Memories as presets.** The V-4's 8 memories (button assignments, PinP boxes,
  key settings) map naturally to Himmel's deck / session presets.
- **Transformer and BPM sync** are good "performance buttons" for Smart Random
  and the MK2's Note Repeat.

## Idea for later: a rack of devices (owner, 2026-11-10)

The owner's idea: like Reason, build Himmel as a **rack of many devices** that you
patch together with cables, to make effects and mixing. A rebuilt V-4 would be
one device: 4 video inputs, a T-bar, two effect channels, one output. Other
devices (a clip player, a camera, an LFO, a beat clock, a keyer) plug into it,
and the controllers (weapons) are patched to their knobs. Not designed yet; the
render rules in `CLAUDE.md` (no allocation, locks or disk I/O in the render loop)
still apply.
