# AI-assisted development

**Himmel is developed with AI assistance, openly.** We think you should know
how the code you read here was made, so you can judge it.

## Who did what

| | Human (the owner, [ReadyMedia.no](https://readymedia.no)) | AI assistant |
|---|---|---|
| Goals, priorities, design | Sets them: show-safe first, low latency, easy to use, light on the GPU. Chooses the stack, licenses and big dependencies. | Proposes options with trade-offs, asks before anything hard to undo. |
| Hardware | Owns the Maschine MK2, the PC, the screens and cameras. Runs every hardware test, presses every button, watches every screen, reports what is wrong. | Writes the test tools (mapper, live view, measurement card), reads the results, fixes what the owner reports. |
| Code and docs | Reviews, accepts or rejects, decides when to commit. | Writes most of the code, tests and documentation. |

The assistant is **Claude** (Anthropic), used through **Claude Code** in the
Claude desktop app. Commits it helped write end with a
`Co-Authored-By: Claude …` line, so `git log` shows exactly which ones.

## How we keep it honest

- **Measure, don't guess.** Every hardware fact (byte positions, pad ids, LED
  order, screen format, timings, velocity curve) was checked on the owner's
  device before it went into `maschine_mk2_map.json` or the spec. Where we
  learned a fact from someone else's project, it is credited in
  [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and was then re-checked.
- **Tests without hardware.** `mk2_tests` and `media_tests` cover the parsing,
  the pad logic, the screen pages, the MOV reader and the HAP decoder,
  including broken and truncated input.
- **Bugs found on the device get a test.** Examples from the log: a false
  "device lost" caused by an unsigned underflow, a pad rebound that gave a
  second weak hit after release, an ignore rule that hid source folders.
- **Decisions are written down** in `CLAUDE.md` with a date (master encoder =
  browse wheel, own MOV reader + Snappy instead of linking FFmpeg, MIT license).
- **Licenses are checked, not assumed.** Before a dependency is added, its
  license is looked up and recorded.

## What that means for you

- Treat the code like any other open-source code: read it, test it, question
  it. It has been run on real hardware, but not audited.
- AI-written code can contain mistakes that look plausible. If something looks
  wrong, it may be wrong: please open an issue.
- AI-assisted contributions are welcome here too. Just say so in your pull
  request (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

## `CLAUDE.md`

`CLAUDE.md` in the repository root is the running hand-off between work
sessions: hardware facts, the agreed stack, what is done, what is next and the
working rules. It is written for the AI assistant, but it is also the best
single overview of the project's state for a human.
