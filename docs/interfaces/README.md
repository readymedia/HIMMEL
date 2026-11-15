# Interfaces (contracts between tracks)

Everything that crosses a process or track boundary is written down here
**before** it is built. A change is first a PR to its document, agreed by the
LEAD; then both sides are built. See `docs/agents/README.md`.

| Interface | Between | State |
|---|---|---|
| `Local\HimmelMk2` (events, LEDs, screens, status) | CONTROL → ENGINE / GUI | Exists. See `docs/mk2-controller-spec.md` and `src/controller/include/himmel/mk2/shared.h` |
| MK2 sim link (UDP 8766) | Simulators → `mk2_controller` | Exists. See `sim_link.h` |
| `Local\HimmelEngine` (engine status) | ENGINE → supervisor / GUI | Exists (basic). Heartbeat / stop for the supervisor; the GUI uses `engine-state.md` |
| [`engine-state.md`](engine-state.md) (`Local\HimmelState`: live show model + catalog) | ENGINE → GUI / weapons | **Version 1, 2026-11-15** (slice) |
| [`command-protocol.md`](command-protocol.md) (`Local\HimmelCommands`: layer, FX, crossfader, take, cut, focus) | GUI / weapons / OSC → ENGINE | **Version 1, 2026-11-15** (slice); scenes / save / transport appended later |
| [`preview-block.md`](preview-block.md) (`Local\HimmelPreview`: 480×270 BGRA of A, B, MAIN, outputs) | ENGINE → GUI | **Version 1, 2026-11-15** (slice) |
| `himmel_perform` mapper API (`src/perform/include/himmel/perform/mk2_perform.h`) | CONTROL → ENGINE (in-process) | **Version 1, 2026-11-15**, `command-protocol.md` §6 |
| `himmel_ui` HTTP (127.0.0.1:8790) | bridge ↔ web app | Internal to the GUI track |
| `weapon-block.md` (`Local\HimmelWeapon<N>`, touch events, roles) | CONTROL → ENGINE | **Phase 1–2** (plan in `weapons/README.md`) |
| `effect-metadata.md` | FX → SHOW / GUI | **Phase 4** (fields in `docs/design/effects-and-generators.md` §1) |
| `library-db.md` + sidecar JSON | MEDIA → SHOW / GUI | **Phase 3** |
| `show-files.md` (`himmel-show/1`: banks, scenes, cues, songs, moods) | SHOW ↔ GUI | **Phase 5** |
| `audio-block.md` (signals, clock) | AUDIO → ENGINE / SHOW | **Phase 7** |
| `table-format.md` (`himmel-table/1`) | GUI (table) ↔ ENGINE | **Phase 10** (draft in `docs/vj-table-design.md`) |
