# CONTROL handoff
Updated: 2026-11-17 by the CONTROL wave-3 session (M4 merged; M9 Lightpad) · Branch: claude/control-m9-lightpad · Last commit: see git log

## State in one paragraph
MK2: verified map, C++ controller (`src/controller`, shared memory `Local\HimmelMk2`, LEDs, screens, reconnect, `--sim` over UDP 8766), screen pages (`pages.cpp`), Python live view + simulator + games. Generic weapon block + drivers for Lightpad / LUMI / NuVJ / APC20 (`weapons/README.md`). **New (M4, 2026-11-17): `himmel::perform::Mk2Perform` plays Perform v4** (`src/perform/`, `docs/design/perform.md`, `hardware-mapping.md` §3): focus PREP / PLAY with double-tap, wheel = TAKE, Top buttons (locks, FREEZE / STROBE / BLACK / ASSIST), knob pages PLAY / CLIP / LAYER / FX / MIX, knobs 5–8 = the PROGRAM scene's macros, pads CLIPS / SLICE / LOOKS / FX PADS (+ MOODS stub), pad targets, PRESS, HIT, `REC` / `ERASE` / `ENTER`, both screens. 219 unit checks in `perform_tests.exe`. **A whole song is played from the MK2 alone** in `himmel.exe --sim` (`python tools/mk2_e2e.py`: build 3 scenes with pads + REC, LOOKS recall, wheel TAKE, ENTER × 3, macro, BLACK, FX PAD, PLAY-focus edit + variation, ERASE undo, SOLO HIT; PASS) and **input → glass is measured: median 32 ms, p90 38 ms, max 40 ms** (`docs/measurements/2026-11-17-control-m4-input-to-glass.md`). **M9 (2026-11-17): `himmel::perform::LightpadPerform`** plays the Block next to the MK2: TOUCH (Touch FX on PROGRAM), MIX (a real crossfader with soft takeover against the wheel, top row TAKE, bottom row CUT) and SCRUB, with its 15 × 15 LEDs; `himmel_render` reads the Lightpad's weapon slot itself. 50 checks in `lightpad_tests.exe`; `mk2_e2e.py` has a Lightpad stage against the simulated Block (`weapon_blocks.exe lightpad --sim`).

## Perform mapping (M4)
`docs/design/perform.md` v4 wins; owner choices are PROPOSED D160–D162 in `docs/decisions.md`. Focus is a **role** (PREP = the PREVIEW deck, PLAY = the PROGRAM deck) + a layer; after a TAKE the rows keep meaning prepare / live, and the mapper re-sends `Focus` for the deck that now has the role. A focus set by the GUI is adopted.

