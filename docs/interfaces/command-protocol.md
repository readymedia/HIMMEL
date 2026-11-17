# Command protocol (`Local\HimmelCommands`)

Status: **version 1, written by the LEAD 2026-11-15** for the playable slice;
**M2 clip transport appended by ENGINE 2026-11-16** (§4a: types 18–23,
`PlayAction` 4–5, `kHitDeck`, `kRelease`; the block layout and
`kCommandVersion` are unchanged, an older engine refuses the new types as
`UnknownType`); **M3 show commands appended by SHOW 2026-11-17** (§4b: types
25–36, producer slot 4 = `Show`, flags `kVariation` / `kClear`, the new block
`Local\HimmelShow`; additive like `FxPress`, so `kCommandVersion` stays 1)
(D90). Header: [`src/protocol/include/himmel/protocol/commands.h`](../../src/protocol/include/himmel/protocol/commands.h)
(the header is the source of truth for field layout; this page is the why and
the rules). Change it only with a PR to this page + the header, agreed by the
LEAD. **Append-only:** never renumber a `CommandType` or an enum value.

## 1. What it is

Everything that wants to change the show (the GUI, weapon mappers, OSC, test
tools) sends **commands**. The render process is the only consumer. It
validates every command and applies it **at a frame boundary**; the result is
visible in the state block ([`engine-state.md`](engine-state.md)). Nobody
else ever touches engine objects (build-v1 §4.3).

```
himmel_ui (GUI) ──ring 1──┐
OSC (M13)       ──ring 2──┤
tools / tests   ──ring 3──┼──► himmel_render: validate ▸ apply at frame start ▸ LiveState
MK2 Perform     ──ring 0──┘        (in-process producer, same Command + same validation)
```

## 2. The block

- Name `Local\HimmelCommands`, struct `CommandBlock`: header (magic `HCMD`,
  version 1, size), 8 `ProducerInfo` (pid, heartbeat) and 8 SPSC rings of
  1024 × 32-byte commands (~260 KB).
- **Whoever comes first creates it** (`CreateFileMappingW` on the paging file
  opens the existing one if it is there); the creator zero-fills it and then
  writes the header last. Everyone checks magic, version and size before use,
  and treats a mismatch as "not there" (log once, retry every second).
- Because the GUI bridge keeps its handle open, the block survives a renderer
  restart.

## 3. Producer slots

| Slot | Producer | Writer process |
|---|---|---|
| 0 `Perform` | weapon mappers (MK2 Perform, later Lightpad / NuVJ) | `himmel_render` itself |
| 1 `Gui` | the web GUI | `himmel_ui` |
| 2 `Osc` | OSC in (M13) | `himmel_osc` or the bridge |
| 3 `Tool` | tests, scripts | `himmel_cmd`, `show_cmd`, test programs |
| 4 `Show` | the show runtime inside the render process (M3): recall, morph, undo, dice as ordinary commands straight into the model (no shared ring); same validation and rate limit as every producer; at most `kShowCommandsPerFrame` (384) per frame, the rest waits in its outbox (show-files.md §1) | `himmel_render` itself |
| 5–7 | spare | — |

Exactly one process writes a slot. A producer writes its `pid` into
`producers[slot]` when it starts, updates `heartbeat_us` on every push, and
sends `Hello` at least every second when idle. The engine shows a producer as
alive in `LiveState.producer_alive` if its heartbeat is < 2 s old.

The in-process Perform producer may skip the shared ring (the render process
queues its commands in a local `SpscRing`), but its commands go through the
**same validation** and are counted under slot 0.

## 4. The command

