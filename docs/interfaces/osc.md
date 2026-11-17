# OSC in (`himmel_osc`, UDP 7000)

Status: **version 1, written by CONTROL 2026-11-17 (M13)**. Design:
[`../design/timeline-and-remote.md`](../design/timeline-and-remote.md) §6.
Code: [`src/osc/`](../../src/osc/) (library `himmel_osc_lib`, process
`himmel_osc.exe`), tests [`tests/osc/osc_tests.cpp`](../../tests/osc/osc_tests.cpp).
Commands: [`command-protocol.md`](command-protocol.md), producer slot 2 `Osc`.

## 1. What it is

A lighting desk, Bitfocus Companion / Stream Deck, QLab or a phone app sends
OSC to the show PC; Himmel turns a small, fixed set of addresses into ordinary
commands. OSC can never crash or flood the engine:

```
UDP :7000 ─► allow list ─► strict OSC 1.0 parser ─► address map ─► commands, slot 2 (Osc) ─► engine validates again
                 │ blocked          │ garbage            │ refused
                 └──────────────────┴────────────────────┴──► "last messages" (Local\HimmelOsc)
```

- `himmel_osc.exe` is its own process, started by the supervisor (`himmel.exe`)
  and restarted when it exits. If it dies, the outputs do not notice; the
  engine only sees the producer go quiet.
- `himmel.exe --no-osc` leaves it out; `--osc "ARGS"` passes its arguments.

| Argument | Default | Means |
|---|---|---|
| `--port N` | 7000 | UDP port (`GUESS:` the common default; the Settings page will set it) |
| `--allow LIST` | `127.0.0.1` | Who may send: `any`, or IPv4 addresses and networks, `192.168.1.0/24,10.0.0.5` |
| `--status` | | Print the counters and the last 32 messages, then exit |
| `--send ADDRESS [ARGS] [--to IP:PORT]` | `--to 127.0.0.1:7000` | A test client: sends one message (`3` = int, `0.5` = float, `T` / `F`, else a string) |

With only `127.0.0.1` allowed it binds to the loopback address, so nothing
outside the PC can even reach the port. With any other allow list it binds to
all addresses and filters by sender.

## 2. Address map (0.1)

Case-sensitive, exact. No OSC patterns (`*`, `?`, `[ ]`, `{ }`) in 0.1.
Numbers may be int or float (a float must be a whole number where a whole
number is asked for: many apps send every number as a float). NaN and
infinity are refused. Out-of-range values are **refused**, not clamped, except
that a 0–1 value may overshoot by 0.001 (faders).

| Address | Args | Command(s) | Notes |
|---|---|---|---|
| `/himmel/go` | — | `SceneRecall` PROGRAM deck, `kSceneNext`, the scene's transition | `ENTER` on the MK2: the next scene of the song (bank) live |
| `/himmel/back` | — | `SceneRecall` PROGRAM deck, `kScenePrev` | |
| `/himmel/go/preview` | — | `SceneRecall` PREVIEW deck, `kSceneNext`, cut | `SHIFT` + `ENTER` |
| `/himmel/scene` | n 1..16 | `SceneRecall` PROGRAM deck, pad n − 1, the scene's transition | Scene n of the current song (or bank) live |
| `/himmel/scene/preview` | n 1..16 | `SceneRecall` PREVIEW deck, pad n − 1, cut | |
| `/himmel/song` | n 1..64 | `ShowSelect` Song, i = n − 1 | A song **name** (string) is refused as "not supported yet" (`GUESS:` needs the song list in the show block) |
| `/himmel/take` | [ms 0..60000] | `Take` i = ms (none / 0 = the show's default) | |
| `/himmel/cut` | — | `Cut` | |
| `/himmel/black` | [0 / 1 / T / F] | `Black` off / on; no argument = toggle | |
| `/himmel/xfade` | 0..1 | `Crossfader` absolute | Added for desks with a fader |
| `/himmel/macro/1` … `/4` | 0..1 | `MacroValue` slot 0..3, absolute | Always the PROGRAM scene (knobs 5–8) |
| `/himmel/master` | 0..1 | — | Refused "not supported yet": the engine has no master dimmer (ENGINE) |
| `/himmel/bpm` | 20..300 | `Clock` Bpm, absolute | |
| `/himmel/beat` | — | `Clock` Tap | `GUESS:` a beat pulse = a tap (the tap lands on a beat) |
| `/himmel/undo`, `/himmel/redo` | — | `Undo` i = 0 / 1 | Added: the desk can undo a live change |

Commands that name a deck (scenes) need the engine's state (which deck is
PROGRAM); before the engine runs they are refused ("engine not running").
Everything else is sent and the engine applies it when it is up.

## 3. Packets

- OSC 1.0: a message or a `#bundle` (nested up to 4 deep, at most 32 messages
  per packet). Bundle time tags are ignored: everything runs at once.
- Argument types: `i`, `f`, `s`, `T`, `F`, `N`. Anything else (`b`, `h`, `d`,
  `t`, `c`, `r`, `m`, arrays) makes the packet garbage. A message without a
  type tag string has no arguments (old senders).
- Addresses and strings must be printable ASCII; the padding must be zeros;
  no bytes may be left over. Packets over 4096 bytes are refused.
- **A packet is checked whole before anything in it is used**: one bad element
  in a bundle drops the whole bundle.
- At most 500 messages per second are mapped (the engine allows 4000 per
  producer); the rest are counted as rate limited.

## 4. The status block `Local\HimmelOsc` (version 1)

Written only by `himmel_osc` (seqlock `status_seq`, at most every 250 ms or on
change); read by the GUI's Settings › Remote page and `himmel_osc --status`.
Header: [`src/osc/include/himmel/osc/osc.h`](../../src/osc/include/himmel/osc/osc.h)
(`OscBlock`, `OscStatus`, `LastMessage`; magic `HOSC`).

- pid, the UDP port it listens on (0 = could not bind, retried every 2 s), its
  heartbeat (steady µs), the allow list as given;
- counters: received, applied (sent to the engine), refused (the map said no),
  blocked (not on the allow list), garbage (the parser said no), rate limited,
  ring full;
- **the last 32 messages** (a ring: `next` = the next slot, `total` = logged so
  far), each with wall-clock time, sender IP and port, verdict (applied /
  refused / blocked / garbage / rate limited), the reason (a `Parse` or `Map`
  code) and the message as text (`/himmel/scene 3`, or `<garbage: 13 bytes>`).

"Applied" means the command went into the ring; the engine's own verdict is in
the state block (`LiveState.acked_seq[2]`, `rejected[2]`) and, for scenes, the
show block.

## 5. Later (0.2)

OSC out (`/himmel/state/...` for Companion and desks), the timeline addresses,
`/himmel/layer/<n>/clip`, `/himmel/fx/...`, `/himmel/pad/<n>`, patterns,
song by name, master dimmer, and the Settings page for port and allow list
(`timeline-and-remote.md` §6).
