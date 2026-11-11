# Presets, shuffle, scenes and cues

Status: approved by the owner, 2026-11-11 (decisions D42–D76). This is the heart of "fun to play":
start from a library, roll looks until something is cool, keep it with one
press, and fire saved looks back and forth. The hardware buttons are in
[`hardware-mapping.md`](hardware-mapping.md) and the screens are in
[`gui.md`](gui.md) §4.3.

## 1. The model (what can be saved)

```
Show
├─ Library refs (clips, generators, effects; tags live in the library DB)
├─ Outputs (layout, per-output source, output effects, mapping)
├─ Weapons + roles
├─ Moods (built-in + own)
├─ Banks of scenes (16 per bank)
└─ Setlist → Songs → Cues

Deck (A or B)
├─ Layers 1–4: source (clip | generator | camera | capture | Spout/NDI | TABLE)
│              + transform + blend + opacity + FX slots 1–4 + reactions + clip FX
├─ Deck FX slots 1–4 (the "group" level)
└─ mood point + palette

Master: FX slots 1–4    ·    Output n: FX slots 1–4
```

| Thing | What it holds | Made by |
|---|---|---|
| **Effect preset** | One effect's parameters, reactions and mix | `SHIFT + pad` in the effect browser, or a shuffle at FX scope |
| **Layer preset** | The source, or a source *rule* such as "any calm water clip", plus transform, blend, opacity, FX and reactions | Shuffle at LAYER scope, save with `REC` while a layer is selected |
| **Scene** (= look) | A whole deck: 4 layers + deck FX, plus the mood point, palette, energy and a thumbnail | `REC`, `REC + pad` |
| **Cue** | A scene (by reference or inline), a transition (cut / fade / morph n bars / wipe), a trigger (manual, beat, bar, timecode, drop, DJ track) and a target (PREVIEW or LIVE) | The SHOW view, or `SHIFT + REC` in Step mode |
| **Song** | An ordered list of cues, plus a mood, BPM and a scene bank | The SHOW view |
| **Mood** | A point on the mood map, plus the rules that shape a shuffle (§3) | Built in (16), editable as files |

**A scene can use a source rule instead of a fixed clip** ("rule": energy 4–5,
cold palette, type city). That gives a scene that never looks the same twice,
which was the old "the scene fetches clips from the theme's filter" idea.

Every saved thing gets an automatic name, so it needs no typing. The name is
made of the mood word, a palette word and a number, for example "TECHNO
Ice 07". Names are optional and can be edited later.

**Effect control levels and parameter scopes** (where an effect sits and
whether a value is fixed, per look, global or moving) are specified in
[`clips-and-playback.md`](clips-and-playback.md) §4–5. Clips can carry up to
four **variants** (A–D: transport + clip FX + PRESS), which ROTATE goes
through when the CLIP level is selected. The MOTION lock also holds the clip
transport (play mode, speed, in / out).

## 2. Shuffle: roll, mutate, back

| Action | Button | What happens |
|---|---|---|
| **ROLL** | `Transport >` or **knob 2 (DICE) right** | A new random look in the current **scope**, inside the current **mood**, respecting the **locks**. It lands on PREVIEW unless the scope is a LIVE item. |
| **BACK** / **FORWARD** | `Transport <` / `Transport >`, or knob 2 left / right | Walk the history. `>` at the end of the history = a new ROLL, like the preset arrows on a synth plugin. |
| **MUTATE** | `SHIFT + Transport >` | Keep the structure and nudge the values by the Variation amount (default ±20 %). |
| **SAVE** | `REC` | Store the result as a scene on the next free pad (§1). |

**Scopes** follow the selection (the GUI breadcrumb):

| Scope | Selected by | A roll changes |
|---|---|---|
| **DECK** | `All` | Every layer, the deck FX and the palette |
| **LAYER** | A group button | That layer's source, transform, blend and FX |
| **FX SLOT** | An FX page | The effect in the slot and its parameters |
| **KNOB** | Touch a knob while pressing ROLL | Just that parameter, within its good range |

**The 8 locks** (MK2: Top 1–4 = CLIP, FX, COLOUR, MOTION; `SHIFT` + Top 1–4 = LAYOUT, SOURCE, REACT, MOOD. Lit = free, dark = held. Top 5–8 are live holds, see `hardware-mapping.md` §3.2):

