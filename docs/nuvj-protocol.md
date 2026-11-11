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

**Status:** checked on the owner's unit on 2026-11-11:
- the input map;
- LEDs;
- LCD text;
- the whole segment map.

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

Mapped on the owner's unit on 2026-11-11, with a camera and his checks. The
map is in `nuvj_map.json` (`lcd_segments`, described in `lcd_layout`).

- **Bits.** Each byte uses bits 0–4 (`0x01`–`0x10`).
- **The glass** (from a photo with every segment on): the parts below.
  Command `01` drives the 8-character dot-matrix row; the 64 bytes drive
  everything else.

| Bytes | What |
|---|---|
| 0 | the words LAST (0x01) and DATA (0x02) |
| 1, 2 | the LAST wedge, 7 stripes from the bottom: byte 1 bits 0–4, byte 2 bits 0–1 |
| 3, 4 | the DATA wedge, the same way |
| 5 | the words FULL (0x01) and 16 LEVELS (0x02) |
| 6 | the A, B, C and D boxes (0x01, 0x02, 0x04, 0x08) |
| 7, 8 | the box letters (camera: C = 7/0x04, D = 7/0x08) and PRESS ENTER (8/0x10) |
| 10–14 | the "188" digits over BPM: units 10/11, tens 12/13, the leading "1" in byte 14 (b = 0x02, c = 0x08) |
| 16–58 | the 11 16-segment characters: 3 bytes each, starting at 16, 19, 24, 27, 32, 35, 40, 43, 48, 51, 56 |
| 22 | the rule (0x01), PROG (0x02), NOTE (0x04), AFT (0x08), CTRL (0x10) |
| 30 | BPM (0x01), MIDI CH (0x02, 0x04) |
| 38 | PAGE (0x01), PRESET (0x02), PROGRAM CHANGE (0x04, one segment), GLOBAL (0x08), EDIT (0x10) |
| 59–62 | the PRESET digits: units 59/60, tens 61/62 |

**7-segment digits** use two bytes, with the lower byte for the rightmost digit:
- first byte: b = `0x02`, c = `0x08`;
- second byte: a = `0x01`, f = `0x02`, g = `0x04`, e = `0x08`, d = `0x10`.

This is the NuVJ software's own encoding. "07" is `0A 1B` (0) and `0A 01` (7).

**A 16-segment character** uses 15 bits. The top bar is one segment, even
though it is drawn as two halves.

| byte | 0x01 | 0x02 | 0x04 | 0x08 | 0x10 |
|---|---|---|---|---|---|
| +0 | bottom right half (d2) | diagonal lower right (m) | centre lower (l) | bottom left half (d1) | left lower (e) |
| +1 | right lower (c) | middle right (g2) | diagonal lower left (k) | middle left (g1) | left upper (f) |
| +2 | right upper (b) | diagonal upper right (j) | centre upper (i) | top bar (a) | diagonal upper left (h) |

Checked by showing text on the device ("HIMMEL VJ 5", owner).

The software also sets bytes `0x2E` and `0x36` to 1 on every update. Himmel
does not need them.
