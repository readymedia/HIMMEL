# Command protocol (`Local\HimmelCommands`)

Status: **version 1, written by the LEAD 2026-11-15** for the playable slice;
**M2 clip transport appended by ENGINE 2026-11-16** (§4a: types 18–23,
`PlayAction` 4–5, `kHitDeck`, `kRelease`; the block layout and
`kCommandVersion` are unchanged, an older engine refuses the new types as
`UnknownType`); **M3 show commands appended by SHOW 2026-11-17** (§4b: types
25–36, producer slot 4 = `Show`, flags `kVariation` / `kClear`, the new block
`Local\HimmelShow`; additive like `FxPress`, so `kCommandVersion` stays 1);
**M6 outputs appended by ENGINE 2026-11-17** (§4c: types 70–73, the new block
`Local\HimmelOutputs`; additive, `kCommandVersion` stays 1);
**`SystemQuit` (74) appended by ENGINE 2026-11-17** (§4e; additive)
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

## 4c. Outputs (M6, ENGINE; [`outputs-block.md`](outputs-block.md))

Types 70–79 are ENGINE's range (LEAD, wave 3). They go through the deck
model like every command (rate limit, validation, ack in `LiveState`), and
the output model applies them. The result is shown in `Local\HimmelOutputs`.
The enums are in `outputs.h`. Types 37–39 and 41–69 are not used yet and are
refused as `UnknownType`.

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `OutputBlack` (70) | `slot` = output 0..7 (`kNone` = all) | `i` = Toggle value | black per output (a cut); master `Black` still applies |
| `Identify` (71) | — | `i` = 0 off (fade out), 1 = 5 s, 2 = until 0 (held) | 0.5 s fade in / out; drawn over the show. A held IDENTIFY belongs to the producer that sent it; when that producer has no heartbeat for 2 s it is let go (fade out), so an output never sticks in IDENTIFY. `himmel_cmd identify hold` therefore lasts ~2 s after the tool exits |
| `LayerPlace` (72) | deck (or `kHitDeck`), layer | `i` = `Place` (FillAll, Out, Fit, Free); `param` = output (Out); `f[0..3]` = x, y, w, h on the canvas 0..1 (Free; w, h > 0) | Out n with n ≥ the output count is refused (`BadTarget`) |
| `OutputConfig` (73) | `slot` = output (`kNone` = all) | `param` = `OutputParam`: Resolution (`i` = preset; a setup action, rebuilds the canvas), Refresh (`i` = `RefreshRate`), BlendWidth (`i` px 0..1024), BlendGamma (`f[0]` 1..3, clamped), Mode (`i` = span / mirror), Adopt (`slot`; `i` = 1 take the new display), SimLost (`slot`; `i` = 1 act as if the cable was pulled, 0 back: tests on one screen) | |

## 4d. Audio reactions (AUDIO, D131, 2026-11-17; `design/music.md` §4)

Types 40–49 are AUDIO's range. `FxReact` gives any FX knob its own reaction
(signal × amount × curve, attack / release, threshold, range, mode,
quantise, hold). The table it edits is in the state block, version 3
(`ReactionSection`, [`engine-state.md`](engine-state.md) §1). It goes
through the deck model like every command (rate limit, validation, ack in
`LiveState`). Packing helpers and enums: `commands.h` (`ReactArgs`,
`pack_react`, `unpack_react`, `react_args_ok`, `make_react`).

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `FxReact` (40) | deck (or `kMasterDeck`), layer (`kNone` = deck level), slot, `param` = knob 0..3 (`kNone` only with Default / Off = all 4) | `i` = `pack_react(ReactArgs)`; `f[0..3]` per op (below) | the slot must hold an effect and the knob must exist (`param < param_count`), else `BadTarget` |

`i`, bit by bit (unused bits must be 0, else `BadValue`):

