# Engine state block (`Local\HimmelState`)

Status: **version 1, written by the LEAD 2026-11-15** for the playable slice
(D90). Header: [`src/protocol/include/himmel/protocol/state.h`](../../src/protocol/include/himmel/protocol/state.h)
(source of truth for the layout). Writer: `himmel_render` only. Readers: the
GUI bridge, weapon mappers, tools. Change rules as in
[`command-protocol.md`](command-protocol.md).

## 1. Sections

| Section | Seqlock | Written | Holds |
|---|---|---|---|
| `status` | none (atomics) | every frame | `heartbeat_us`, `pid` |
| `live` (`LiveState`, ~8 KB) | `live_seq` | when something changed, at most once per frame, and at least every 250 ms | the whole show model on air: crossfader, `program_deck`, TAKE progress, black, focus, 2 decks × 4 layers (clip, blend, mute, playing, missing, opacity, speed, position, 4 FX slots), deck FX, master FX, per-producer acks / refusals / alive |
| `catalog` (`Catalog`, ~25 KB) | `catalog_seq` | when the clip or effect list changes | clips (name, size, fps, length, codec, placeholder colour) and effects (name, param names and defaults) |

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

## 3. Mapping to the design

`program_deck` is the deck on air (PROGRAM, red); the other is PREVIEW
(green) — `design/perform.md`. `focus_deck` / `focus_layer` are the "last
group pressed" focus (D48–D53); the GUI shows it and may change it with a
`Focus` command. `missing` = the layer's clip failed to open; the renderer
shows the clip's `thumb_rgb` placeholder colour instead (build-v1 §4.4).
