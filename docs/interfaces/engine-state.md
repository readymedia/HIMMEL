# Engine state block (`Local\HimmelState`)

Status: **version 1, written by the LEAD 2026-11-15** for the playable slice;
**version 2 (ENGINE, 2026-11-16): the transport section (M2) is appended**
after the catalog. `LiveState` and `Catalog` are unchanged, but the block
size changed, so every reader is rebuilt with the new header (a version 1
reader sees a mismatch and stays disconnected, as §2 says)
(D90). **Version 3 (AUDIO, 2026-11-17, D131): the reaction table is
appended** after the transport section; the sections before it are
unchanged, the size changed, so every reader is rebuilt again. Header: [`src/protocol/include/himmel/protocol/state.h`](../../src/protocol/include/himmel/protocol/state.h)
(source of truth for the layout). Writer: `himmel_render` only. Readers: the
GUI bridge, weapon mappers, tools. Change rules as in
[`command-protocol.md`](command-protocol.md).

## 1. Sections

| Section | Seqlock | Written | Holds |
|---|---|---|---|
| `status` | none (atomics) | every frame | `heartbeat_us`, `pid` |
| `live` (`LiveState`, ~8 KB) | `live_seq` | when something changed, at most once per frame, and at least every 250 ms | the whole show model on air: crossfader, `program_deck`, TAKE progress, black, focus, 2 decks × 4 layers (clip, blend, mute, playing, missing, opacity, speed, position, 4 FX slots), deck FX, master FX, per-producer acks / refusals / alive |
| `catalog` (`Catalog`, ~25 KB) | `catalog_seq` | when the clip or effect list changes | clips (name, size, fps, length, codec, placeholder colour) and effects (name, param names and defaults) |
| `transport` (`TransportSection`, ~2 KB, v2) | `transport_seq` | together with `live` | the beat clock (BPM, beat, grid, tapped), per layer a `LayerTransport` (every `LayerParam` setting; status: phase, frame on screen, glided speed, fade gain, transition progress, beat loop, scratch, waiting launch + beats left, loops, the loaded clip's 16 cue points), and the **HIT layer** (`LayerState` + `LayerTransport`) |
| `react` (`ReactionSection`, ~8 KB, v3) | `react_seq` | every frame while any FX slot holds an effect, else together with `live` | per FX slot (layers, decks, master, same order as `live`) and per knob 0..3 a `ReactionEntry`: where it comes from (`source`: Default / Custom / Off), the reaction in use (signal, mode, curve, amount, attack / release ms, threshold, range lo / hi, quantise / hold beats; a Default entry mirrors the effect's default reaction), and live `env` (envelope 0..1) and `value` (the knob as the shader gets it, for the "knob ghosts"); `gen` bumps on every settings change. Changed with `FxReact` ([`command-protocol.md`](command-protocol.md) §4d) |

Outputs (M6) are **not** in this block: output slots, displays, canvas size,
layer places, black per output and IDENTIFY are in their own block,
`Local\HimmelOutputs` ([`outputs-block.md`](outputs-block.md)), and are
changed by commands 70–73 ([`command-protocol.md`](command-protocol.md) §4c).
The state block stays at version 2.

`position` is written at frame rate while a clip plays; readers that only
care about structure can compare everything else. GUIs should poll `live` at
their own rate (30 Hz is plenty) and diff.

## 2. Rules

- Readers copy a section with `seq_read` and work on the copy. They never
  keep a pointer into the block and never write to it.
- `live.epoch` changes whenever the render process starts. A reader that sees
  a new epoch drops its cached view and rebuilds it from this block.
- **Restoring after a renderer restart (slice: should; M1: must).** If the
  block already exists with the right magic / version when the renderer
  starts (the bridge or supervisor kept it alive), the renderer reads the
  previous `live` + `catalog` and restores the model from it (clips by
  **name**, since ids may change if the media folder changed), so a restart
  shows the same show (build-v1 §4.2). Only then does it bump `epoch`.
- Strings are UTF-8, NUL-terminated, truncated to fit.
- Ids: clip id = index into `catalog.clip`, effect id = index into
  `catalog.effect`; −1 = none. Ids are stable while the catalog's seq is
  unchanged.
- Effects (M5, FX, additive): `FxSlotState::press` (was `reserved`) = the
  slot's PRESS 0..255 from `FxPress`; `EffectInfo::reserved[0]` = kind
  (0 effect, 1 generator). The full metadata (units, presets, reactions)
  is in `fx/<id>/effect.json` ([`effect-metadata.md`](effect-metadata.md)).

- The transport section is written in the same frame as `live`; a reader
  that needs both consistent reads `live` first, then `transport`, and may
  see a transport one frame newer. `LayerState.position` and
  `LayerTransport.frame` are both the playhead on screen.
- `ClockState::source`: 0 = internal, 1 = tapped, 2 = an external MIDI clock
  leads (M8, D130; Tap / Bpm commands are ignored meanwhile). Additive, no
  version bump.
- **The catalog grows while the show runs (M7, MEDIA, 2026-11-17):** library
  clips that turn ready are appended (ids of existing clips never change);
  `catalog_seq` changes each time. Re-read the catalog on every change
  ([`library-db.md`](library-db.md) §8). Additive, no version bump.
- After a restart the renderer restores the transport settings, cue points,
  BPM and grid from a version 2 block (the HIT layer is never restored).
- **Reactions (v3):** after a restart the renderer restores the reaction
  table of every slot whose effect came back (matched by name). An entry with
  `source` = Default always shows the effect's current default. `env` and
  `value` are 0 for a slot that is not drawn this frame (bypassed, mix 0, on
  a layer nobody sees). Readers diff on `gen` for the settings and read
  `env` / `value` at their own rate.

## 3. Mapping to the design

`program_deck` is the deck on air (PROGRAM, red); the other is PREVIEW
(green) — `design/perform.md`. `focus_deck` / `focus_layer` are the "last
group pressed" focus (D48–D53); the GUI shows it and may change it with a
`Focus` command. `missing` = the layer's clip failed to open; the renderer
shows the clip's `thumb_rgb` placeholder colour instead (build-v1 §4.4).
