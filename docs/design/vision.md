# Himmel: vision and product rules

Status: agreed with the owner, 2026-11-11. This page states what Himmel is and
the rules every design choice is checked against. The details are in the other
files in `docs/design/`, and the plan is in [`../roadmap.md`](../roadmap.md).

## In one paragraph

Himmel is a VJ and interactive-installation system for Windows that you play
like an instrument. You plug in a controller you already own (a "weapon"),
drop in a folder of clips, choose your outputs, and play. Most of the time you
shuffle looks that fit the music and the mood, keep the ones you like, and
fire them back and forth. Everything is tuned for very low latency and for a
whole night without failing.

## What Himmel is not

- **It is not yet another VJ program that nobody can use.** No wall of tiny
  sliders and no node graph to start. The depth is there, but it stays
  underneath, the way a synth plugin has hundreds of parameters and you start
  by browsing presets.
- **It is not a media server that buys features with latency.** When latency
  and features conflict, latency wins. For productions where the live picture
  must stay in lip sync, the live video can bypass Himmel entirely (Overlay
  mode, [`engine.md`](engine.md)).

## The six rules

1. **Show-safe first.** A part that fails never takes down the output. Nothing
   is compiled, allocated or converted in the frame loop. Imports and analysis
   run in the background, never in front of the picture.
2. **Lowest possible latency,** from pad to light and from camera in to screen
   out ("glass to glass"). It is measured, not guessed.
3. **Instrument, not spreadsheet.** Every function lives where your hand
   expects it on the hardware, and the screen shows it in the same place,
   with the same name and the same colour.
4. **Shuffle, keep, fire.** Randomise the whole look or part of it until
   something is cool. Save it with one press, and trigger saved looks back and
   forth. Presets need no names.
5. **Smart by default.** Every clip is analysed on import (speed, colours,
   energy, mood and type), and those tags drive the shuffle, the mood filters
   and the reactions to the music.
6. **Light on the GPU,** so you can stack many layers. HAP clips, single-pass
   effects, nothing rendered that nobody sees, and a visible GPU budget.

## The main ideas

| Idea | Short version | Where |
|---|---|---|
| **Weapons** | A controller is a weapon. "Choose your weapon" at start-up. The GUI adapts to the weapon. Weapons you do not own can be simulated on screen. | [`gui.md`](gui.md), [`hardware-mapping.md`](hardware-mapping.md), [`../../weapons/README.md`](../../weapons/README.md) |
| **Main weapon + side arms** | One **main weapon** (MK2 or NuVJ) decides the GUI layout. **Side arms** (Block, LUMI, Ohm64, KPE1) only add control. The GUI **mimics** the main weapon: PREVIEW on the left, PROGRAM on the right, the pads as clip thumbnails, the knobs as tiles that show what turning will do. | [`hardware-mapping.md`](hardware-mapping.md), [`gui.md`](gui.md) |
| **Two decks + crossfader** | Deck A and Deck B, each a stack of layers. You build on the deck that is not live (PREVIEW), then take it LIVE with a crossfade, as on the NuVJ. It is always clear what is live. | [`gui.md`](gui.md), [`engine.md`](engine.md) |
| **Effects at every level** | Clip, layer, deck (group), master and output. The same knob layout everywhere. | [`effects-and-generators.md`](effects-and-generators.md) |
| **Shuffle with locks and history** | `>` rolls a new look within the mood, `<` goes back. Locks keep the parts you like. | [`presets-and-shuffle.md`](presets-and-shuffle.md) |
| **Smart library** | Drop a folder: clips are converted to HAP, analysed and tagged in the background, even during a show. | [`library-and-analysis.md`](library-and-analysis.md) |
| **Music in** | Line or mic, loopback from apps on the PC, DJ sync (Pro DJ Link, Ableton Link, MIDI clock, timecode). Effects react to bands, beats, drops and energy. | [`music.md`](music.md) |
| **Wizard start** | 1 Add clips, 2 Choose weapon, 3 Set outputs, 4 Play. | [`gui.md`](gui.md) |
| **VJ table** | A second, playful view: lay the boxes on a table and patch cables. It uses the same engine and goes to the same outputs. | [`gui.md`](gui.md) §9, `docs/vj-table-design.md` |
| **Overlay mode** | A separate mode: effects keyed over a live production mixer (fill + key), so the live picture keeps its lip sync. | [`engine.md`](engine.md) §8 |
| **Add-ons** | Built-in effects are tested and show-safe. Add-ons (ISF shaders, generators you describe to Claude) sit on their own shelf, behind a safety gate. | [`engine.md`](engine.md) §7 |
| **Nostalgia** | The NuVJ works alone in a classic two-deck setup, with rebuilt NuVJ-style effects. | [`hardware-mapping.md`](hardware-mapping.md) §4 |

## Who it is for

- **The owner and VJs at live shows** (large crowds, musicians, DJs), who need
  it to just work.
- **Beginners** who have never done VJ work. They should have fun within a
  minute: add clips, pick a weapon, press `>` and `▶`.
- **People with old controllers in a drawer** (NuVJ, LUMI, Ohm64, Entrancer):
  their gear becomes a weapon.

## Language and look

- Everything in the app (UI, help, logs, code comments) is in **English**. The
  owner is talked to in Norwegian.
- **Dark only** (owner, 2026-11-11). The light theme is dropped. A GUI
  brightness control replaces the old "Night" theme.
- Controllers are drawn as "HIMMEL · <model>", never with the maker's logo.
- Effect names: common words (strobe, blur, mirror, kaleido) are fine. Any
  name that belongs to one product gets a Himmel name of its own.