| Control | Does | Command(s) |
|---|---|---|
| Group A–D / E–H | Focus PREVIEW / PROGRAM layer 1–4. **Double-tap** (< 350 ms) = the same layer on the other side | `Focus` |
| `SHIFT` + group | Mute / unmute that layer (focus unchanged) | `LayerMute` toggle |
| hold `ERASE` + group | Reset that layer to its scene | `SceneReset` (deck, layer) |
| hold pad + group / `ALL` / `SOLO` / `SELECT` | The pad's target: LAYER 1–4 / ALL / HIT / SELECTED (screen: "PAD 6 > LAYER 2") | — (mapper, session only, D162) |
| Wheel | Endless T-bar, 30 detents = all the way, clockwise = PREVIEW in | `Crossfader` relative |
| `TEMPO` + wheel / `TEMPO` alone | BPM ±0.5 per detent / tap | `Clock` Bpm / Tap |
| `SELECT` + wheel | Scratch the focused layer (let go on `SELECT` up) | `LayerScratch` |
| `SHIFT` + wheel | System menu (not in 0.1) | — |
| `PLAY`, wheel push / `RESTART` | TAKE (MIX page time, 0 = show default) / CUT | `Take` / `Cut` |
| Top 1–4 (`SHIFT`: the second four) | Dice locks CLIP FX COLOUR MOTION (LAYOUT SOURCE REACT MOOD); LED lit = free | `Locks` Toggle |
| Top 5 / 6 / 7 (`SHIFT` = latch) | FREEZE (pause PROGRAM layers) / STROBE ("Strobe" in master FX slot 4) / BLACK, while held | `LayerPlay` / `FxSet` / `Black` |
| Top 8 | ASSIST stub | — |
| `PAGE` < > (`SHIFT` = PLAY) | Knob pages PLAY → CLIP → LAYER → FX → MIX (clamped) | — |
| Knobs 1–4 PLAY | SCROLL (next / previous clip, 100 units a step) · DICE (150 units: right Roll, left Back; `ALL` = whole deck) · FX AMT (FX slot 1 mix) · SPEED (2× per turn) | `LayerClip` · `Dice` · `FxMix` · `LayerSpeed` |
| Knobs 1–4 CLIP | IN · OUT · MODE (steps PlayMode) · SPEED | `LayerSet` In / Out / Mode · `LayerSpeed` |
| Knobs 1–4 LAYER | OPACITY · BLEND (steps) · FADE IN · FADE OUT (5 s per turn) | `LayerOpacity` · `LayerBlend` · `LayerSet` |
| Knobs 1–4 FX | EFFECT (slot 1, steps through none) · P1 · P2 · MIX | `FxSet` · `FxParam` · `FxMix` |
| Knobs 1–4 MIX | TAKE time (local, 100 ms–10 s) · BPM (40 per turn) · GRID (steps) · master FX mix | — · `Clock` · `FxMix` master |
| Knobs 5–8 (every page, either focus) | The PROGRAM scene's macros (names / values from the show block); `SHIFT` + knob 8 = choose the master effect (slot 1) | `MacroValue` relative · `FxSet` master |
| `SHIFT` + knob 1–7 | Fine (1/10) | same |
| `PATTERN` | CLIPS ↔ SLICE (LED blinks in SLICE); `SHIFT` = KIT stub | — |
| `SCENE` / `SHIFT` + `SCENE` | LOOKS / MOODS stub | — |
| `PAD MODE` | FX PADS | — |
| `MASTER` < > | CLIPS: clip page; LOOKS: bank (song with the setlist) | — / `ShowSelect` relative |
| `TRANSPORT` < > (`SHIFT` + > = mutate) | Dice back / forward on the focus deck (`ALL` = whole deck) | `Dice` |
| `GRID` | Global snap off ↔ 1 beat | `Clock` Grid |
| `REC` (on release) | PREP: save PREVIEW as a new scene (next free pad); PLAY: a new variation of the live scene. `REC` + pad: LOOKS = save to that pad, SLICE = set a cue | `SceneSave` (`kVariation`) · `LayerCue` Set |
| `ERASE` (on release) / `SHIFT` + `ERASE` | Undo / redo. `ERASE` + `ALL` = reset the focus deck's scene; `ERASE` + SLICE pad = clear the cue | `Undo` · `SceneReset` · `LayerCue` Clear |
| `ENTER` / `SHIFT` + `ENTER` | GO: next scene live (its transition) / next scene into PREVIEW (cut) | `SceneRecall` kSceneNext |
| `NOTE REPEAT` | 1/4-beat loop on every PROGRAM layer while held | `LayerBeatLoop` |
| `MUTE` / `SHIFT` + `MUTE` | Mute the focused layer / PANIC = black latched | `LayerMute` / `Black` |
| `NAVIGATE` | OUTPUTS stub (M6) | — |
| CLIPS pad | Clip `page × 16 + pad − 1` into the pad's target on the focus deck; velocity → opacity 0.2..1 on PREVIEW only; release sends `kRelease` (piano / HIT); `SHIFT` + pad in PREP = load + TAKE; `SOLO` + pad = HIT | `LayerTrigger` (+ `LayerOpacity`) |
| CLIPS pad pressure | PRESS of the target layer's FX slot 1 (once per frame, 64/4095 deadband) | `FxPress` |
| SLICE pad | Jump to cue *n* of the focused layer's clip; pressure > 1200 = beat loop 1 → 1/16 beat; release ends it | `LayerCue` Jump · `LayerBeatLoop` |
| LOOKS pad | Scene on that pad: PREP = into PREVIEW (cut), PLAY = live with its transition; `SHIFT` = the other way | `SceneRecall` |
| FX PADS pad | Catalog effect *n* in master FX slot 3 while held (velocity = mix 0.35..1, pressure = PRESS); `SHIFT` = latch / unlatch | `FxSet` · `FxMix` · `FxPress` |

