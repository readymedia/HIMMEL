# Maschine MK2 controller module – integration spec

Everything the Himmel `controller` module (C++20, Windows) needs to drive the
Maschine MK2 without NI software. Facts marked **[verified]** were checked on
the owner's device on 2026-11-09. **[measured]** numbers come from
`docs/mk2-measurements.json` (MK2 Live → Measure card). **[proposal]** is the
design we intend to build; change it here first if we change our minds.

Prototype code (Python) that already does all of this: `tools/maschine_mk2_test.py`
(transport, LED tables), `tools/mk2_live.py` (live loop, reconnect, pad edges,
encoder menu), `tools/mk2_screens.py` (screen format, font, dithering),
`tools/mk2_fun.py` (wide canvas, `KnobTracker`), `tools/mk2_measure.py`.

## 1. Device access [verified]

| Item | Value |
|---|---|
| USB id | `17cc:1140` (Native Instruments Maschine MK2) |
| USB speed | high speed [measured] |
| Interfaces | 0, 1 = MIDI (class 0x01), **2 = vendor HID (class 0x03)**, 3 = DFU |
| Endpoints (interface 2) | `0x82` interrupt IN, `0x02` interrupt OUT, max packet 64, bInterval 1 (= every 125 µs at high speed) [measured] |
| Driver | the normal Windows HID driver. **No Zadig/WinUSB needed.** Open with hidapi (`hid_open_path` on the interface-2 path). |
| Conflicts | close Maschine, Controller Editor and NI Hardware Agent / NIHardwareService first |
| Output reports | Windows pads every output report to the largest one (265 bytes), so an LED write costs about as much as a screen strip (~0.8–0.9 ms) [measured for screen strips] |

## 2. Input reports

### 2.1 `0x01` controls (25 bytes incl. id) [verified]
Sent **only when something changes**. Payload index = byte after the report id.

| Byte | Bit 0 → bit 7 |
|---|---|
| 0 | Top 1, Top 2 … Top 8 (buttons above the screens) |
| 1 | Control, Step, Browse, Sampling, Page <, Page >, All, Auto Write |
| 2 | Volume, Swing, Tempo, Master <, Master >, Enter, Note Repeat, Master encoder push |
| 3 | Group A … Group H |
| 4 | Restart, Transport <, Transport >, Grid, Play, Rec, Erase, Shift |
| 5 | Scene, Pattern, Pad Mode, Navigate, Duplicate, Select, Solo, Mute |
| 6 | reserved |
| 7 | low nibble = master encoder position 0..15 (wraps) |
| 8–23 | Knob 1 … Knob 8, uint16 little-endian, absolute 0..~1000, wraps |

Bit set = pressed. The knobs and the encoder are **endless and absolute**:
the module must turn them into deltas (§5.3).

### 2.2 `0x20` pads (65 bytes incl. id) [verified]
32 uint16 little-endian words: `word = (hardware_id << 12) | pressure` with
pressure 0..4095. Words 16–31 repeat 0–15. **Streams continuously** at
~750 Hz (mean interval 1.335 ms) even when nothing is touched [measured].

**Pad numbering is flipped**: hardware id 0 is the top-left pad. Pad numbers
follow Maschine (Pad 1 = bottom-left):

| | col 1 | col 2 | col 3 | col 4 |
|---|---|---|---|---|
| top row | Pad 13 = id 0 | Pad 14 = id 1 | Pad 15 = id 2 | Pad 16 = id 3 |
| | Pad 9 = id 4 | Pad 10 = id 5 | Pad 11 = id 6 | Pad 12 = id 7 |
| | Pad 5 = id 8 | Pad 6 = id 9 | Pad 7 = id 10 | Pad 8 = id 11 |
| bottom row | Pad 1 = id 12 | Pad 2 = id 13 | Pad 3 = id 14 | Pad 4 = id 15 |

**Pad 12 (id 7) is worn**: it reads ~120–136 when untouched. Use the per-pad
`pad_noise_floor` from the map (§4).

## 3. Output reports [verified]
LED values are 0..127. Write the whole report each time.

| Report | Length (incl. id) | Layout |
|---|---|---|
| `0x80` pad LEDs | 49 | 16 × (R, G, B) in **hardware id order** (id 0 = Pad 13, top-left) |
| `0x81` groups + transport | 57 | Group A…H, each (left R, G, B, right R, G, B) = 48 bytes, then 8 transport LEDs: Restart, Transport <, Transport >, Grid, Play, Rec, Erase, Shift |
| `0x82` buttons | 32 | 31 single-colour LEDs in this order: Control, Step, Browse, Sampling, Page <, Page >, All, Auto Write, Top 1…Top 8, Scene, Pattern, Pad Mode, Navigate, Duplicate, Select, Solo, Mute, Volume, Swing, Tempo, Master <, Master >, Enter, Note Repeat |
| `0xE0` / `0xE1` screens | 265 | left / right screen, one 8-row strip per report: `id, 00, 00, Y, 00, 20, 00, 08, 00` + 256 bytes (8 rows × 32 bytes, row-major). Y = 0, 8 … 56. **Bit 7 = leftmost pixel, 1 = lit, no inversion.** |

