# How Himmel is built: agents, tracks and handoffs

Status: decided by the owner on 2026-11-11 (D41). Himmel is written mostly by
AI agents (Claude Code), led by the owner, who tests every step on real
hardware. This page is the working agreement, so that **any human or agent can
take over at any time**, even in the middle of a task.

## 1. Where to start (every session, every agent)

Read these in this order, and stop when you have enough for your task:

1. [`/AGENTS.md`](../../AGENTS.md): the 30-second version.
2. [`/CLAUDE.md`](../../CLAUDE.md): the hardware facts, verified decisions and
   gotchas.
3. [`../roadmap.md`](../roadmap.md): the phases, and which track owns what.
4. **Your track's handoff file:** `docs/agents/handoff/<track>.md`.
5. The interface docs your track touches (`docs/interfaces/`) and the design
   pages it builds (`docs/design/`).
6. [`../decisions.md`](../decisions.md) when anything looks like a choice.

## 2. Tracks (one owner agent each)

| Track | Owns (paths) | Builds | Must not change without an interface PR |
|---|---|---|---|
| **LEAD / integrator** | `CLAUDE.md`, `AGENTS.md`, `docs/roadmap.md`, `docs/decisions.md`, `docs/interfaces/*`, merges | The plan, merging, interface contracts, releases | — |
| **ENGINE** | `src/engine/`, `src/table/` (later), `tests/engine/` | Render graph, decks / layers, effect slots, outputs, curtains, previews, budget guard | The command protocol, the status and preview blocks |
| **FX** | `src/engine/fx/`, `fx/` (effect folders + metadata), `tools/fx_check` | Effects, generators, ISF loader, safety gate | The effect metadata format |
| **MEDIA** | `src/media/`, `src/ingest/`, `tests/media/` | HAP / MOV, ingest pipeline, analysis, library DB | The library DB schema, sidecar format |
| **SHOW** | `src/show/` (presets, shuffle, cues), `moods/` | The preset model, the shuffle sampler, moods, scenes / cues, Prepare Show | The show file formats |
| **CONTROL** | `src/controller/`, `src/weapons/`, `weapons/`, `tools/*_sim.py`, `tools/*_live.*`, maps | Weapon drivers (MK2, BLOCKS, MIDI), roles, simulators, screen pages, LED feedback | The weapon event / output blocks, the roles list |
| **AUDIO** | `src/audio/`, bridges | WASAPI / ASIO analysis, clock arbitration, Link / Pro DJ Link bridges | The audio block |
| **GUI** | `gui/` (web app), `src/ui_bridge/` | Wizard, weapon select, Perform, library, show, outputs, table view | — (it uses the protocols) |
| **QA** | `tests/soak/`, `tools/measure/`, `docs/measurements/` | Soak tests, latency rigs, test plans, release checks | — |

The tracks can run in parallel. The interface docs are what lets them:
everything that crosses a track boundary is written down in
`docs/interfaces/` **before** it is built.

## 3. Rules of work

1. **Small, short-lived branches:** `claude/<track>-<topic>`, merged into
   `main` within 1–2 days. Rebase on `main` daily.
   - Lesson from 2026-11-10: seven branches drifted apart and conflicted in
     `CLAUDE.md`, `weapons/README.md`, `THIRD_PARTY_NOTICES.md` and
     `launch.json`. Keep shared files short, and put per-track details in
     per-track files.
2. **Docs in the same commit as the code.** Behaviour changes update the
   matching `docs/` page and the track's handoff. A new dependency updates
   `THIRD_PARTY_NOTICES.md` + `licenses/`.
3. **Never guess silently.** If you have to assume something:
   - mark it **`GUESS:`** in the code comment and in the handoff, with how
     sure you are and how to check it;
   - if it is an owner choice (look, behaviour, licence, a big dependency),
     write a **PROPOSED** row in `docs/decisions.md` and ask.
4. **Hardware facts are marked** "checked on the owner's unit (date)" or
   "not checked yet". Never promote a guess to a fact without a test.
5. **The frame loop is sacred:** no allocation, locks, disk or network in
   it. Every PR that touches `src/engine/render/` says how this was kept.
6. **Measure, don't guess:** latency, GPU ms and memory go in
   `docs/measurements/` with date, machine and method.
7. **Tests:**
   - unit tests without hardware for the logic;
   - simulators for the weapons;
   - `--selftest` for the Python tools;
   - the soak test before releases.

   A PR is green only when the track's tests pass.
8. **English in the repo** (code, comments, docs, UI); **Norwegian** when
   talking to the owner.

## 4. Handoffs (the most important rule)

Every track keeps `docs/agents/handoff/<track>.md` up to date. Write it:
- at the end of every task;
- **before your context gets tight** (at about 70 % used, stop, write the
  handoff, commit, then go on or hand over);
- whenever you had to guess or summarise something;
- before you ask the owner something, so the question is not lost.

Template (`handoff/_template.md`):

```markdown
# <TRACK> handoff
Updated: <date> by <agent/session> · Branch: <branch> · Last commit: <sha>

## State in one paragraph
## Done (with commits)
## In progress (exact next step, files, line refs)
## Next up (ordered)
## GUESSES (each: what, why, confidence, how to verify)
## Open questions for the owner (link PROPOSED rows in docs/decisions.md)
## How to build / run / test this track
## Gotchas found
## Interfaces touched (and whether docs/interfaces/ is updated)
```

A summary must say what it **left out**. "Summarised from a long session;
details of X are in commit abc123" is fine; losing information silently is
not.

## 5. Spawning agents (for the LEAD)

The lead (a Claude Code session with the owner) splits work into tasks that
each **fit in one context**, and spawns one agent per task with this prompt
skeleton:

```
You are the <TRACK> agent for Himmel (open source VJ system, Windows, C++20/D3D11).
Read, in order: AGENTS.md, CLAUDE.md, docs/roadmap.md (phase <n>), docs/agents/README.md,
docs/agents/handoff/<track>.md, docs/interfaces/<…>.md, docs/design/<…>.md.
Task: <one sentence>. Done when: <testable criteria>.
You own: <paths>. Do not edit other tracks' paths; propose interface changes as a PR to docs/interfaces/.
Rules: no allocation/locks/IO in the frame loop; mark guesses GUESS:; owner choices → PROPOSED in docs/decisions.md.
Before you stop (or at ~70% context): update docs/agents/handoff/<track>.md, commit, push branch claude/<track>-<topic>.
Report: what changed, tests run, guesses made, questions for the owner.
```

The lead reads each report, merges, updates `roadmap.md` (strikes done items)
and decides what runs next. Tasks that cross tracks are split along the
interface: first the interface doc PR, then the two sides in parallel.

## 6. Documentation map

| Kind | Where |
|---|---|
| Why and what (product) | `docs/design/*.md` |
| What is decided | `docs/decisions.md` |
| What is planned | `docs/roadmap.md` |
| Contracts between processes and files | `docs/interfaces/*.md` |
| How it is built today | `docs/architecture.md` |
| Hardware facts | `CLAUDE.md`, `docs/mk2-controller-spec.md`, `weapons/README.md` |
| Measurements | `docs/measurements/` |
| Where each track stands | `docs/agents/handoff/*.md` |
| For users | `README.md`, `docs/getting-started.md`, the in-app Help |
