# Hardware mapping (v3): main weapon, side arms, PREVIEW / PROGRAM

Status: approved by the owner, 2026-11-11 (decisions D42–D76) (v3, after the owner's feedback on
v2). This page goes with [`gui.md`](gui.md). **The GUI mimics the main weapon:
it is laid out like the controller in front of you, so you know where to
press and what will happen.** It is not a mirror of the device. Control names
are the names in `weapons/*.weapon.json`.

> **v4 note (2026-11-11):** how a show is *played* (focus, kits and pad
> targets, songs and scenes, live undo, the NuVJ wheels) is decided in
> [`perform.md`](perform.md). This file gives the button-level mapping and
> follows it.

## 1. Main weapon and side arms

| Role | What it does | Weapons |
|---|---|---|
| **Main weapon** (exactly one) | Sets up the GUI layout, the pages, the screens and the LEDs. Everything can be done from it alone. | **MK2** (default), **NuVJ** (classic two-deck) |
| **Side arms** (zero or more) | **Only add control** (touch effects, more pads, keys, faders, a real crossfader). They never change the layout and never take over the main weapon's job. Each shows up as a docked card. | **Block**, **LUMI**, **Ohm64** (side arm for now, could become a main weapon later), **KPE1 / Entrancer**, any MIDI device (learn) |

Rules for side arms:
- Each card shows the mode, the target (red = PROGRAM, green = PREVIEW) and
  live values.
- Faders and crossfaders use **soft takeover**: a fader acts only after it
  has passed the current value, so nothing jumps.
- If a side arm is unplugged mid-show, its card greys out and the show goes
  on.
- If a side arm has something the main lacks (Ohm64 / Block MIX: a
  crossfader), both work and the GUI shows both.

## 2. One idea: PREVIEW on the left, PROGRAM on the right

The MK2 has two screens with four knobs under each. Himmel uses that shape:

- **Left screen = PREVIEW** (green): you prepare here, the audience cannot
  see it. Knobs 1–4 under it act on the focus (normally the preview).
- **Right screen = PROGRAM** (red): this is what is live. Knobs 5–8 under it
  change what is live.
- **Group buttons, top row A–D = the PREVIEW layers 1–4; bottom row E–H = the
  PROGRAM layers 1–4.** These are *roles*, not fixed decks: after a TAKE the
  two stacks swap, and the rows keep meaning prepare / live. (The engine
  still has decks A / B; the MK2 shows them as roles. The NuVJ shows them as
  fixed decks, as on its panel.)
