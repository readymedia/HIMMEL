# CONTROL handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
MK2: verified map, C++ controller (`src/controller`, shared memory `Local\HimmelMk2`, LEDs, screens, reconnect, `--sim` over UDP 8766), screen pages (`pages.cpp`), Python live view + simulator + games. Lightpad: Python sim, WinMM driver, our own LittleFoot LED renderer, games (branch). LUMI, Entrancer, NuVJ, KF4, V-4 and Ohm64: profiles + simulators on branches (see `docs/roadmap.md`).

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. Phase 0: help LEAD merge the weapon branches (the conflicts are only in docs/config).
2. The generic weapon block `Local\HimmelWeapon<N>` + touch events (plan in `weapons/README.md`).
3. `weapon_blocks.exe` (Lightpad in C++), with modes TOUCH/MIX/SCENES/MOOD.
4. The `roles` section in the profiles; the Perform mapping of `docs/design/hardware-mapping.md` in the engine's input layer.

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