| Lock | What it protects |
|---|---|
| CLIP | Which clips are used |
| SOURCE | Source kinds: clip, generator or camera mix |
| FX | Which effects are used |
| COLOUR | Palette and colour-related parameters |
| MOTION | Speeds, zoom and rotation rates |
| LAYOUT | Blend modes, opacities and transforms |
| REACT | Audio links |
| MOOD | Off = the roll stays at the mood point; on = it may drift to nearby moods |

**ROTATE** (the third verb, owner 2026-11-11):
- It steps through the **saved presets of the current level, in order**, so
  you can cycle effects and effect settings you already like.
- It works at every level: effect, layer, look (= the LOOKS pads), master and
  output.
- On the MK2: `SHIFT` + knob 2 (or `SHIFT` + knob 1 on the FX page, `SHIFT` +
  knob 8 for the master).

**Auto-rotate:**
- A level can rotate by itself every n bars (8 by default), landing on the
  beat or the phrase and respecting GRID.
- It stops as soon as you touch that level. `SHIFT + REC` toggles it.
- Good for installations and songs you don't want to babysit.

**Saving follows the level:** `REC` saves an effect preset on an FX page, a
layer preset when a group is selected, and a look with `ALL`. Levels nest: a
look holds its layer presets, and a layer holds its effect presets.

**History:** 64 rolls per deck, kept with the show. Each entry is a seed plus
the locks plus the scope, so it can be rebuilt exactly (§4). Saved entries are
starred. Undo (`ERASE`) is a separate stack and covers every edit, not only
shuffles.

## 3. How a smart roll is chosen

A roll is a **constraint sampler**, not pure chance. It runs in a worker,
never in the frame loop, and the result is applied at the next frame boundary
(or beat / bar with GRID).

1. **Read the context:** the mood point and its rules, the music now (energy,
   BPM, section), the locks, the scope, the GPU budget left, and the last 10
   results (to avoid repeats).
2. **Pick the sources:** query the library for clips and generators that
   match the mood:
   - energy within ±1;
   - palette distance to the mood palette;
   - motion and cut rate;
   - type tags;
   - loop quality;
   - no flicker warning when safe mode is on.

   Weight the candidates by fit, stars and novelty, and pick one per layer.
   Layer counts and opacities come from the mood (calm = 1–2 layers,
   crazy = 3–4).
3. **Pick the effects:** by the mood's family weights (for example techno:
   space, light, glitch; lounge: colour, blur, slow space).
   - **Compatibility rules** are applied, for example:
     - no invert on a dark clip;
     - no second geometry effect on the same layer as kaleido;
     - keying only when there is something under the layer.
   - The summed **GPU cost** (measured per effect at Prepare) must fit the
     budget.
4. **Pick the parameters:** each effect declares *good ranges* (from its
   presets) and which parameters are speeds. The values are sampled inside
   those ranges; MUTATE moves within ±Variation.
5. **Add the reactions:** 1–3 audio links per deck (for example kick → zoom,
   bass → displace, hi-hat → strobe), stronger at higher music energy.
6. **Colour:** either the mood palette (the colour macro maps clips towards
   it) or the clip's own palette, set on the `SWING` page.
7. **Score and reject.** A candidate is rejected and rolled again (at most 8
   tries, then the best one is kept) when:
   - the average brightness is too low or too high;
   - the effects cancel each other out;
   - the flicker risk is too high in safe mode;
   - it is too close to recent results;
   - it is over the GPU budget.
8. **Seeded:** every roll has a 64-bit seed and is deterministic given the
   library state, so history and saved rolls rebuild exactly.

What makes it "smart" lives in **data, not code**: mood files, effect
metadata (families, good ranges, costs, default reactions) and library tags.
Adding a mood or tuning one is editing a JSON file.

## 4. Moods (the dial for "what kind of night")

The **mood map** has two axes:
- **X = energy:** calm → intense;
- **Y = style:** organic / warm → synthetic / cold.

There are 16 built-in moods, placed on a 4 × 4 grid (rows = energy, columns =
style). These are the pads in `SHIFT + SCENE`. The 12 themes from design
spec v1 are kept, and four are added:

