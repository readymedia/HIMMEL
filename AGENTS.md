# AGENTS.md: start here (humans welcome too)

Himmel is an open-source VJ / installation system for Windows (C++20,
D3D11), played from old controllers called "weapons". The main instrument is
the Maschine MK2 + the ROLI Lightpad Block. The owner speaks Norwegian;
everything in the repo is English.

**Read in this order:**
1. `CLAUDE.md`: hardware facts, verified decisions, gotchas.
2. `docs/roadmap.md`: the plan, the phases, and who owns what.
3. `docs/agents/README.md`: how we work (tracks, branches, handoffs, guesses).
4. `docs/agents/handoff/<your track>.md`: where your track stands.
5. `docs/design/` (what to build) and `docs/interfaces/` (the contracts).
6. `docs/decisions.md`: before making any choice.

**Five rules:**
1. Never allocate, lock or do I/O in the frame loop.
2. Never guess silently: write `GUESS:` in the code and the handoff; owner
   choices go as PROPOSED in `docs/decisions.md`.
3. Small branches `claude/<track>-<topic>`, merged within 1–2 days; docs in
   the same commit.
4. Update your handoff before your context runs low (about 70 %) and at the
   end of every task.
5. Measure on real hardware (or the simulators) before calling something
   done.
