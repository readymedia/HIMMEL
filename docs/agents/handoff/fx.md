# FX handoff
Updated: 2026-11-16 by the FX M5 session (Claude Code) · Branch: `claude/fx-m5-effects` · Last commit: see `git log`

## State in one paragraph
M5 is built: the effect metadata format (`docs/interfaces/effect-metadata.md`),
19 effects + 6 generators in `fx/` (one `effect.json` + one `.hlsl` each,
embedded into the engine at build time), the per-frame resolver for scope /
PRESS / reaction / the optional flash cap (`himmel_fx`), and the GPU runner
(`himmel_fx_gpu`: one shader per effect compiled at start-up in worker
threads, chains on the engine's ping-pong targets, a 4-texture feedback pool
for trails, a pixel-font strip for the text ticker). The slice engine uses it
for every FX slot (layer, deck, master); a generator on an empty layer draws
by itself; `FxPress` is a new (additive) command. `himmel_cmd`, the web GUI
(catalog-driven) and the MK2 (catalog-driven) see the 25 entries. Reactions
are data: the engine feeds silence until M8, or `TestSignal` with
`--test-signal`.

## Done (with commits)
- d112c0f + following commits on `claude/fx-m5-effects` (see `git log`):
  - `fx/` (catalog.json, _common.hlsl, _main.hlsl, 25 effect folders).
  - `src/engine/fx/`: `embed_fx.cmake`, `himmel/fx/fx.h` (metadata, Library,
    resolve), `signals.h` (TestSignal), `fx_gpu.h` (FxGpu).
  - `tests/fx/fx_tests.cpp` → `fx_tests.exe` (76 051 checks: metadata rules,
    catalog order = `fx_catalog.h` enum, every shader compiles, scope / PRESS /
    reaction, **flash cap never exceeded**, FxPress in the deck model).
  - `tools/fx_check/` → `fx_check.exe` (PNG per effect at defaults + extreme,
    contact sheets `docs/images/fx-sheet*.png`, GPU time, `--md`).
  - Engine hooks (ENGINE was told): `render_main.cpp` run_fx → FxGpu,
    begin_frame, `make_canvas` → `FxGpu::resize`, generator layers;
    `slice_shaders.h` lost the old `ps_fx` uber-shader, `ps_source` mode 6
    (transparent); `deck_model.cpp` FxPress; `fx_catalog.h/.cpp` = enum +
    wrapper; protocol `FxPress = 18`, `FxSlotState::press`,
    `EffectInfo::reserved[0]` = kind; `ui_bridge` json_codec knows `FxPress`;
    `himmel_cmd fx T S EFFECT [PRESET]`, `himmel_cmd press T S V`.
  - Render args: `--flash-cap N`, `--test-signal`, `--ticker "TEXT"`.

## How the frame loop stays clean
- Start-up only: JSON parse (`builtin()`), shader compiles (worker threads,
  joined before the loop), sampler / CB / ticker texture creation.
- Resize only: the 4 feedback textures.
- Per frame: `begin_frame` (struct copy); per active slot: `resolve()` (maths
  on PODs, no heap), one `Map(DISCARD)` of a 64-byte CB, binds, `Draw(3)`;
  trails add one `CopyResource`. `VSGetShader`/`VSSetShader` restore the
  engine's VS once per chain. No locks, no files, no allocation.

## Checked
- `fx_tests` 76 051 checks, all other test programs green (deck 143,
  entrancer 238, media 31, mk2 4306, perform 120, protocol 1111, touch_fx 77,
  ui_bridge 122, weapon 163).
- `fx_check`: every effect within its cost class on the GTX 1070,
  `docs/measurements/2026-11-16-fx-m5-gpu.md`; sheets in `docs/images/fx-sheet*.png`.
- In the engine (windowed, 960x540): ~11 FX passes, 3.5 min, 60 fps, 0 late
  frames after start-up, GPU 2.4 ms; generators on empty layers, trails,
  ticker text, PRESS and presets via `himmel_cmd` all seen in the preview.
- Not checked: the web GUI's effect list with 25 entries (catalog-driven,
  should just work), the MK2 effect stepping (SHIFT + knob 8 / PAD MODE) over
  25 entries, a fullscreen soak.

## Next up (ordered)
1. Owner plays the effects (GUI on 8790 / `himmel_cmd`); tune defaults,
   presets and reactions (D104).
2. M8 hook: replace the silent `FrameCtx::sig` with the audio block, the BPM /
   beats with the show clock (`render_main.cpp`, the `FrameCtx` block).
3. REACT macro + knob 6 AUDIO amount (scale `reaction.amount` per slot):
   needs a per-slot field or command (with M8 / M3).
4. `FxScope` command + LOOK values from scenes (M3, D102).
5. More tier-1 effects from the design table: threshold, palette map,
   levels, tint, flip, 3D turn, twirl, bulge, edge detect, flash, chroma key,
   luma to alpha, crop; generators: plasma, checker, starfield, spectrum,
   beat pulse, test card.
6. Clip and output FX levels (chains 11–15 are reserved in `FxGpu`).

## GUESSES
- GUESS: default reactions where the design table says "—" (bright /
  contrast: energy → bright; colorize: bass → light; mirror: bass → center;
  rotozoom: bass → zoom; blur: bass → size; trails: bass → decay; luma key:
  bass → level; text: kick → size) and all `good` ranges / presets. Medium
  confidence; the owner tunes them by playing. D104.
- GUESS: invert's snare reaction flips the picture *back* (amount −1),
  because its default AMOUNT is 1 (fully inverted). Medium.
- GUESS: 4 feedback textures are enough (`kFeedbackPool`); a 5th trails slot
  passes through. High for 0.1.
- GUESS: the text ticker's text is a render argument until shows exist; a
  quarter screen gap between repeats. Low stakes.
- GUESS: 120 BPM show clock (the engine's existing guess) for strobe, zoom
  pulse, rings and PRESS release until M8.

## Open questions for the owner
- D102 (protocol additions), D103 (generators in FX slots), D104 (flash cap
  model + guessed default reactions). D101 is TECH.

## How to build / run / test this track
- Build as usual (see `CLAUDE.md`); the `fx/` folder is embedded by a custom
  build step, so editing a `.json` / `.hlsl` rebuilds `himmel_fx`.
- `build\bin\Release\fx_tests.exe` (must print `0 failed`).
- `build\bin\Release\fx_check.exe [tools/_screens/fx] [--md file]` → PNGs,
  sheets, timings (exit 1 if an effect is over its cost class).
- New effect: add `fx/<id>/effect.json` + `fx/<id>/<id>.hlsl`, append the id
  to `fx/catalog.json` and the enum in `fx_catalog.h`, run both tools.

## Gotchas found
- CMake eats backslashes when embedding: `fx/` files must not contain any
  (the embed step fails with a clear message).
- `near` and `far` are macros from `windows.h`; `min` / `max` too unless
  `NOMINMAX` (the mk2 `output.h` breaks without it).
- Per-pixel random jitter in tap patterns (blur / glow) costs a lot on the
  1070 (texture cache): glow at max size went 0.99 → 0.35 ms without jitter.
- GPU timings are only honest with no other Himmel renderer running (another
  agent's stack inflated them by 30–50 %).

## Interfaces touched
- New: `docs/interfaces/effect-metadata.md` (version 1).
- `command-protocol.md` (FxPress), `engine-state.md` (press, kind byte),
  `interfaces/README.md`. All additive; LEAD to confirm (D102).