| Bits | Field | Values |
|---|---|---|
| 0–3 | `op` (`ReactOp`) | 0 Set, 1 Amount, 2 Signal, 3 Range, 4 Default, 5 Off |
| 4–11 | `signal` (`ReactSignal`) | 0 none, 1 level, 2 bass, 3 mid, 4 high, 5 kick, 6 snare, 7 hat, 8 onset, 9 beat, 10 bar, 11 energy, 12 build, 13 drop (= `himmel::fx::Signal`) |
| 12–15 | `mode` (`ReactMode`) | 0 ADD (on top of the knob), 1 FOLLOW (replaces it), 2 TRIGGER (fires on a rising edge over the threshold) |
| 16–19 | `curve` (`ReactCurve`) | 0 linear, 1 soft, 2 hard, 3 steps, 4 exp |
| 20–23 | `quantise` (`ReactQuantise`) | TRIGGER grid: 0 off, 1 = 1/16 (¼ beat), 2 = 1/8, 3 = 1/4 (1 beat), 4 = 1/2, 5 = 1 bar |
| 24–28 | `hold` | TRIGGER hold in quarter beats, 0..16 (0..4 beats) |
| 29–31 | — | 0 |

The ops (`f[]` values out of range are clamped; NaN / inf = `BadValue`):

| `op` | Uses | Does |
|---|---|---|
| Set (0) | every `i` field; `f[0]` amount −1..1, `f[1]` attack ms 0..2000, `f[2]` release ms 0..2000, `f[3]` threshold 0..1 | the whole reaction; the range stays as it was. Readers that change one setting (the GUI editor, the MK2 reaction page) read the entry from the state block, change it and send Set |
| Amount (1) | `f[0]` −1..1 (`kRelative` = delta) | MK2 knob 6 (AUDIO). A Default entry becomes Custom with the default's other settings |
| Signal (2) | `i`'s signal | `AUTO WRITE` + turn. As Amount, a Default entry keeps the default's other settings; on a knob with no reaction yet, the rest is ADD, amount 0.5, attack 10 ms, release 200 ms, linear (GUESS: owner to tune) |
| Range (3) | `f[0]` lo, `f[1]` hi (knob positions 0..1; lo > hi is allowed and moves the other way); `f[0]` < 0 = back to the param's good range | |
| Default (4) | — | back to the effect's default reaction (`param` = `kNone`: all 4 knobs) |
| Off (5) | — | no reaction on this knob, not even the default (`param` = `kNone`: all 4) |

- **FxSet** resets the slot's 4 entries to Default (the new effect's own
  reaction). `FxParam` never touches the table.
- An effect's default reaction is the one in its `effect.json`
  ([`effect-metadata.md`](effect-metadata.md) §3); it lives on one knob, so
  the other knobs of a fresh slot have no reaction (signal None).
- The resolver runs each knob's reaction on its own envelope, in knob order,
  after scope and PRESS (effect-metadata.md §4). A knob with scope MOVE
  follows its reaction over the whole range.
- Not in this version: the REACT macro (scales every reaction of a deck),
  spectrum-band signals, reactions on layer parameters (opacity, speed …)
  and on macros. They come as new ops / types in 41–49.

## 4e. Quit (ENGINE, 2026-11-17)

One way to close the whole of Himmel cleanly, from the GUI's QUIT button or
the MK2 system menu (`SHIFT` + wheel → QUIT, push and hold 2 s).

| Type | Targets | Argument | Notes |
|---|---|---|---|
| `SystemQuit` (74) | — | `f[0]` = 1 (confirmed) | anything else is refused (`BadValue`); only from producer `Perform`, `Gui` or `Tool` (`BadTarget` from `Osc` and `Show`, so the network or a show file can never quit) |

Quit must never happen by accident, so the **producer asks first** and sends
`f[0]` = 1 only after the person confirmed: the GUI asks "Quit Himmel?", the
MK2 needs a 2 s push-and-hold with a countdown on the screen, `himmel_cmd quit`
needs `--confirm`.

What happens:
1. The renderer acks it and sets `quit_request` (producer slot + 1) in its
   status block `Local\HimmelEngine` (`EngineStatus`, version 2). It keeps
   drawing the show until it is told to stop.
2. The supervisor sees it within 50 ms and runs the same clean stop as
   Ctrl+C: renderer (`stop_request`), GUI, audio, ingest, then every weapon
   driver gets `stop_request` (the Lightpad gets config 13 back, LEDs off),
   then the MK2 controller, then the curtains drop. `himmel.exe` exits 0.
3. Without a supervisor (`himmel_render` on its own) the renderer quits by
   itself when nobody has asked it to stop 3 s after `quit_request`.

A renderer restart clears `quit_request` (the block is recreated).

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