Screens are 256 × 64, 1 bit, white on blue. A full frame for both screens is
16 strips ≈ 13–15 ms, so send only strips that changed and cap at ~30 Hz.
The 8 knobs sit under the screens at about x = 10, 87, 163, 240 on each screen
(from a photo — re-measure with a test pattern before a final page layout).

## 4. Map file `maschine_mk2_map.json` [verified]
The module loads this file instead of hard-coding §2. Fields it needs:

```jsonc
{
  "verified": true,                       // all 73 controls checked on the device
  "buttons": { "Play": {"byte": 4, "bit": 4}, ... },          // 48 entries, names as in §2.1
  "knobs": {
    "Knob 1": {"byte": 8, "nibble": "both"},                  // uint16 LE at byte, byte+1
    "Master encoder (turn)": {"byte": 7, "nibble": "low"}     // 4-bit value in the low nibble
  },
  "pads": { "Pad 1": {"id": 12}, ..., "_id_to_slot": {"12": 0, ...} },  // slot = pad number - 1
  "pad_noise_floor": [0, ..., 136, ...],  // index = pad number - 1, raw pressure when untouched
  "screen": {"msb_left": true, "invert": false, "checked": true},
  "hardware_notes": ["..."]               // human notes, ignore in code
}
```
Other fields (`seen`, `skipped`, `noise_bytes`, `pad_max`, `source`, `time`,
`device`, `next_byte_moves`) are tool bookkeeping: ignore them. The editor for
this file is MK2 Live (click a control on screen, press the real one).

## 5. Controller module [proposal]

### 5.1 Process and threads
- Own **process** started by the supervisor (survives a GUI or engine crash and
  vice versa). It talks to the engine and the GUI through **shared memory**:
  - an **event ring** (single producer, one ring per consumer, lock-free),
  - a **state block** for outputs (LED arrays + two 2048-byte screen buffers),
    each with a sequence counter; writers bump the counter after writing.
- Threads inside the module:
  1. **Reader**: blocking `hid_read_timeout(…, 100 ms)` loop → parse → events.
     No allocation, no locks, no I/O besides the read.
  2. **Writer**: the only thread that writes to the device (one write lock is
     not needed if only one thread writes). Coalesces: at most one LED write per
     report per 16 ms (60 Hz) when changed; screens diffed per strip at ≤ 30 Hz.
  3. **Watchdog**: the pad report is a free heartbeat (~750 Hz). **No report
     for 250 ms = device lost** → close, retry `open` every 500 ms, then resend
     every LED report and every screen strip, and emit `Connected`.
- Never block the reader on the writer or on the consumers. If a ring is full,
  drop the oldest pad-pressure events first, never button/hit events.

### 5.2 Events (module → engine/GUI)
```cpp
struct Mk2Event {
    uint64_t t_us;    // QueryPerformanceCounter time of report arrival
    uint8_t  type;    // see below
    uint8_t  id;      // button index (§2.1 order), knob 0..7, pad 1..16
    int16_t  value;   // delta / velocity / pressure
    uint16_t raw;     // raw pressure peak etc.
};
enum : uint8_t { ButtonDown, ButtonUp, KnobDelta, EncoderDelta,
                 PadHit, PadPressure, PadRelease, Connected, Disconnected };
```
- `ButtonDown/Up`: edges of the bits in §2.1 (Master encoder push included).
- `KnobDelta`: units since the last report, wrap-corrected (§5.3).
- `EncoderDelta`: steps (±1 per detent), wrap-corrected at 16.
- `PadHit`: when a pad crosses its hit threshold; `value` = velocity 1..127
  (§5.4), `raw` = the pressure used for it.
- `PadPressure`: aftertouch while held, only when it moved by > 16 (of 4095),
  at most every 4 ms per pad.
- `PadRelease`: pressure fell below the release threshold.

### 5.3 Knobs and encoder
Knobs: `d = v − last; if d > 500 d −= 1000; if d < −500 d += 1000`. Encoder: the
same with 16 / 8; one turn = 30 detents [measured]. **One full knob turn = ~1008 units** [measured], so the
raw range 0..~1000 is exactly one turn. Default mapping: one turn = the full
parameter range (delta / 1000). While a knob turns, a report arrives every
~10 ms with ~6 units per report.

### 5.4 Pads
- **Onset** per pad: pressure rises above `pad_noise_floor + 60`.
- **Velocity [measured, §7]**: take the highest pressure in the **first 4 ms
  after onset** (3 pad reports) and map it:
  `v = max(1, round(127 * clamp((p4 - 60) / 3240, 0, 1) ^ 0.8))`.
  Emit `PadHit` 4 ms after onset (or at release, if the pad is let go sooner).
  **The peak value is useless for velocity**: a soft hit that you then press
  through reaches the same peak (~2500) as a medium hit, only 80–280 ms later.
  What separates soft / medium / hard is how fast the pressure rises.
  Keep `window_ms = 4`, `full = 3300`, `gamma = 0.8` as settings (per show), so
  the feel can be tuned without code changes.
