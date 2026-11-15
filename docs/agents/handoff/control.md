# CONTROL handoff
Updated: 2026-11-15 by the S2 CONTROL session · Branch: claude/control-slice-perform · Last commit: see git log

## State in one paragraph
MK2: verified map, C++ controller (`src/controller`, shared memory `Local\HimmelMk2`, LEDs, screens, reconnect, `--sim` over UDP 8766), screen pages (`pages.cpp`), Python live view + simulator + games. Generic weapon block + drivers for Lightpad / LUMI / NuVJ / APC20 (`weapons/README.md`). **New (S2): `himmel::perform::Mk2Perform` plays the slice** (`src/perform/`): focus by groups, wheel = TAKE T-bar, knobs, pads = clips, FX buttons, LEDs and both screens; 120 unit checks in `perform_tests.exe`. Not yet run end-to-end: S1 (`claude/engine-slice-decks`) was not pushed when this was written, so the engine does not call the mapper yet.

## Slice mapping (2026-11)
Subset of `docs/design/perform.md` v4 / `hardware-mapping.md` §3. Focus is a **role** (PREP = the PREVIEW deck, PLAY = the PROGRAM deck) + a layer: after a TAKE the rows keep meaning prepare / live, and the mapper re-sends `Focus` for the deck that now has the role. A focus set by the GUI is adopted.

| Control | Does | Command(s) |
|---|---|---|
| Group A–D | Focus PREVIEW layer 1–4 (PREP) | `Focus` (preview deck, layer) |
| Group E–H | Focus PROGRAM layer 1–4 (PLAY) | `Focus` (program deck, layer) |
| `SHIFT` + group | Mute / unmute that layer (focus unchanged) | `LayerMute` toggle |
| Wheel (master encoder) | Endless T-bar: clockwise blends PREVIEW in, 30 detents = all the way, whichever deck is PREVIEW | `Crossfader` relative ±detents/30 |
| `SHIFT` + wheel | Reserved for the system menu (no-op) | — |
| `PLAY`, wheel push | TAKE (show default time) | `Take` i = 0 |
| `RESTART` | CUT | `Cut` |
| Knob 1 | Opacity of the focused layer (1 turn = 0..1) | `LayerOpacity` relative |
| Knob 2 | Speed of the focused layer (1 turn = ±2) | `LayerSpeed` relative |
| Knob 3 / 4 | FX slot 1 param 1 / 2 of the focused layer (only if the effect has it) | `FxParam` relative |
| Knob 5–8 | `GUESS:` stand-in macros = master FX slot 1 params 1–4 | `FxParam` master, relative |
| `SHIFT` + knob 1–7 | Fine (1/10) | same |
| `SHIFT` + knob 8 | Choose the master effect (one step per ~1/10 turn, wraps through "none") | `FxSet` master slot 1 |
| Pad 1–16 | Load clip `page × 16 + pad − 1` into the focused layer; velocity → opacity 0.2..1 | `LayerClip` + `LayerOpacity` absolute |
| `TRANSPORT <` / `>` | Pad page back / forward (clamped) | — |
| `PAD MODE` | Next effect in FX slot 1 of the focused layer (`SHIFT` = previous; wraps through "none") | `FxSet` |
| `NAVIGATE` | Bypass FX slot 1 of the focused layer (toggle; only if it has an effect) | `FxBypass` toggle |
| `MUTE` | Mute / unmute the focused layer | `LayerMute` toggle |

**LEDs:** groups A–D green, E–H red: full = focused, dim (28) = has a clip, off = empty. Pads: the clip's `thumb_rgb` scaled (very dark colours → grey): full = the clip on the focused layer, medium (56) = on any other layer, dim (16) = only in the catalog, white = finger down, off = no clip. `PLAY` full while a TAKE runs, `TRANSPORT < >` lit when that page exists, `PATTERN` lit (CLIPS), `PAD MODE` full when FX slot 1 has an effect, `NAVIGATE` full when bypassed, `MUTE` full when muted, `SHIFT` while held.

