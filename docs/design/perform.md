# Perform: Himmel as a live instrument (v4)

Status: v4, 2026-11-11. **This file wins** where older docs disagree about
how a show is played. It settles the conflicts between "prepare on PREVIEW
and TAKE" and "play straight on PROGRAM", between pads per bank and pads per
scene, and between the two main weapons. Related:
[`gui.md`](gui.md), [`hardware-mapping.md`](hardware-mapping.md),
[`clips-and-playback.md`](clips-and-playback.md),
[`presets-and-shuffle.md`](presets-and-shuffle.md). The build order for the
first version is [`../build-v1.md`](../build-v1.md).

## 0. The promise

> **Everything you set up is playable, and everything you play is safe.**
> Hands on the weapon, eyes on the room. Nothing on stage needs a mouse.

Three tests every feature must pass:
1. **Can I reach it in one move** (one button, one knob, or button + pad)?
2. **Do I know where it lands** (PREVIEW green or PROGRAM red), before I
   press?
3. **Can I undo it live** in one move, without black or a jump?

## 1. Two ways to play, one surface

Both are good, for different moments, and both are always there. You never
switch "modes"; your hand decides.

| | **PREP → TAKE** (green) | **PLAY** (red) |
|---|---|---|
| Feels like | A TV gallery, a DJ cueing the next track | An instrument, a drum machine |
| Good for | Big changes, new songs, safe builds, trying things | Hits, accents, drops, riding the music |
| Where it acts | PREVIEW (the off-air deck) | PROGRAM (what the room sees) |
| How it lands | The wheel / crossfader / auto TAKE | At once, or on the next beat (`GRID`) |
| Undo | Not needed: nobody saw it | `ERASE` = undo; hold `ERASE` + group = reset that layer to the scene |

**Focus** is the one rule that joins them:

- **The focus is the side of the last group button you pressed.** A–D =
  PREP focus (green frame on the GUI, left MK2 screen title inverted).
  E–H = PLAY focus (red frame, right screen title inverted).
