# Show block (`Local\HimmelShow`)

Status: **version 1, written by SHOW 2026-11-17 (M3)**. Header:
[`src/protocol/include/himmel/protocol/show.h`](../../src/protocol/include/himmel/protocol/show.h)
(the source of truth for the layout; this page is the why and the rules).
Writers: the render process (status) and its save worker (files); each
producer writes only its own text box. Readers: the GUI bridge, weapon
mappers, `show_cmd`, tools. Change rules as in
[`command-protocol.md`](command-protocol.md) (append-only, version bump on a
layout change). The commands that drive it: `command-protocol.md` §4b; the
files behind it: [`show-files.md`](show-files.md).

## 1. Parts

Header magic `HMSW`, version 1, size = `sizeof(ShowBlock)` (pinned in
`tests/protocol/protocol_tests.cpp`). The render process creates it at
start-up (`open_or_create`; it zero-fills and writes the header last when the
header is missing or wrong). Readers and producers open the existing block
and check magic, version and size; a mismatch = "not there" (log once, retry
every second). Writers: `status` = the render process's frame loop only,
`files` = its save worker only, `text[n]` = producer slot n only.

| Part | Seqlock | Writer | Written | Holds |
|---|---|---|---|---|
| `status` (`ShowStatus`) | `status_seq` | render process (frame loop) | when something changed, at most once per frame, and every 250 ms | project name, dirty dot, busy, bank / song / setlist mode, song name + BPM, the **16 pads of the page** (name, used, variation count, on deck A / B, colour), **per deck** the scene / pad / variation letter / edited / morphing + progress / dice position, locks, undo / redo depth, the PROGRAM scene's 4 macro names + values, last recall µs, scene count, **acks of show commands** (`acked_seq`, `rejected`, `last_reject` per producer), last message + `message_seq` |
| `files` (`FilesStatus`) | `files_seq` | save worker | after every file operation and journal batch | project path (`""` = untitled), last save, newest autosave, `restored` (0 none, 1 autosave, 2 interrupted save), save ok, saves, journal records, **recent projects** (10: name, path, date, outputs), missing clips (count + the first 8 names), relink result, last error |
| `text[8]` (`TextBox`) | one per box (over `id` + `text`) | each producer its own | before a command that needs it | `id` (the handshake) + a UTF-8 path or name, 520 bytes |

## 2. Rules

- Readers copy a part with `seq_read` and use the copy; they never write to
  `status` or `files`.
- **Acks.** Show commands are acked in `status.acked_seq[producer]`, not in
  `LiveState.acked_seq` (that stays the deck model's). A producer that sends
  both kinds keeps one running `seq` and watches both. Refusals count in
  `status.rejected[producer]` with the reason in `last_reject`.
- **Text boxes: the handshake.** Commands carry no strings. The producer
  picks a new non-zero id (its next command `seq` is a good choice), writes id
  + text into its own box with `write_text()` (one seqlock covers both), then
  sends the command with `i` = that id. The runtime reads the box with
  `read_text()` and refuses the command (`BadValue`) when the id differs, the
  writer was busy, or the text is empty — so a stale or half-written text is
  never used. Commands that take a text: `Project SaveAs` / `Relink` /
  `Export` (`i` ≠ 0) / `Open` (`slot` = `kNone`), `SongEdit New` (`i` ≠ 0) /
  `Rename` / `RenameScene`, `MacroSet` rename (`param` = `kNone`).
- **Knob reactions (D131).** A scene's look includes its FX slots' knob
  reactions (Default / Custom / Off, `show-files.md` §2). Recall, reset,
  undo and dice set them with ordinary `FxReact` commands on slot 4, after
  the look's `FxSet`s, so a reader sees them in the state block's
  `ReactionSection` (`engine-state.md`), not here. A deck's `edited` flag
  also turns on when a reaction differs from its scene. The block layout is
  unchanged (version 1).
- **Messages.** `status.message` is short and meant for a screen line
  ("3c saved", "Saved My Show", "Autosave 22:41 restored"); `message_seq`
  bumps on each new one, so a reader can show it once.
- **Restore offer.** `files.restored != 0` after start = the show came back
  from the autosave (the GUI offers *Revert*, D109); `files.autosave_unix >
  files.saved_unix` with `restored == 0` after an open = a newer autosave is
  offered (*Restore the autosave from 22:41?* → `Project RestoreAutosave`).
- **Survives restarts?** The block lives as long as one process holds it
  (the GUI bridge usually does). A restarted renderer rewrites `status` from
  the project it opens; it does not read the old one back.

## 3. Not in version 1 (append later)

- The **kit of the focus deck's scene** (16 pads: kind, target, id, name) for
  weapon mappers and the GUI pad view (CONTROL M4 asks; then version 2).
- Scene thumbnails, cues / timeline state (0.2), moods.
