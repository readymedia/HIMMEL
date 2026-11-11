# Hardware mapping: the main instrument and the other weapons

Status: proposal for the owner, 2026-11-11. This page goes with
[`gui.md`](gui.md): the GUI shows the same functions, in the same places, with
the same names. Control names are the names in `weapons/mk2.weapon.json` and
`maschine_mk2_map.json`.

Decisions this builds on (kept as they are):
- The master encoder is the browse/select wheel; `SHIFT` + encoder opens the
  system menu (2026-11-09).
- `SHIFT + PATTERN` picks the theme/mood; `SHIFT + MUTE` is panic;
  `SHIFT + ERASE` is undo (design spec v1).
- `AUTO WRITE` + knob links that knob to an audio signal (design spec v1).

What changes compared with v1:
- **Next / previous preset moves to `TRANSPORT < >`** (v1 used
  `DUPLICATE + < >`).
- **`PLAY` = TAKE** and **`RESTART` = CUT** (the crossfader between the
  decks).
- **`SAMPLING` = the music page** (the latency test moves to the system menu).
- **The groups are split: A–D = Deck A, E–H = Deck B.**

## 1. MK2: the brain

### 1.1 Zones

| Zone | Controls | Job in Perform |
|---|---|---|
| Top row (above the screens) | `Top 1`–`Top 8` | Context buttons. In Perform: **the 8 shuffle locks**. In browsers: categories / filters. Labels sit on the top line of the screens. |
| Screens | Left + right, 256 × 64 each | Page name, deck states, knob labels / values (bottom), Top-button labels (top), the main info in the middle. |
| Knobs | `Knob 1`–`Knob 8` | The current page (MACRO, LAYER, FX1–4, MIX, REACT). Relative (deltas), so values never jump. |
| Left column | `Control`, `Step`, `Browse`, `Sampling`, `Page <`, `Page >`, `All`, `Auto Write` | Views and pages |
| Master section | Master encoder (turn / push), `Volume`, `Swing`, `Tempo`, `Note Repeat`, `Master <`, `Master >`, `Enter` | Browse wheel, mix, mood, tempo, stutter |
| Groups | `Group A`–`Group H` (RGB) | **Layers: A–D = Deck A layers 1–4, E–H = Deck B layers 1–4** |
| Transport | `Restart`, `Transport <`, `Transport >`, `Grid`, `Play`, `Rec`, `Erase`, `Shift` | **CUT, history back, roll / forward, quantise, TAKE, save, undo, shift** |
| Pad modes | `Scene`, `Pattern`, `Pad Mode`, `Navigate`, `Duplicate`, `Select`, `Solo`, `Mute` | What the 16 pads do, plus copy, select, preview-solo and mute |
| Pads | `Pad 1`–`Pad 16` (RGB, velocity, aftertouch) | Scenes (default), clips, FX pads, cues or outputs |

### 1.2 Every control in Perform

