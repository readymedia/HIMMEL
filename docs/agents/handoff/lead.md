# LEAD handoff
Updated: 2026-11-11 by the planning session (claude.ai) · Branch: claude/roadmap

## State in one paragraph
The v2 plan and design are written: `docs/roadmap.md`, `docs/design/*`, `docs/decisions.md`, `docs/agents/*`, `docs/interfaces/README.md`, `AGENTS.md`, and the GUI v2 mockups in `docs/gui-mockups/v2/`. Seven feature branches are still unmerged (see roadmap phase 0).

## Next up (ordered)
1. Get the owner's answers on D42–D47 (`docs/decisions.md`).
2. Phase 0: merge the branches in the order in `docs/roadmap.md`, then delete them.
3. Write `docs/interfaces/command-protocol.md` and `preview-block.md` with ENGINE (phase 1).
4. Spawn the track agents for phase 1 (ENGINE, CONTROL), phase 3 (MEDIA) and phase 5 (SHOW) using the template in `docs/agents/README.md` §5.
- **0.1 "Playback" is scoped in `docs/build-v1.md` (M0–M12).** Every track takes its milestone from there; `docs/design/perform.md` (v4) decides behaviour where docs disagree.

## GUESSES
- GUESS: the timeline weeks in `docs/roadmap.md` §3 are estimates (medium confidence); re-plan after phase 1.
- GUESS: the NuVJ mapping in `design/hardware-mapping.md` §4 is not checked on the unit.

## Open questions for the owner
- 2026-11-11: D42–D76 all decided (web GUI, merge now, 4 layers, 0.1 scope, MK2/NuVJ perform flow; D65 = no flash cap by default). Next for LEAD: **M0 merge**.
D42 mapping changes · D43 GUI tech · D44 Lightpad modes · D45 16 moods · D46 layers per deck · D47 merge now.