| Energy ↓ / Style → | Organic, warm | Soft | Graphic | Synthetic, cold |
|---|---|---|---|---|
| **4 Peak** | Rave Kaos | Disco Prisme | Industri Glitch | Techno Bunker |
| **3 Build** | Grafitti Gate | Deep House Glow | Synthwave Neon | Minimal Mono |
| **2 Groove** | Organic | Sunset | Retro Tape *(new)* | Data Pulse *(new)* |
| **1 Calm** | Ocean & Ambient | Lounge Drift | Ink & Smoke *(new)* | Night Drive *(new)* |

A mood file holds:
- a palette (5 colours) and a clip filter (energy / palette / type / motion);
- effect-family weights, layer count range and blend preferences;
- strobe / flash caps and a cut-rate range;
- default reactions and a default transition;
- macro mappings (what ENERGY, COLOUR, SPEED, DENSITY, CHAOS, GLOW, SPACE and
  REACT move in this mood).

The moods can be blended: hold one mood pad and tap another to take the
palette from the second ("Techno Bunker with the Sunset palette").

## 5. Macros

The 8 knobs on the MACRO page have the same names everywhere; the mood decides
what moves under them. They are kept from design spec v1:

| Knob | Macro | Lounge Drift | Techno Bunker |
|---|---|---|---|
| 1 | ENERGY | A bit more speed and glow | Speed, contrast, strobe level, cut rate |
| 2 | COLOUR | Original colours → fully in the mood palette | Black and white → full red accent |
| 3 | SPEED | Slow drift, ½–2 bars per cycle | Tempo-locked, 1/16–1 bar |
| 4 | DENSITY | Few layers, calm particles | Many layers, particles and cuts |
| 5 | CHAOS | A little colour noise | Glitch, block errors, displacement |
| 6 | GLOW | Bloom, soft feedback | Feedback trails, flash |
| 7 | SPACE | Slow zoom, depth | Tunnel, kaleido, mirror |
| 8 | REACT | The picture breathes with the music | Everything hits hard on kick and drop |

Each macro drives up to 8 parameters, each with its own curve and limits, set
so that the whole range looks good. Hold `SHIFT` and touch a macro knob to see
and trim what it drives.

## 6. Scenes, banks and triggering back and forth

> **v4:** firing follows the focus (scenes land where your focus is),
> `ENTER` = the next scene live, and a scene also carries a kit, 4 macros
> and variations. See [`perform.md`](perform.md) §2. The gestures below are
> the PREP-focus case.

- **Banks** have 16 scenes each. `Master < >` changes the bank. A song can
  bring its own bank.
- **Firing:**
  - tap a pad = load into PREVIEW, then TAKE or CUT;
  - `SHIFT` + pad = straight to LIVE as a morph;
  - hold the pad for a longer morph (a short press is a cut, 1 s = 1 bar,
    2 s = 4 bars).
- **Back and forth:** because TAKE swaps the decks, two scenes can be played
  like a ping-pong: load scene 3 → TAKE → load scene 7 → TAKE → `CUT` flips
  back instantly, since the other deck still holds scene 3.
- **Morph** between two scenes interpolates every parameter that both share.
  Sources cross-fade, and parameters that cannot be interpolated switch at
  50 %.
- **Auto-generated banks:** "Fill bank from mood" rolls 16 scenes over the
  energy grid (rows calm → peak), then you keep or re-roll pads one at a
  time. This is how a new user gets a playable bank in 10 seconds.

## 7. Cues and songs (show structure)

- A **cue** = scene + transition + trigger + target.
- The triggers:
  - manual (`Play` / `Enter` in Step mode);
  - after n bars;
  - at a timecode (LTC / MTC / Pro DJ Link track position);
  - on the drop detector;
  - when the DJ loads the named track.
- **Free play in between:** whatever you do by hand runs until the next GO.
  "Auto-cue off" plays the rest of the song by hand.
- **Assist levels:**
  - **Assist** suggests the next scene at builds and drops (the pad blinks),
    keeps the colours in the palette, replaces clips that have run long, and
    keeps the picture out of long black or white;
  - **Autopilot** plays by itself (follows the energy, changes on 8 / 16-bar
    phrases, drops on the drop); touching anything takes over until you let
    go.

## 8. Files

Everything is plain JSON in the show folder (`himmel-show/1`), with one file
per bank, song and mood, so it diffs well in git and is easy to copy. Formats
are owned by the presets track and documented in `docs/interfaces/`
(phase 5).