| Control | Press | With `SHIFT` | Notes |
|---|---|---|---|
| `Play` | **TAKE:** crossfade PREVIEW → LIVE over the fade time | TAKE at the next bar, even with GRID off | After a TAKE the decks swap roles |
| `Restart` | **CUT:** instant swap | Black-out MAIN (toggle) | |
| `Transport >` | **ROLL:** a new shuffle in the scope, or forward in history if you went back | **MUTATE:** a small variation of the current look | Changes go to the PREVIEW deck unless the scope is a LIVE item |
| `Transport <` | **BACK** one step in the shuffle history | Jump to the oldest kept roll | History holds 64 rolls per deck |
| `Rec` | **SAVE:** the PREVIEW deck as a scene in the next free pad (that pad blinks) | Save the LIVE deck | `REC + pad` = save to that pad |
| `Erase` | Undo | Redo | v1 had undo on `SHIFT + ERASE`; now undo needs no shift |
| `Grid` | Quantise on / off (changes land on the next beat or bar) | Choose beat / bar / 4 bars | The LED is lit when on |
| `Shift` | Hold for the second functions | | |
| `Group A`–`D` | Select layer 1–4 of Deck A | Mute / unmute that layer | Press the selected one again = deck scope; again = master |
| `Group E`–`H` | Select layer 1–4 of Deck B | Mute / unmute | Same |
| `All` | Scope = the whole deck of the selected layer | Scope = both decks + master | Shown as a breadcrumb in the GUI |
| `Select` | Hold + pad = select a scene without loading it (for Rec / Duplicate) | | |
| `Duplicate` | Hold + pad = copy a scene to another pad. Hold + group = copy that layer to the same slot on the other deck | Copy the whole LIVE deck into PREVIEW | "Take what is live and tweak it" |
| `Solo` | Hold = PREVIEW shows only the selected layer; the outputs band shows "next" | | Never changes LIVE |
| `Mute` | Mute the selected layer (instant on PREVIEW, quantised on LIVE) | **PANIC:** the safe scene, LIVE, instant | |
| `Scene` | Pads = **scenes** (default) | Hold + pad = save over that scene | v1: `SHIFT + SCENE + pad` saves |
| `Pattern` | Pads = **clips** of the selected layer's bank | **Mood picker:** 16 moods on the pads | Rows = energy, columns = style |
| `Pad Mode` | Pads = **FX pads** (momentary performance effects on LIVE) | Lightpad mode picker on Top 5–8 | |
| `Navigate` | Pads 1–6 = **outputs** (Top 1–5 = MAIN / A / B / OWN / TABLE for the selected output) | Show next on / off | v1: Navigate = span / mirror; now in the output pages |
| `Step` | Pads = **cues** of the current song; `Transport < >` = previous / next cue; `Play` = GO | Song list on the screens | |
| `Control` | Home: back to Perform, pads to Scenes, knobs to MACRO | | The escape button |
| `Browse` | **Library:** encoder scrolls, Top 1–8 = mood / colour / motion / NEW / FAV filters, pads = 16 results, push or pad = load into the selected layer on PREVIEW | `SHIFT + BROWSE` = Explain mode | On an FX page: opens the effect browser |
| `Sampling` | **Music page:** input levels, bands, BPM, clock source, AV offset on the knobs | | v1 used this for the latency test |
| `Page <` / `Page >` | Knob page: MACRO · LAYER · FX1 · FX2 · FX3 · FX4 · MIX · REACT | `SHIFT + PAGE` = MACRO | |
| `Auto Write` | Hold + turn a knob = link it to an audio signal (turn to choose the signal) | Hold + turn a knob = remove its audio link | The A-mark shows on the screen |
| Master encoder turn | Browse / select in the current list | System menu (fun modes, latency test, settings) | Decided 2026-11-09 |
| Master encoder push | Load / confirm | Effect on / off in the selected FX slot | |
| `Volume` | **MIX page** on the knobs (crossfader, fade time, curve, master dimmer, deck A / B level, output dimmer, preview level) | | |
| `Swing` | **Mood page:** knobs 1–2 = energy / style, 3 = colour from mood ↔ from clip, 4 = variation | | |
| `Tempo` | Tap tempo | Clock-source menu | |
| `Note Repeat` | Hold = **beat stutter** on LIVE; the encoder sets the rate while held | | |
| `Master <` / `Master >` | Previous / next scene bank | Previous / next song | |
| `Enter` | GO (next cue), the same as `Play` in Step mode | | For cue-based shows |

### 1.3 Pads

- **Scenes:** 16 per bank. Rows go from calm at the bottom to peak at the top.
  The colour is the scene's main colour; the brightness is its energy; the
  active scene pulses on the beat; empty pads are dark.
  - Tap = load into PREVIEW.
  - `SHIFT` + tap = straight to LIVE (morph).
  - Hold = longer morph: a short press is a cut on the beat, 1 s = 1 bar,
    2 s = 4 bars.
  - Velocity scales the morph's "kick" (for example a flash on arrival).
