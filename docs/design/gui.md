# GUI specification (v3 + v4 perform flow: the GUI mimics the main weapon)

Status: approved by the owner, 2026-11-11 (decisions D42–D76).
- **v4** (the basis for 0.1): the instrument flow in [`perform.md`](perform.md)
  (focus, kits and pad targets, songs and scenes, live undo, the NuVJ wheels)
  and the v4 artboards (§17). Where v3 text differs, v4 wins.
- **v3** replaces v2 for Perform, the weapon select and the adaptive layout,
  after the owner's feedback: *mimic the controller, do not mirror it; the
  pads are clip thumbnails; prepare vs program must be crystal clear; one
  main weapon plus side arms.*
- The drawings are the **v3** artboards on the design canvas (page
  "v3 · Instrument") and their copies in `docs/gui-mockups/v3/`. v2 is kept
  in `docs/gui-mockups/v2/` for the parts v3 does not redraw (wizard steps 1
  and 3, library, shuffle close-up, table).
- The hardware side is [`hardware-mapping.md`](hardware-mapping.md); the two
  are one design and change together.

## 1. Principles

1. **Mimic, don't mirror.** The screen is laid out like the main weapon on
   the desk in front of you: the pads where the pads are, the screens where
   the screens are, the transport where the transport is. You look at the
   GUI, put your hand on the same place on the controller, and know what will
   happen.
   - We never draw knobs. We draw **what turning will do** (tiles).
   - We never draw pads as pads. We draw **what they hold** (clip
     thumbnails).
   - Every control carries a small hardware chip (`PLAY`, `SHIFT+MUTE`).
2. **PREVIEW on the left, PROGRAM on the right.** Green = prepare (the
   audience cannot see it), red = live (on the outputs), as in a TV gallery.
   - On the MK2, the left screen + knobs 1–4 + group row A–D are PREVIEW;
     the right screen + knobs 5–8 + group row E–H are PROGRAM.
   - **Focus** (v4): the last group pressed. PREP focus = green frame,
     PLAY focus (playing PROGRAM directly) = red frame, so you always know
     where the pads and knobs 1–4 land. Knobs 5–8 are always PROGRAM
     (`perform.md` §1).
3. **Dark only.** The background is near-black (`#0B0D10`), the panels are
   slightly lighter, and colour is reserved for state (live, preview, mood,
   layers) and for the content itself. A GUI brightness setting dims
   everything for dark venues.
4. **Three depths, one at a time:**
   - **Play:** scenes, shuffle, crossfader and macros.
   - **Shape:** layers, effects and parameters.
   - **Build:** library, cues, outputs and mapping.

   Perform always shows Play. Shape opens in place (the knob strip changes
   page) and Build lives in its own views. Nothing deeper than Play is ever
   needed to run a show.
5. **The GUI never touches the show directly.** It is a separate process that
   sends commands and shows previews (see §10). If it crashes, the output does
   not change.
6. **It adapts to the weapon** (see §8).
7. **Big targets and big text.** At least a 44 px hit area and at least 13 px
   text. It must be readable from one metre in a dark booth.

## 2. Colours and type

| Token | Value | Use |
|---|---|---|
| `bg` | `#0B0D10` | Window background |
| `panel` | `#14181D` | Panels |
| `panel-2` | `#1B2027` | Raised panels, inputs |
| `line` | `#2A313A` | Borders |
| `text` | `#E8ECF1` | Main text |
| `text-dim` | `#9AA4B2` | Secondary text (≥ 4.5:1 on `panel`) |
| `live` | `#FF3B30` | LIVE tally, on-air outputs |
| `preview` | `#30D158` | PREVIEW tally |
| `deck-a` | `#2EC5FF` | Deck A (cyan) |
| `deck-b` | `#FF9F1C` | Deck B (amber) |
| `accent` | `#B78CFF` | Shuffle / dice, mood |
| `warn` | `#FFD60A` | Warnings |

Type: Inter for the UI and JetBrains Mono for numbers (BPM, timecode, ms). The
MK2 screen mirror uses the real 1-bit font from `pages.cpp`.

The deck colours are cyan and amber, not red and green, so they can never be
confused with the tallies. Together with the tally colours they also stay
apart for the common colour-blindness types.

## 3. The start: a four-step wizard

The first run is a four-step wizard. A bar at the top always shows
**1 CLIPS · 2 WEAPON · 3 OUTPUTS · 4 PLAY**, and you can click any step.

**Before the wizard: the start screen** (`projects.md`): *New project*,
*Open…* (a project folder or a `.himmelshow` file), and the last 10
projects as cards (thumbnail, name, date, outputs). "Restore autosave" shows
here after a crash.

On later starts Himmel opens the last project and goes straight to step 4 after a one-line health check
("All good: 312 clips, MK2 + Lightpad, 3 outputs"). If something changed (a
display missing, a controller gone), it opens the step that needs you.