32 bytes: `type`, `flags`, `deck`, `layer`, `slot`, `param`, `seq`, `i`,
`f[4]`. Unused targets are `kNone` (0xFF); master-level FX use
`deck = kMasterDeck` (0xFE). `seq` is the producer's own running number
(starting at 1); the engine writes the last applied one into
`LiveState.acked_seq[slot]`, so a producer can tell when its command is on air.
`flags & kRelative` makes `f[0]` a delta (knobs and wheels send deltas;
the GUI sends absolute values).

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `LayerClip` | deck, layer | `i` = clip id, −1 = empty | Clip ids index `Catalog.clip`. Loading never blocks the frame: the clip player opens in its worker; until the first frame is ready the layer shows the clip's placeholder colour (build-v1 §4.4) |
| `LayerOpacity` | deck, layer | `f[0]` 0..1 | clamped |
| `LayerBlend` | deck, layer | `i` = `BlendMode` | |
| `LayerMute` | deck, layer | `i` = 0 / 1 / 2 (toggle) | |
| `LayerPlay` | deck, layer | `i` = `PlayAction` | |
| `LayerSpeed` | deck, layer | `f[0]` −4..4 | |
| `LayerJump` | deck, layer | `f[0]` 0..1 | |
| `FxSet` | deck / master, layer or `kNone`, slot | `i` = effect id, −1 = empty | Effect ids index `Catalog.effect` (order: `fx/catalog.json`, append only; [`effect-metadata.md`](effect-metadata.md)). Setting an effect resets its params to the catalog defaults and PRESS to 0. A generator on a layer without a clip still draws (effect-metadata.md §6) |
| `FxParam` | same + `param` | `f[0]` 0..1 | normalised; the meaning is in the catalog |
| `FxBypass` | same | `i` = 0 / 1 / 2 | |
| `FxMix` | same | `f[0]` 0..1 | dry / wet |
| `Crossfader` | — | `f[0]` 0 (A) .. 1 (B) | a manual move cancels a running TAKE |
| `Take` | — | `i` = ms, 0 = default (slice: 2000 ms) | travels to the other end; when it lands, `program_deck` flips |
| `Cut` | — | — | jumps to the other end now; `program_deck` flips |
| `Black` | — | `i` = 0 / 1 / 2 | master black (the outputs stay alive) |
| `Focus` | deck, layer | — | the focused layer, shared by the MK2 and the GUI |
| `Hello` | — | `i` = pid | keeps the producer alive |
| `FxPress` (24, M5) | deck / master, layer or `kNone`, slot | `f[0]` 0..1 pressure (0 = released), absolute only | the slot's PRESS target ([`effect-metadata.md`](effect-metadata.md) §5); the engine shapes the release. Added by FX 2026-11-16, additive (no version bump) |

