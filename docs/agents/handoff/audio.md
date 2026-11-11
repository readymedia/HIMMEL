# AUDIO handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
Not started. The design is in `docs/design/music.md`. Touch FX uses a fixed 120 BPM until this track exists.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. `himmel_audio.exe`: WASAPI capture + loopback, bands, onsets, BPM/phase, the shared audio block → `docs/interfaces/audio-block.md`.
2. Clock arbitration + tap.
3. Link bridge (GPL, separate process), Pro DJ Link bridge.

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
