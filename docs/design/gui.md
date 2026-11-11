# GUI specification (v2, dark)

Status: proposal for the owner, 2026-11-11. It replaces the v1 sketches in
`docs/gui-mockups/` (Perform in light and night, the setup wizard and the
library). The drawings for this spec are the v2 artboards on the design canvas
and their copies in `docs/gui-mockups/v2/`. The hardware side of every screen
is in [`hardware-mapping.md`](hardware-mapping.md); the two are one design and
change together.

## 1. Principles

1. **Hardware first.** The screen is laid out like the controller. Every area
   of the GUI has a twin on the weapon, with the same name, place and colour.
   Every clickable thing shows its hardware chip (for example `▶ PLAY`,
   `SHIFT+MUTE`). In a show you should rarely need the mouse.
2. **LIVE and PREVIEW at a glance.** A red tally means LIVE (on the outputs)
   and a green tally means PREVIEW (not seen by the audience), as in a TV
   gallery. The same two colours are used on the monitors, the deck headers,
   the output thumbnails, the MK2 group buttons and the edge of the Lightpad.
   The tally shows the percentage when a crossfade is half way.
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

On later starts Himmel goes straight to step 4 after a one-line health check
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

### Step 2: Choose your weapon

- A game-style select screen. Each weapon is a card with the drawing from its
  profile, a stats line (for example "16 pads · 8 knobs · 2 screens") and its
  status:
  - **CONNECTED**, glowing;
  - **SIMULATE**, dim;
  - **NOT FOUND**.
- **Touch your weapon to choose it:** pressing any control on a connected
  device selects its card. This is the first moment of fun, and it doubles as a
  hardware test.
- **Loadouts** combine weapons: **MK2 + Lightpad** is the default, the
  **NuVJ** alone gives the classic two-deck setup, and **Keyboard + mouse**
  needs no hardware.
- Each card has a **Demo** button that plays a 20-second tour on the drawing:
  the lit controls, with a caption for what each one does.
- Choosing a weapon loads its map, its pages, its LED colours and its legend
  for the GUI.

### Step 3: Set outputs

- The detected displays and projectors are drawn as boxes, in the arrangement
  Windows reports. Drag them to fix the order. **Identify** flashes a big
  number on each real screen.
- Grouping per output: **SPAN** (one big canvas over several outputs),
  **MIRROR** or **SEPARATE**.
- **Source** per output:

  | Source | What the output shows |
  |---|---|
  | **MAIN** | Follows the crossfader (the default) |
  | **A** | Always Deck A |
  | **B** | Always Deck B |
  | **OWN** | Its own small layer stack, for example a logo on an LED strip |
  | **TABLE** | The output of the VJ table |

- **Test card** (on or off per output) and **Measure latency** (needs a
  camera pointed at the output).
- **Projector?** opens keystone, mesh warp and edge blend (phase 11). Until
  then it shows "coming".
- **Show type:** **VJ** (Himmel owns the picture) or **Overlay** (effects
  keyed over a live mixer, see `engine.md` §8). Overlay is greyed out until an
  SDI card is found.

### Step 4: Play

This opens the Perform view.

## 4. Perform: the main view

The view is laid out for one 1920 × 1080 operator monitor and also works at
1440 × 900. It has five horizontal bands.

```
┌ TOP BAR ── HIMMEL · weapons · BPM ●●●● · mood · assist · status lights · GPU ms · clock · [PANIC] ┐
├ MONITORS ─ [ DECK A ]      [   MAIN / LIVE   ]      [ DECK B ] ─────────────────────────────────────┤
│            tally+layers     crossfader + TAKE/CUT    tally+layers                                    │
├ PLAY ───── [ PADS 4×4 ]  [ MK2 SCREEN MIRROR + KNOBS ]  [ SHUFFLE: locks · history · mood ]       ┤
├ OUTPUTS ── [OUT1 ●LIVE MAIN] [OUT2 ●LIVE MAIN] [OUT3 ●LIVE MAIN] [OUT4 OWN]  · show next ·         ┤
└ WEAPON ─── live drawing of the controller(s), controls light up as pressed (toggle, F2) ──────────┘
```

### 4.1 Top bar

- **HIMMEL**, then the weapon chips (MK2 ●, Lightpad ●). Green = connected,
  grey = simulated, red = lost.
- **Tempo:** BPM, four beat squares, the clock source (LINK, PRO DJ, MIDI,
  AUDIO, TAP) and a confidence dot.
- **Mood chip**, for example "TECHNO · cold". Click it, or use `SHIFT+PATTERN`,
  to open the mood map.
- **Assist level:** OFF, ASSIST or AUTOPILOT.
- **Status lights** (SYNC, AUDIO, CAMS, OUTPUTS, DISK, IMPORT): green, yellow
  or red, always with an icon and a word, never colour alone. IMPORT shows the
  background queue, for example "IMPORT 12 ▸".
