# NuVJ protocol (weapon no. 5)

This is how the Numark NuVJ controller (2008) talks over USB-MIDI. It is a
class-compliant device with one MIDI in port and one MIDI out port.

**Where these facts come from**
- Input: a NuVJ owner's public forum post, confirmed by the `mappings.xml`
  that ships inside the NuVJ 1.0 software.
- Feedback (LEDs, LCD): read from the owner's copy of the NuVJ 1.0 software
  by static analysis, on 2026-11-10.
  - We unpacked the installer and disassembled `NuVJ.exe` with Microsoft's
    `dumpbin`.
  - The program was never run, and no code or files were copied into Himmel.
  - This was done for interoperability with hardware the owner owns.

Credits are in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md). The code
is [`tools/nuvj_sim.py`](../tools/nuvj_sim.py).

**Status:** this has not been checked on the owner's unit yet. The plan is to
check it with the live view.

## Device → host (controls)

Everything below is on MIDI channel 1, except the faders, which use pitch bend
on channels 1–3.

| Control | Message |
|---|---|
| Pads, deck A (PAD 7 8 9 / 4 5 6 / 1 2 3) | notes 12 14 16 / 18 20 22 / 24 26 28 |
| Pads, deck B | the same notes + 1 (13 … 29) |
| SCRATCH A / B | notes 0 / 1 |
| BANK push A / B | notes 2 / 3 (`ID_DECKx_ENTER`) |
| AUTO A / B, SYNC A / B, LATCH A / B | notes 4 / 5, 6 / 7, 8 / 9 |
| INSTA A / B | notes 10 / 11 |
| MASTER FX, INSTA BLACK, BPM TAP | notes 30, 31, 32 |
| FADER START switch | note 33, velocity 0 = off, 65 = on |
| PMTR 1 / 2, deck A | CC 0 / 2, absolute |
| PMTR 1 / 2, deck B | CC 1 / 3, absolute |
| Master PMTR 1 / 2 | CC 4 / 5, absolute |
| CONTRAST, BRIGHT | CC 6, 7, absolute |
| FX LEVEL | CC 8, absolute |
| MASTER FX (select) | CC 9, relative (endless) |
| AUTO FADE SPEED | CC 13, absolute |
| BANK A / B | CC 14 / 15, relative |
| Jog wheel B / A | CC 16 / 17, relative |
| FADER MODE switch | CC 18: A = 1, B = 2, C = 0 |
| SPEED A / SPEED B / crossfader | pitch bend on channel 1 / 2 / 3 |

## Host → device (feedback)

Every feedback message has the same shape:

```
F0 00 01 3F 7F 20 <cmd> 00 <len> <data …> F7
```

- `00 01 3F` is Numark's manufacturer id.
- `7F` means all devices.
- `20` is the NuVJ.
- Without these messages the LEDs stay dark and the LCD keeps showing "NuVJ".

| cmd | len | Data | What the software does with it |
|---|---|---|---|
| `60` | 4 | `01 01 01 00` | Sent once when it opens the controller. |
| `01` | 8 | 8 ASCII characters | LCD text row. Text longer than 8 is cut to 7 characters + "." (e.g. `BRIGHTN.`). The software shows the name of the parameter you turn. |
| `02` | 64 | LCD segment memory | Sent with the text. See *LCD segments* below. |
| `03` | 8 | LED bits | All LEDs at once. See *LEDs* below. |
| `7A` | 1 | 0–99 | LCD contrast while the preference slider moves. Not stored. |
| `7B` | 1 | 0–99 | The same, **stored in the controller** (the OK button). **Himmel never sends this.** |
| `50` / `51` | 16 / … | nibbles | A request/reply pair with two 32-bit values; it looks like a challenge. Not used by Himmel. |

The software also sends the MIDI standard identity request,
`F0 7E 7F 06 01 F7`.

### LEDs (`cmd 03`, 8 bytes, one bit per LED)

| Byte | 0x01 | 0x02 | 0x04 | 0x08 |
|---|---|---|---|---|
| 0 | LATCH A | AUTO A | SYNC A | PAD 7 A |
| 1 | PAD 8 A | PAD 9 A | PAD 4 A | PAD 5 A |
| 2 | PAD 6 A | PAD 1 A | PAD 2 A | PAD 3 A |
| 3 | SCRATCH A | INSTA A | MASTER FX | INSTA BLACK |
| 4 | LATCH B | AUTO B | SYNC B | PAD 7 B |
| 5 | PAD 8 B | PAD 9 B | PAD 4 B | PAD 5 B |
| 6 | PAD 6 B | PAD 1 B | PAD 2 B | PAD 3 B |
| 7 | SCRATCH B | INSTA B | BPM TAP | always set by the software (meaning unknown) |

The software numbers its 9 cells from the top left, so cell 1 is PAD 7.

### LCD segments (`cmd 02`, 64 bytes)

**What the software uses:**
- Bytes `0x2E` and `0x36` are always 1.
- A 2-digit number (0–99, the parameter value):
  - tens in bytes `0x3D`/`0x3E`;
  - units in bytes `0x3B`/`0x3C`.
- Each digit is two bytes:

  | Byte | Segments |
  |---|---|
  | first | b = `0x02`, c = `0x08` |
  | second | a = `0x01`, f = `0x02`, g = `0x04`, e = `0x08`, d = `0x10` |

  So "07" is `0A 1B` (0) and `0A 01` (7).

**What is not known yet:** the other segments (small digits, bar, icons,
labels). We learn them on the owner's unit with the **segment hunt** in the
live view:
1. It lights one bit at a time.
2. It writes `SEGbb.b` on the text row.
3. The owner clicks the part of the drawn LCD that lit up.
4. The result is saved in `nuvj_map.json` under `lcd_segments`.
