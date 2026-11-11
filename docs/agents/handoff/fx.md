# FX handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
Touch FX gives 9 effects in one pass (`ps_touchfx`, branch `entrancer-touch-fx`) and a `touch_fx_check.exe` PNG/timing tool. There is no effect metadata format and no ISF loader yet.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. Write the effect metadata format (`docs/design/effects-and-generators.md` §1) as `docs/interfaces/effect-metadata.md`.
2. Port the Touch FX effects to stand-alone slot effects with metadata.
3. Tier 1 effects and generators, each with a PNG + timing check.

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
