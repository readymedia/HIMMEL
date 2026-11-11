# ENGINE handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap · Last commit: see git log

## State in one paragraph
Skeleton on `main`: supervisor (`himmel.exe`, restarts, job object, logs), `himmel_render.exe` (D3D11, 1–8 outputs, span/mirror, waitable flip-model, status block `Local\HimmelEngine`), ClipPlayer (HAP, 4 frames ahead), CameraCapture (Media Foundation, NV12, triple buffer), latency test. On branch `entrancer-touch-fx`: Touch FX slot (`touch_fx.h/.cpp`, `ps_touchfx`), MIDI input, Entrancer link. The layers are fixed (test card, clip, camera, Touch FX); there are no decks yet.

## Done (with commits)
See `CLAUDE.md` "Next steps" history and `docs/roadmap.md` "What we have".

## In progress
Nothing yet: this file was created with the v2 plan.

## Next up (ordered)
1. Phase 1: decks A/B, 4 layers each, crossfader, effect slots at clip/layer/deck/master/output, render graph built in a worker (`docs/design/engine.md` §2).
2. The command ring + status/preview blocks for the GUI (`docs/interfaces/command-protocol.md`, write it first with LEAD).
3. Curtain windows in the supervisor.
4. Off-air deck at thumbnail size; GPU meter per layer/effect.
5. Phase 1c: the playhead state machine in the ClipPlayer worker (play modes, in/out, inner loop, cue points, beat loop + catch-up, FREE/BPM/TIMECODE), trigger styles, HIT layer, warm cue frames, parameter scopes + MOVE modulators (`docs/design/clips-and-playback.md` §2–5, §10).
6. Phase 2: the live sampler (§6 there).
- M6: displays by identity, off-screen render while lost, 50/60 Hz, clips at their own fps (`docs/design/operations.md` §1–3).

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