- **Pads, knobs 1–4 and the CLIP / LAYER / FX pages act on the focus.**
- **Knobs 5–8 are always PROGRAM** (the scene's four live macros, §3), so you
  can ride the live picture while you prepare the next one.
- **Double-tap a group** = jump to the same layer on the other side (A ↔ E):
  "take me to that layer, live" or "let me prepare that layer".
- The focus is shown everywhere: the GUI frame, the MK2 screen titles, the
  pad LED tint (green-tinted vs red-tinted), and the Block's border.
- A show setting chooses the **start focus**: PREP (default) or PLAY (for
  people who only want to play live).

This replaces the "LIVE EDIT" name: PLAY focus *is* live editing, and it is
normal, not an exception. The red frame stays.

## 2. Songs, scenes and kits (what you prepare, so you can play)

```
Setlist ─► Song ─► up to 16 Scenes ─► each Scene = Look + Kit + Macros + Transition
```

- **Scene** (the old word "look" means the same; the GUI says *scene*):
  4 layers of one deck, with clips, transport, effects, places and reactions.
- **Kit:** what the 16 pads do *while this scene is on*. Like a drum kit in
  a groovebox: each pad is a ready-made instrument (§4).
- **Macros:** the 4 PROGRAM knobs (5–8) for this scene, chosen at prep
  (default INTENSITY, COLOUR, SPACE, MASTER FX), each with good limits.
- **Transition:** how this scene comes in when taken (time, curve, on the
  beat, or a cut).
- **Variations:** a scene can hold variations (3a, 3b, 3c) made by DICE /
  MUTATE or saved live (§5). ROTATE goes through them.
- **Several scenes per song** give you plenty to play with: an intro scene,
  two verse scenes, a build, a drop, a breakdown. Each brings its own kit, so
  the pads change character with the song.
- **Without a setlist**, the pads in LOOKS mode show a scene bank of 16 (the
  same as before); with a setlist, they show the **current song's scenes**.
  `Master ◀ ▶` in LOOKS = previous / next song (bank without a setlist).

**Firing scenes (one rule: scenes land where your focus is):**

| Gesture | PREP focus | PLAY focus |
|---|---|---|
| Tap a scene pad (LOOKS mode) | Load into PREVIEW (with its kit, so you can rehearse the pads off air) | Take it live on the beat with its transition |
| `SHIFT` + scene pad | Take it live now (cut) | Load into PREVIEW (prepare the next one without leaving PLAY) |
| Hold a scene pad | Morph in over the hold time (1 s = 1 bar, 2 s = 4 bars) | The same, live |
| `ENTER` (GO) | Next scene of the song **live**, on the beat | The same |
| `SHIFT` + `ENTER` | Next scene into PREVIEW | The same |
| Wheel / push / `PLAY` | TAKE PREVIEW → PROGRAM | The same |

So a fully prepared song is played with `ENTER` and the pads; a loose night is
played with dice, PREVIEW and the wheel. Both mix freely.

## 3. Live tweaking on PROGRAM (intuitive, and safe)

- **Knobs 5–8 = the scene's macros.** Each macro drives up to 8 parameters
  inside limits that always look good. You cannot turn the picture into mud
  or black with them.
- **The program layers E–H** (PLAY focus): knobs 1–4 = the CLIP / LAYER / FX
  page of that layer, directly live. Speed and FX changes glide (1 beat by
  default), so nothing jumps.
- **Live holds** (Top 5–8): FREEZE, STROBE, BLACK, ASSIST (latch with
  `SHIFT`).
- **Undo live:** `ERASE` = undo the last change (stepwise, 64 deep);
  `SHIFT` + `ERASE` = redo; **hold `ERASE` + a group = reset that layer to the
  scene as saved**; hold `ERASE` + `ALL` = reset the whole scene. The reset
  glides over 1 beat.
- **Keep what you found:** `REC` in PLAY focus = save the live state as a new
  **variation** of the scene (never overwrites; the screen says "3c saved").
  `REC` in PREP focus = save the preview scene (to the next free scene pad,
  or `REC` + pad).
- **Copy live to prep:** `DUPLICATE` + a program group = that layer into the
  same preview layer; `DUPLICATE` + `ALL` = the whole scene into PREVIEW.

## 4. The pads: kits, targets, focus

### 4.1 What a pad can hold

| Pad holds | Tap | Hold | Pressure |
|---|---|---|---|
| **Clip** (or generator, camera, text line) | Load into its target layer(s) | Piano clips play while held | PRESS target (`clips-and-playback.md` §8.2) |
| **Slice / cue** of a clip | Jump | Loop the slice | Shorter beat loop |
| **Effect hit** | On while held (latch with `SHIFT`) | — | The effect's PRESS |
| **Scene** | §2 | Morph | Morph speed |
| **Action** (black, white flash, freeze, next scene, dice a layer) | Do it | — | Amount |

### 4.2 Where a clip pad plays: its target

Every clip pad has a **target**:

| Target | Means |
|---|---|
| **SELECTED** (default) | The selected layer of the focus side |
| **LAYER 1–4** | Always that layer (of the focus deck) |
| **ALL** | All 4 layers of the focus deck at once, each through its own effects (one clip, four treatments) |
| **FREE** | The lowest free layer (piano pads stack) |
| **HIT** | The hit layer on top of PROGRAM (momentary, `clips-and-playback.md` §3.3) |

- **Set a target on the MK2:** hold the pad, press a group button (A / E =
  layer 1 … D / H = layer 4), `ALL` = ALL, `SOLO` = HIT, `SELECT` =
  SELECTED. The screen confirms ("PAD 6 → LAYER 2").
- **In the GUI:** each clip thumbnail has a small target tag (`L2`, `ALL`,
  `HIT`), and the layer strips light where the pad will land while you hover
  or hold it.
- **Two kit types:**
  - **BANK kit** (no scene kit prepared): the 16 clips of the selected
    layer's bank, target SELECTED. This is the simple default and works with
    no preparation.
  - **SCENE kit:** made at prep. Typical layout, suggested by "Fill kit"
    (`GUESS:` a good default, the owner can change it):

    | Pad row | Holds | Target |
    |---|---|---|
    | 13–16 (top) | 4 background clips | LAYER 1 |
    | 9–12 | 4 overlay clips (alpha / keyed) | LAYER 3 |
    | 5–8 | 4 effect hits (strobe, RGB split, zoom pulse, freeze) | — |
    | 1–4 (bottom) | 2 HIT clips, black, next scene | HIT / actions |

- `PATTERN` toggles CLIPS ↔ SLICE as before; `PAD MODE` = FX PADS; `SCENE` =
  LOOKS (scenes); `SHIFT` + `PATTERN` = KIT / BANK (which kit the CLIPS pads
  show).

### 4.3 Pads follow the focus

- PREP focus: pads load and play on PREVIEW at once (never quantised). You
  can rehearse a scene's kit, slices and pressure off air.
- PLAY focus: pads play on PROGRAM, on the `GRID` beat; hits and slices are
  instant by default (`GUESS:` they feel wrong when delayed; a show setting
  can quantise them).
- `SOLO` + pad = HIT on PROGRAM from **either** focus: the one "fire it live
  now" move that always works.

## 5. Both main weapons, each at its best

The engine, scenes and kits are the same. Each main weapon plays them with
what it has.

### 5.1 MK2: the drum machine

- **Strength:** 16 velocity + pressure pads, 8 knobs, two screens, many
  buttons. **Weakness:** no fader.
- So: **kits, slices and pressure** are the heart. The wheel is the T-bar
  (D48). A Block in MIX mode (or the Ohm64) adds a real fader when wanted.
- PREP and PLAY live side by side (left / right), switched by focus (§1).

### 5.2 NuVJ: the turntables

The NuVJ was sold to DJs as "mix video like you mix music": two decks of
illuminated pads, **two big wheels for scratch, scrub, effect and speed
control**, a DJ crossfader, per-deck auto-fade buttons and one-touch fade
speeds. No pressure, no screen to speak of, but simple and physical.

- **No focus on the NuVJ:** the **crossfader decides**. The deck the
  crossfader favours is PROGRAM (red tally); the other deck is PREVIEW (green).
  Preparing = working on the deck the crossfader is away from.
- **Per deck (A / B):** the 9 pads = the deck's kit (3 × 3: the scene kit's
  top-left 9, or 9 clips of the bank); `BANK` turn = bank, push = dice the
  deck; `PMTR 1 / 2` = the deck's two main parameters; `LATCH` = lock the
  deck's scene (no dice); `AUTO` = dice (long press = mutate); `SYNC` = the
  deck's clip on the beat (BPM time base); **SPEED fader** = clip speed
  (centre detent = ×1 or the synced speed).
