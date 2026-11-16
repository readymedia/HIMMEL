# CONTROL handoff
Updated: 2026-11-16 by the CONTROL session (MK2 input end to end) · Branch: claude/control-mk2-input · Last commit: see git log

## State in one paragraph
MK2: verified map, C++ controller (`src/controller`, shared memory `Local\HimmelMk2`, LEDs, screens, reconnect, `--sim` over UDP 8766), screen pages (`pages.cpp`), Python live view + simulator + games. Generic weapon block + drivers for Lightpad / LUMI / NuVJ / APC20 (`weapons/README.md`). **New (S2): `himmel::perform::Mk2Perform` plays the slice** (`src/perform/`): focus by groups, wheel = TAKE T-bar, knobs, pads = clips, FX buttons, LEDs and both screens; 120 unit checks in `perform_tests.exe`. **Checked end-to-end 2026-11-15** (main fa84ed0 + the sim link fix): a simulated MK2 through `himmel.exe --sim` sets the focus (Group C → B3), moves the crossfader (wheel +6 → 0.200), runs a TAKE (PLAY) that flips PROGRAM, after which the focus follows its role (B3 → A3), and the engine's screens / LEDs reach the simulator.

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
- Nothing. Last task: MK2 input end to end (below). Waiting for the owner's real-MK2 run.

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
1. The owner runs the real MK2 under plain `himmel.exe` (steps in the 2026-11-16 report to LEAD) and sends `build\bin\Release\logs\mk2-<date>.log`. If presses are counted there but the engine does not move, the break is in `himmel_render` (ENGINE's file) after ring 0.
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
VS 2022 Build Tools' bundled CMake: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, `cmake --build build --config Release`, then `ctest --test-dir build -C Release` (9 suites incl. `mk2_chain_tests`) or `build\bin\Release\perform_tests.exe`.

## Gotchas found
- A simulator is ignored while a newer one runs: look for the red pill / "ignoring another MK2 simulator" in the mk2 log before debugging anything else.
- The supervisor's own log is opened without read sharing (LEAD noted it); read `logs\mk2-*.log` instead for MK2 input.
- Writing docs from a bash heredoc through Python: `\b` in a Windows path became a backspace. Check paths after such edits.
- Only one simulator per sim port is used (the newest). A forgotten live view does no harm now, but its own panel then shows its own screens, not the engine's. Close stray `mk2_live.py --sim` processes when testing.
- `himmel.exe --help` is not a help flag: it starts the real (hardware) stack.
- `Mk2Perform::sync()` runs at the start of `on_event` and `tick`, so events before the first tick are fine. Commands sent in a frame are not in the `LiveState` passed until the next frame; the mapper keeps its own focus so a group press is not undone by the stale state.
- `pages.h`'s `draw_perform` fills the whole screen with knob slots, so the slice screen is drawn by the mapper itself with the `pages.h` helpers (`text_in`, `text_anchored`, `kKnobColumn`); no controller change was needed.

## Interfaces touched
- 2026-11-16: sim link report `0xFD` (controller -> ignored MK2 simulator), `sim_link.h` only; no shared-memory or protocol change. `Controller` got private counters (header change, ABI-internal).
- `src/perform/include/himmel/perform/mk2_perform.h`: **private members only** (public API unchanged). No protocol change.