- **Clips:** the selected layer's bank. Tap = load; aftertouch = speed nudge
  while held.
- **FX pads:** hold = on, velocity = intensity, aftertouch = amount. They go
  to LIVE (stutter, strobe, flash, kaleido, invert, freeze, zoom punch,
  RGB split, …). They are set per mood.
- **Cues:** the next cue is outlined; tap = arm it in PREVIEW.
- **Outputs:** pads 1–6, coloured by source (MAIN white, A cyan, B amber,
  OWN violet, TABLE green).

### 1.4 LEDs

- **Group buttons:** the layer's colour (its source's main colour). The LIVE
  deck is at full brightness and the PREVIEW deck at 30 %. The selected layer
  blinks once on selection and then shows white on its second LED. A muted
  layer is off.
- **Play:** red while a TAKE is running, dim red at rest. **Restart:** dim.
- **Transport >:** lit violet when MUTATE / ROLL is available.
  **Transport <:** lit when there is history to go back to.
- **Rec:** blinks red while a pad is waiting for a save target. **Grid:** lit
  when quantise is on.
- **Top 1–8 (locks):** lit = free to change, off = locked.
- **Pad-mode buttons:** the active mode is lit.

### 1.5 Screens in Perform

```
LEFT 256×64                                   RIGHT 256×64
CLIP  SOURCE  FX    COLOUR   ← Top 1–4 labels  MOTION LAYOUT REACT  MOOD  ← Top 5–8
A ▮LIVE 100%   B ▯PREV        ♩128.0 LINK ●●○○  ROLL #23  TECHNO·cold   GPU 3.1ms
[ big: scene name / clip name of the selected layer ]   [ big: shuffle info / mood ]
ENERGY  COLOUR  SPEED  DENSITY  ← knob 1–4      CHAOS   GLOW   SPACE  REACT ← knob 5–8
 ▮▮▮▯    ▮▮▯▯   ▮▮▮▮    ▮▯▯▯                     ▮▯▯▯    ▮▮▯▯   ▮▮▮▯   ▮▮▮▮
```

The labels sit over the physical knob centres (`kKnobX` = 10, 87, 163, 240,
266, 343, 419, 496). Drawing uses the existing `pages.cpp` page types: Perform
(knob slots), list (browser, menus), detail (thumbnail), audio (meters) and
message.

## 2. Lightpad Block: the hand

The block keeps one job at a time. **Its mode is chosen with a double-tap on
its Mode button** (cycle) or from the MK2 (`SHIFT + PAD MODE`, then Top 5–8).
A corner LED shows the mode colour.

| Mode | Colour | X / Y / pressure | Mode button | LEDs |
|---|---|---|---|---|
| **TOUCH** (default) | effect colour | **Touch FX** on the target: X = effect choice or param 1, Y = amount or param 2, pressure = depth; two fingers = third parameter | Hold (keep the effect after lifting) | Effect colour field, finger cursor, held cross, beat pulse on the border. The border is **red when acting on LIVE** and green on PREVIEW |
| **MIX** | white | Horizontal swipe = **crossfader**; tap the top row = TAKE; tap the bottom row = CUT; the left column = master dimmer | Black-out toggle | A bar from deck A cyan to deck B amber, with the position, and red on the live side |
| **SCENES** | scene colours | A 4 × 4 grid = the scene pads, with velocity from the strike | Bank + 1 | Scene colours, mirroring the MK2 when its pads are in another mode, so you get a second scene surface |
| **MOOD** | gradient | **The mood map:** X = energy (calm → intense), Y = style (organic → synthetic). The next ROLL draws near the point | ROLL (shuffle now) | Mood-map colours, with a dot at the point |

The Touch FX target is LIVE MAIN by default. MK2 `PAD MODE` page, knob 8 =
target (LIVE, PREVIEW, selected layer).