**LEDs:** groups A–D green, E–H red (full = focus, dim = has a clip). CLIPS pads: on PROGRAM red pulsing with the beat, on PREVIEW green-tinted, in the bank the clip colour dim, white = finger. SLICE: clip colour, bright = cue set. LOOKS: PROGRAM red pulse, PREVIEW green, other scenes their colour dim. FX PADS: magenta = on. Top 1–4 = locks, Top 5–8 = holds, `PATTERN` / `SCENE` / `PAD MODE` = pad mode (blink = SLICE / MOODS), `PAGE` / `MASTER` < > = can move, `ERASE` dim when there is something to undo, `GRID`, `NOTE REPEAT`, `SOLO` / `ALL` / `SELECT` / `TEMPO` / `REC` while held.

**Screens** (left = PREVIEW, right = PROGRAM; the focus side's header inverted): header "PREVIEW B 3b*" (scene pad + variation, `*` = edited) / pad mode + page; "PROGRAM A 1a" / ON AIR · TAKE · BLACK · FREEZE · MORPH. Row 2: the 4 layers over the knobs (focused cell inverted, muted struck through, `!` = missing) + opacity bars. Row 3 left: a flash message for 1.5 s, inverted ("UNDO", "PAD 6 > LAYER 2", the show's own "3b saved"), else page + focused layer + FX; right: TAKE bar + % + BPM with a beat dot. Bottom: knob labels / values / bars over the physical knobs; knobs 5–8 = macro names. Top 1–8 labels are not on the screens (no room; the LEDs show them). Sample: `perform_tests --dump file.pbm`.

## Lightpad mapping (M9)
`docs/design/hardware-mapping.md` §5 "Block". PROPOSED D163 in `docs/decisions.md`. Code: `src/perform/include/himmel/perform/lightpad_perform.h`. `himmel_render` finds the first weapon slot whose header says `lightpad` (checked every second), drains its ring 0 into the mapper (commands on producer slot 0, like the MK2) and writes the mapper's 15 × 15 picture into the slot's `pixel` output (on change, ≤ 30 Hz). The local sink now numbers slot-0 commands itself (two mappers share the slot).

| Input | TOUCH (default) | MIX | SCRUB |
|---|---|---|---|
| Finger down / move | X = which effect (the first ≤ 5 catalog effects, not generators, left → right), Y = amount (mix, top = full), pressure = PRESS; in master FX slot 2 | Middle rows: the crossfader, absolute (left = deck A, right = deck B), **soft takeover**: the finger drives it only after it touches (within 0.03) or crosses the fader; if the wheel / a TAKE moves the fader under it (> 0.03 from what we sent, after 120 ms) it lets go until it catches the fader again. Top row = TAKE, bottom row = CUT (on touch) | X = the focused layer's playhead between in and out (`LayerScratch` hold, absolute); pressure = the layer's FX slot 1 PRESS |
| Second finger | X = param 2 of the touch effect | — | — |
| Lift (last finger) | The effect goes off (`FxSet` −1), unless held | Let go of the fader | Let go of the playhead (back to the play mode) |
| Mode, single tap | HOLD on / off (the effect stays after the lift) | — | — |
| Mode, double tap (< 400 ms) | → MIX | → SCRUB | → TOUCH |
| LEDs | One colour band per effect, the active one filled to its amount; red border (PROGRAM), full while held; white dot per finger | Top row amber (full while a TAKE runs), bottom row dim red; the track red on the PROGRAM side, green on the PREVIEW side; **white line = the crossfader**; a finger that has not caught it blinks blue, a caught finger is white | The clip's colour, its playhead column bright, the finger column white; top / bottom rows green (focus on PREVIEW) or red (PROGRAM); an X when the focused layer is empty |

Not built (M9 follow-ups): Y = speed in SCRUB; LOOKS and MOOD modes; `SHIFT` + `PAD MODE` on the MK2 to switch the Block's mode; the strip under the finger in SCRUB glowing the clip colour (the whole band glows instead).

