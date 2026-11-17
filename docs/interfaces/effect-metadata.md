# Effect metadata (`fx/<id>/effect.json`) and slot resolution

Version 1, 2026-11-16 (M5, FX track). Owner of the format: FX. Readers: the
engine (`himmel_fx`), the GUI and the MK2 pages (through the catalog), later
SHOW (presets, shuffle) and AUDIO (reactions).

Design source: [`design/effects-and-generators.md`](../design/effects-and-generators.md)
§1, [`design/clips-and-playback.md`](../design/clips-and-playback.md) §4 and
§8.2, [`design/music.md`](../design/music.md) §4, decisions D65 and D88.

## 1. Where effects live

```
fx/
  catalog.json          order = catalog ids (APPEND ONLY, never reorder or remove)
  _common.hlsl          shared prelude: cbuffer, samplers, helpers
  _main.hlsl            the ps_fx entry (mixes the wet picture with MIX)
  <id>/effect.json      metadata (this page)
  <id>/<id>.hlsl        float4 fx(float2 uv, float4 s)  - the effect body
```

- The build embeds the whole folder into the engine
  (`src/engine/fx/embed_fx.cmake` → `himmel/fx/builtin_data.h`). The engine
  never reads effect files at run time, so nothing can go missing on a show
  PC. **No backslashes in `fx/` files** (the embed step refuses them).
- Every folder must be listed in `catalog.json` and every listed id must have
  a folder; `fx_tests` and the start-up check both refuse a mismatch.
- The **catalog id** (position in `order`) is what the command ring
  (`FxSet i`) and the state block use. `src/engine/include/himmel/engine/fx_catalog.h`
  mirrors the order as an enum for code and tests.
- All shaders are compiled once at start-up, in worker threads
  (`FxGpu::init`, ~0.23 s for 25 on the dev laptop). One that fails to
  compile is logged and passes the picture through; it never stops the engine.

## 2. `effect.json`

All knob values (`default`, `good`, preset values, `press.lo / hi`,
`reaction.range`) are **knob positions 0..1**, the same numbers the command
ring and the state block carry. `min`, `max`, `unit` and `curve` only tell a
GUI or the MK2 screen how to print a position in real units.

```json
{
  "id": "kaleido", "name": "Kaleido", "kind": "effect", "family": "space",
  "tier": 1, "shelf": "builtin",
  "params": [
    {"id": "segments", "name": "SEGMENTS", "min": 2, "max": 16, "unit": "",
     "default": 0.3, "good": [0.07, 0.72], "steps": 15},
    {"id": "turn", "name": "TURN", "min": 0, "max": 360, "unit": "deg", "default": 0.0},
    {"id": "zoom", "name": "ZOOM", "min": 0.25, "max": 4, "unit": "x", "default": 0.5, "curve": "exp"},
    {"id": "spin", "name": "SPIN", "min": -0.25, "max": 0.25, "unit": "turn/s",
     "default": 0.5, "good": [0.4, 0.6], "speed": true}
  ],
  "main": ["segments", "turn"],
  "reaction": {"signal": "mid", "param": "turn", "amount": 0.1, "attack_ms": 30,
               "release_ms": 400, "mode": "add", "curve": "soft", "threshold": 0.1},
  "press": {"param": "segments", "lo": 0.07, "hi": 0.71, "curve": "steps", "release_beats": 0.5},
  "presets": [
    {"name": "subtle", "values": {"segments": 0.07, "turn": 0.0}},
    {"name": "extreme", "values": {"segments": 0.8, "zoom": 0.3}}
  ],
  "scopes": {"spin": ["slot", "look", "global"]},
  "flash": null,
  "levels": ["clip", "layer", "deck", "master", "output"],
  "cost": {"class": "S"}, "passes": 1, "state": "none",
  "compat": {"avoid": [], "not_with": ["mirror"]},
  "help": ["Folds the picture into a kaleidoscope.", "SEGMENTS 2..16; PRESS adds segments."]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Folder name, `[a-z0-9_]`, < 24 chars. Generators start with `gen_` (convention). |
| `name` | yes | Shown in the browser, the GUI list and on the MK2 (< 48 bytes). |
| `kind` | no (`effect`) | `effect` changes the picture below it; `generator` makes one (§6). |
| `family`, `tier`, `shelf` | no | Browser grouping, roadmap tier, `builtin` / `addon`. |
| `params[]` | yes | 1–4 knobs (knobs 1–4 of an FX page; the shader gets them as `P`). `id`, `name` (< 16, upper case on the MK2) and `default` are required. `good: [lo, hi]` (default `[0, 1]`) = the range the shuffle and the reaction use. `steps: n` = a stepped knob with n positions (`step_index`, §4). `speed: true` = can lock to the clock (knob 7 SYNC, M8). `labels` = names of the steps (display only). |
| `main` | yes | The two main params: knobs 1–2, PMTR 1 / 2 on the NuVJ, X / Y on the Lightpad. Must differ. |
| `reaction` | **yes (D88)** | The default music reaction (§3). Every effect and generator has one. |
| `press` | yes | The PRESS target (§5): `param`, `lo`, `hi` (knob positions), `curve` (`lin`, `soft`, `hard`, `exp`, `steps`), `release_beats`. |
| `presets[]` | yes | Named knob sets; **`subtle` and `extreme` are required** (FX-pad velocity picks them, `clips-and-playback.md` §8.5). Values by param id; missing ones = the default. Up to 8. |
| `scopes` | no | Per param, the scopes it allows (§4.2 of the clip design): `slot`, `look`, `global`, `move`. Default: all. `slot` cannot be removed. |
| `flash` | no | How the optional flash cap (D65) reaches this effect (§7). |
| `levels` | no | Where it may sit: `clip`, `layer`, `deck`, `master`, `output`. Default: all. The GUI should only offer it there (luma key: clip / layer). |
| `cost` | yes | `class` `S` (< 0.15 ms), `M` (< 0.6 ms), `L` at 1080p on a GTX 1070, worst of the defaults and the `extreme` preset; `fx_check` checks it. |
| `passes`, `state` | no | 0.1 allows only 1 pass. `state`: `none`, `feedback` (the engine keeps this slot's previous output, §6), `history` (later). |
| `compat` | no | Shuffle rules (SHOW track, later): `avoid` tags, `not_with` effect ids. |
| `help` | yes | Two short lines for the MK2 screen and the Help menu. |

The checks (`Library::validate`, run at start-up and by `fx_tests`): values in
0..1, `good.lo <= good.hi`, steps 0 or 2..64, unique param ids, a reaction on a
real signal with a non-zero amount in −1..1, attack / release 0..2000 ms, hold
0..4 beats, a press target, `subtle` + `extreme`, a cost class, a help line,
`flash.beats` matching the rate knob's steps.

## 3. The reaction (data now, signals in M8)

```json
"reaction": {"signal": "kick", "param": "amount", "amount": 0.4,
             "attack_ms": 5, "release_ms": 180, "mode": "add", "curve": "hard",
             "threshold": 0.2, "range": [0.1, 0.6], "quantise_beats": 0, "hold_beats": 0}