- **Focus (v4, `perform.md` §1):** the last group pressed sets the focus.
  A–D = PREP focus; E–H = **PLAY focus** (you play PROGRAM directly; the GUI
  frame turns red and the right screen's title inverts). Pads, knobs 1–4 and
  the CLIP / LAYER / FX pages act on the focus; **knobs 5–8 always act on
  PROGRAM** (the scene's macros). Double-tap a group = the same layer on the
  other side.

## 3. MK2 as the main weapon (PERFORM)

### 3.1 The crossfader: the wheel

The MK2 has no fader. **The master encoder (the wheel) becomes an endless
T-bar:**
- **Turning blends PREVIEW into PROGRAM.** One full turn (30 detents) = done.
  Then the roles swap and the wheel starts again from 0. Turning back half
  way reverses the blend.
- **Pushing the wheel**, or `PLAY`, = **automatic TAKE** over the fade time,
  on the beat when GRID is on.
- `RESTART` = **CUT**.
- **Hold a master button + turn the wheel** for other big values:

  | Hold | Wheel sets |
  |---|---|
  | `VOLUME` | Master dimmer |
  | `SWING` | Mood (energy) |
  | `TEMPO` | BPM nudge (tap with `TEMPO` alone) |

- `SHIFT` + wheel = the system menu (kept from 2026-11-09).
- In LIBRARY (`BROWSE`) the wheel scrolls the list. The screen says so, and
  nothing is crossfaded there.

This replaces "encoder = browse / select" (D11). Browsing in Perform moves to
knob 1 (SCROLL) and to `BROWSE`. See D48 in `docs/decisions.md`.

The real crossfader can also come from a side arm (Block MIX mode, Ohm64
crossfader), with soft takeover.

### 3.2 Top buttons (above the screens)

| Button | In PERFORM | With `SHIFT` |
|---|---|---|
| `Top 1`–`Top 4` (over PREVIEW) | **What the dice may change**: CLIP, FX, COLOUR, MOTION (lit = free, dark = held) | LAYOUT, SOURCE, REACT, MOOD-drift |
| `Top 5` (over PROGRAM) | **FREEZE** while held | Latch |
| `Top 6` | **STROBE** while held (beat-locked) | Latch |
| `Top 7` | **BLACK** while held | Latch |
| `Top 8` | **ASSIST** on / off | Autopilot |

### 3.3 Knobs: the PLAY page (default)

| Knob | Acts on | Function | GUI tile |
|---|---|---|---|
| 1 | PREVIEW, selected layer | **SCROLL**: the next / previous clip in the bank loads at once | A film strip |
| 2 | PREVIEW | **DICE**: right = a new look, left = back through history | A fanned deck of cards |
| 3 | PREVIEW, selected layer | **FX amount** of the layer's main effect | before / now / more |
| 4 | PREVIEW, selected layer | **SPEED** (beat-locked steps; `SHIFT` = free) | Streaks |
| 5 | PROGRAM | **INTENSITY** (the energy macro) | Level bars |
| 6 | PROGRAM | **COLOUR** (palette turn; fully left = the clip's own colours) | A colour ring |
| 7 | PROGRAM | **SPACE** (mirror / tiles ← flat → tunnel / zoom) | Tunnel rings |
| 8 | PROGRAM | **MASTER FX** amount (`SHIFT` + turn = choose the effect) | A dry / wet wipe |

The other pages (`PAGE ◀ ▶`) follow the same split: left = preview, right =
program. The full table is in `gui.md` §5.

| Page | Knobs 1–4 (PREVIEW) | Knobs 5–8 (PROGRAM) |
|---|---|---|
| LAYER | **PLACE** (snap: FILL ALL / OUT 1–3 / FIT / REPEAT / FREE), ZOOM / crop, PAN (`SHIFT` = up / down), OPACITY | The opacities of program layers E–H |
| CLIP | **IN** (`SHIFT` = move range), **OUT** (`SHIFT` = length in beats), **MODE** (loop / ping-pong / once / random / chop / step / scratch), **SPEED** or beats per loop | Program layer: speed, play mode, chop / random interval, beat-loop length |
| FX | Choose, param 1, param 2, audio link | Master FX: choose, param 1, param 2, mix |
| MIX | Fade time, curve, preview dim, transition | Master dim, outputs 1–3, output 4, safe limit |
| REACT | Signal, amount, attack, release | REACT macro, bass, mid, high |

### 3.4 Pads: clips by default

The pads sit on the right of the MK2, so the GUI puts a **4 × 4 grid of clip
thumbnails** on the right.

| Mode (button) | Pads | Tap | Pressure (aftertouch) |
|---|---|---|---|
| **CLIPS** (`PATTERN`, **default**) | 16 clips of the selected layer's bank; `Master ◀ ▶` = bank | Load into the selected layer (A–D = PREVIEW at once; E–H = PROGRAM, on the beat); velocity = punch-in; the clip's trigger style applies (normal / toggle / piano / legato) | **PRESS:** the clip's pressure target (e.g. strobe rate + brightness, blur size) |
| **SLICE** (`PATTERN` again; LED blinks) | 16 cue points / slices of the selected layer's clip | Jump (on GRID on PROGRAM); hold = loop the slice; hold A + tap B = in / out; two held = ping-pong | **Harder = shorter beat loop** (1 → 1/16 beat) |
| **LOOKS** (`SCENE`) | The current song's scenes (or a scene bank of 16 without a setlist) | **Lands where the focus is:** PREP = into PREVIEW (with its kit), PLAY = live on the beat; `SHIFT` + tap = the other way; hold = morph | Morph speed |
| **KIT / BANK** (`SHIFT` + `PATTERN`) | Switches what CLIPS shows: the scene's kit (mixed targets) or the bank | — | — |
| **MOODS** (`SHIFT` + `SCENE`) | The 16 moods on the 4 × 4 map | Set the mood (steers the dice) | — |
| **FX PADS** (`PAD MODE`) | 16 momentary effects on PROGRAM | On while held; velocity = punch + variation (soft = subtle, hard = extreme); `SHIFT` + pad = latch | The effect's PRESS target |
| **OUTPUTS** (`NAVIGATE`) | Pads 1–6 = outputs; Top buttons = MAIN / A / B / OWN / TABLE | Choose the output | — |

Modifiers: `DUPLICATE` = copy (pad → pad, group → group), `SELECT` = pick
without firing, `SOLO` = peek (hold: PREVIEW shows only the selected layer,
and the output thumbnails show "next"), `MUTE` = mute the layer;
`SHIFT + MUTE` = **PANIC** (safe scene).

**Pads on PREVIEW and PROGRAM** (full spec: `clips-and-playback.md` §8,
`perform.md` §4):
- The pads follow the focus: A–D = PREVIEW, E–H = PROGRAM (PLAY focus).
- **Pad targets:** hold a clip pad + a group button = that pad always plays on
  that layer (A / E = layer 1 … D / H = layer 4); + `ALL` = all 4 layers;
  + `SOLO` = HIT; + `SELECT` = back to the selected layer.
- `SHIFT` + clip pad (PREVIEW selected) = load and TAKE that layer at once.
- **`SOLO` held + clip pad = HIT:** the clip plays on top of PROGRAM (the hit
  layer) while held; velocity = start opacity, pressure = PRESS, release =
  the clip's fade-out.
- **`REC` held + empty clip pad = SAMPLE** 4 beats of the selected layer's
  source into that pad (the wheel sets the length while `REC` is held).
- In SLICE: `REC` + pad = store a cue point, `ERASE` + pad = clear it,
  `SHIFT` + pad = reverse from there, `NOTE REPEAT` + pad = retrigger
  (pressure = rate).
- The per-pad noise floor (Pad 12) applies before velocity and pressure.
- No flash-rate cap by default; an optional show setting can add one (D65).

**Pad LEDs:**

| LED | Means |
|---|---|
| The clip's own main colour, dim | In the bank |
| Green-tinted bright | In a PREVIEW layer |
| **Red, pulsing on the beat** | On PROGRAM |
| White | Your finger |
| Dark | Empty |

### 3.5 Other buttons

| Button | Function |
|---|---|
| `CONTROL` / `STEP` / `BROWSE` / `SAMPLING` | Views: PERFORM, CUES, LIBRARY, MUSIC |
| `PAGE ◀ ▶` | Knob pages (`SHIFT` = jump to PLAY) |
| `ALL` | The dice scope = the whole look (default: the selected layer) |
| `AUTO WRITE` + knob | Link that knob to an audio signal (REACT); `SHIFT` + `AUTO WRITE` + knob = add an LFO (MOVE) |
| `SELECT` held + wheel | **Scratch** the selected layer's playhead (instead of TAKE) |
| `SHIFT` + `SELECT` + knob | Cycle that parameter's scope: slot / LOOK / GLOBAL / MOVE |
| `Transport ◀ / ▶` | Dice back / forward (the same as knob 2); `SHIFT + ▶` = mutate; **`SHIFT + ◀ / ▶` held with knob 2 = rotate** (see below) |
| `GRID` | On the beat (quantise) |
| `REC` | PREP focus: save the preview scene to the next free scene pad (`REC` + pad = that pad). **PLAY focus: save the live state as a new variation** of the scene (never overwrites) |
| `ERASE` | Undo, 64 deep (`SHIFT` = redo); **hold `ERASE` + group = reset that layer to the scene as saved**; hold `ERASE` + `ALL` = reset the scene (glides 1 beat) |
| `NOTE REPEAT` | Hold = beat stutter on PROGRAM; the wheel sets the rate while held |
| `ENTER` | **GO: the next scene of the song, live, on the beat**; `SHIFT` + `ENTER` = the next scene into PREVIEW |
| Groups | Select a layer (`SHIFT` = mute); the LED = the layer's content colour, brighter in the selected row |

### 3.6 MK2 screens in PERFORM

- **Left screen (PREVIEW):**
  - top line = the labels of Top 1–4;
  - the middle = the look number / name, the selected layer and its clip;
  - bottom line = knob 1–4 labels and values.
- **Right screen (PROGRAM):**
  - top line = the labels of Top 5–8;
  - the middle = what is live, BPM, the TAKE % while the wheel turns;
  - bottom line = knob 5–8.
- The title line of the focus side is inverted (left = PREP, right = PLAY).
- The labels sit over the physical knobs (`kKnobX`).

### 3.7 Presets: dice, mutate, rotate

| Gesture | What it does |
|---|---|
| Knob 2 right / `▶` | **DICE** (a new random look) |
| Knob 2 left / `◀` | Back through the dice history |
| `SHIFT` + `▶` | **MUTATE** |
| `SHIFT` + knob 2 | **ROTATE** through saved presets of the current level |
| FX page, `SHIFT` + knob 1 | Rotate the effect's own presets |
| PLAY page, `SHIFT` + knob 1 | Rotate the selected clip's variants A–D |
| `SHIFT` + knob 8 | Rotate the master FX presets |
| `REC` | Save at the current level |
| `SHIFT` + `REC` | Toggle **auto-rotate** for the current level (every 8 bars by default; the MIX page sets the interval) |

The level follows what is selected:
- effect = an FX page;
- layer = a group button;
- look = `ALL`;
- master = knob 8;
- output = `NAVIGATE` + pad.

### 3.8 Mapping mode (OUTPUTS › Mapping, projectors only)

| Control | Function |
|---|---|
| Knobs 1 / 2 | Move the selected point X / Y (`SHIFT` = finer) |
| Knob 3 | Feather |
| Knob 4 | Blend width (BLEND tool) |
| Pads | Pick a point / corner |
| `REC` | Add a point at the crosshair |
| `ERASE` | Delete the point |
| `ENTER` | Close the mask |
| Wheel | Zoom the view |
| Top 1–6 | The tools (CORNERS, WARP, BLEND, PEN, MAGIC, TEST GRID) |

The **Block** is the trackpad for the crosshair.

## 4. NuVJ as the main weapon (classic)

The GUI is laid out like the NuVJ: **deck A | master column | deck B**, with
the crossfader at the bottom centre. The decks are fixed A / B as on the
panel, and the tallies follow the real crossfader.

| NuVJ | Himmel |
|---|---|
| Deck pads 1–9 | Clips of the deck's bank (GUI: 3 × 3 thumbnails) |
| `BANK` turn / push | Bank − / +; push = dice the deck |
| `PMTR 1` / `PMTR 2` | The two main parameters of the deck's effect (GUI tiles) |
| `LATCH` | Lock the deck's look (no dice) |
| `AUTO` | Dice (long press = mutate) |
| `SYNC` | On the beat |
| `SCRATCH` | Cycles the wheel mode: off = **BEND**, lit = **SCRATCH**, blinking = **FX WHEEL** (the LCD names it) |
| Wheel (jog) | BEND: nudge speed (scrub when paused); SCRATCH: drag the playhead like vinyl, let go = catch-up; FX WHEEL: sweep the deck's main effect, springs back. No touch sensor: "let go" = no movement for ~120 ms (`GUESS:`) |
| Hold a pad + wheel | Trim that clip's in point (the loop length stays) |
| Hold `SYNC` + wheel | The deck's beat-loop length; release `SYNC` = loop off with catch-up |
| Hold `LATCH` + pad | Load scene 1–9 of the song into that deck |
| SPEED fader | Clip speed (centre = ×1 or the synced speed) |
| A / B channel faders | Deck levels |
| Crossfader, `FADER MODE`, `AUTO FADE`, `FADER START` | The crossfader, its curve, TAKE time, start on fader move |
| `INSTA A` / `INSTA B` / `INSTA BLACK` | Fade to deck A / B / black at the `AUTO FADE` speed (INSTANT … SLOW) |
| Tallies | **The crossfader decides:** the deck it favours is PROGRAM (red), the other PREVIEW (green); no focus on the NuVJ |
| `FX LEVEL`, `MASTER FX` (encoder + button), `PMTR 1 / 2` (master) | Master FX: amount, choose, on / off, params |
| `CONTRAST`, `BRIGHT` | Master contrast / brightness |
| `BPM TAP` | Tap tempo |
| LCD (8 characters) | Mood or look name, BPM |

Not checked on the owner's unit yet (GUESS until tested).

## 5. Side arms

### Block (Lightpad)

Switch modes with a double-tap on MODE, or `SHIFT + PAD MODE` on the MK2.

| Mode | What it does |
|---|---|
| **TOUCH** (default) | Touch FX on PROGRAM: X = effect, Y = amount, pressure = depth, a second finger = the third parameter. MODE = hold. The LED border is red on PROGRAM. |
| **MIX** | A sideways swipe = crossfader; the top row = TAKE, the bottom row = CUT. This gives the MK2 a real fader. |
| **LOOKS** | 4 × 4 looks with strike velocity (a second pad surface while the MK2 pads stay on clips). |
| **MOOD** | The mood map (X = calm → intense, Y = warm → cold) steers the dice. MODE = dice now. |
| **SCRUB** | X = the selected layer's playhead between in and out (SCRATCH while touched), Y = speed, pressure = PRESS target; lift = back to the play mode. The strip under the finger glows in the clip's colour. |

### LUMI Keys

MODE switches the layout; OCT ± shifts the range.

| Layout | Keys |
|---|---|
| **FX KEYS** (default) | Left half = colour washes over PROGRAM (strike = punch, pressure = depth, glide = hue). Right half = FX hits. |
| **LOOK KEYS** | Keys = looks from calm to peak |
| **CLIP KEYS** | 24 clips of the selected layer's bank |
| **SLICE KEYS** | The selected clip's 16 slices / cue points on 16 keys (chromatic chop); strike = jump, pressure = beat-loop length, slide = speed bend |

### Ohm64

- Faders 1–4 = PREVIEW layers A–D, faders 5–8 = PROGRAM layers E–H.
- **The crossfader = the crossfader** (soft takeover with the wheel).
- The 8 × 8 grid = 64 clips.
- 16 knobs = the FX parameters of the selected layer.
- `SHIFT` + grid row = that layer's 8 cue points / slices.

### KPE1 / Entrancer

- The pad = Touch FX.
- Programme keys 1–8 = 8 favourite effects.
- HOLD; PAD MOTION = a gesture loop on the beat.
- TAP / BPM.

## 6. Keyboard and mouse (no hardware)

The GUI is the MK2 layout, so the mouse plays it directly.

| Key | Function |
|---|---|
| `SPACE` | TAKE |
| `ENTER` | CUT |
| `←` / `→` | Dice back / forward |
| `R` | Save |
| `1`–`4`, `Q`–`R`, `A`–`F`, `Z`–`V` | Pads |
| Mouse wheel over a tile | Turns that knob |
| `F1` / `?` | Explain |
| `Shift+Esc` | PANIC |

## 7. Profile roles (format addition)

The `himmel-weapon/1` profiles get:
- `"role": "main" | "side" | "both"`;
- a `roles` map from Himmel functions to control names, for example
  `take`, `cut`, `wheel.take`, `dice.next`, `dice.back`, `save`,
  `pads.grid`, `knobs.preview[4]`, `knobs.program[4]`,
  `layers.preview[4]`, `layers.program[4]`, `crossfader`, `touch.xy`,
  `keys`.

The GUI legend chips and the generic weapon driver read it. The CONTROL track
owns the full list.

## 8. Migration from today's engine skeleton

Today the engine uses temporary knob pages:
- 1 = test card, 2 = clip, 3 = camera, 4 = Touch FX;
- `NAVIGATE` = span / mirror, `SAMPLING` = latency test;
- the encoder = next clip, the pads = jump points.

These move to the mapping above when the deck / layer model lands (roadmap
phase 1). The latency test moves to the system menu.
