# Interfaces (contracts between tracks)

Everything that crosses a process or track boundary is written down here
**before** it is built. A change is first a PR to its document, agreed by the
LEAD; then both sides are built. See `docs/agents/README.md`.

| Interface | Between | State |
|---|---|---|
| `Local\HimmelMk2` (events, LEDs, screens, status) | CONTROL → ENGINE / GUI | Exists. See `docs/mk2-controller-spec.md` and `src/controller/include/himmel/mk2/shared.h` |
| MK2 sim link (UDP 8766) | Simulators → `mk2_controller` | Exists. See `sim_link.h` |
| `Local\HimmelEngine` (engine status) | ENGINE → supervisor / GUI | Exists (basic). Heartbeat / stop for the supervisor; the GUI uses `engine-state.md` |
| [`engine-state.md`](engine-state.md) (`Local\HimmelState`: live show model + catalog + transport) | ENGINE → GUI / weapons | **Version 2, 2026-11-16** (M2 transport section) |
| [`command-protocol.md`](command-protocol.md) (`Local\HimmelCommands`: layer, FX, crossfader, take, cut, focus) | GUI / weapons / OSC → ENGINE | **Version 1, 2026-11-15** (slice); **M2 clip transport appended 2026-11-16** (§4a); **M3 show commands 25–36 appended 2026-11-17** (§4b) |
| [`preview-block.md`](preview-block.md) (`Local\HimmelPreview`: 480×270 BGRA of A, B, MAIN, outputs) | ENGINE → GUI | **Version 1, 2026-11-15** (slice) |
| `himmel_perform` mapper API (`src/perform/include/himmel/perform/mk2_perform.h`) | CONTROL → ENGINE (in-process) | **Version 1, 2026-11-15**, `command-protocol.md` §6 |
| `himmel_ui` HTTP (127.0.0.1:8790) | bridge ↔ web app | Internal to the GUI track |
| `weapon-block.md` (`Local\HimmelWeapon<N>`, touch events, roles) | CONTROL → ENGINE | **Phase 1–2** (plan in `weapons/README.md`) |
| [`effect-metadata.md`](effect-metadata.md) (`fx/<id>/effect.json`, PRESS, scopes, reactions, flash cap, shader contract) | FX → ENGINE / SHOW / GUI | **Version 1, 2026-11-16** (M5) |
| `library-db.md` + sidecar JSON | MEDIA → SHOW / GUI | **Phase 3** |
| [`show-files.md`](show-files.md) (`himmel-show/1` project folder + `.himmelshow`, autosave journal, `Local\HimmelShow`: show status, files, text boxes) | SHOW ↔ GUI / weapons | **Version 1, 2026-11-17** (M3; moods and cues later); the block: [`show-block.md`](show-block.md) |
| [`outputs-block.md`](outputs-block.md) (`Local\HimmelOutputs`: output slots, displays by identity, canvas, layer places, curtains) | ENGINE → supervisor / GUI / weapons | **Version 1, 2026-11-17** (M6); commands 70–73 in `command-protocol.md` §4c |
| [`audio-block.md`](audio-block.md) (`Local\HimmelAudio`: signals, input, MIDI clock, config) | AUDIO → ENGINE / GUI | **Version 1, 2026-11-17** (M8) |
| `table-format.md` (`himmel-table/1`) | GUI (table) ↔ ENGINE | **Phase 10** (draft in `docs/vj-table-design.md`) |