What is known about the block (from the hardware notes):
- touches come about every 28 ms;
- LED frames run at about 55 fps;
- pressure sensitivity is set to 127;
- y = 0 is at the top with the USB-C pointing away from you.

## 3. Keyboard and mouse

| Key | Function |
|---|---|
| `SPACE` | TAKE |
| `ENTER` | CUT |
| `→` / `←` | ROLL / BACK |
| `SHIFT + →` | MUTATE |
| `R` | SAVE |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `1`–`4`, `Q`–`R`, `A`–`F`, `Z`–`V` | 16 pads (4 × 4) |
| `F1` / `?` | Explain mode |
| `F2` | Weapon band |
| `Esc` | Home |
| `Shift+Esc` | **PANIC** |

## 4. NuVJ alone: the classic two-deck setup

The NuVJ already matches the A/B + crossfader model. These are proposals,
because the hardware is not yet checked on the owner's unit.

| NuVJ | Himmel |
|---|---|
| Deck A / B pads 1–9 | The cells of the deck's current bank (9 per bank, as in the classic view) |
| `BANK` | Bank − / + (turn) |
| `PMTR 1` / `PMTR 2` | Knob 1 / 2 of the selected cell's effect |
| `LATCH` | Lock the current look of that deck (no shuffle on it) |
| `AUTO` | **ROLL** for that deck (`SHIFT`-less: a long press = MUTATE) |
| `SYNC` | Quantise on / off |
| Jog / `SCRATCH` | Scrub the clip / speed |
| Crossfader + `FADER MODE` | **The crossfader** (curves) |
| `AUTO FADE`, `INSTANT` / `SLOW` | TAKE / CUT |
| `INSTA A` / `INSTA B` | Flash that deck to full |
| `MASTER FX`, `FX LEVEL` | Master slot 1 + mix |
| `INSTA BLACK` | Black-out |
| `BPM TAP` | Tap tempo |
| `CONTRAST`, `BRIGHT` | Master contrast / brightness |
| LCD (8 characters) | Mood / scene name, BPM |

## 5. The other weapons (integrations)

Each integration gets **roles** in its profile (§6), so it plays the same
functions without new GUI code. Suggested defaults:

| Weapon | Default roles |
|---|---|
| LUMI Keys | Keys = scenes across the current bank (low → high = calm → peak); buttons = ROLL / BACK / TAKE |
| Entrancer | Touch FX (the same as the Lightpad's TOUCH mode) |
| Ohm64 | 8 × 8 grid = 4 scene banks; faders = layer opacities; crossfader = crossfader |
| V-4 (sim) | T-bar = crossfader; effect buttons = FX pads |
| KF4 | No data port: its output can be captured as a camera |

## 6. Profile roles (format addition)

The `himmel-weapon/1` profiles get an optional `roles` map from a Himmel
function to the profile's control names. The GUI legend and the generic
weapon driver both read it.

```json
"roles": {
  "take": "Play", "cut": "Restart", "roll": "Transport >", "back": "Transport <",
  "save": "Rec", "undo": "Erase", "panic": ["Shift", "Mute"],
  "crossfader": null, "pads.scenes": ["Pad 1", "...", "Pad 16"],
  "knobs.page": ["Knob 1", "...", "Knob 8"], "layers": ["Group A", "...", "Group H"]
}
```

Missing roles get an on-screen control. The full list of role names is owned
by the controller track (`docs/agents/README.md`) and documented with the
generic weapon driver.

## 7. Migration from today's engine skeleton

The engine skeleton uses temporary knob pages (1 test card, 2 clip, 3 camera,
4 Touch FX), `NAVIGATE` = span / mirror, `SAMPLING` = latency test, the
encoder for next clip and the pads for jump points. These are stand-ins. When
the deck / layer model lands (roadmap phase 1), they move to the mapping
above:
- the clip, camera and Touch FX pages become the LAYER and FX pages;
- span / mirror moves to the output pages;
- the latency test moves to the system menu (`SHIFT` + encoder).
