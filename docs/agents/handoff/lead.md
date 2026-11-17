# LEAD handoff
Updated: 2026-11-16 by the LEAD session (Claude Code, main checkout) · Branch: main

## State in one paragraph
M0 is done (merged, built, tested, pushed; remote branches deleted). The owner's
answers of 2026-11-15 are D89–D92 (`docs/decisions.md`): the LEAD merges and
pushes on its own when build + tests + self-tests are green; a playable slice
comes first (D90, `build-v1.md` §3a, `roadmap.md` §0a); 2–3 track agents at a
time; dev laptop until M12. The slice contracts are written and committed:
`docs/interfaces/command-protocol.md`, `engine-state.md`, `preview-block.md`,
headers in `src/protocol/` (`himmel_protocol`, `protocol_tests`), and the MK2
mapper contract `src/perform/` (`himmel_perform`, a LEAD stub CONTROL replaces).
Three agents run the slice: S1 ENGINE, S2 CONTROL, S3 GUI.

## Done (with commits)
- a36f078 M0: merged rack-design, entrancer-touch-fx, weapon-6-kf4,
  weapon-roland-v4, weapon-ohm64; one weapon list + port table in
  `weapons/README.md`; CLAUDE.md slimmed (per-weapon notes moved verbatim to
  `weapons/README.md` "Notes per weapon"); D89–D93.
- Next commit: slice contracts (protocol headers + docs + perform stub).
- Tests green at M0: mk2_tests 4286, media_tests 39, weapon_tests 163,
  touch_fx_tests 77, entrancer_tests 238 checks; Python `--selftest` of
  apc20, entrancer, kf4, lightpad, lightpad_games, lightpad_littlefoot, lumi,
  maschine_test, nuvj, nuvj_games, ohm64, v4; `mk2_fun_check`.

## In progress
- The slice is merged into main (2026-11-15): S1 ENGINE b2003f6, S2 CONTROL
  681900d, S3 GUI 1ea2217. Build green; 9 test programs pass (deck 143,
  entrancer 238, media 39, mk2 4286, perform 120, protocol 1111, touch_fx 77,
  ui_bridge 122, weapon 163). Checked together: `himmel.exe --sim` starts
  `himmel_ui`; `himmel_cmd` loads clips / FX; the web page on 8790 shows
  previews and TAKE flips PROGRAM; the MK2 sim shows the engine's Perform
  screens. MK2 sim input: fixed by CONTROL (bba57af, merged 2026-11-15:
  newest simulator wins, resets ignored, no flapping; mk2_tests 4306). CONTROL
  checked groups / wheel / TAKE reaching the engine; the LEAD has not re-run it.
- D94–D100: PROPOSED, defaults in use (LEAD 2026-11-16); the owner can still change them.
- Soak: 6 late frames in 30 min windowed on the laptop (an idle run had 7 in
  10 min); a fullscreen soak on a quiet machine is still owed (S1 handoff).

## Autonomous waves (owner 2026-11-16, `roadmap.md` §0b)
The owner's session (local_84946746…) starts each wave's sessions; the LEAD
merges each branch when green and writes the next wave here. **Standing
approval:** the owner told the LEAD session on 2026-11-16 "Yes, merge when
green" (build + every test + every `--selftest`), for every wave branch.

### Wave 1 — **done 2026-11-16** (all three merged; main builds, 12 test programs + every self-test + `mk2_e2e.py` green)
| Track · branch | Task | Merged |
|---|---|---|
| CONTROL · `claude/control-mk2-input` | MK2 sim + real MK2 drive the engine; e2e check; owner steps for the real MK2 | **merged 2026-11-16** (d43ac94): one sim works end to end (`tools/mk2_e2e.py` PASS, `mk2_chain_tests` 18); an ignored second sim now says so (red pill + Take over); `logs/mk2-<date>.log`. Real MK2 not yet checked by the owner |
| ENGINE · `claude/engine-m2-transport` | M2 clip transport (`design/clips-and-playback.md` §2–3, §10); supervisor log read sharing | **merged 2026-11-16** (a355739): transport_tests 1440; state block **v2** (transport section); commands 18–23 appended; D101–D104 PROPOSED with defaults |
| FX · `claude/fx-m5-effects` | M5: metadata + default reaction, 16 tier-1 effects, 6 generators, PRESS / scopes, optional cap | **merged 2026-11-16** (a4de4a7): 19 effects + 6 generators in `fx/`, `effect-metadata.md`, fx_tests 76051, max 0.40 ms GPU (blur) on the GTX 1070; `FxPress` = command 24; D105 TECH, D106–D108 PROPOSED with defaults |

