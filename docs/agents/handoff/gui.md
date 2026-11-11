# GUI handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
There is no operator GUI yet. The browser live view (`tools/mk2_live.py` + `.html`, plus the per-weapon pages on the branches) already draws every weapon from its profile, and the VJ table mockup exists (`docs/vj-table-mockup.html`, rack-design branch). The v2 design: `docs/design/gui.md` + the mockups in `docs/gui-mockups/v2/`.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. Wait for D43 (web GUI vs ImGui).
2. The `himmel_ui` bridge: serves the web app, relays commands/state, previews from shared memory.
3. Wizard (4 steps) + weapon select (reuse the live-view weapon renderer).
4. Perform v1: monitors with tallies, crossfader/TAKE, layer strips, pads, the knob strip (screen mirror), the shuffle panel, outputs band.

## GUESSES
- None recorded yet.

## Open questions for the owner
- See the PROPOSED rows in `docs/decisions.md`.

## How to build / run / test this track
See `docs/getting-started.md` and `CLAUDE.md` (build: VS 2022 Build Tools, CMake).

## Gotchas found
See `CLAUDE.md`.

## Interfaces touched
None yet.