### Step 1: Add clips

- A big drop zone: "Drop folders or clips here". There is also **Use the
  Himmel starter library** (about 50 clips plus generator looks), so you can
  play right away.
- An import queue with one row per clip, which runs through these stages:

  > *convert → analyse → tag → ready*

  The thumbnail fills in as the clip is analysed. You do not have to wait:
  **Next** is always active. The import goes on in the background, and clips
  join the library as they become ready.
- Every ready clip shows its tags right away:
  - a palette strip with 5 colours;
  - energy pips from 1 to 5;
  - motion (calm, medium or fast);
  - mood chips (chill, groove, techno, crazy);
  - type tags (water, city, people, abstract, and so on).
- Options, folded away by default:
  - Keep resolution (the default) or Optimise for show (1080p).
  - Codec Auto (HAP, HAP Q for fine detail, HAP Alpha when the clip has
    transparency).
  - Make loops seamless (crossfade the ends).
  - Analyse people / pose (slower, used by tracking effects).

### Step 2: Choose your weapon (main + side arms)

- A game-style select screen in two rows:
  - **MAIN WEAPON** (pick exactly one: **MK2** or **NuVJ**). The main weapon
    decides the GUI layout, the pages and the screens.
  - **SIDE ARMS** (add any: **Block, LUMI, Ohm64, KPE1**, any MIDI device by
    learning). Side arms never move the layout; they add control (touch,
    keys, faders, a real crossfader).
- Each card shows:
  - the drawing from the profile;
  - a stats line ("2 screens · 8 knobs · wheel · 16 pressure pads");
  - its status: **CONNECTED** (glowing), **SIMULATE**, **NOT FOUND**;
  - one line on the layout it gives.
- **Touch your weapon to choose it:** pressing any control on a connected
  device selects or adds it. This doubles as a hardware test.
- The **loadout line** at the bottom names the result, for example
  "MK2 + Block + LUMI". It is saved with the show.
- No hardware at all: **Keyboard + mouse** plays the MK2 layout on screen.
- **Sound in** (a card under the loadout): pick the input (microphone, line
  in, or "sound on this PC"), see the live meter and the three bands, and a
  green "hearing music" tick when onsets arrive. Auto gain is on. This is
  what makes every effect move with the music (`music.md` §5).

### Step 3: Set outputs (screens or projectors)

0. **IDENTIFY** (top right, always there): big numbers on every connected
   display; drag them into the physical order (`operations.md` §2).
1. **Where does the picture go?** Two big cards:
   - **Screens** (TVs, monitors, LED walls): the edges just meet, **no mapping
     is needed**, with optional bezel compensation (mm) so lines cross the
     frames straight.
   - **Projectors**: keystone, warp, **edge blend** where they overlap, and
     pen-tool masks. Mapping comes next (§13).
2. **How many:** **1**, **2 side by side** or **3 side by side** (2 × 2 and
   custom grids later).
3. **Resolution per output**, four big chips: **720p · 1080p · 1440p · 4K**
   (1080p is the default and marked "recommended"; 1440p and 4K are marked
   "recommended for big rigs" with the GPU cost shown). The wizard
   preselects the display's native mode and warns if outputs on one canvas
   differ. A custom size hides under "Advanced" (`projects.md` §5).
4. **The canvas you play on** is drawn to scale, with its size:
   - 1 output = 1920 × 1080;
   - 3 screens = 5760 × 1080 (edges meet);
   - 3 projectors with a 14 % blend = 5222 × 1080, with hatched blend zones.

   The blend overlap slider changes the canvas width live, and every monitor
   in Perform follows.
5. **Extra outputs** (an LED strip, a TV at the bar), each with its own source
   (MAIN, A, B, OWN, TABLE).
6. **Refresh:** 60 Hz (default) or 50 Hz for the whole canvas
   (`operations.md` §3). Checks: the same refresh rate on the canvas outputs, flip model, and
   measure latency.
7. **Show type:** VJ or Overlay (§ engine), as before.

The GUI screens are never show outputs; the wizard keeps them apart (§14).

### Step 4: Play

This opens the Perform view.

## 4. Perform with the MK2 as main weapon

The layout follows the MK2 from top to bottom and left to right. It is drawn
for a 1920 × 1080 operator screen and works at 1440 × 900. See the
interactive artboard "Perform · MAIN = MK2".