- **GPU ms:** frame time against the budget, as a bar. **Clock**.
- **PANIC** (`SHIFT+MUTE`): a big red-outlined button that goes to the safe
  scene.

### 4.2 The monitors and the crossfader

Three monitors: **Deck A** on the left, **MAIN** in the middle (the biggest)
and **Deck B** on the right.

**The tallies follow the crossfader:**
- the deck with weight 0 has a green **PREVIEW** tag;
- a deck with weight > 0 has a red **LIVE nn %** tag and a red frame;
- MAIN always has the red **LIVE** frame. It shows exactly what goes to the
  MAIN outputs, before output effects.

Under MAIN is the **crossfader**: a wide bar with A and B at the ends, the
current position, and a ghost marker where an automatic fade is heading. Next
to it:
- **TAKE** (`▶ PLAY`): an auto-crossfade over the fade time, quantised to the
  next beat or bar when GRID is on;
- **CUT** (`RESTART`): instant;
- the fade time (`VOLUME` page, knob 2);
- the curve (linear, smooth or hard).

**After a TAKE the decks swap roles** (the old LIVE deck becomes PREVIEW and
keeps its content), so the loop is always: build or shuffle on PREVIEW → TAKE
→ build or shuffle on the other deck. This is the DJ habit (cue on one deck,
play the other) and the NuVJ habit (A/B with a crossfader).

**Under each deck monitor is its layer strip:** four rows (deck A = groups
A–D, deck B = groups E–H). Each row shows:
- a thumbnail and the source name;
- an opacity bar and the blend mode;
- four small FX chips (lit when the slot is on);
- the mute / solo state and lock icons.

The selected layer has a white outline, and its group button on the MK2 is
lit brightest. Clicking a row selects it, and dragging a clip from the
library onto a row loads it.

### 4.3 Play band: pads, knob strip and shuffle

**Pads (left):** a 4 × 4 grid that mirrors the MK2 pads in their current
pad mode (§5). A header names the mode and the bank, for example
"SCENES · Bank 2 · Song: Opening".

| Pad mode | What the pads show |
|---|---|
| Scenes | Each pad shows the scene's main colour and a thumbnail on hover. Rows are labelled on the left: PEAK, BUILD, GROOVE, CALM, from top to bottom. The active scene pulses on the beat. |
| Clips (`PATTERN`) | The 16 clips of the selected layer's bank, in each clip's own main colour. |
| FX pads (`PAD MODE`) | Momentary performance effects: hold for on, velocity = intensity. |
| Cues (`STEP`) | The cues of the current song, with the next cue outlined. |
| Outputs (`NAVIGATE`) | Pads 1–6 are outputs, with sources shown. |

**Knob strip (centre):** a pixel-exact mirror of both MK2 screens (512 × 64,
scaled ×2). Under it are the 8 knob values as arcs, at the same x positions as
the real knobs (`kKnobX`). Above it are the 8 Top-button labels, also at their
real positions.
- The **page tabs** (MACRO · LAYER · FX1 · FX2 · FX3 · FX4 · MIX · REACT)
  follow `PAGE < >`.
- Each knob arc shows two values: where the knob is, and the live value after
  the music has pushed it (a thin "ghost" ring).
- Each knob also shows the name of its audio link, if it has one.

**Shuffle panel (right):** see [`presets-and-shuffle.md`](presets-and-shuffle.md)
for the logic.
- **Scope:** DECK, LAYER, FX SLOT or KNOB, following what is selected. `ALL`
  sets DECK.
- **8 lock toggles,** the same as the 8 Top buttons in Perform: CLIP, SOURCE,
  FX, COLOUR, MOTION, LAYOUT, REACT and MOOD. Lit means it will change; dark
  means it is locked.
- **History strip:** small thumbnails of the last rolls, with the current one
  outlined. `◀` and `▶` match the transport arrows. A star marks a roll that
  was saved.
- **ROLL** (`TRANSPORT >`) and **MUTATE** (`SHIFT + TRANSPORT >`), plus a
  **Variation** dial (how far MUTATE moves).
- **Mood mini-map:** a dot on the energy × style map. Drag it, or use the
  Lightpad in MOOD mode.
- **SAVE** (`REC`) goes to the next free pad in the scene bank, and that pad
  blinks. `REC + pad` saves to that pad.

### 4.4 Outputs band

- One thumbnail per output with its tally (LIVE red when anything is shown,
  black when blacked out), its number and its source (MAIN, A, B, OWN or
  TABLE).
- Its output effects as chips, and its measured latency in ms, if it has
  been measured.