- **The two wheels (jogs)**, one per deck, on the deck's selected layer.
  `SCRATCH` cycles the wheel mode (LED off / lit / blinking), and the LCD
  names it:

  | Wheel mode | `SCRATCH` LED | Turning does | Let go (no movement for ~120 ms, `GUESS:` the wheel has no touch sensor) |
  |---|---|---|---|
  | **BEND** (default) | Off | Nudges the speed while turning (faster / slower), like a DJ nudging a record; on a paused clip it **scrubs** frame by frame | Back to normal speed |
  | **SCRATCH** | Lit | Drags the playhead like vinyl: back and forth, fast or slow | Plays on from the **catch-up** position (stays in phase); a show setting can choose "from here" |
  | **FX WHEEL** | Blinking | Sweeps the deck's main effect amount (big movements, beat-locked steps) | Springs back over 1 beat |

  - **Hold a pad + turn the wheel** = trim that clip's in point (the out point
    moves with it, so the loop length stays); the LCD shows the beat offset.
    This is how you set loops on the NuVJ.
  - **Hold `SYNC` + turn** = the deck's beat-loop length (1/16 … 4 beats);
    release `SYNC` = loop off with catch-up.
  - **Both wheels at once + the crossfader** = juggling: scratch both decks
    and cut between them with the fader, as a scratch DJ does.