```
┌ status strip: HIMMEL · MAIN MK2 + side arms · BPM ●○○○ · mood · status · GPU ms · PANIC ┐
│ LEFT RAIL        TOP 1–4: what the dice may change   TOP 5–8: FREEZE STROBE BLACK ASSIST  │ OUTPUTS │
│ PERFORM  CUES   ┌──── PREVIEW (left screen, green) ─┐┌──── PROGRAM (right screen, red) ───┐│ 1 ● MAIN│
│ LIBRARY  MUSIC  │                                    ││                                    ││ 2 ● MAIN│
│ ◀ PAGE  PAGE ▶  └────────────────────────────────────┘└────────────────────────────────────┘│ 3 ● MAIN│
│ WHOLE   LINK    [SCROLL][DICE][FX AMT][SPEED]          [INTENSITY][COLOUR][SPACE][MASTER FX]│ 4 ● OWN │
├ MASTER: DIM/MOOD/TAP + the TAKE WHEEL ┬ PADS SHOW ┬ 4×4 CLIP GRID (thumbnails) ┬ SIDE ARMS ┤
│ GROUPS: preview row A–D / program E–H │ LOOKS     │                            │ Block card│
│ TRANSPORT: CUT ◀ ▶ ON-BEAT / TAKE SAVE│ CLIPS ●   │                            │ LUMI card │
│ UNDO SHIFT                            │ FX · OUT  │                            │           │
└───────────────────────────────────────┴───────────┴────────────────────────────┴───────────┘
```

### 4.1 Status strip

- **The loadout:** MAIN · MK2 (white chip), then the side arms (green chips
  when connected).
- **Tempo:** BPM, four beat squares, the clock source.
- **The mood.**
- **Status lights:** sync, audio, cams, import, always with a word, never
  colour alone.
- **GPU ms** against the budget.
- **PANIC** (`SHIFT+MUTE`).

### 4.2 Upper half: the two screens and the knob tiles

- **The left rail** mimics the left button column: PERFORM (`CONTROL`), CUES
  (`STEP`), LIBRARY (`BROWSE`), MUSIC (`SAMPLING`), `◀ PAGE ▶`, WHOLE LOOK
  (`ALL`), LINK AUDIO (`AUTO`). It also shows the current page
  (PLAY · LAYER · FX · MIX · REACT).
- **The top buttons row:**
  - over PREVIEW: four **dice locks** (CLIP, FX, COLOUR, MOTION; lit = free
    to change);
  - over PROGRAM: four **live holds** (FREEZE, STROBE, BLACK, and ASSIST
    on / off).
- **The monitors take the shape of the canvas** (§12):
  - with **1 output** they are 16:9, side by side (PREVIEW left, PROGRAM
    right);
  - with **2–3 outputs** (32:9, 48:9, or the blended width) they become two
    **full-width strips**, PREVIEW above and PROGRAM below, so the superwide
    picture stays readable;
  - the output edges are thin lines, and the blend overlaps are hatched;
  - the knob tiles stay split left (preview) / right (program) under them.