Merge order: CONTROL first (unblocks playing), then ENGINE, then FX (FX
likely touches the uber-shader / effect catalog that ENGINE also touches:
expect conflicts in `src/engine/`; resolve keeping both, rebuild, retest).

### Wave 2 — **done 2026-11-17** (all three merged)

| Track | Branch | State |
|---|---|---|
| SHOW M3 | `claude/show-m3-projects` | **merged** 2026-11-17 (99d78b5 + 106ffb9 → main f3058d9). Command types 25–36, slot 4 = Show (rate limited, 384 per frame cap), `Local\HimmelShow` (show-block.md), D109–D110 PROPOSED |
| AUDIO M8 | `claude/audio-m8-reactions` | **merged** 2026-11-17 (cab5ea1 + 73d0ddc). Own block `Local\HimmelAudio` v1 (audio-block.md), `himmel_audio.exe` supervisor child, `ClockState::source` 2 = MIDI leads, D130–D132 PROPOSED (D131 FxReact = command 40, not added yet) |
| GUI M11 | `claude/gui-m11-v1` | **merged** 2026-11-17 (efcb916 → 8c87280). Clip editor / transport, cues, beat loop, clock, effect browser, presets, PRESS, generators, HIT; checked live in `himmel.exe --sim` incl. killing `himmel_ui` mid-show. No protocol changes. SHOW / AUDIO panels not built yet (wave 4) |

Merged main: build clean, 14 test programs, `himmel_audio --selftest`, 12 Python selftests, `mk2_e2e.py` PASS.
Not verified live: a scene recall and an autosave restore in a running `himmel.exe`, and audio reactions under `himmel.exe`. Gotcha (GUI, 2026-11-17): test runs wrote the owner's autosave. `mk2_e2e.py` now passes `-- --no-show-files`; every test stack must do the same.

**Wave 2 number ranges (LEAD, 2026-11-17).** These stop the parallel branches colliding:

| Track | CommandType | Producer slot | Decisions |
|---|---|---|---|
| SHOW | 25–39 | 4 = Show | D109–D119 |
| AUDIO | 40–49 | 5 = Audio, if needed | D130–D139 |
| GUI | 50–59 | — | D120–D129 |