- **Show next** (a toggle, or hold `SOLO`) shows on each thumbnail what that
  output *would* show after the next TAKE, with a green frame. This is how
  you check a multi-output look before it goes out.
- Click an output, or use `NAVIGATE` + pad, to change its source or black it
  out.

### 4.5 Weapon band (on-site simulator)

- A live drawing of the chosen weapon(s), from their profiles. It is the same
  renderer as the browser live view in `tools/mk2_live.html`.
- Controls light up when pressed, knobs turn, and the screens and LEDs are
  mirrored.
- With **Explain** on, hovering or pressing any control shows what it does in
  the current mode.
- `F2` hides the band. **Audience view** sends the drawing full screen to a
  second monitor, so people at the booth can see what each button does (owner
  request).
- With no hardware, the drawing *is* the controller: click, drag and scroll.

## 5. Shape: layers and effects

Shape needs no extra screen. Selecting a layer (a group button) or a slot
(the FX pages) changes the knob strip page. To see all four slots of a layer
side by side, there is an **effect chain row** under the layer strip: four
slot cards, each showing the effect name, its 2 main values, mix and audio
link. Click a card, or use `PAGE` to FXn.

**The effect browser** (`BROWSE` on an FX page, or the master encoder push):
- The 8 Top buttons are categories: COLOUR, SPACE, DISTORT, LIGHT, STYLE,
  TIME, KEY and FAVS.
- The pads show up to 16 effects in that category. A dimmer colour means a
  heavier effect for the GPU.
- Tap a pad: the effect shows on the PREVIEW monitor only, and the MK2 screen
  explains it in two lines.
- Tap the same pad again, or press the encoder, to put it in the slot.
- `SHIFT + pad` lists the effect's own presets.
- **Add-ons** are a ninth category, reached with `SHIFT + FAVS`. They carry an
  "ADD-ON" badge.

**The effect levels are always visible as a breadcrumb:**

> CLIP › LAYER › DECK › MASTER › OUTPUT

The current level is lit. `ALL` moves up a level. Pressing the selected group
button again also moves up a level (to the deck and then to the master), as
decided before.

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
- **Safe mode** for strobe and flash is on by default for public shows
  (photosensitivity limits). It is shown as a small "SAFE" badge on the top
  bar.
- The status lights link to Troubleshooting pages.

## 8. Weapon-adaptive layout

Each weapon profile declares its **capabilities** (pads with velocity / RGB,
knob count and kind, faders, crossfader, jog wheels, screens, touch surface,
keys) and **roles**: which of its controls play which Himmel function (see
`hardware-mapping.md` §6). The GUI picks a layout variant from these.

| Loadout | Layout |
|---|---|
| **MK2 + Lightpad** (default) | Full Perform as above. The knob strip is the MK2 screen mirror, and the Lightpad has a small panel (current mode, X/Y/pressure, LED mirror) in the Shuffle column. |
| **MK2 only** | Same. The crossfader is on knob 1 of the MIX page and on TAKE / CUT. |
| **Lightpad only** | A touch-centred variant: the mood map is big, the Touch FX panel is big, and scenes are on the block's SCENES mode. The knob strip is replaced by on-screen sliders. |
| **NuVJ only** | **Classic two-deck variant:** two big deck panels with their 3 × 3 cell grids (9 cells × banks, as on the device). The hardware crossfader is drawn prominently, PMTR 1 / 2 are shown per deck, and there is a segment-LCD mirror. Shuffle is on the deck's AUTO button (§4 of the mapping). |
| **Keyboard + mouse** | Full Perform with the keyboard map shown as chips (`SPACE` = TAKE, `←` `→` = history, `Q…V` = 16 pads, and so on). The weapon band is the clickable MK2 simulator. |
| **Integrations** (LUMI, Ohm64, Entrancer, …) | The default Perform; the legend chips come from the weapon's roles. Controls without a role get a "learn" dot (click-then-press, as in the live view). |

Rules for adaptive panels:
- A panel never moves between loadouts, it only swaps content, so muscle
  memory survives.
- A function that the weapon cannot reach gets an on-screen control with a
  keyboard chip.
- The legend chips always name the weapon's own control, for example
  `AUTO (deck A)` on the NuVJ and `▶ PLAY` on the MK2.

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

## 11. Mockups

The v2 artboards ("v2 · Dark" page on the design canvas; copies in
`docs/gui-mockups/v2/`):

1. Wizard 1: Add clips
2. Wizard 2: Choose your weapon
3. Wizard 3: Set outputs
4. Perform (MK2 + Lightpad)
5. Perform, NuVJ classic variant
6. Library (tags, mood map, background import)
7. Shuffle, scenes and cues (close-up)
8. MK2 screen pages (1-bit, at real pixel size × 2)
9. VJ table (secondary view)

The v1 artboards (light and night themes) are kept for history only.