`LayerPlay` also takes `Release` (4: leave the inner loop, play the outro) and
`Stop` (5: fade out by the layer's fade-out time, then empty) since M2.
`LayerSpeed` is the **target** speed since M2: the playhead glides to it over
the layer's glide time (default 1 beat, `design/clips-and-playback.md` §2.7).
Every layer command above also accepts the **HIT layer**: `deck = kHitDeck`
(0xFD), `layer = 0`. FX and `Focus` do not (no FX slots on the HIT layer in
0.1).

## 4a. Clip transport (M2, `design/clips-and-playback.md` §2–3)

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `LayerSet` (18) | deck (or `kHitDeck`), layer, `param` = `LayerParam` | `i` (enums, counts, toggles) or `f[0]` (floats; `kRelative` = delta) | One command for every transport setting: mode, direction, in / out, inner loop (loop-in / loop-out / on), time base + beats, random / chop interval + distance + slices, step frames + division, glide, trigger style, start position, fade in / out, beat snap (−1 = global), clip target, autopilot + count, transition + time, catch-up, scratch snap, REVERSE held, seed. Enums out of range are refused; numbers are clamped (ranges in the header) |
| `LayerTrigger` (19) | deck (or `kHitDeck`), layer | `i` = clip id (−1 = the layer's clip), `f[0]` = velocity 0..1 (0 = full); `flags & kRelease` = the pad was let go | A **pad launch**: the addressed layer's trigger style (NORMAL / TOGGLE / PIANO / LEGATO), start position, clip target (OWN / FREE / HIT) and beat snap apply. On PROGRAM (and on the HIT layer) it waits for the snap; PREVIEW never waits. On the HIT layer the velocity is the start opacity and a release fades it out |
| `LayerCue` (20) | deck, layer, `slot` = cue 0..15 | `i` = `CueAction` (Jump 0, Set 1, Clear 2); Set: `f[0]` 0..1, < 0 = at the playhead | Cue points belong to the **clip** (every layer playing it sees them). A pad with no cue point jumps to that 16th of in..out. A jump ends a beat loop / scratch; on PROGRAM it waits for the snap. Needs a clip on the layer (`BadTarget` otherwise) |
| `LayerBeatLoop` (21) | deck, layer | `f[0]` = beats (1/16..16), 0 = off | Starts at the playhead; a new length keeps the start (pressure = shorter). Off returns to where the clip would be when catch-up is on |
| `LayerScratch` (22) | deck, layer | `i` = 1 hold / move, 0 = let go; `f[0]` = 0..1 of the clip, `kRelative` = delta in clip seconds | In SCRATCH mode it moves the playhead itself; in other modes it holds the playhead and lets go with snap-back or stay |
| `Clock` (23) | — , `param` = `ClockParam` | Tap; Bpm `f[0]` 20..300 (`kRelative` = delta); Nudge `f[0]` ±1 beat; Resync; Grid `i` = `GridDiv` | The internal beat clock until the audio clock (M8). Grid = the global beat snap (default off) |

The resulting state (settings + playhead status + clock + the HIT layer) is
in the state block's **transport section** ([`engine-state.md`](engine-state.md)).

**PREVIEW / PROGRAM.** Commands always name a real deck (0 = A, 1 = B). The
deck on air is `LiveState.program_deck` (the side the crossfader rests on);
the other one is PREVIEW. Weapon mappers translate "PREVIEW layer 2" into a
deck using the state they read; the engine never guesses what a producer meant.

## 4b. Show (M3, SHOW; [`show-files.md`](show-files.md))

The render process hands these to the show runtime (`src/show/`), which
validates them, acks them in `Local\HimmelShow` ([`show-block.md`](show-block.md), `ShowStatus.acked_seq`,
not `LiveState`) and changes the decks only through ordinary commands on slot
4. Paths and names come from the producer's **text box** in the show block:
write an id + the text, then send the command with `i` = that id; the
runtime refuses it (`BadValue`) when the box holds another id
([`show-block.md`](show-block.md) §2). Enums are in `show.h`. Types 25–39 are SHOW's range (LEAD,
2026-11-17): 37–39 are free and already go to the show runtime.

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `SceneRecall` (25) | deck | `i` = pad 0..15 of the page, `kSceneNext` (−2, GO) / `kScenePrev` (−3); `slot` = variation 0..7 (`kNone` = the main one); `f[0]` = morph s (0 = cut, < 0 = the scene's transition) | Lands in the frame it is applied (only the differences are sent). GO past the song's end goes to the next song (setlist on) or wraps (bank) |
| `SceneSave` (26) | deck = source | `i` = pad 0..15, −1 = next free; `flags & kVariation` = a new variation of the deck's scene | `REC`. An occupied pad keeps its kit / macros / variations and gets the new look |
| `Undo` (27) | — | `i` = 0 undo, 1 redo | 64 steps, decks + master FX (not the mixer) |
| `SceneReset` (28) | deck, layer (`kNone` = all) | `f[0]` = glide s (0 = 1 beat) | hold `ERASE` + group / `ALL` |
| `Dice` (29) | deck; layer / slot = scope (`kNone` = deck) | `param` = `DiceVerb` (Roll, Mutate, Back, Forward, Rotate); `i` = seed (0 = auto) or rotate step ±1; `f[0]` = mutate amount (0 = 0.2) | applies to the addressed deck at once (producers pick PREVIEW per focus) |
| `Locks` (30) | — | `param` = `LockOp` (Set, Toggle, Clear); `i` = `LockBit` mask | the 8 locks, kept in the show |
| `Project` (31) | — | `param` = `ProjectAction` (New, Open, Save, SaveAs, Export, Revert, RestoreAutosave, Relink); `i` = New: screens 1 / 3; Open: `slot` = recent 0..9, or `slot` = `kNone` and `i` = text id; SaveAs / Relink: `i` = text id; Export: `i` = 0 next to the project, else a text id | done by the save worker; the result is in `FilesStatus` and the message |
| `ShowSelect` (32) | — | `param` = `SelectWhat` (Bank, Song, UseSetlist); `i` = index (`kRelative` = delta) or toggle value | which page the pads show |
| `KitSet` (33) | deck (its scene), `slot` = pad 0..15, `layer` = `PadTarget` | `param` = `PadKind`; `i` = clip / effect id, scene pad, cue, `PadAction` | the deck must hold a saved scene (`BadTarget` otherwise) |
| `MacroSet` (34) | deck (its scene), `slot` = macro 0..3, `layer` = target layer (`kNone` = deck FX) | `param` = binding 0..7 (`kNone` = rename: `i` = text id); `i` = FX slot × 16 + param; `f[0]` min, `f[1]` max; `flags & kClear` = remove | |
| `MacroValue` (35) | `slot` = macro 0..3 | `f[0]` 0..1 (`kRelative` = delta) | knobs 5–8: always the PROGRAM deck |
| `SongEdit` (36) | deck (AddScene: source), `slot` = position | `param` = `SongAction` (New, Delete, AddScene, RemoveScene, Bpm, Rename, Move, RenameScene); `i` = song; New: `i` = text id (0 = "Song n"); Rename: `slot` = song, `i` = text id; RenameScene: `slot` = pad, `i` = text id; `f[0]` = BPM | |

## 5. Validation (the engine, before anything else)

1. Unknown `type` → refused (`Reject::UnknownType`).
2. A target out of range for that type → refused (`BadTarget`).
3. A clip or effect id not in the catalog → refused (`BadId`).
4. NaN / inf, or an enum value out of range → refused (`BadValue`). Values in
   range but too big (opacity 1.3) are **clamped**, not refused.
5. More than `kMaxCommandsPerSecond` (4000) from one producer in one second →
   the rest are refused (`RateLimited`). A stuck producer cannot flood the engine.
6. Every refusal increments `LiveState.rejected[slot]` and sets
   `last_reject[slot]`; the engine logs the first refusal per producer per
   second, never more.
7. The engine drains at most 256 commands per producer per frame, so even a
   full ring costs a bounded time; the rest wait for the next frame.

## 6. Weapon mappers inside the engine (`himmel_perform`)

Mappers turn weapon events into commands. They live in `src/perform/`
(CONTROL track) and are called by the render process:

- `Mk2Perform::on_event(event, live, catalog, sink)` for each MK2 event;
- `Mk2Perform::tick(now, live, catalog, sink, leds, screens)` once per frame
  (or at ~30 Hz) for time-based behaviour and the LED / screen outputs, which
  the engine then writes into `Local\HimmelMk2`;
- `Mk2Perform::reset()` when the MK2 (re)connects.
- `Mk2Perform::set_context(show_status, transport)` once after the mapper is
  made (M4): read-only pointers to the show runtime's status and the transport
  model, so pages and pads can show scenes, cues and beat state.

They are pure logic: **no heap, no locks, no I/O, no Windows calls** (they
run in the frame loop), unit-tested without hardware. Header:
[`src/perform/include/himmel/perform/mk2_perform.h`](../../src/perform/include/himmel/perform/mk2_perform.h).
The LEAD committed a stub (wheel = crossfader, PLAY = TAKE, RESTART = CUT) so
ENGINE can call it before CONTROL delivers the real mapping.

## 7. Lifecycle and failure rules

- **Engine starts / restarts:** it opens or creates the block, then calls
  `skip_all()` on every ring: commands queued before the restart are dropped,
  never replayed. It bumps `LiveState.epoch`, so producers resync from the
  state block.
- **A producer dies:** nothing happens to the show. Its slot simply goes quiet;
  its heartbeat ages out. A restarted producer reuses its slot and continues
  its `seq` from 1 (the engine does not require seq to be monotonic across
  pids; acks are per slot).
- **Ring full:** the producer drops the command and counts it (`dropped`);
  the GUI shows a warning. It never blocks.
- **Version mismatch:** a reader that finds another version logs once and
  stays disconnected (the show plays on).

## 8. Not in version 1 (later, append-only)

Output routing and black per output (M6), reactions (M8), outputs and
resolution (M6). Strings travel through the show block's text boxes (M3), not
in commands.
