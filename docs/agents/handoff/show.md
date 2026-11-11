# SHOW handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
Not started. The design is in `docs/design/presets-and-shuffle.md`. The NuVJ classic view's Python model (`tools/vj_classic.py`, NuVJ branch) is a useful prototype of decks/banks/cells.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. Show file formats (`himmel-show/1`) → `docs/interfaces/show-files.md`.
2. The scene / bank / cue model + save/recall over the command protocol.
3. The shuffle sampler v1 (seeded, locks, scopes, history), mood files (16).
- Clip variants A–D; parameter scopes (LOOK values stored per look; GLOBAL values in the show); the MOTION lock holds the transport (`docs/design/clips-and-playback.md` §4).
- M3 includes **projects** (`docs/design/projects.md`): folder format, new / open / recent / save / save as / `.himmelshow`, atomic save worker, autosave journal + restore, relink missing clips.

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
