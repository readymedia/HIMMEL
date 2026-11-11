# Interfaces (contracts between tracks)

Everything that crosses a process or track boundary is written down here
**before** it is built. A change is first a PR to its document, agreed by the
LEAD; then both sides are built. See `docs/agents/README.md`.

| Interface | Between | State |
|---|---|---|
| `Local\HimmelMk2` (events, LEDs, screens, status) | CONTROL → ENGINE / GUI | Exists. See `docs/mk2-controller-spec.md` and `src/controller/include/himmel/mk2/shared.h` |
| MK2 sim link (UDP 8766) | Simulators → `mk2_controller` | Exists. See `sim_link.h` |
| `Local\HimmelEngine` (engine status) | ENGINE → supervisor / GUI | Exists (basic). To be documented as `engine-status.md` |
| `command-protocol.md` (command ring: load, set, take, roll, save, …) | GUI / weapons → ENGINE / SHOW | **To write in phase 1** |
| `preview-block.md` (480×270 thumbnails of A, B, MAIN and the outputs) | ENGINE → GUI | **To write in phase 1** |
| `weapon-block.md` (`Local\HimmelWeapon<N>`, touch events, roles) | CONTROL → ENGINE | **Phase 1–2** (plan in `weapons/README.md`) |
| `effect-metadata.md` | FX → SHOW / GUI | **Phase 4** (fields in `docs/design/effects-and-generators.md` §1) |
| `library-db.md` + sidecar JSON | MEDIA → SHOW / GUI | **Phase 3** |
| `show-files.md` (`himmel-show/1`: banks, scenes, cues, songs, moods) | SHOW ↔ GUI | **Phase 5** |
| `audio-block.md` (signals, clock) | AUDIO → ENGINE / SHOW | **Phase 7** |
| `table-format.md` (`himmel-table/1`) | GUI (table) ↔ ENGINE | **Phase 10** (draft in `docs/vj-table-design.md`) |