**Screens** (one 512 × 64 canvas, left = PREVIEW deck, right = PROGRAM deck): header "PREVIEW B" / "PADS 1/3" (or SHIFT) and "PROGRAM A" / ON AIR · TAKE · BLACK; the focus side's header is inverted, the other underlined. Row 2: the 4 layers in the knob columns ("1 clipname", focused cell inverted, muted struck through, `!` = missing) with opacity bars. Row 3 left: "L1 FX name (BYP)"; right: TAKE bar + % (how much PREVIEW is blended in). Bottom: knob labels / values / bars over the physical knobs (`kKnobX`), the turned knob's column inverted for 0.8 s. Render a sample with `perform_tests --dump file.pbm`.

## Done (with commits)
- S2 slice mapper + tests (branch `claude/control-slice-perform`, see git log). Earlier history: `CLAUDE.md` "Next steps" and `docs/roadmap.md`.

## In progress
- Waiting for S1 to be pushed; then merge it locally and check through `himmel.exe --sim` + `python tools/mk2_live.py --sim` (screens + LEDs in the live view).

## Next up (ordered)
1. End-to-end check with S1 (above); fix whatever the engine's validation refuses (watch `LiveState.rejected[0]`).
2. The owner plays it; settle the PROPOSED rows D97–D99 in `docs/decisions.md`.
3. Then the full perform.md mapping: double-tap group, LOOKS / SLICE / FX PADS pad modes, knob pages (`PAGE < >`), Top 5–8 holds, `ERASE` undo, `REC`, `ENTER`, pad targets.
- Older items kept: `roles` section in the profiles; Phase 1c MK2 (SLICE, PRESS, HIT, scratch); Block SCRUB, LUMI SLICE KEYS; M13 OSC; M6 display drop-out with ENGINE.

## GUESSES
- `GUESS:` the engine flips `program_deck` when a **manual** crossfader move reaches the far end (engine-state.md says "the side the crossfader rests on"). If it only flips on `Take` / `Cut`, the wheel stops at the end and never swaps; check with S1. Confidence: medium.
- `GUESS:` loading a clip starts it playing (the mapper sends no `LayerPlay`). Check with S1.
- `GUESS:` tuning: speed 2× per knob turn, SHIFT = 1/10 fine, 100 raw units per master-effect step, pad floor opacity 0.2, 0.8 s knob highlight. Owner to feel it.
- `GUESS:` pad 1 (bottom-left) = first clip of the page (Maschine numbering). The GUI clip grid should match (S3).
- `GUESS:` knobs 5–8 = master FX params until scenes / macros exist (perform.md §3).
- The mapper does not send `Hello`; it runs inside the render process, so the engine marks slot 0 alive itself.

## Open questions for the owner
- PROPOSED D97 (FX buttons `PAD MODE` / `NAVIGATE`), D98 (pad paging on `TRANSPORT < >`, pad order), D99 (knobs 5–8 + SHIFT) in `docs/decisions.md`.

## How to build / run / test this track
VS 2022 Build Tools' bundled CMake: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, `cmake --build build --config Release`, then `ctest --test-dir build -C Release` (7 suites) or `build\bin\Release\perform_tests.exe`.

## Gotchas found
- `Mk2Perform::sync()` runs at the start of `on_event` and `tick`, so events before the first tick are fine. Commands sent in a frame are not in the `LiveState` passed until the next frame; the mapper keeps its own focus so a group press is not undone by the stale state.
- `pages.h`'s `draw_perform` fills the whole screen with knob slots, so the slice screen is drawn by the mapper itself with the `pages.h` helpers (`text_in`, `text_anchored`, `kKnobColumn`); no controller change was needed.

## Interfaces touched
- `src/perform/include/himmel/perform/mk2_perform.h`: **private members only** (public API unchanged). No protocol change.