- **The take tools the NuVJ was built with:** `INSTA A` / `INSTA B` = fade to
  that deck at the `AUTO FADE` speed (INSTANT … SLOW), `INSTA BLACK` = fade to
  black; `FADER START` on = a deck's clip restarts when the crossfader moves
  toward it; `FADER MODE` A / B / C = the crossfader curve (smooth / sharp
  cut / cut-in).
- **Scenes on the NuVJ:** hold `LATCH` + a pad = load scene 1–9 of the song
  into that deck (it becomes the off-air deck's next scene when the fader is
  away). The 8-character LCD shows the scene name and BPM.
- **Where it is weaker:** no pressure (the FX WHEEL replaces PRESS), only 9
  pads per deck, no knob pages. Deep editing stays in the GUI or on an MK2 as
  a side arm.

### 5.3 Side arms

Unchanged (`hardware-mapping.md` §5): the Block adds touch, a fader (MIX) and
scrub (SCRUB); the LUMI adds keys (FX, slices); the Ohm64 adds faders and a
grid. With the NuVJ as main, the MK2 can join as a side arm for kits and
pressure.

## 6. Flow: from an empty show to the gig

1. **Wizard** (clips → weapon → outputs → play). Playable at once with BANK
   kits and the dice.
2. **Prepare a song** (Build › Show, or on the weapon):
   - add scenes: dice + keep (`REC`), or "Fill song from mood";
   - per scene: pick its kit ("Fill kit" suggests one), its 4 macros, its
     transition, clip in / out and modes;
   - rehearse on PREVIEW: load the scene, play its kit off air, tweak, `REC`.
3. **Prepare Show** checks everything (missing files, GPU cost per scene,
   flicker, warm caches) and pre-decodes the first frames of every scene.
4. **At the gig:** `ENTER` through the songs, play the kits, ride the macros,
   dice into PREVIEW between songs, TAKE with the wheel or the fader. Any
   live find is one `REC` away from being a variation for next time.

## 7. Decisions made here (decided by the owner 2026-11-11, `decisions.md` D68–D76)

| # | Decision |
|---|---|
| D68 | Two flows, one surface; **focus = last group pressed**; knobs 5–8 always PROGRAM; "LIVE EDIT" is renamed PLAY focus |
| D69 | **Scene = look + kit + 4 macros + transition + variations**; songs hold up to 16 scenes; LOOKS pads show the song's scenes |
| D70 | **Scenes land where the focus is**; `ENTER` = next scene live, `SHIFT` + `ENTER` = into PREVIEW |
| D71 | **Pad targets** SELECTED / LAYER 1–4 / ALL / FREE / HIT, set with hold pad + group; BANK kit vs SCENE kit (`SHIFT` + `PATTERN`) |
| D72 | **Live undo and reset:** `ERASE` undo, hold `ERASE` + group = reset the layer to the scene; `REC` in PLAY focus = new variation, never overwrite |
| D73 | **NuVJ:** the crossfader decides PROGRAM; wheel modes BEND / SCRATCH / FX WHEEL on `SCRATCH`; hold pad + wheel = trim; hold `SYNC` + wheel = beat loop |
| D74 | Hits and slices in PLAY focus are instant by default; clip and scene launches wait for `GRID` |
| D75 | Start focus is a show setting (PREP default, PLAY for play-only nights) |
| D76 | The first version ("0.1 Playback") is scoped in `docs/build-v1.md` |