- After the hit, `PadPressure` (aftertouch) carries the pressure, so a slow,
  soft press still works as a fade-in.
- Release when the pressure falls below `pad_noise_floor + 30` (hysteresis).
- LED feedback for pads is the app's choice (the module only shows what it is
  told), but a "pressure glow" mode is cheap to do inside the module.

### 5.5 Commands (engine/GUI → module)
Written into the shared state block, not sent as messages:
`pad_rgb[16][3]` (by pad number; the module maps to hardware ids),
`group_rgb[8][2][3]`, `transport[8]`, `buttons[31]`, `screen[2][2048]`
(canvas format: MSB-left rows, 1 = lit). The module diffs and sends.

### 5.6 Who owns what [decided 2026-11-09]
1. The **8 knobs** belong to the current page/mode (they sit under the screen
   labels for that page).
2. **Shift** is a modifier layer: Shift + knob = a second function for that knob,
   Shift + button = the alternative action printed on the device.
3. The **master encoder is the browse/select wheel** in Himmel (owner's decision
   2026-11-09): turn = move through lists (clips, presets, themes, cues), push =
   select / load. **Shift + master encoder** opens the system menu (in the
   prototype tools: the screen-mode menu). The module reports the encoder as
   plain `EncoderDelta` / button events; the app decides what they do.
4. Screens: the module draws only system overlays itself (e.g. "MK2 reconnecting",
   a menu); everything else comes from the app as screen buffers.

## 6. Porting the prototypes to C++
| Python | What to keep |
|---|---|
| `mk2_screens.FONT`, `Canvas.text/blit` | 5 × 7 font table (upper + lower case), drawing helpers |
| `mk2_screens.pack_buf`, `report()`, `ScreenSender` | strip packing, header bytes, send-only-changed |
| `mk2_screens.BAYER` | 4 × 4 ordered dither |
| `mk2_fun.KnobTracker` | wrap-correct deltas |
| `mk2_live.pad_tick` | hit edge + hysteresis |
| `mk2_live.check_errors` / `device_loop` | reconnect loop (the C++ version should use the pad-report heartbeat instead of error counts) |
| `mk2_fun.*` games, `menu_overlay` | demo material only; not part of the controller module |

## 7. Measurements
Filled in from `docs/mk2-measurements.json`.

Measured 2026-11-09 on the owner's MK2 (Pad 13, one player, Python host), in two sessions.

| What | Result |
|---|---|
| Pad report interval | mean 1.334 ms (~750 Hz), p50 1.33, p99 4.8, max 8.2 ms host-side (Python; C++ will be tighter) |
| Controls report interval while turning a knob | p50 10.7 ms, mean 14.5 ms (the device seems to send knob changes at ~100 Hz). Button-press latency on its own is not measured yet. |
| USB | high speed; interrupt IN `0x82` / OUT `0x02`, bInterval 1 (125 µs) |
| Knob units per full turn | **~1008** (Knob 1, 175 reports for one turn) |
| Master encoder steps per turn | **30 detents**, ±1 per detent on the 0..15 counter (wraps twice per turn) |
| Pad hit curves | see the table below |
| Velocity formula | max pressure in the first 4 ms after onset, `^0.8` curve (§5.4) |
| Reconnect after cable pull | **works**: the prototype noticed the loss, kept retrying, and reopened ~1 s after it logged the loss (the cable was out part of that time). The prototype detects loss by read errors, which can take up to ~2 s; the C++ module should use the 250 ms pad-report heartbeat (§5.1). |

Pad hit curves (pressure 0..4095, from the recorded samples) and the velocity the §5.4 formula gives:

| Hit | Peak | Time to peak | p@2ms | max in first 4 ms | Velocity |
|---|---|---|---|---|---|
| soft 1 | 2622 | 283 ms | 70 | 78 | 2 |
| soft 2 | 2761 | 102 ms | 227 | 337 | 18 |
| soft 3 | 2525 | 78 ms | 335 | 535 | 27 |
| medium 1 | 2467 | 22 ms | 1943 | 2361 | 97 |
| medium 2 | 2494 | 30 ms | 2202 | 2265 | 93 |
| medium 3 | 2442 | 3 ms | 2188 | 2442 | 99 |
| medium 4 | 2596 | 27 ms | 2476 | 2476 | 100 |
| hard 1 | 3234 | 3 ms | 2434 | 3234 | 125 |
| hard 2 | 3347 | 30 ms | 3294 | 3327 | 127 |
| hard 3 | 3397 | 1 ms | 3237 | 3237 | 125 |

Soft 2–27, medium 93–100, hard 125–127: three clear groups.

Raw curves: `docs/mk2-measurements.json` (`hits[].samples`, [ms, pressure], every 2nd
sample; the 4-ms max above also uses `p_2ms`/`p_4ms`, which came from every sample).
Only one pad was measured; check a few other pads (and Pad 12) when the C++
module exists, and tune `full` / `gamma` by feel.

## 8. Open items
- Re-measure the knob x positions with a test pattern before the final page layout.
- Glass-to-glass latency (button → LED / screen) needs a camera test once the
  C++ module exists; the host-side numbers above are the input half.
