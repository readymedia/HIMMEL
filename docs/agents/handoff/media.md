# MEDIA handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
`src/media`: our own MOV reader + HAP decoder (Hap, Hap Alpha, Hap Q, chunked; not Hap Q Alpha), `hap_probe.exe`, `media_tests.exe`, `tools/make_test_clips.py`. There is no ingest pipeline or library DB yet.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. The ingest worker (`himmel_ingest.exe`): probe → ffmpeg HAP → sidecar → SQLite (`docs/design/library-and-analysis.md`).
2. Analysis v1: palette, brightness/contrast, motion (low-res flow), cuts, loop quality, flicker, energy, mood scores by rules.
3. Background throttling tied to the engine status block.
4. Hap Q Alpha.

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