- **The PREVIEW monitor** (left, green frame, "prepare here, the audience
  can't see this") and **the PROGRAM monitor** (right, red frame, "on
  air → OUT 1 · 2 · 3").
- **8 knob tiles** under the monitors, each exactly above its physical knob:
  - left four act on the preview: SCROLL clips, DICE, FX amount, SPEED;
  - right four act on program: INTENSITY, COLOUR, SPACE, MASTER FX.
- **Tiles never show knobs.** Each tile shows the outcome: the middle is now,
  the faded edges are what you get by turning left or right, and a thin bar
  is the value. Every tile type is on the "Knob tiles" artboard. The last
  turned tile lights up white for 1 s (MK2 knobs are not touch-sensitive).
- **The outputs column** on the right (the panel's empty corner on the MK2):
  one tile per output, with a red tally, its source (MAIN / A / B / OWN /
  TABLE), and `NAVIGATE` + pad to route.

### 4.3 Lower half: master, groups, transport, pads, side arms

- **Master block** (lower-left, as on the MK2):
  - DIM (`VOLUME`), MOOD (`SWING`) and TAP (`TEMPO`) next to **the TAKE
    wheel**;
  - the wheel is a ring that fills red as the wheel turns ("64 %");
  - "Turn = crossfade PREVIEW → PROGRAM, one turn = done, then the screens
    swap; push = auto TAKE".
- **Group cards, 2 × 4, as on the MK2:**
  - the top row = PREVIEW layers A–D (green labels);
  - the bottom row = PROGRAM layers E–H (red labels);
  - each card shows the layer's thumbnail;
  - the selected card is outlined white. Selecting a bottom-row card starts
    **PLAY focus** (the red frame).
- **Transport, 2 × 4, as on the MK2:**

  | | | | |
  |---|---|---|---|
  | CUT (`RESTART`) | ◀ BACK | DICE ▶ | ON BEAT (`GRID`) |
  | **TAKE** (`PLAY`) | SAVE (`REC`) | UNDO (`ERASE`) | SHIFT |

- **"Pads show" tabs** (the MK2's pad-mode column): LOOKS (`SCENE`),
  **CLIPS** (`PATTERN`, default), FX PADS (`PAD MODE`), OUTPUTS
  (`NAVIGATE`), then COPY, PICK, PEEK and MUTE.
- **The 4 × 4 grid** (right, where the pads are):
  - in CLIPS: **thumbnails of the 16 clips** of the selected layer's bank,
    with the header "→ loads into PREVIEW layer A" (green) or "→ loads into
    PROGRAM layer F (live)" (red);
  - the ring and badge on each thumbnail match the pad LED: green = in a
    preview layer, red = on program, white = pressed (with the pressure in %,
    "squeeze");
  - in FX PADS: effect names on PROGRAM, with the held pad's depth;
  - in LOOKS: saved looks.
- **Side arms** dock as cards to the right of the pads (§8). Each card shows
  its current mode and target.

### 4.4 Why this works

- **The answer to "where do I press?" is "where it is on the screen".**
- **The answer to "what will happen?" is in the tile above the knob** (the
  edges show it).
- **The answer to "is this live?" is the colour of the half you are in:**
  left / top row = green, right / bottom row = red.
- **The crossfader the MK2 does not have becomes its biggest control,** the
  wheel. It is turned like a T-bar and shown as a filling ring.

### 4.5 Weapon view and audience view

The "MK2 cheat sheet" artboard draws the MK2 from its profile with every
control labelled with its Himmel function, coloured green / red / violet.
- It can go full screen on a second monitor (the **audience view**), so
  people at the booth see what each button does.
- With **Explain** on (`SHIFT + BROWSE`), pressing a control shows its job
  instead of doing it.

## 5. Shape: knob pages and the effect browser

`PAGE ◀ ▶` changes what the tiles mean. Knobs 1–4 always act on the selected
PREVIEW layer, and knobs 5–8 always act on PROGRAM. The tiles are pictures:
"scroll" tiles are film or word strips, and value tiles are pictures with a
bar.

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 | Knob 5 | Knob 6 | Knob 7 | Knob 8 |
|---|---|---|---|---|---|---|---|---|
| **PLAY** | Scroll clips | Dice | FX amount | Speed | Intensity | Colour | Space | Master FX |
| **LAYER** | Opacity | Blend (scroll) | Zoom | Position | Prog. layer E | F | G | H opacity |
| **FX** | Choose (scroll) | Param 1 | Param 2 | Audio link | Master FX choose | Param 1 | Param 2 | Mix |
| **MIX** | Fade time | Curve | Preview dim | Transition (scroll) | Master dim | Out 1–3 | Out 4 | Safe limit |
| **REACT** | Reacts to (scroll) | Amount | Attack | Release | REACT macro | Bass | Mid | High |

**Presets on every level** (see `presets-and-shuffle.md` §2 and the
"Presets" artboard):
- The same three verbs work at the effect, layer, look, master and output
  levels: **DICE** (knob 2 / `▶`), **MUTATE** (`SHIFT+▶`) and **ROTATE**
  through saved presets (`SHIFT` + knob 2, `SHIFT+◀ ▶`; on the FX page
  `SHIFT` + knob 1 rotates the effect's own presets).
- `REC` saves at the level you are on.
- **Auto-rotate** can step through a level's presets every n bars.

**The effect browser** (`BROWSE` on the FX page):
- Top 1–8 are the categories (COLOUR, SPACE, DISTORT, LIGHT, STYLE, TIME,
  KEY, FAVS).
- The 4 × 4 grid shows **the selected layer with each effect applied**, as
  live thumbnails, so you pick by looking. A dimmer colour means a heavier
  effect.
- Tap = try it on PREVIEW; tap again = keep it.
- Add-ons are the ninth category (`SHIFT + FAVS`), with an "ADD-ON" badge.

The breadcrumb CLIP › LAYER › DECK › MASTER › OUTPUT shows the effect level.
`ALL` moves up a level.

## 6. Build views

These are full-screen, and you reach them from the tabs in the top-left
corner (**PERFORM · LIBRARY · SHOW · OUTPUTS · TABLE**). They are never needed
during a show.

**Library**

| Part | What it has |
|---|---|
| Left column, filters | Mood map; colour wheel ("clips like this colour"); energy slider (1–5); motion; type tags; bars at the show BPM; loop quality; NEW / ANALYSING; favourites |
| Centre | A grid of cards. Hovering plays the clip. Each card shows its palette strip, energy pips, mood chip, type tags and badges (HAP ✓, ANALYSING…, ⚠ FLICKER, ALPHA, TRACKED) |
| Right panel | The selected clip's analysis in full, and **Find similar** |
| Bottom | The import queue, with the throttle state ("Show running: analysis at low priority") |

**Show**
- Setlist → songs → cues. A cue is a scene, a transition and a trigger
  (manual, beat, bar, timecode, drop, or the DJ's track).
- A timeline per song, with the cue markers on it.
- Scene banks: 16 pads per bank, with thumbnails.
- **Prepare show:** one button that runs the checklist (convert, read every
  clip, compile, budget, warm up) and gives a green, yellow or red report.

**Outputs:** the same as wizard step 3, plus mapping (later), per-output
effects and output latency.

**Table:** the VJ table (§9).

## 7. Help and safety in the GUI

- **Explain mode** (`SHIFT + BROWSE` on the MK2, or `F1` / `?` in the GUI):
  the next control you touch is explained instead of acting.
- First-time tips: the MK2 screen explains a control the first time you use it
  in a session. This can be turned off.
- The Help menu (Getting started, Controller map, Effects reference,
  Troubleshooting) is built from the same descriptions as the code, so it
  never goes out of date.
- **Flash cap** for strobe and flash is **off by default** (D65). A show can
  switch it on (for example 3 flashes / s) for venues that require it; it is
  then shown as a small "SAFE" badge on the top bar.
- The status lights link to Troubleshooting pages.

## 8. Main weapon, side arms and the adaptive layout

- **The main weapon decides the layout.** Each one has its own mimic layout,
  built from its profile (the positions of its controls). A panel never
  moves within a loadout; only its content changes.

  | Main weapon | Layout |
  |---|---|
  | **MK2** | §4: PREVIEW / PROGRAM screens with tiles, the wheel = TAKE, groups as 2 × 4 preview / program layers, a 4 × 4 clip grid on the right. |
  | **NuVJ** | **Deck A \| master column \| deck B**, as on the panel. Each deck has a monitor, BANK / PMTR tiles, LATCH / AUTO (dice) / SYNC, a 3 × 3 clip-thumbnail grid and a jog = scrub strip. The centre has the PROGRAM monitor, the LCD, the master FX tiles, the A / B level faders and **the real crossfader**. The decks are fixed A / B, and the tallies follow the crossfader. See the "Perform · MAIN = NuVJ" artboard. |
  | **Keyboard + mouse** | The MK2 layout, clickable; the mouse wheel over a tile turns it; the keys are in `hardware-mapping.md` §6. |

- **Side arms dock as cards** to the right of the pads (MK2) or under the
  master column (NuVJ). A card shows a mini mimic of the device, its current
  mode, its target (red PROGRAM / green PREVIEW) and live values.
  - **Block:** TOUCH / MIX / LOOKS / MOOD.
  - **LUMI:** FX KEYS / LOOK KEYS / CLIP KEYS.
  - **Ohm64:** layer faders, the real crossfader, a 64-clip grid.
  - **KPE1:** Touch FX.

  Details are in `hardware-mapping.md` §5 and on the "Side arms" artboard.
- **Side arms only add.** They never remap the main weapon. Faders use soft
  takeover. If one is unplugged, its card greys out.
- **Legend chips** always name the weapon's own control (`AUTO` on the NuVJ,
  `▶ PLAY` on the MK2).

## 9. The VJ table (secondary view)

Tab **TABLE**. The design is in `docs/vj-table-design.md`, which comes with
the merge of the rack-design branch. What matters for the GUI:

- The table is a playful **second way** to build a look: boxes (players,
  cameras, pedals, mixers, simulated V-4 and KF4, monitors, projectors) on a
  table, patched with cables.
- It runs on **the same engine**. Its output is a source called **TABLE**,
  which can feed:
  - a layer in Deck A or B (then crossfade it in as usual);
  - an output directly (output source TABLE);
  - or, when you "go table mode", MAIN itself, via **Use table as MAIN**.
- **Switching views never cuts the output.** PERFORM and TABLE are tabs, and
  the outputs keep showing whatever MAIN is.
- Projectors on the table that are switched LIVE are the real outputs. The
  room view shows exactly what goes out, with the same red LIVE tally.
- The table costs nothing while it is not routed anywhere: boxes that nobody
  sees are not rendered.
- Browser first (owner, 2026-11-10); later the same in the native GUI if we
  ever build one.

## 10. How the GUI talks to the engine

- **The GUI is its own process.** It sends **commands** (load clip, set
  parameter, take, roll, save, …) into a command ring and reads **state**
  (decks, layers, crossfader, tallies, BPM, status) from a shared status
  block. The engine applies commands at frame boundaries.
- **Previews:** the engine writes small thumbnails (480 × 270 BGRA, 15–30
  fps) of Deck A, Deck B, MAIN and each output into shared memory. This is
  one cheap downsample pass. The GUI never renders the show itself.
- **Proposal (needs the owner's OK):** build the GUI as a **local web app**
  (served by a small `himmel_ui` bridge process) instead of Dear ImGui.
  - Why:
    - the weapon drawings, simulators, live view and VJ table are already
      HTML;
    - iterations are faster;
    - a tablet or phone can be a second control screen for free;
    - the latency of the previews does not matter for the show.
  - Dear ImGui stays the fallback, for a native shell.

  See `../decisions.md`.
- Interfaces: `../interfaces/command-protocol.md` (to be written in phase 1)
  and `../interfaces/engine-status.md`.

## 12. The canvas: outputs, layers and snapping

**Model:** the outputs are windows onto **one canvas**, as in Modul8.
- Its size comes from the outputs (step 3):
  - screens add up edge to edge (N × 1920);
  - projectors overlap by the blend width (N × 1920 − (N − 1) × overlap).
- Changing the overlap changes the canvas, and every monitor follows.

**Layers snap.** Each layer has a **place**:

| Place | What it does |
|---|---|
| **FILL ALL** | Cover the whole canvas, zoomed and cropped. The default for a clip on a multi-output canvas. |
| **OUT 1 / OUT 2 / OUT 3** | Fill one output only |
| **FIT** | The whole clip, letterboxed |
| **REPEAT** | The same picture in each output |
| **FREE** | Drag and scale anywhere. Edges snap to the output lines; `SHIFT` = no snap. |

A typical stack:
- a 16:9 clip zoomed to FILL ALL (cropped top and bottom);
- a PNG logo with alpha on OUT 2;
- luma-keyed particles across;
- a text ticker across the bottom.

Keys and alpha come from the layer's blend: normal, add, screen, luma key,
chroma key, or alpha from HAP Alpha / PNG.

**On the MK2,** the LAYER page:

| Knob | Function |
|---|---|
| 1 | **PLACE** (scrolls the targets above) |
| 2 | **ZOOM / crop** |
| 3 | **PAN** (`SHIFT` = up / down) |
| 4 | **OPACITY** |
| 5–8 | The opacities of the PROGRAM layers |

With the mouse, drag in the PREVIEW strip; the snap targets are buttons under
it.

**Text generator** (a source like a clip; details in
`effects-and-generators.md` §5):
- **Styles:** ticker, one word per beat, typewriter, kinetic stack, outline,
  a clip inside the letters, glitch letters, countdown / clock.
- **Text from:** a typed list, a .txt / .csv file, the DJ's track title
  (Pro DJ Link), BPM / clock, messages from a phone (a local web page) and
  OSC.
- On a text layer, the CLIPS pads = 16 lines of text.

## 13. Mapping and masks (projectors only)

Reached from step 3, or OUTPUTS › Mapping. **Screens skip this entirely.**

- **The tools:**
  - **CORNERS** (4-point keystone);
  - **WARP** (a 3 × 3 … 9 × 9 grid);
  - **BLEND** (width, curve, gamma, black level per seam);
  - **PEN · MASK**;
  - **TEST GRID**;
  - later **MAGIC** (camera-assisted: tap an object in a webcam picture of
    the stage, get a mask suggestion).
- **The pen tool:** click the corners, click the first point to close.
  - Drag = move, Alt-drag = a curved edge; feather; HIDE or SHOW ONLY;
    invert.
  - One or many masks per output.
  - Masks belong to the **output**, so every look respects them. They are
    saved with the show and the venue.
- **A crosshair on the real projection:** while you draw, the projector shows
  a big yellow crosshair at your pointer, plus the mask outlines and an
  optional test grid. You put the crosshair on the real corner of the
  physical object and click: what you see on stage is what you get.
- **With the hardware:**
  - **Block** = a trackpad for the crosshair (absolute X / Y on the output).
    Tap = add a point; MODE = close.
  - **MK2:** knobs 1 / 2 move the selected point (`SHIFT` = finer), pads pick
    a point, `REC` adds, `ERASE` deletes, and the wheel zooms the view.
- A photo backdrop (a webcam pointed at the stage) is optional; the GUI view
  works on a plain test grid too.

## 14. One or two GUI screens

- **Settings › GUI screens: 1 or 2.** One screen is the default and must
  always work.
- **With two:**
  - **Screen 1 = the main weapon's layout**, unchanged; the side-arm column
    becomes a small "→ screen 2" note, and the monitors may grow.
  - **Screen 2 = the room and the side arms:**
    - PROGRAM and PREVIEW in their true superwide shape;
    - the side arms large (the Block's LED mirror, the full LUMI keyboard,
      the Ohm64 faders);
    - cues or the library.
- **Screen 2 presets:** Room + side arms (default) · Library + cues ·
  Audience view (the cheat sheet) · Table.
- **Rules:**
  - Screen 1 never changes because screen 2 exists.
  - Unplugging screen 2 folds its panels back into screen 1 as cards.
  - Both are windows of the one GUI process.
  - GUI screens are never show outputs.

## 15. Mockups

**v3** (page "v3 · Instrument" on the canvas; copies in
`docs/gui-mockups/v3/`):
1. Perform · MAIN = MK2 (+ Block, LUMI). It is **interactive**: click group
   A or F to switch between PREP and PLAY focus, and click the pad-mode
   tabs.
2. Knob tiles: what turning will do.
3. Choose your weapon: main + side arms (interactive).
4. Perform · MAIN = NuVJ.
5. Side arms: Block, LUMI, Ohm64, KPE1.
6. MK2 cheat sheet (audience view), drawn from the profile.
7. Set outputs: screens or projectors, 1 / 2 / 3, blend, with a live canvas
   size (interactive).
8. Canvas, layers and snapping, plus the text generator.
9. Mapping: pen-tool masks with a crosshair on the projection.
10. Presets: dice, mutate and rotate on every level.
11. Two GUI screens: the main layout on 1, side arms on 2.

Perform · MAIN = MK2 also has a **CANVAS 1 / 2 / 3 screens / 3 projectors
blend** switch in its status strip, which shows how the monitors adapt.

12. Clip editor and pads: the transport strip (in / out, cue points,
    slices, play modes), effect levels with scope badges, and the MK2 pads in
    SLICE mode with pressure shown (interactive).

**v2** (still valid for these): wizard 1 Add clips,
Library, Shuffle close-up, the MK2 screen pages, the VJ table.

## 16. The clip editor (transport, loops, effect levels)

Spec: [`clips-and-playback.md`](clips-and-playback.md). The editor is a
**drawer under the monitors** that opens when a layer is selected and the
CLIP knob page is shown (or on double-click on a clip pad). It edits the
clip **on the selected layer**: on PREVIEW by default, on PROGRAM only in
PLAY focus (the drawer then has the red frame). It never covers the monitors.

**Row 1: the timeline.**
- A filmstrip of the clip (thumbnails every few seconds) with the waveform-like
  **motion curve** from import analysis under it.
- **In / out brackets** (drag; `SHIFT` + drag the range = move it). The beat
  grid is drawn when BPM sync is on; brackets snap to beats and to analysed
  cuts.
- An optional **inner loop** pair (thinner brackets).
- **Cue markers 1–16**, numbered and coloured like the pad they sit on.
  When there are none, the 16 equal slices are drawn faintly.
- The **playhead** (white). In catch-up modes a faint ghost playhead shows
  where the clip would be.

**Row 2: the transport chips** (each is one click, and each has its MK2 knob
on the CLIP page):
- Direction `◀ ‖ ▶`.
- Mode: LOOP · PING-PONG · ONCE (hold / eject / next) · RANDOM · CHOP · STEP
  · SCRATCH. Mode options appear next to it (interval, distance, slices).
- Time base: FREE ×speed · BPM *n* beats · TIMECODE.
- Trigger: NORMAL · TOGGLE · PIANO · LEGATO; start RESTART / CONTINUE; fade
  in / out times; beat snap; target OWN / FREE.
- Autopilot: off · NEXT · RANDOM · PREV after *n* loops.

**Row 3: the clip's effects and PRESS.**
- The 4 clip-level FX slots as small tiles, each with its **level tag**
  (CLIP / LAYER / DECK / MASTER / OUTPUT appear in the same style elsewhere).
- Parameter **scope badges**: none = this slot, `L` = per look, `G` = global,
  `~` = moving (LFO / REACT). Right-click to change.
- **PRESS:** which parameter the pads, Block Z and LUMI pressure push, with
  its range drawn as a bar that fills live while a pad is held.
- **Variants A B C D** as four small swatches; click = switch, the dice icon
  = dice this clip only, `REC` icon = save as the current variant.

**Pads in the GUI follow the mode.** In SLICE mode the 4 × 4 clip grid turns
into the 16 slices of the clip (a thumbnail of each slice start), with the
playhead square lit white, the in / out range in the clip's colour, and a
**pressure ring** around any held pad.

**Test it on PREVIEW:** the drawer has **TRY** (copy the PROGRAM layer to
PREVIEW and open it here), **A / B** (hold: show the state before this edit),
and **AUDITION** in the library (the hovered clip plays in PREVIEW with
variant A, nothing is loaded).

## 17. Perform v4: the instrument layout (basis for 0.1)

Spec of behaviour: [`perform.md`](perform.md). Artboards: page "v4 ·
Playback 0.1" on the design canvas, copies in `docs/gui-mockups/v4/`.

**MK2 as main (V4PerformMK2):**
- **Top bar:** HIMMEL · the **song strip** (song name, its scenes as small
  numbered chips: the live one red, the one in PREVIEW green, the next GO
  outlined) · BPM with a beat dot · GRID division · the clock.
- **The two monitors** (PREVIEW left, PROGRAM right) with the **focus frame**:
  the side with the focus has a thick green or red frame and a label
  ("PREP · pads land here" / "PLAY · pads land here"). The monitors keep the
  canvas shape (§4.2).
- **Under PREVIEW:** knob tiles 1–4 of the current page for the focus side;
  the four preview layer strips (A–D).
- **Under PROGRAM:** the **four macro tiles** of the live scene (knobs 5–8),
  always there; the four program layer strips (E–H), each with a small
  **↺ reset** chip (hold `ERASE` + group).
- **The TAKE column** between them: the wheel as a ring that fills with the
  TAKE %, `PLAY` = auto TAKE, `RESTART` = CUT, and `ENTER` = GO with the next
  scene's name.
- **The pads (right, 4 × 4)**: in CLIPS mode each tile is a thumbnail with
  its **target tag** (`SEL`, `L1`–`L4`, `ALL`, `HIT`) and its trigger style
  icon; holding a pad lights the layer strips it will land on, in the focus
  colour. In SLICE mode the tiles are the slices; in FX PADS the effect names
  with a PRESS bar; in LOOKS the song's scenes with their thumbnails.
- **Pressure is visible:** a held pad grows a ring; the PRESS bar on the tile
  fills.
- **Undo strip** (small, under the pads): the last 5 changes as chips
  ("L2 speed ×1.5", "Kaleido on"); a click undoes back to that point.
- **Side arms:** docked cards at the bottom right (Block mode, LUMI, Ohm64).

**NuVJ as main (V4PerformNuVJ):**
- Laid out like the NuVJ: **deck A | master | deck B**, the real crossfader
  at the bottom centre.
- The deck the crossfader favours has the red frame (PROGRAM), the other the
  green frame (PREVIEW); the frames move as the fader moves past the middle.
- Each deck: its monitor; a **wheel ring** showing the wheel mode (BEND /
  SCRATCH / FX WHEEL) and a playhead needle that moves with the clip; the
  clip's in / out arc on the ring; the 3 × 3 pad thumbnails; BANK, PMTR 1 / 2
  tiles; LATCH / AUTO / SYNC; the SPEED fader.
- Master column: PROGRAM monitor, the 8-character LCD mirror, master FX tiles,
  `INSTA A / B / BLACK` with the AUTO FADE speed, FADER MODE, FADER START.

**Build › Song and scene editor (V4SceneKit):**
- Left: the setlist and the song's scenes (drag to reorder; dice + keep;
  "Fill song from mood").
- Centre: the selected scene: its 4 layers, its 4 macros (pick from a list,
  set limits by dragging), its transition, its variations (3a, 3b, …).
- Right: **the kit editor** = the 16 pads (or the NuVJ 3 × 3): drag a clip,
  an effect, a scene or an action onto a pad; set its target, trigger style
  and PRESS; "Fill kit" proposes the default layout (`perform.md` §4.2).
- Every change can be tried at once on PREVIEW ("Rehearse": load the scene
  to PREVIEW with its kit active on the weapon).

## 18. Projects and files

Spec: [`projects.md`](projects.md).
- **File menu** (top bar, left of the song strip): New, Open…, Open recent,
  Save, Save as…, Save a copy as file…, Revert, Pack with media. Keys
  `Ctrl+N / O / S / Shift+S`.
- The top bar shows the **project name** with a dot for unsaved changes; the
  autosave time is in its tooltip.
- **On the MK2:** the system menu (`SHIFT` + wheel) has Save, Open recent and
  New; the wheel picks, push confirms. Opening during a show loads into the
  PREVIEW side and asks for a TAKE, so the outputs never drop.
- **Find missing clips** opens by itself when a project has missing files.

## 19. Timeline, settings, remote (views)

- **Build › Timeline** (`timeline-and-remote.md`, 0.2): the song's lanes
  (MARK, SCENE, L1–L4, FX / MACRO curves, HIT, OSC OUT, reference AUDIO)
  under a bar / timecode ruler; the playhead in red while it drives PROGRAM;
  blocks dragged from the library and the scene list; curves drawn or
  recorded; a loop region; wait markers. The bottom has the same PREVIEW /
  PROGRAM monitors, small. Artboard: V4Timeline.
- **Settings** (gear icon; `operations.md` §5): one page per group with test
  buttons. The audio group has the live meter and the reaction graph.
- **Reaction editor** (the "~" ear on every knob tile; `music.md` §4):
  signal list with live meters, input → output graph, amount, range, mode,
  attack / release, threshold, curve, quantise, hold. Artboard: V4Audio.
- **Remote:** Settings › Remote shows the OSC port, allowed addresses, the
  last 20 messages received (for debugging a lighting desk), and the phone
  remote QR code (0.2).
- **Output lost:** a red "OUT 2 lost" chip in the top bar, the output's
  monitor strip hatched, until it comes back (`operations.md` §1).