- SHOW adds `Local\HimmelShow`, specified in `docs/interfaces/show-block.md`.
- AUDIO either bumps the state block to v3 or uses its own block.
- SHOW and AUDIO both hook the render_main drain loop and frame tick, so the LEAD merges those by hand.
Rules for every wave-2 agent (add to each brief): **one Himmel stack per
machine** — before starting `himmel.exe` / `himmel_render.exe` / a test
renderer, check `tasklist`; never leave one running when you stop (two
renderers share `Local\Himmel*` and corrupt each other's state / input).
Read `docs/interfaces/engine-state.md` v2 (transport) and
`effect-metadata.md`; new commands are appended after 24 (`FxPress`).
1. **SHOW · `claude/show-m3-projects`.** M3 (`build-v1.md`): projects
   (`design/projects.md`: folder format, new / open / recent / save / save as,
   `.himmelshow` export, atomic save worker, autosave journal + restore,
   relink), show files (scenes, variations, kits with pad targets, macros,
   songs, setlist), save / recall / morph, undo stack 64 + reset-to-scene,
   dice / mutate / rotate v1 with locks. First write
   `docs/interfaces/show-files.md` and append the needed commands (Save,
   Recall, Undo, …) to `command-protocol.md` as a PR the LEAD approves.
   *Done when:* round-trip tests; a 4-layer scene recall hitches < 1 frame;
   kill the engine mid-save → autosave restores. *Owns:* `src/show/`,
   `tests/show/`, `docs/interfaces/show-files.md`, `docs/agents/handoff/show.md`.
2. **GUI · `claude/gui-m11-v1`.** M11 on what wave 1 delivered: clip editor
   drawer (transport modes, in / out, cue points), effect browser with the FX
   metadata, generators as sources, the MK2 picture with live feedback
   (`design/gui.md` §4, §16, §17, mockups `gui-mockups/v4/`), settings view
   stub, start wizard steps 1–4 against what exists; PRESS (`FxPress`),
   effect presets and generator kinds from the catalog; the transport section
   (state v2: modes, cues, beat loop, HIT layer). *Done when:* every new
   engine feature of wave 1 is reachable from the browser; killing / restarting
   `himmel_ui` mid-show changes nothing. *Owns:* `gui/`, `src/ui_bridge/`,
   `tests/ui_bridge/`, `docs/agents/handoff/gui.md`.
3. **AUDIO · `claude/audio-m8-reactions`.** M8 (`design/music.md` §3–5):
   `himmel_audio.exe` (WASAPI mic / line in / loopback, auto gain, gate;
   level, 3 bands, spectrum, kick / snare / hat onsets, energy; tap tempo, MIDI
   clock in, internal clock, beat / bar phase), `docs/interfaces/audio-block.md`
   first, reactions on every effect parameter in the engine (using FX's
   default `reaction` metadata), supervisor starts it. *Done when:* mic and a
   line in both drive zoom pulse on the kick on time; unplug / replug the input
   during a soak. *Owns:* `src/audio/`, `tests/audio/`,
   `docs/interfaces/audio-block.md`, `docs/agents/handoff/audio.md`; engine-side
   reaction code by agreement with ENGINE (small, in `src/engine/src/reactions*`).

### Wave 3 — **done 2026-11-17** (M4, M6, M7 merged; CONTROL continues with M9)

| Track | Branch | State |
|---|---|---|
| CONTROL M4 → M9 | `claude/control-m4-perform` → `claude/control-m9-lightpad` | **merged** 2026-11-17 (b354cae → d93acb1). A whole song from the MK2 alone in `mk2_e2e.py` (song stage); input → glass median 32 ms, p90 38 ms (`docs/measurements/2026-11-17-control-m4-input-to-glass.md`). `Mk2Perform::set_context` hook in render_main. D160–D162 PROPOSED. **M9 Lightpad merged** 2026-11-17 (450a1dc → fadee8b): TOUCH, MIX with soft takeover against the wheel, SCRUB, LED feedback; `mk2_e2e.py` starts `--weapons lightpad` and has a Lightpad stage (61 checks); D163 PROPOSED. **M13 OSC merged** 2026-11-17 (b246176 → 325e509): `himmel_osc.exe` on UDP 7000 (loopback by default, `--allow` for the LAN), producer slot 2, strict parser (200 000-packet fuzz, 0 garbage delivered), `Local\HimmelOsc` with the last 32 messages, `docs/interfaces/osc.md`; `mk2_e2e.py` OSC stage (74 checks); D164 PROPOSED. **MK2 BROWSE + M10 NuVJ merged** 2026-11-17 (357ca4f → 8a18850): library list on the MK2 screens, load to the focused layer, catalog-full notice; NuVJ crossfader = PROGRAM with soft takeover, wheels BEND / SCRATCH / FX, trims, INSTA, master FX; e2e NuVJ stage (93 checks, never SysEx 7B); D165 PROPOSED. Owner's real-NuVJ check: `docs/agents/handoff/control.md` **QA M12 rig merged** 2026-11-17 (dd935b2): `tools/qa_rig.py` (kill each process, unplug sim weapons, missing file, soak with CSV), PASS on the dev laptop (renderer back in 1.4 s, others 0.7 s), 20 min soak 0 late frames; owner's 8 h soak + show-PC recipe in `docs/qa/README.md`. GPU device loss not forceable without admin `dxcap -forcetdr` |
| MEDIA M7 | `claude/media-m7-ingest` | **merged** 2026-11-17 (d3f9ad9 → 01bd059, hand merge with M6). 100 mixed files imported during a 3-layer show, 0 late frames (`docs/measurements/2026-11-17-ingest-during-show.md`). SQLite, `himmel_ingest.exe` supervisor child (off with `--no-show-files`), catalog append-only and grows live, D140–D147 PROPOSED. Follow-ups: MK2 BROWSE, ingest commands 60–69, SHOW worker keeps its start-up catalog copy, paged catalog |
| ENGINE M6 | `claude/engine-m6-outputs` | **merged** 2026-11-17 (3e8d15b → dcc01f9). Outputs by identity, IDENTIFY, presets, 60/50/59.94 Hz, blend, places, black per output, **curtains** (kill → black at 0 ms, back in 1.0–1.6 s), commands 70–73, `Local\HimmelOutputs`, D150–D154 PROPOSED. MAIN preview aspect = GUI CSS, fixed (`object-fit: contain`). **Spout merged** 2026-11-17 (0adfb08 → 699b2f4): `--spout NAME`, off by default, SpoutDX 2.007h at commit 7d32b05 (SHA256), own worker + device, 0 late frames with a receiver; a held IDENTIFY is let go when its producer goes quiet. A real cable pull with 2+ displays needs the owner's hardware |


**LEAD call:** CONTROL takes **M4** (MK2 Perform v4) before M9. Wave 2 delivered SHOW, so the MK2 can now play a whole song: scenes, REC, ERASE and ENTER. That is the shortest path to "a working version". M9 Lightpad and M13 OSC come next.

| Track | CommandType | Producer slot | Decisions |
|---|---|---|---|
| MEDIA | 60–69 | — | D140–D149 |
| ENGINE | 70–79 | — | D150–D159 |
| CONTROL | 80–89 | — | D160–D169 |

**Rules for every wave-3 agent:**
- **One Himmel stack per machine.** Check `tasklist` first, and stop your stack when you are done.
- Test runs use `-- --no-show-files` or `--show-root <temp>`, so they never write the owner's autosave.
- "Done" = build, every `*_tests.exe`, `himmel_audio --selftest`, the 12 Python selftests and `mk2_e2e.py` all green, then report "ready to merge" with the commit.
- Do not push again after the report unless the LEAD asks.

1. **MEDIA · `claude/media-m7-ingest`.** M7 in `build-v1.md` and `design/library-and-analysis.md`:
   - a watch folder, and ffmpeg.exe → HAP (×4, chunks) as an external tool, never linked;
   - stills, image sequences, GIF and SVG (`design/operations.md` §4);
   - fps stored, thumbnails, analysis v1 (palette, brightness, motion, cuts, loop points, slices);
   - SQLite + sidecar JSON, with background throttling;
   - the catalog in the engine is refreshed without a hitch.

   Start by writing `docs/interfaces/library-db.md`. Ask the LEAD before adding SQLite (license: public domain; it goes in THIRD_PARTY_NOTICES).
   - *Done when:* 100 mixed clips import during a running show without a late frame (measured).
   - *Owns:* `src/media/ingest*`, `src/library/`, `tests/library/`, `docs/interfaces/library-db.md`, `docs/agents/handoff/media.md`.
2. **ENGINE · `claude/engine-m6-outputs`.** M6 (`design/operations.md` §1–3, `design/projects.md` §5, `design/engine.md` §2.0, §4):
   - displays that drop out and come back by identity, IDENTIFY, 60 / 50 Hz, clips at their own fps;
   - 1–3 outputs as one canvas, resolution presets 720p / 1080p / 1440p / 4K, simple blend;
   - layer places (FILL ALL / OUT n / FIT / FREE), black per output;
   - **curtains in the supervisor** (a renderer restart shows black, not the desktop);
   - Spout out;
   - **fix: the MAIN preview is 480×90 for a 960×540 output.** Preview aspect, reported by FX on 2026-11-16.

   Ask the LEAD before adding the Spout SDK (BSD-2).
   - *Done when:* killing the renderer shows black on every output and they come back in < 2 s, and pulling an output cable mid-show never stutters the others.
   - *Owns:* `src/engine/` (outputs, supervisor), `docs/agents/handoff/engine.md`.
3. **CONTROL · `claude/control-m4-perform`.** M4 (`design/perform.md`, `design/hardware-mapping.md` §3):
   - MK2 Perform v4: focus, wheel TAKE, Top buttons, and the pages PLAY / CLIP / LAYER / FX / MIX;
   - pads CLIPS / SLICE / LOOKS (SHOW scenes, command types 25–36) / FX PADS;
   - PRESS, targets, HIT;
   - `REC` = save, `ERASE` = undo / reset, `ENTER` = next scene, `SHIFT` + `SCENE` = moods stub;
   - the screens.

   Then M9 Lightpad (TOUCH / MIX / SCRUB with soft takeover) if time allows.
   - *Done when:* a song is played from the MK2 alone in `himmel.exe --sim` (scripted in `mk2_e2e.py` or a sibling), and the input → glass time is measured.
   - *Owns:* `src/perform/`, `tests/perform/`, `tools/mk2_e2e.py`, `docs/agents/handoff/control.md`.

Merged main after wave 3: build clean, 16 test programs, every selftest, `mk2_e2e.py` PASS. A live `himmel.exe --sim` with `--show-root <temp>` was checked too: curtains + ingest + audio + render all up.

### Wave 4 — **done 2026-11-17**
- **GUI M11 part 2 merged** (a5a13cd → b349c44). It adds the SHOW, OUTPUTS, LIBRARY, AUDIO and OSC panels, all optional (a missing block hides the panel), and wizard steps 1–4 on real data. No protocol change. D121 is PROPOSED.
- The supervisor now passes `--library` / `--no-library` to `himmel_ui`.
- Gotcha: `himmel_ingest` has no `--selftest`. Run without arguments, it is a normal worker and creates `Documents\Himmel\Library`.

**All 0.1 milestones are now built (M1–M13).** M10 and M12 are checked in sim and on the dev laptop only.

What is left needs the owner and real hardware:
- a song on the real MK2, Lightpad and NuVJ;
- a real display cable pull with 2+ screens;
- the 8 h soak and the show-PC run (`docs/qa/README.md`);
- ~~the wave 2–4 PROPOSED decisions~~ **decided** by the owner 2026-11-17 (7997cfb); D89, D94–D103 and D106–D108 are still PROPOSED.

### Wave 5 — started 2026-11-17 (owner: "avslutt og fortsett")
Polish of 0.1 on the decided defaults. The owner's `himmel.exe` may be running with the real MK2 + Lightpad:
**no agent starts a stack (himmel.exe, mk2_e2e, qa_rig) while `tasklist` shows any himmel process, and never kills one.**
Build and unit tests in the agent's own worktree are fine. If e2e cannot run, say "e2e pending" and the LEAD runs it.

| Track · session | Task | Owns | Done when |
|---|---|---|---|
| AUDIO (audio-m8) | D131 `FxReact` = command 40 + per-slot reaction table in `Local\HimmelState` (version bump, LEAD-approved); engine applies per-parameter reactions; **first commit = the contract** in `docs/interfaces/` pushed early so GUI can build on it | `src/audio/`, protocol range 40–49, reaction apply in `src/engine/`, `docs/agents/handoff/audio.md` | unit tests for packing + apply; a knob with its own reaction moves in `himmel.exe --sim` (or e2e pending) |
| MEDIA (media-m7) | ingest commands 60–69 (import folder, rescan, remove, relink, reanalyse) consumed by `himmel_ingest`; `--selftest` for `himmel_ingest`; contract doc pushed first | `src/library/`, `himmel_ingest`, `docs/interfaces/library-db.md`, `docs/agents/handoff/media.md` | `media_tests`/library tests cover each command; ingest `--selftest` green; no catalog-v3 paging yet (would clash with AUDIO's state bump) |
| GUI (gui-m11) | the reaction editor on AUDIO's contract; library import / rescan / remove on MEDIA's contract; the rest of M11 still missing (clip editor drawer, wizard steps 5+, scene / kit editor gaps) | `gui/`, `src/ui/`, `docs/agents/handoff/gui.md` | panels work against the fake engine and the real one; GUI kill / restart never changes the outputs |

**Wave 5 — done 2026-11-17.** All merged on main, green each time: the build, 19 test programs, every selftest (now including `himmel_ingest --selftest`), `mk2_e2e.py` 94 checks, and `src/engine/tools/quit_check.py`.
- AUDIO D131 FxReact (aa36c95 → 97dd7cb → 59e1562): command 40, state v3 `ReactionSection`, per-knob reactions in the engine.
- ENGINE quit (+ owner request; 1e754ad → fd8e0e9): `SystemQuit` 74 (command-protocol §4e; §4d is FxReact), MK2 SYSTEM → QUIT hold 2 s, supervisor clean stop, `himmel_cmd quit --confirm`.
- MEDIA ingest commands 60–67 (072ddaf → e41e130) in library block v2; D148–D149 PROPOSED.
- GUI M11 part 3 (bf581f2 → 41010c0): BUILD view, clip drawer filmstrip, wizard step 0, reaction editor, library actions, QUIT. D122 PROPOSED.
- LEAD live check: the reaction editor against the real engine (shows the engine's table, signal change and OFF round-trip; gui ack 73 rej 0), and GUI QUIT stops every process (exit 0). The GUI uses `window.confirm`, which the in-app browser auto-cancels; a normal browser shows it.
- Incidents: two agent test stacks ran at once (results void, re-run by the LEAD); `himmel_ingest --help` starts a real import worker (twice today). Rule kept for the rest of 0.1: **only the LEAD runs a stack** during a wave.

Follow-ups (not started):
- CONTROL: `mk2_controller` has no `stop_request`, so quit waits 3 s and then terminates it (most of the 4.5 s quit).
- MEDIA: `himmel_ingest` must print usage and exit on unknown args (`--help`).
- ENGINE: `himmel_cmd` prints `?` as the name of FxReact.
- SHOW: scenes do not save reactions yet; a recall's FxSet resets a slot to Default.
- Library buttons are not checked live against a real library (test stacks run with `--no-show-files`).

### Wave 6 — started 2026-11-17 (owner: "Ja, runde 6")
The wave 5 follow-ups. **The owner's show runs from the main checkout**: nobody builds in the main checkout, nobody starts or stops a Himmel process they did not start, and **no agent runs a stack** (himmel.exe, mk2_e2e, qa_rig, quit_check). The LEAD builds merges in its own worktree and runs e2e only when the owner's show is stopped.

| Track · session | Task | Owns | Done when |
|---|---|---|---|
| CONTROL (control-m4) | `stop_request` in the MK2 status block; `mk2_controller` lets go cleanly (LEDs off, screens to a "stopped" page, log "Stopped") so quit no longer waits 3 s; the supervisor sets it in its stop path | `src/controller/`, the supervisor's stop path (fenced), `docs/mk2-controller-spec.md`, `docs/agents/handoff/control.md` | `mk2_tests` cover it; quit time measured by the LEAD with `quit_check.py` < 2 s |
| SHOW (show-m3) | scenes / looks / variations save and recall the per-knob reactions (state v3 `ReactionSection`, FxReact); a recall applies FxSet, then the saved reactions; old show files load with Default | `src/show/`, `docs/interfaces/show-block.md`, show file format, `docs/agents/handoff/show.md` | `show_tests` round-trip a scene with Custom + Off rows; an old file loads |
| MEDIA (media-m7) | `himmel_ingest`: unknown args or `--help` print usage and exit 2 (never start a worker); `himmel_cmd` prints `react` for FxReact (one line in `himmel_cmd_main.cpp`); a `--library <temp>` recipe in the handoff so the LEAD can check the GUI library buttons live | `src/library/`, `himmel_ingest`, that one line, `docs/agents/handoff/media.md` | tests / selftest green; `--help` starts nothing |

**Wave 6 — done 2026-11-17.** Merged in the LEAD worktree (`claude/lead-wave6`), checked there while the owner's show ran, then e2e once the show was stopped; fast-forwarded to main. Green: build, 19 test programs, every selftest, `mk2_e2e.py` 94 checks, `quit_check.py`.
- CONTROL a434717 → 87fb70a: `Local\HimmelMk2` v2 with `Status.stop_request`; the controller lets go in ~60 ms (LEDs off, "stopped" page). **Quit now 1.46 s** (was 4.5 s).
- MEDIA 50a237b → ad6b7b9: `himmel_ingest` bad args / `--help` = usage, exit 2, nothing opened; `command_name(FxReact)` = "react"; temp-library recipe in `handoff/media.md`.
- SHOW f49425a → 75acc8e: looks / scenes / variations / undo / live.json keep per-knob reactions (Custom + Off); journal v2 (a v1 journal is not restored, the show comes from the last save).
- The owner could not stop his show; `himmel_cmd quit --confirm` stopped it cleanly. Ask him what he tried (GUI ⋯ → Quit uses `window.confirm`).
- Open: the GUI library buttons live on a temp library (recipe ready); `mk2_chain_tests` flaked once under load (SHOW saw it, passed on re-run).

### Wave 7 — started 2026-11-17 (after the owner's first user test)
Owner's verdict: "not ready for user testing yet". He could not get library clips onto layers, the import side lacks a lot, and 89 AI clips (~17 GB of HAP) nearly filled 7 GB of free disk. The parent session set HOLD, removed the 72 queued clips (14 remain) and set the mode back to AUTO.

**Done when (the wave as a whole):** a first-time user, in the browser alone, can go **start → import a folder → put clips on layers → play → add effects → save a scene → save the project** with no terminal and no docs. The LEAD runs exactly that flow after merging, on a temp library (`handoff/media.md` recipe), and records it in `docs/qa/`.

| Track · session · branch | Task | Decisions |
|---|---|---|
| GUI · gui-w7 (local_210ce077) · `claude/gui-w7-flows` | library → layer (drag, double-click, Load to…, picker from an empty slot); library view at scale (search, filter, multi-select, clear queue, progress, disk); clip editor drawer; wizard 5+; scene / kit editor gaps; help everywhere | D170–D179 |
| MEDIA · media-w7 (local_0e3d6e91) · `claude/media-w7-import` | disk-space guard; persistent HOLD / AUTO; clear queue; progress fields; import options (selection, downscale, HAP vs HAP Q, dry run); library location / move; judder: source-fps metadata | D180–D189 |
| ENGINE · engine-w7 (local_c5d09b0c) · `claude/engine-w7-catalog` | catalog v3 paging (past 256); load by library id; smooth 24 / 30 fps on 60 Hz; fixes on import → load → play | D190–D199 |

**LEAD calls (protocol):**
- `Local\HimmelState`: only ENGINE changes it this wave (catalog v3 paging = state **v4**). It must be additive after the v3 `ReactionSection`, and its contract is pushed first.
- `HimmelCommands` CommandTypes: ENGINE **75–79** (e.g. load by library id). GUI **50–59** if it needs engine commands. Count moves with ENGINE.
- Library block (`Local\HimmelLibrary`) ingest codes are their own namespace. MEDIA uses **68–69**, then **100–119** if more are needed. Any block version bump is pushed as a contract first. GUI builds on both contracts.
- **Judder:** playback-side first. ENGINE owns smooth playback (cadence / frame blend, cheap, on by default where the fps does not divide the refresh). MEDIA records source fps and cadence metadata, and an import-time conform stays an **option, off by default** (it costs disk and changes the clip). ENGINE and MEDIA agree on the metadata fields through the LEAD.
- CONTROL's MK2 BROWSE moves to catalog v3 after ENGINE lands (a wave 8 item).
- Rules as before: no agent runs a stack, builds in the main checkout, or stops a Himmel process it did not start. The LEAD merges in its own worktree.

**Wave 7 — done 2026-11-17.** Merged in `claude/lead-wave7`, fast-forwarded to main. Green: clean build, 20 test programs, every selftest, `mk2_e2e.py` 94 checks, `quit_check.py`, `tools/gui_flow_e2e.py` 35 checks (imports 7 of 7). **Done-when met:** the LEAD walked start → import → layers → TAKE → FX → scene → project → quit in the browser alone (`docs/qa/2026-11-17-wave7-first-user-flow.md`).
- MEDIA ae2797e + e04a2f3: library block v3, disk guard, HOLD/AUTO stored, ClearQueue 68, Library 69, SetReserve 100, import options. D180 reserve = clamp(5 %, 2 GB, 20 GB), **DECIDED by the owner 2026-11-17** (his 1 TB disk with ~7 GB free still refuses until he lowers "Keep free on disk").
- ENGINE 1e02044 + 7ffb39c: state v4 clip pages (8192 ids, library_id), LayerClipLib 75, Smooth Auto (Even / Blend), pre-roll, `--reserve-gb` pass-through, `Mk2Perform::set_clips` hook.
- GUI 41a839c + 217fffd: clips onto layers everywhere, library at scale, wizard 5–7, H.confirm everywhere, visible QUIT, Keep free on disk.
- Stack lock `%TEMP%\himmel_stack.lock` (mkdir) is the rule for every stack from now on.

Open for wave 8:
- GUI bugs from the walk: layer rows D / H cover the bottom panel at ≤ 1600×900; ESTIMATE DISK never shows the plan; "library uses 0.0 GB" after an import.
- CONTROL: MK2 BROWSE on catalog v3 (`set_clips`, LayerClipLib).
- SHOW: look_ops finds clips by name only in ids < 256; store library ids in looks.

**Old wave 4 candidates (now done or folded in):**
- GUI: the SHOW and AUDIO panels, the reaction editor and the wizard (rest of M11);
- M9 / M13 if CONTROL did not reach them;
- M10 NuVJ;
- M12 QA soak rig.

## Start here tomorrow (2026-11-15 evening)
- The owner reported: **neither the MK2 simulator nor the real MK2 drives the
  engine yet.** Real MK2: `himmel.exe --sim` never touches hardware; run
  `himmel.exe` without `--sim` with every MK2 Live page closed, and check.
  Simulator: with the sim link fix merged, only 1 of several group presses
  reached the engine (Perform ack 12 → 13), and the sim never showed the
  engine's screens. An orphaned `mk2_live.py --sim` from S1's soak was
  sending at the same time; it is closed now. Re-test with exactly one
  simulator. If it still fails, send to CONTROL: `SimPeerPolicy`
  (`src/controller/include/himmel/mk2/sim_link.h`) hands the peer back to an
  ignored sender after 1 s of silence from the current one, and an idle sim
  then stays dropped. Also check that the live view's sim sends anything when
  idle.
- Small bug: the supervisor opens `logs/himmel-*.log` without read sharing, so
  nobody can read the log while Himmel runs.
- Everything was shut down at the owner's request; nothing runs.

## Next up (ordered)
1. Owner plays the slice; collect feedback; settle D94–D100.
2. After owner feedback: M1 (rest), M2 transport, M3 projects/scenes,
   M5 effects — max 3 agents at a time.
3. A CI build (GitHub Actions on windows-latest) is not set up; M0 asked for
   it. Propose it to the owner if he wants CI (it is free for public repos).

## GUESSES
- GUESS: the slice's TAKE default is 2000 ms (command-protocol.md §4); the
  owner may want another default. Medium confidence.
- GUESS: `himmel_ui` port 8790 is free on the owner's PCs (all other Himmel
  ports are 8765–8787). High confidence.
- GUESS: the timeline weeks in `roadmap.md` §3 are estimates.

## Open questions for the owner
- None blocking. Ask after the slice: does the GUI layout fit, TAKE length,
  which effects matter most.

## How to build / run / test
- `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then
  `cmake --build build --config Release` (VS Build Tools 2022's bundled cmake:
  `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`).
- Run every `build/bin/Release/*_tests.exe`, every `python tools/<x>.py --selftest`.
- `python tools/make_test_clips.py` recreates the HAP test clips in `media/test/`.

## Gotchas found
- Two renderers at once (e.g. an agent's test `himmel_render` from its worktree) share the `Local\Himmel*` blocks and steal each other's ring-0 events: `mk2_e2e.py` failed until the other one exited. Check `tasklist` before e2e runs.
- Git auto-merge of `weapons/README.md` interleaved sections from different
  branches without conflict markers. After a merge that touches it, check the
  `## ` headings order.
- Worktrees of finished sessions still hold their local branches; they go away
  when those sessions are archived (owner's choice).

## Interfaces touched
- New: command-protocol.md, engine-state.md, preview-block.md, the
  `himmel_perform` API. `docs/interfaces/README.md` updated.