```

- `signal`: `level`, `bass`, `mid`, `high`, `kick`, `snare`, `hat`, `onset`,
  `beat`, `bar`, `energy`, `build`, `drop` (`music.md` §4; spectrum bands
  come with M8).
- The resolver: gate (`threshold`) → envelope (`attack_ms` / `release_ms`,
  one-pole) → `curve` → applied to `param` in `range` (default: the param's
  `good` range):
  - `add`: knob + amount × curve(env) × (hi − lo);
  - `follow`: moves towards lo + (hi − lo) × curve(env) by |amount| (negative
    amount = the other way);
  - `trigger`: fires on a rising edge over the threshold (on the
    `quantise_beats` grid when set), stays up `hold_beats`, then releases.
- A param with scope **MOVE** follows the reaction over its whole range.
- **Per-knob reactions (D131, AUDIO, 2026-11-17):** this is each effect's
  *default* reaction. `FxReact` (command 40, [`command-protocol.md`](command-protocol.md) §4d)
  gives any knob of a slot its own reaction, or switches one off; the table
  is in the state block (v3, `ReactionSection`). Each knob has its own
  envelope; the resolver runs them in knob order.
- **0.1 status:** the engine feeds silence until the AUDIO track exists, so
  reactions are data only. `himmel_render --test-signal` feeds
  `himmel::fx::TestSignal` (kick on every beat, snare on 2 and 4, hats on the
  eighths, slow mid / high / energy swells, from the show clock) for checks;
  `fx_tests` checks that every default reaction moves its param with it.

## 4. Scope resolution (per param)

Order inside `himmel::fx::resolve()` (runs per active slot per frame; pure
maths, no allocation):

1. **Scope**: SLOT = the slot's knob (the command ring's `FxParam`); LOOK =
   the scene's value (the M3 scene model fills `SlotRuntime::look`; until a
   scene sets one, the slot value); GLOBAL = one value per effect id shared
   by every instance (`Globals`); MOVE = driven by the reaction / a
   modulator. A scope the effect does not allow falls back to SLOT.
2. **PRESS** (§5).
3. **Reaction** (§3).
4. Stepped knobs snap to their step centre (`step_center`), all values clamp
   to 0..1 (NaN → 0).
5. **Flash cap** (§7).

In 0.1 the per-slot scope choice lives in the engine's `SlotRuntime`
(default SLOT); there is no command to change it yet. PROPOSED (D106): an
`FxScope` command with the scene model in M3.

Levels: the slice has slots on **layers, decks and master** (`kFxSlots` = 4
each, command-protocol.md §4). The resolver does not care which level a slot
is on; each chain has its own runtime (`FxGpu` chains: layers 0–7, decks 8–9,
master 10, the HIT layer 11, outputs 12–15 reserved). **Clip** and **output** slots come with
the clip variants (M2 / M3) and the output model (M6); they only need their
own chain ids.

## 5. PRESS

- Command `FxPress` (command-protocol.md §4): `f[0]` = pressure 0..1 on one
  slot; the state block keeps it in `FxSlotState::press` (0..255).
  Weapons send it from pad velocity / aftertouch, the Lightpad Z, LUMI
  pressure.
- The engine shapes it (`SlotRuntime::press_env`): it rises at once
  (velocity sets the start, aftertouch moves it), and after a release it
  falls back over `release_beats` at the show BPM.
- The PRESS param moves from the knob towards
  `lo + (hi − lo) × curve(press)`, fading in over the first 5 % of
  pressure; stepped targets land on steps.
- The shader also gets the envelope as `X.y` for effects that want it.

## 6. Generators and feedback

- A **generator** (`kind: generator`) is an effect that paints a picture
  (`over()` in `_common.hlsl`: colour + coverage alpha) on top of what is
  below it, so lines / rings / shapes / ticker can sit over a clip.
- On an **empty layer** (no clip) the engine draws the layer when one of its
  active slots holds a generator: the source is transparent black
  (`ps_source` mode 6), the generator paints it, and the layer's opacity and
  blend mode apply as for a clip. PROPOSED (D107): a proper "layer source =
  generator" command with M3; this rule keeps the protocol unchanged for 0.1.
- **Feedback** (`state: feedback`, trails): the engine keeps the slot's
  previous output in one of 4 canvas-sized textures (made at resize, never per
  frame; `t2` / `aux`). With more than 4 feedback slots on air, the extra ones
  pass the picture through. A slot that is off for a frame loses its history.
- **Text ticker** gets a text strip in `aux` (5×7 pixel font, made at
  start-up). Text: `himmel_render --ticker "TEXT"` until shows carry it.

## 7. The flash cap (D65, off by default)

`FrameCtx::flash_cap_hz` (`himmel_render --flash-cap N`, 0 = off; later a
show setting). Effects declare how they flash:

- `"flash": {"mode": "rate", "param": "rate", "duty": "duty", "beats": [4, 2, 1, ...]}`
  (strobe): the rate knob is clamped to the fastest step at or under the cap
  at the current BPM, and the on/off gate is computed on the CPU and passed to
  the shader (`X.x`). Every flash start is at least 1 / cap (+1 ms) after the
  last one, so knob jumps, PRESS, reactions, BPM changes or frame jitter can
  never make more.
- `"flash": {"mode": "level", "param": "amount"}` (invert): a rise of the
  param over 0.5 is a flash; a rise that comes too soon is held below.

`fx_tests` checks: caps 1 / 2 / 3 / 5 per second, BPM 60–300, random knobs,
PRESS mashing, the test signal and jittery frames for 30 s each: never more
flashes than the cap in any one-second window. Without a cap, 1/32 at
120 BPM = 64 flashes / s (the knob means what it says).

## 8. Shader contract

`_common.hlsl` (registers chosen so they never clash with the engine's own
passes: b5, s4–s7, t0, t2):

| | |
|---|---|
| `P` | the 4 resolved params, 0..1 |
| `I` | x MIX, y time (s), z beats (show clock), w BPM |
| `S` | target width, height, 1/width, 1/height |
| `X` | x flash gate, y PRESS, z / w aux texture size |
| `src` (t0) | the picture below (straight alpha) |
| `aux` (t2) | feedback or the text strip |
| `sLin`, `sPnt`, `sRep`, `sMir` | linear clamp, point clamp, linear wrap, linear mirror |

Helpers: `tex`, `tex_mirror`, `tex_clip`, `luma`, `hsv`, `hue_turn`, `rot`,
`step_of`, `hash12`, `hash11`, `vnoise`, `vnoise1`, `over`. Use
`SampleLevel` only. `ps_fx` returns `lerp(s, saturate(fx(uv, s)), MIX)`.

## 9. Tools

- `fx_tests.exe`: metadata rules, catalog order, every shader compiles,
  resolver (scope, PRESS, reaction), flash cap, `FxPress` in the deck model.
- `fx_check.exe [out] [--size WxH] [--md file]`: a PNG per effect at its
  defaults and its `extreme` preset, two contact sheets, GPU time per pass;
  exit 1 if an effect is over its cost class.
- `himmel_cmd fx T S EFFECT [PRESET]`, `himmel_cmd press T S V`.
