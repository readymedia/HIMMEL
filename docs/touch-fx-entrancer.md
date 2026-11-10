# Touch FX: what Himmel can learn from the Korg Entrancer

Status (2026-11-10): design note. **Step 1 is built**: a Touch FX slot over
the whole canvas with the 8 tier-A effects + Trails, played from the MK2 (knob
page 4, pads as X/Y surface) or the mouse; see [Built so far](#9-built-so-far).
The Lightpad driver comes from the weapon-2 work. Written to answer one
question from the owner: *should Himmel take the form and effects of the old
Korg KAOSS PAD Entrancer and play them from the ROLI Lightpad Block (weapon
no. 2)?*

Short answer: **do part of it.** Take the *idea* (one finger on a surface plays
one effect, X and Y are its two most musical parameters, Hold keeps the last
position, recorded finger motion loops, tempo-synced variants), rebuild about
16 cheap effects in Himmel's own effect engine under Himmel names, and let the
Lightpad's pressure, second finger and LED grid do what the Entrancer could
not. Do **not** copy its panel, its 100-program list, its audio side or its
analog limits. Reasons are in [Recommendation](#7-recommendation).

Korg, KAOSS PAD and Entrancer are trademarks of KORG Inc.; ROLI, BLOCKS and
Lightpad are trademarks of their owners. Himmel is not affiliated with either.
Effects described here get Himmel names and no Korg look-alike art.

---

## 1. The Entrancer in short

| Fact | Detail | Source |
|---|---|---|
| Model | KAOSS PAD Entrancer, model KPE1. Audio + video effects processor with an X-Y touch pad. | [M] p.1, [SOS] |
| Programs | 100 video, 100 audio, 100 combination (video + paired audio). 8 program memory keys. | [M] spec page, [PL] |
| Date | Manual, MIDI chart and program list are dated 2003 (chart "2003.7.7"); reviewed by Sound On Sound in June 2005. | [M], [PL], [SOS] |
| Price at review | GBP 599 (with the KF4 Krossfour vision mixer at GBP 499 as companion). | [SOS] |
| Size | 240 × 247 × 83 mm, 2.0 kg, 7 V DC adaptor. | [M] spec page |

### 1.1 Touch behaviour

- **X and Y**: rub or tap the pad; horizontal and vertical move two effect
  parameters at once. The display shows the position as `1.1`–`9.9`
  (centre `5.5`), so the pad is effectively read as 9 × 9 steps on screen,
  although MIDI sends 0–127. [M] "Using the touch-pad"
- **Taps vs slides**: a finger on the pad switches the effect on; lifting
  switches it off (touch on/off is its own MIDI message). Slides give smooth
  changes, taps give hits. [M], [SOS]
- **HOLD**: when on, the effect keeps the last touched position after the
  finger lifts. HOLD state and position are stored in program memory. [M]
- **FX BALANCE knob**: dry ↔ wet. For most video programs it is a *third*
  parameter instead (see the program table below). In COMBI mode it only
  affects audio. [M] FAQ, [PL]
- **PAD MOTION** (lever up): records about 6 s of finger movement, including
  lifts, and loops it while the lever is held. Touching the pad starts a new
  recording. Stored per program memory. [M] "Pad Motion", [SOS]
- **MUTE/FREEZE** (lever down): freezes the video image (mutes the dry audio).
  [M]
- **Light**: the pad's backlight colour changes with finger position and the
  selected effect. [M] "Main features", [SOS]
- **TAP/BPM**: manual 40–300 BPM, tap tempo, Auto BPM from the audio input
  (80–160), or MIDI clock. The "BPM FX" programs lock to it. [M]

### 1.2 The video programs

From Korg's official *Effect Program List* [PL]. Our own grouping; X/Y/third
are the pad axes and the FX BALANCE knob. (The list was read from the PDF's
text layer; the column alignment looks right but should be re-checked against
the printed sheet before anyone quotes a single row.)

| Nos. | Kind | Examples (Korg's names) | X | Y | Knob |
|---|---|---|---|---|---|
| 00–13 | Colour / tone | Inverse Video, Hue, Monochrome, Color Solid, Noize, Gradation, Emboss, Metal, Sketch, Binary, Fill, Filter, Brightness | hue / colour balance / threshold / level | mostly blend (wet) | hue offset or depth |
| 14–26 | Tiling | Tile, Delay Tile 1–3, Spread, Rhombic, Oblong, Mirror Tile 1–3 | division / scale / delay time | blend | depth / division |
| 27–37 | Geometry + time | Compress, Horizon, Stroboscope, Delay, Blurring, Feedback 1–2, Drag 1–2, Moving Mirror, Turn mirror | H / V position, feedback, time | V or blend | blend / depth |
| 38–53 | Stylise / break-up | Autorotation, Mosaic, Lens 1–2, Out of Sync, 11 Puzzle, Split 12, Snake, Random Disc, Stripe, Dispersal, Cluster, Cut, Random Snap, Move, Wipe | scale / speed / H position | blend or V | depth |
| 54–60 | **BPM FX** (tempo-synced) | Freeze, Delay Tile, Lens, Random Disc, Random Snap, Strobe, Looper | beat division (e.g. 4-2-1-1/2) | blend | – |
| 61–75 | Flip / rotate / zoom | Mirror, Inverse, Rotate 90/180/270, Spin, Expanse 1–2, Reduction, Press, Diffusion 1–2, Live Reduction, Live Spin, Warp | scale / H / speed | blend or V | depth |
| 76–93 | Split / slip / wave | Vertical/Horizontal/Cross Split, Split Delay ×5, Four Corners, Vertical/Horizontal Slip, Squash, Vertical/Horizontal/Distortion Wave, Ripple | scale / delay / density | blend or scale | depth / blend |
| 94–99 | **SAMPLE/PLAY** (~6 s video buffer) | Loop 1–2, Forward Reverse (normal / high speed), Scratch, Time Stretch | loop end/start, reverse↔forward, scratch, slow↔fast | blend | – |

Pattern: **X is almost always "the effect" and Y is almost always "how much"
(blend).** Only geometric effects use X and Y as a real 2-D position.

Each video program also has a fixed paired audio program for COMBI mode
(e.g. Emboss → audio 12). [PL], [SOS]

### 1.3 Graphic patterns (generators)

Patterns 00–39 overlay the audio as oscilloscope, two spectrum analysers,
L-R Lissajous, pan meter and scrolling waveforms (input or output signal),
coloured by pad position or by BPM; some draw a pad pointer / cross cursor.
Patterns 40–59 are picture sources: moving tiles, "sound scopes" with
after-image, colour bars, grey scale, checkerboard, crosshatch. [PL] p.4,
[M] "Graphic patterns". With no video input these make the unit a small
audio-reactive generator. [SOS]

### 1.4 Audio side

Audio programs are grouped FILTER, MOD, DELAY, REVERB, SFX, BPM FX, BPM PTN,
SYNTH, VOCODER and SAMPLE/PLAY: filters and isolators, flanger/phaser/pan,
tape and ping-pong delays, reverbs, pitch shift, slicer, synth and drum
patterns, vocoder, 6 s phrase sampling. Same family as the KAOSS PAD 2.
44.1 kHz, 20-bit converters, stereo RCA in/out, headphone out. [PL] p.3,
[M] spec page, [SOS]

### 1.5 Video I/O

- Input 1: composite (RCA). Input 2: composite **or** S-video (S-video wins).
  A switch picks one; **no mixing of the two inputs.** [M] p.6, FAQ Q7
- Output: composite + S-video. NTSC/PAL switch (power off to change).
  Interlaced input only; copy-protected video does not work. [M] p.6
- Sampling: two still images (one per SAMPLE key) and about 6 s of motion
  video, all lost at power-off. [M], [SOS]
- The Korg tips sheet and SOS describe it used with the KF4 Krossfour mixer
  in front of / after it. [SOS], [TIPS]

### 1.6 MIDI

From the manual's MIDI section and the 2003 implementation chart [M]:

| Message | Default | Direction |
|---|---|---|
| Pad X | CC 12 (Effect Control 1) | send + receive |
| Pad Y | CC 13 (Effect Control 2); also CC 1 and CC 2 | send + receive (CC 1/2 send only) |
| Pad X+Y | CC 93 | send only |
| Pad touch on/off | CC 92 | send + receive |
| Assignable keys | CC 1–31, 64–95 or pitch bend, re-assignable in MIDI Edit | |
| Program change | 0–99 | send + receive |
| Clock | MIDI timing clock | **receive only** (tempo sync) |
| SysEx | Korg header `F0 42 3n 6F`; dumps of maps/global data, device inquiry | send + receive |

Pad motion playback is sent out as normal pad messages. So the Entrancer is a
usable X/Y MIDI controller on its own (7-bit resolution).

### 1.7 Panel

Top panel, left to right in the manual's numbering: control select lever
(AUDIO / COMBI / VIDEO), peak LED, input volume, FX BALANCE, program/BPM
display + knob, 8 program memory keys, TAP/BPM, the touch pad, PAD MOTION /
MUTE-FREEZE lever, HOLD, FUNCTION, SAMPLE 1/2, REC/STOP, VIDEO 2 LED. The pad
and keys are backlit. [M] p.4, [SOS]

### 1.8 Place in VJ history

It is one of the first affordable *hardware* units that put realtime video
effects under a finger: the KAOSS idea (X/Y pad, introduced by Korg for audio
around 1999; KAOSS PAD 2 in 2002 [WP]) moved to standard-definition video.
The reviewer used it on a 2004 tour with DVD content in and the KF4 mixing
two cameras, and judged it in most cases more practical live than software
of the time [SOS]. Its limits are those of 2003 SD hardware: one input at a
time, 7-bit control, volatile samples, interlaced composite. Today laptop
VJ software (Resolume, VDMX, Modul8 — see the design doc's inspiration tab)
does far more, but rarely with this *one finger = one gesture* immediacy. That
immediacy is the part worth keeping.

---

## 2. Which effects are cheap in Himmel

Himmel's goals: show-safe, low latency, light on the GPU. All effects below
are one full-screen pixel shader pass on the canvas (or a deck), with three
normalized inputs: `x`, `y`, `amount` (0..1), plus time and BPM phase. They
fit the planned ISF/HLSL effect slot.

Cost tiers (estimates, to be measured on the GTX 1070 dev laptop and the 5090):

- **A — one pass, no memory** (≈ a texture fetch or a few per pixel):
  colour and geometry.
- **B — one feedback texture** (previous output kept on the GPU): trails,
  drag, freeze, spin-zoom feedback.
- **C — frame history ring** (N past frames on the GPU): time-shifted tiles,
  split delay, RGB time offset, beat looper, scratch on a *live* input.
  1080p RGBA8 is 8 MB per frame; a 1 s ring at 60 fps is ~0.5 GB. Use
  half-resolution history (2 MB/frame) or 30 fps; allocate once at start-up,
  never in the render loop.

| Himmel name (draft) | Inspired by (Entrancer nos.) | Tier | X | Y | Pressure / amount |
|---|---|---|---|---|---|
| **Negate** | Inverse Video (00) | A | – (or hue of the negative) | – | mix |
| **Hue Turn** | Hue, Color Solid, Fill (01, 03, 10–12) | A | hue rotate | saturation | mix |
| **Duotone** | Monochrome, Gradation, Metal (02, 05, 07) | A | dark colour on the palette wheel | light colour | mix |
| **Cut Out** | Binary, Sketch (08, 09) | A | threshold | edge vs fill | mix |
| **Relief** | Emboss (06) | A | light angle | depth | mix |
| **Grid** | Tile, Mirror Tile, Oblong/Rhombic (14, 19–26) | A | tiles 1–16 | mirror on/off band, shape | mix |
| **Kaleido** | Moving Mirror, Turn mirror, Four Corners (36, 37, 86) | A | segments / angle | centre offset | mix |
| **Blocks** | Mosaic (39) | A | block size | aspect | mix |
| **Bulge** | Lens 1–2 (40, 41) | A | position X | position Y | strength (pressure!) |
| **Squeeze** | Compress, Squash, Press, Expanse (27, 89, 70, 67–68) | A | horizontal scale | vertical scale | mix |
| **Slide** | Horizon, Move, Wipe, Drag 2 (28, 52, 53, 35) | A | horizontal offset (wraps) | vertical offset | mix |
| **Waves** | Vertical/Horizontal/Distortion Wave, Ripple (90–93) | A | frequency | amplitude | mix |
| **Slip** | Vertical/Horizontal Slip, Stripe, Out of Sync (87, 88, 47, 42) | A | slice count | offset | mix |
| **Strobe** | Stroboscope, BPM Strobe (29, 59) | A | rate (beat division when BPM is on) | black ↔ white flash | mix |
| **Trails** | Feedback 1–2, Drag, Blurring (32–34, 31) | B | decay | drift direction | mix |
| **Spin Out** | Autorotation, Spin, Live Spin, Warp (38, 66, 74, 75) | B | spin speed | zoom per frame | mix |
| **Hold Frame** | MUTE/FREEZE, BPM Freeze (54) | B | – (or beat division) | – | on while pressed |
| **Echo Grid** | Delay Tile, Split Delay (15–17, 81–85) | C | tiles | frames between tiles | mix |
| **Stutter** | BPM Looper, Loop 1–2 (60, 94, 98) | C | loop length (beat division) | loop start | mix |
| **Scrub** | Scratch, Forward Reverse, Time Stretch (95–97, 99) | C (on clips: free) | play position / speed | – | mix |

Notes:

- **Scrub on clips is free**: Himmel's `ClipPlayer` already plays -2..2× and
  jumps; a Touch FX "scrub" can drive it directly with no history ring. Only
  scrubbing a *camera* or the whole mixed canvas needs tier C.
- **Leave out**: all audio effects (Himmel analyses audio, it does not process
  it), the stateful toys (11 Puzzle, Snake, Random Disc, Dispersal, Cluster:
  fun, but each is real code, so later), and anything about composite/S-video,
  NTSC/PAL and interlacing.
- **Graphic patterns → generators**: oscilloscope, spectrum, Lissajous and
  waveform scroll are cheap generators once the audio-analysis module exists
  (simple level: levels, bands, onsets). They also fit the MK2 screens. Put
  them on the generator list, not in Touch FX.
- **Combi idea, Himmel version**: instead of a fixed audio pairing, an effect
  preset can store an "audio link" (e.g. bass band → amount). Smart Random can
  then pick Touch FX that fit the theme.

---

## 3. Touch → parameters on the Lightpad

What the Lightpad Block gives (from the weapon-2 session's profile,
`weapons/lightpad.weapon.json` on its branch): a 15 × 15 RGB LED grid under a
multi-touch surface, per finger x and y (12 bit), pressure z (8 bit) and
strike velocity, up to 16 fingers, one Mode button on the side.

Checked on the owner's Lightpad Block M (firmware 1.1.0, 2026-11-10, by the
weapon-2 session; details in its `weapons/README.md`):

- Finger indexes start at 1. Start / end messages carry a velocity, moves do
  not. x and y span about 146..3953 (not 0..4095), z spans 0..255, and y = 0 is
  the **top** (USB-C pointing away). The weapon driver normalises x, y to
  0..1 over that range and flips y, so Touch FX gets y = 0 at the bottom, the
  same as the MK2 pads and the mouse.
- The BLOCKS API strike velocity is almost always 1: **useless**. The usable
  strike is the factory program's MPE note-on velocity (3..127), which it keeps
  sending in API mode, or the peak z over the first few ms of a touch (the same
  idea as the MK2's velocity window).
- Mode = button 0 down / up, works in API mode.
- LEDs need Himmel's own LittleFoot renderer uploaded first; until then the
  factory 4 × 4 pad display keeps running. The maximum LED frame rate is not
  measured yet.

### 3.1 One finger (the Entrancer gesture)

- Touch down = effect on (amount ramps in over ~20 ms to avoid a hard pop
  unless the effect is a "hit" effect like Strobe).
- **x → X parameter, y → Y parameter**, full 12-bit, lightly smoothed
  (one-pole, ~5 ms; no smoothing for Strobe/Stutter hits).
- **Pressure → amount** (mix, or the effect's third parameter). This replaces
  the Entrancer's separate FX BALANCE knob, so one hand does all three. A per
  effect curve; a light touch is never zero (minimum 30 %) so a slide without
  pressing still shows the effect.
- **Strike → initial amount** for hit effects (Strobe, Hold Frame). The strike
  comes from the MPE note-on velocity or the early-z peak, never from the
  API's velocity field (see above). Touch FX takes it as the `pressure` of
  `touch_down`, as it does with MK2 pad velocity.
- Lift = effect off (amount ramps out ~60 ms), **unless Hold**.

### 3.2 Hold, motion, more fingers

- **Hold** = the Mode button (toggle). Lift keeps the last x/y/pressure, like
  the Entrancer. A held position shows on the LEDs (see 4).
- **Motion loop** (the Entrancer's Pad Motion, but beat-locked): Shift on the
  MK2 + touch, or a double-tap on the Mode button, records 1/2/4 bars of
  finger movement and loops it in time with the BPM. Touching again overrides
  the loop live; lifting returns to the loop.
- **Second finger** adds what the Entrancer could not:
  - *default*: the distance between the two fingers = the effect's third
    parameter (e.g. Kaleido segments, Bulge radius), the angle = rotation;
  - *option "dual"*: finger 2 plays a second Touch FX slot (two effects at
    once, one per hand).
- **Three or more fingers**: no meaning in v1 (ignored), to keep it learnable.
  Possible later: three-finger tap = Hold Frame.
- **Palm / all fingers off for > 2 s with Hold off** = everything back to dry.

### 3.3 Choosing the effect

On the MK2 (encoder browses the Touch FX list, the screen shows it; see 5)
and, with no MK2, on the Lightpad itself: long-press Mode shows a 4 × 4 grid
of effect tiles on the LEDs (each a 3 × 3 colour block); tap one to choose,
lift to return to playing. Same idea as the Entrancer's 8 memory keys, but on
the surface.

---

## 4. What the 15 × 15 LEDs show

The Entrancer's backlight only changed colour. The Lightpad can draw state:

- **Background tint** = the effect's colour (from the current theme palette),
  dim (≈15 %).
- **Finger cursor**: a bright cross on the touched LED; a short fading trail
  while sliding.
- **Pressure**: cursor size (1 → 3 × 3 LEDs) or brightness.
- **Held position**: a steady cross plus a slow "breathing" ring, so the
  operator sees that the effect is still on with no finger down.
- **Effect-shaped hints** where they help: Grid draws the tile lines,
  Slide draws the offset, Strobe flashes the whole pad on the beat,
  Stutter shows a beat-length bar along the bottom row.
- **Motion loop**: the recorded path drawn faintly, the playback head bright.
- **BPM**: a one-LED pulse in a corner on every beat (always on, so the pad
  also shows that sync is alive).
- **Effect picker**: the 4 × 4 tile grid (3.3).
- **Lost engine** (no LED frames for > 1 s): the weapon shows a dim red
  border by itself, so the operator knows the pad is not playing anything.

LED frames are drawn by the Touch FX layer at ≤ 30 fps (to be checked on the
real block; the LED path is a SysEx shared-data update, so it is slower than
the MK2's HID). Only changed frames are sent. None of this shows until the
weapon has uploaded its own LittleFoot LED program; before that the block
shows its factory 4 × 4 pad display, so Touch FX must work without LED
feedback (the MK2 screen and pad LEDs still show the state).

---

## 5. Where the code goes

Same split as the MK2: the weapon knows the device, nothing else.

| Part | Owns | Does not own |
|---|---|---|
| **Lightpad weapon** (driver + simulator, weapon-2 session) | USB-MIDI / BLOCKS SysEx, pings, reconnect; raw touch events (finger index, start/move/end, x, y, z, velocity); Mode button; "write this 15 × 15 frame"; the lost-engine border | any effect logic, gestures |
| **Touch FX layer** (new; in the app side, like the MK2 pages — never in the render loop) | gestures (hold, motion loop, pressure curve, smoothing, two-finger distance/angle), the effect picker, mapping to effect parameters, drawing the LED frames, the MK2 Touch FX page | device bytes, shaders |
| **Effect engine** (render process) | the shaders, the feedback texture and history ring, a Touch FX slot per deck and one on the master, parameter smoothing per frame | where the numbers came from |

Interfaces:

- Touch events reach the Touch FX layer through the same kind of SPSC event
  ring as the MK2 (`Local\HimmelMk2` today; a weapon-neutral ring later).
- The Touch FX layer writes plain parameters (`slot`, `effect id`, `x`, `y`,
  `amount`, `third`, `on`) into the engine's shared block; the renderer reads
  them lock-free each frame.
- **Any other weapon can play Touch FX**: an MK2 knob pair, a MIDI X/Y pad, a
  mouse on the GUI, or a real Entrancer's CC 12/13/92 (see 6). The Lightpad is
  the best one, not the only one.
- Show safety: if the Lightpad or the Touch FX layer dies, the renderer keeps
  the last parameters for 0.5 s and then fades the Touch FX amount to 0 over
  1 s (held effects stay if "keep held FX on weapon loss" is on — default
  off). A dead history ring allocation at start-up disables only the tier C
  effects.

---

## 6. If the owner has a real Entrancer

It could be a weapon of its own, cheaply:

- **MIDI weapon**: pad X = CC 12, Y = CC 13, touch = CC 92, program change
  0–99, all on one channel; the generic MIDI weapon (planned next) covers it
  with a small profile. 7-bit only, no pressure, no LEDs we can drive (its pad
  light is internal). It receives MIDI clock, so Himmel (or Link via a bridge)
  could keep its BPM FX in time.
- **Its video**: its composite / S-video out can come in through a USB
  capture dongle as a Himmel camera (Media Foundation already does this). Cost:
  SD and interlaced, and the dongle adds latency (measure it with the
  Sampling latency test before any show). It would be a fun "vintage FX" input,
  not a core feature.

---

## 7. Recommendation

**Do part of it.**

Do:
1. A **Touch FX slot** in the effect engine with the tier A effects above
   (start with 8: Hue Turn, Duotone, Grid, Kaleido, Bulge, Slide, Waves,
   Strobe) plus **Trails** (tier B). One pass each; the whole slot costs about
   one extra full-screen pass.
2. The **Touch FX layer**: one-finger X/Y + pressure, Hold on Mode, LED
   cursor + tint + beat pulse, MK2 Touch FX page. Test it with the Lightpad
   simulator before the real block.
3. Then **beat-locked motion loop**, **two-finger third parameter**, **Scrub**
   on clips, **Stutter** / **Echo Grid** (needs the history ring, tier C).

Do not:
- Clone the Entrancer as a product: no panel look-alike, no 100-program list,
  no Korg names, no audio processing, no COMBI audio pairings.
- Rebuild its analog / SD limits (one input, interlace, 6 s volatile buffer).

Why:
- The Entrancer's lasting value is the **gesture**: one finger, X = effect,
  Y = amount, Hold, motion loop, beat-sync. That is easy, fast to learn
  (goal 3) and maps to the Lightpad naturally.
- Most of its effects are **one cheap pixel pass** (goal 4), and touch → shader
  parameter adds no latency beyond the controller (goal 2).
- The Lightpad gives more than the Entrancer had: **pressure** (no FX BALANCE
  knob needed), **more fingers**, and an **LED grid that shows state** (held
  position, loop path, beat) — exactly what made the Entrancer's "Hold" and
  "Pad Motion" hard to read in the dark.
- Keeping Touch FX weapon-neutral means it is not wasted if the Lightpad
  (discontinued, second-hand only) fails at a gig: the MK2 or a mouse plays
  the same slot.

---

## 8. MK2 Touch FX page (sketch)

512 × 64 across both screens; knob labels sit over the physical knob centres
(`kKnobX` = 10, 87, 163, 240 | 266, 343, 419, 496 in `pages.h`).

```
+------------------------------- left 256 ------------++------------------------------ right 256 -----------+
| TOUCH FX  DECK A       07/20  KALEIDO      HOLD  ♩128 || .:::::::::::::. |  X  segments   6                  |
|                                                      || :::::::+:::::::: |  Y  centre   0.42                 |
|  ███████████████▒▒▒▒▒▒▒   X 0.62                      || :::::::+:::::::: |  P  amount   78%  ██████████▒▒▒   |
|  ██████████▒▒▒▒▒▒▒▒▒▒▒▒   Y 0.42                      || ::::+++#+++::::: |  LOOP 2 bars  ▶                  |
|                                                      || :::::::+:::::::: |                                    |
| EFFECT   AMOUNT    X        Y      || HOLD   LOOP     BEAT    TARGET                                       |
+------------------------------------------------------++----------------------------------------------------+
```

- Left half: effect name and number, deck/slot, Hold and BPM flags, the X / Y
  values as bars; knobs 1–4 = **Effect** (browse, same as the encoder),
  **Amount**, **X**, **Y** (so the MK2 alone can play Touch FX).
- Right half: a 1-bit mirror of the 15 × 15 LED grid (4 × 4 px per LED =
  60 × 60 px, with the cursor and held cross), and the live values; knobs 5–8
  = **Hold** (turn = on/off), **Loop** length (off / 1 / 2 / 4 bars),
  **Beat** division for synced effects, **Target** (deck A–D / master).
- Touching the Lightpad inverts the X / Y / amount fields that it is moving,
  like the touched-knob highlight on the other pages.
- It is drawn by `pages.cpp` like the other pages (pure drawing, no heap);
  `mk2_pages_check.exe` can render it to PNG without hardware.

---

## Sources

- **[M]** KORG Inc., *KAOSS PAD Entrancer Owner's Manual* (KPE1), incl. the
  MIDI implementation chart dated 2003.7.7 and the KPE1 MIDI implementation.
  Korg's download: <https://cdn.korg.com/us/support/download/files/fafe124c7ee3e4db52af73477bb64bba.pdf>;
  text copy read at <https://www.manualsdir.com/manuals/162018/korg-kaoss-pad-entrancer.html>
  (pages 4, 6, 8–12, 14–16, 65–68).
- **[PL]** KORG Inc., *KAOSS PAD Entrancer Effect Program List* (2003):
  <https://cdn.korg.com/us/support/download/files/d60a6e5ae13602f7d7db8b541586fc90.pdf>
  (video list p.2, audio list p.3, graphic pattern list p.4).
- **[TIPS]** KORG, *DJ Icewater entrancer Performance Tips* (Rodney Sino-Cruz):
  <https://cdn.korg.com/us/support/download/files/134188f17e21b43ab202e0a677afed14.pdf>
  (found, only its search summary was read).
- **[SOS]** Paul Gilby, "Korg KPE1 Kaoss Pad Entrancer", *Sound On Sound*,
  June 2005: <https://www.soundonsound.com/reviews/korg-kpe1-kaoss-pad-entrancer>.
- **[WP]** Wikipedia, "Kaoss Pad" (KP2 year; it does not cover the Entrancer):
  <https://en.wikipedia.org/wiki/Kaoss_Pad>.
- Lightpad facts: the weapon-2 session's `weapons/lightpad.weapon.json` and
  `tools/lightpad_sim.py` (on its own branch; that session credits its
  BLOCKS protocol sources, ROLI's ISC-licensed `roli_blocks_basics` and
  `blocksd`, in `THIRD_PARTY_NOTICES.md`).

No code, images or text were copied from these; only facts (program names,
parameter assignments, MIDI numbers) are cited, with credit.

---

## 9. Built so far

Step 1 of the recommendation (2026-11-10):

- `src/engine/include/himmel/engine/touch_fx.h` + `src/touch_fx.cpp`: the
  gesture logic (touch down / move / up per source, Hold, depth, pressure →
  amount with a 30 % floor, 20 ms fade in, 60 ms fade out, 8 ms smoothing,
  Strobe as a "hit" with no fades, a lost source lifts its finger). Pure code,
  tested by `touch_fx_tests.exe`.
- `ps_touchfx` in `src/engine/render/shaders.h`: one pass over a copy of the
  canvas, after the clip and camera layers; Trails keeps the last output in a
  second texture. Both textures are made with the canvas (start-up / resize),
  never in the frame loop. The latency test switches Touch FX off.
- Effects (X / Y / pressure): Hue Turn (hue / saturation), Duotone (dark
  colour / light colour), Grid (1–16 tiles / repeat → mirror), Kaleido (2–16
  segments / turn), Bulge (centre X / Y / strength), Slide (shift X / Y,
  wraps), Waves (frequency / height), Strobe (4 … 1/4 beats / flash black →
  white), Trails (decay / drift).
- Tempo: a fixed 120 BPM until Himmel has a tempo source (Strobe).
- Input: MK2 knob page 4 (pads = 4×4 surface, encoder = effect), the mouse on
  any output window (`1`–`9`, `H`). The Lightpad will call the same API with
  `Source::Lightpad`.
- `touch_fx_check.exe`: renders all nine to PNG with the real shader and
  times them. GTX 1070, 1920×1080: 0.05–0.09 ms per pass, Grid 0.3–0.5 ms
  (it shrinks the whole picture per tile without mipmaps; mipmaps or a
  half-size copy would fix that if it ever matters).
- Not done yet: the Lightpad, LED feedback on it, the MK2 page drawing with
  the 15 × 15 mirror (section 8; the page now uses the standard knob layout),
  motion loop, second finger, per-deck slots, tier C effects.