## Done (with commits)
- **M9 (2026-11-17, branch `claude/control-m9-lightpad`):** `LightpadPerform` + `lightpad_tests` (50 checks), the Lightpad slot in `render_main.cpp` (ENGINE file: `LightpadLink`, ~45 lines, `himmel_render` links `himmel_weapon`), `mk2_e2e.py` Lightpad stage (the stack now starts with `--weapons lightpad`; `--no-lightpad` = the old way), D163.
- **M4 (2026-11-17, branch `claude/control-m4-perform`):** the mapper above, `set_context(ShowStatus*, TransportSection*)` (one call in `render_main.cpp`), 219 unit checks, `mk2_chain_tests` updated (knob 1 opacity now on the LAYER page), `tools/mk2_e2e.py` = basic + song + input → glass, measurement doc, D160–D162.
- S2 slice mapper (branch `claude/control-slice-perform`). Earlier history: `CLAUDE.md` "Next steps" and `docs/roadmap.md`.

## In progress
- Nothing. M4 merged (main a9f6198); M9 ready to merge.

## Done 2026-11-16: MK2 input end to end (owner: "neither the sim nor the MK2 drives the engine")
- **Measured, not guessed.** On main (35e662e) with exactly ONE simulator, the whole chain works: scripted `SimMk2`, the live view over HTTP, and clicks in the browser page all reached the engine (every group, 12 quick presses, knob 1, wheel +6 = 0.200, PLAY TAKE), and the engine's Perform screens / LEDs showed on the page.
- **Cause reproduced:** a SECOND simulator started later (here a scripted one, on 2026-11-15 S1's forgotten soak `mk2_live.py --sim`) silently becomes the peer (`SimPeerPolicy`: the newest wins). The page in front of the owner is then ignored: its presses vanish (ack stayed at 12) and its panel shows its own screens, not the engine's. Exactly the report. When the other one stops, the page gets the link back after 1 s (the LEAD's worry that an idle sim "stays dropped" did not hold: the live view streams pads every 4 ms even when idle, and a returning old peer is treated as new).
- **Fix: ignored is never silent.** `SimLink::set_busy_notice(true)` (MK2 link only; the weapon links are unchanged) answers an ignored sender with report `0xFD` every 250 ms (`SimPeerPolicy::on_dropped`, rate-limited, first notice flagged). `mk2_sim.SimMk2.busy()`; the live view turns the pill red ("IGNORED: another simulator plays Himmel") and shows **Take over** (re-creates the simulator socket, so it is the newest). The controller logs "ignoring another MK2 simulator (udp port N)". Checked in the browser: banner, Take over, presses reach the engine again.
- **Diagnostics for the real MK2** (`mk2_controller`): log file `logs\mk2-YYYY-MM-DD.log` next to the exe (shared read, readable while running); first lines say `Mode: REAL MK2 over USB` or `Mode: SIMULATED ... the real MK2 is NOT used`; `MK2 opened over USB (17cc:1140, HID interface 2, release 1a)`; clearer "not found" / "plugged in but could not be opened (close NI ... MK2 Live)" reasons; an `input: ...` summary 1 s after connect and whenever controls are used (control / pad reports, presses, knob/wheel moves, pad hits, ring 0 attached or NOT, events, dropped); a warning if nobody reads ring 0 after 3 s.
- **Tests:** `mk2_chain_tests.exe` (new, ctest `mk2_chain_tests`): the real `Controller` in sim mode on a private port + private shared block, two UDP simulators, `Mk2Perform` on ring 0 with a stand-in model: 8 groups, 24 quick presses (none lost), wheel, knob 1, LEDs + strips back, hello; then a second simulator (wins, the first is told 0xFD, the log line, its presses ignored), then it closes and the first gets the link back. `mk2_tests` +1 policy block (notices). `python tools/mk2_e2e.py`: starts `himmel.exe --sim --no-ui --no-weapons` (or `--attach`), asserts every change through `himmel_cmd state`.
- **Seen on 2026-11-16:** the owner's real MK2 is plugged into the dev laptop. A 3 s run of `mk2_controller.exe` (no --sim) opened it (`release 1a`, 758 pad reports/s), so device access works; it blanked the MK2 screens / LEDs for that moment. Not touched again.

## Done 2026-11-15: MK2 sim link no longer flaps with two simulators
- **Cause:** `SimLink::recv` treated every datagram from a different UDP source port as "a new simulator", and `SimDevice::read` turned that into a lost device. With two simulators sending at once (a forgotten `mk2_live.py --sim` from a soak run plus a new one), the sender alternated on almost every datagram, so `mk2_controller --sim` reconnected many times per second ("MK2 lost (USB read/write error)") and input never settled. With one simulator it was stable.
- **Fix:** `SimPeerPolicy` (`src/controller/include/himmel/mk2/sim_link.h`, pure, tested in `mk2_tests`): one sender is the peer. A **brand-new** sender takes over once (the last started simulator wins), and the old one is then ignored (its datagrams are dropped). A peer that has been silent for 1 s (`kSimPeerIdleUs`) is replaced by whoever sends next. WSAECONNRESET and ignored datagrams never end a `recv` wait. A planned switch logs "another simulator took over". The weapon drivers' UDP links (`weapon/midi.cpp`) use the same `SimLink`, so they get the same behaviour.
- **Checked live:** `himmel.exe --sim --no-ui --no-weapons` with a forgotten `mk2_live.py --sim` (PID 7284) still sending to 8766, plus a scripted `mk2_sim.SimMk2`. Focus, crossfader and TAKE all changed in `himmel_cmd state`. The log shows one switch when ours started and one reconnect when it closed, no flapping. Script: the session scratchpad `sim_e2e.py` (not in git).
- `CLAUDE.md` line "A new simulator port = new session" is now: "the newest simulator wins; others are ignored while it is alive" (LEAD file, not edited here).

## Next up (ordered)
1. The owner plays M4 on the real MK2 under plain `himmel.exe` (every MK2 Live page closed) and settles D160–D162 (and the older D97–D99, now mostly replaced by D160–D161).
2. The owner plays the Block (MIX against the wheel, TOUCH, SCRUB) on the real Lightpad M under `himmel.exe --weapons lightpad` and settles D163. Then the M9 follow-ups listed under "Lightpad mapping (M9)".
3. Gaps left in M4 (each needs another track first): hold a LOOKS pad = morph over the hold time; SCENE kits (needs the kit in the show block, SHOW v2) and KitSet from the MK2; LAYER page PLACE / ZOOM / PAN (ENGINE M6 places); OUTPUTS on `NAVIGATE` (M6); MOODS (SHOW moods); ASSIST; real FREEZE (an engine freeze-frame instead of pausing); `DUPLICATE` (copy layer / scene to PREVIEW needs a command); `AUTO WRITE` (reactions, M8 / D131); `REC` + empty clip pad = SAMPLE; SLICE hold A + tap B = in / out; `VOLUME` / `SWING` + wheel; CONTROL / STEP / BROWSE / SAMPLING views.
- Older items kept: `roles` section in the profiles; Block SCRUB, LUMI SLICE KEYS; M13 OSC; M6 display drop-out with ENGINE.

## GUESSES
- `GUESS:` Lightpad MIX is absolute (left = deck A, right = deck B, like a DJ fader); pickup 0.03, takeover gap 0.03, state lag 120 ms; TOUCH zones = the first 5 non-generator catalog effects; Mode double-tap 400 ms (D163). Owner to play. Medium.
- `GUESS:` every tuning number in `mk2_perform.cpp` "tuning" (step sizes 100 / 150 raw units, speed 2× and fades 5 s per turn, BPM 40 per turn, SHIFT = 1/10, pad floor opacity 0.2, double-tap 350 ms, flash 1.5 s, PRESS deadband 64, SLICE loop above pressure 1200, stutter 1/4 beat, scratch 0.05 s per detent, TEMPO 0.5 BPM per detent). Owner to feel them. Confidence: medium.
- `GUESS:` velocity sets the opacity only on PREVIEW (a PROGRAM launch may wait for the grid, so its opacity is left alone). Medium.
- `GUESS:` FREEZE = pause the PROGRAM layers; STROBE = the effect named "Strobe" in master FX slot 4; FX PADS = catalog effect *n* in master FX slot 3; PANIC = black (D161). Medium-low: these wait for real engine features.
- `GUESS:` pad targets are kept by the mapper for the session, not in the scene kit (D162); SHOW's block has no kit yet. High that this is a stop-gap.
- `GUESS:` LOOKS fires on the hit (no hold-to-morph yet): lower latency, but the morph gesture is missing. The PLAY-focus scene uses its own transition (`f[0] = -1`); PREP and the `SHIFT` ways cut.
- `GUESS:` pad 1 (bottom-left) = first clip of the page (Maschine numbering). The GUI clip grid should match.
- The input → glass number covers UDP + engine + DWM, not USB and not the panel (see the measurement doc).

## Open questions for the owner
- PROPOSED D163 (Lightpad: MIX absolute A ↔ B with soft takeover, TOUCH zones = the first 5 effects in master FX slot 2, Mode tap = hold / double-tap = next mode).
- PROPOSED D160 (knob pages, knobs 5–8 always macros), D161 (stand-ins for FREEZE / STROBE / FX PADS / PANIC / NOTE REPEAT / GRID / dice buttons), D162 (session pad targets, REC / ERASE on release, LOOKS fires on the hit) in `docs/decisions.md`.

## How to build / run / test this track
VS 2022 Build Tools' bundled CMake: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, `cmake --build build --config Release --parallel`, then every `build\bin\Release\*_tests.exe` (incl. `perform_tests`, `mk2_chain_tests`). End to end: `python tools/mk2_e2e.py` (needs `media/test/*.mov`: `python tools/make_test_clips.py`, or copy them; starts and stops its own `himmel.exe --sim --no-ui --no-weapons -- --no-show-files`; `--basic`, `--no-latency`, `--runs N`, `--attach`). Takes about a minute; it pins the output window on top while measuring (no focus change) and writes `build/mk2_e2e_latency.json`.

## Gotchas found
- A simulator is ignored while a newer one runs: look for the red pill / "ignoring another MK2 simulator" in the mk2 log before debugging anything else.
- The supervisor's own log is opened without read sharing (LEAD noted it); read `logs\mk2-*.log` instead for MK2 input.
- Writing docs from a bash heredoc through Python: `\b` in a Windows path became a backspace. Check paths after such edits.
- Only one simulator per sim port is used (the newest). A forgotten live view does no harm now, but its own panel then shows its own screens, not the engine's. Close stray `mk2_live.py --sim` processes when testing.
- `himmel.exe --help` is not a help flag: it starts the real (hardware) stack.
- `Mk2Perform::sync()` runs at the start of `on_event` and `tick`, so events before the first tick are fine. Commands sent in a frame are not in the `LiveState` passed until the next frame; the mapper keeps its own focus so a group press is not undone by the stale state.
- `pages.h`'s `draw_perform` fills the whole screen with knob slots, so the slice screen is drawn by the mapper itself with the `pages.h` helpers (`text_in`, `text_anchored`, `kKnobColumn`); no controller change was needed.
- `himmel_cmd quit` is not a command (it prints the usage). `mk2_e2e.py` stops its stack with `taskkill /T /F` on the supervisor; a stack you start by hand must be stopped the same way, or the next e2e run plays against two stacks.
- Reading the composed desktop (GDI on the screen DC) waits for the next DWM frame: one poll = one 16.5 ms frame. Sample the whole output (StretchBlt to a few samples) and judge the mean: the test clips flash in the middle.
- The simulated Lightpad (`lightpad_sim.SimLightpad`) sends nothing until it is touched, and `weapon_blocks --sim` only finds a simulator that sends: `mk2_e2e.py` sends active sensing (0xFE) every 300 ms for it, like the NuVJ / APC20 sims. A live view's Lightpad does the same job when someone touches it.
- Several agents share this PC. A run that collides with another stack shows "MK2 lost (a different simulator is playing now)" in `logs\mk2-*.log`: check that first when an e2e run fails in odd places, and wait until `tasklist` has been free for a while (another agent may be about to start).
- Git Bash turns `-- /m` into a path for MSBuild ("Only one project can be specified"): use `cmake --build ... --parallel`.


## Interfaces touched
- 2026-11-16: sim link report `0xFD` (controller -> ignored MK2 simulator), `sim_link.h` only; no shared-memory or protocol change. `Controller` got private counters (header change, ABI-internal).
- `src/perform/include/himmel/perform/mk2_perform.h`: **private members only** (public API unchanged). No protocol change.
- 2026-11-17 (M4): `Mk2Perform::set_context(const ShowStatus*, const TransportSection*)` (new public inline method) + read-only accessors `page()`, `pad_mode()`, `pad_target()`, `flash()`; private members rewritten. ENGINE's `render_main.cpp` got one line (`perform->set_context(&show->status(), &model->transport())`). No change to `src/protocol/` or any shared block; only existing command types are used (none from CONTROL's range 80–89).
