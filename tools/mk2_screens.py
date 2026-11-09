"""
Maschine MK2 screen driver (prototype)
======================================

Two 1-bit screens, 256 x 64 each. Report 0xE0 = left, 0xE1 = right.
One report draws an 8-row strip (from github.com/abique/ni-mk2):

    [id] 00 00 Y 00 20 00 08 00  + 256 bytes   (265 bytes in all)

Y = first row (0, 8 .. 56), 0x20 = 32 bytes per row, 0x08 = 8 rows.
The 256 bytes are 8 rows x 32 bytes, row-major. A full frame is 8 strips per
screen; only strips that changed are sent.

Canvas keeps pixels MSB-left (bit 7 of byte 0 = pixel x=0). pack() converts to
the device format with two switches, msb_left and invert, so the owner can
pick what looks right on the real screen.
"""

import time

W, H = 256, 64
ROW_BYTES = W // 8
STRIP_ROWS = 8
STRIPS = H // STRIP_ROWS
REPORT = {"left": 0xE0, "right": 0xE1}

# Classic 5x7 font, 5 columns per glyph, bit 0 = top row.
FONT = {
    " ": "0000000000", "!": "00005f0000", "#": "147f147f14", "%": "2313086462", "(": "001c224100",
    ")": "0041221c00", "*": "2a1c7f1c2a", "+": "08083e0808", ",": "0050300000", "-": "0808080808",
    ".": "0060600000", "/": "2010080402", "0": "3e5149453e", "1": "00427f4000", "2": "4261514946",
    "3": "2141454b31", "4": "1814127f10", "5": "2745454539", "6": "3c4a494930", "7": "0171090503",
    "8": "3649494936", "9": "064949291e", ":": "0036360000", "<": "0814224100", "=": "1414141414",
    ">": "0041221408", "?": "0201510906", "A": "7e1111117e", "B": "7f49494936", "C": "3e41414122",
    "D": "7f4141221c", "E": "7f49494941", "F": "7f09090901", "G": "3e4149497a", "H": "7f0808087f",
    "I": "00417f4100", "J": "2040413f01", "K": "7f08142241", "L": "7f40404040", "M": "7f020c027f",
    "N": "7f0408107f", "O": "3e4141413e", "P": "7f09090906", "Q": "3e4151215e", "R": "7f09192946",
    "S": "4649494931", "T": "01017f0101", "U": "3f4040403f", "V": "1f2040201f", "W": "3f4038403f",
    "X": "6314081463", "Y": "0708700807", "Z": "6151494543", "[": "007f414100", "]": "0041417f00",
    "_": "4040404040",
}
GLYPH_W = 6  # 5 px + 1 px gap


class Canvas:
    def __init__(self):
        self.buf = bytearray(ROW_BYTES * H)

    def clear(self, on=False):
        v = 0xFF if on else 0
        for i in range(len(self.buf)):
            self.buf[i] = v

    def pixel(self, x, y, on=True):
        if 0 <= x < W and 0 <= y < H:
            i, m = y * ROW_BYTES + (x >> 3), 0x80 >> (x & 7)
            if on:
                self.buf[i] |= m
            else:
                self.buf[i] &= ~m & 0xFF

    def hline(self, x0, x1, y, on=True):
        for x in range(x0, x1 + 1):
            self.pixel(x, y, on)

    def vline(self, x, y0, y1, on=True):
        for y in range(y0, y1 + 1):
            self.pixel(x, y, on)

    def rect(self, x, y, w, h, on=True):
        self.hline(x, x + w - 1, y, on)
        self.hline(x, x + w - 1, y + h - 1, on)
        self.vline(x, y, y + h - 1, on)
        self.vline(x + w - 1, y, y + h - 1, on)

    def fill(self, x, y, w, h, on=True):
        for yy in range(y, y + h):
            self.hline(x, x + w - 1, yy, on)

    def invert(self, x, y, w, h):
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                i, m = yy * ROW_BYTES + (xx >> 3), 0x80 >> (xx & 7)
                self.buf[i] ^= m

    def text(self, x, y, s, on=True):
        """Draws upper-case 5x7 text; returns the x after the last glyph."""
        for ch in str(s).upper():
            cols = bytes.fromhex(FONT.get(ch, FONT["?"]))
            for cx, col in enumerate(cols):
                for cy in range(7):
                    if col >> cy & 1:
                        self.pixel(x + cx, y + cy, on)
            x += GLYPH_W
        return x


def text_width(s):
    return len(str(s)) * GLYPH_W


REV = bytes(int("{:08b}".format(i)[::-1], 2) for i in range(256))


def pack(canvas, msb_left=True, invert=False):
    """Canvas -> list of 8 strips (256 bytes each) in device format."""
    return pack_buf(canvas.buf, msb_left, invert)


def pack_buf(buf, msb_left=True, invert=False):
    out = []
    for s in range(STRIPS):
        raw = bytes(buf[s * 256:(s + 1) * 256])
        if not msb_left:
            raw = raw.translate(REV)
        if invert:
            raw = bytes(b ^ 0xFF for b in raw)
        out.append(bytes(raw))
    return out


def report(side, strip, data):
    return bytes([REPORT[side], 0x00, 0x00, strip * STRIP_ROWS, 0x00, 0x20, 0x00, 0x08, 0x00]) + data


class ScreenSender:
    """Sends only the strips that changed. Keeps simple timing numbers."""

    def __init__(self):
        self.last = {"left": [None] * STRIPS, "right": [None] * STRIPS}
        self.writes = 0
        self.write_ms = 0.0  # moving average per strip write

    def reset(self):
        self.last = {"left": [None] * STRIPS, "right": [None] * STRIPS}

    def send(self, transport, side, strips):
        n = 0
        for i, data in enumerate(strips):
            if self.last[side][i] == data:
                continue
            t0 = time.perf_counter()
            transport.write(report(side, i, data))
            dt = (time.perf_counter() - t0) * 1000
            self.write_ms = dt if self.writes == 0 else self.write_ms * 0.9 + dt * 0.1
            self.writes += 1
            self.last[side][i] = data
            n += 1
        return n


# ---------- pages ----------

def test_pattern(c, side):
    """Asymmetric pattern: shows bit order (mirrored text), invert, strip order and knob columns."""
    c.clear()
    c.rect(0, 0, W, H)
    c.fill(2, 2, 12, 12)                                   # solid block top-left
    c.text(18, 4, "%s TOP LEFT" % side.upper())
    c.text(W - 2 - text_width("TOP RIGHT >"), 4, "TOP RIGHT >")
    c.text(4, 17, "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    c.text(4, 26, "0123456789 <>-+%:/*#")
    for col in range(1, 4):                                # knob column borders (64 px)
        for y in range(36, H - 1, 2):
            c.pixel(col * 64, y)
    base = 0 if side == "left" else 4
    for col in range(4):
        c.text(col * 64 + 4, 38, "K%d" % (base + col + 1))
    for x in range(0, W, 8):                               # ruler: tick every 8 px, longer every 32
        c.vline(x, H - 3 if x % 32 else H - 7, H - 2)
    for s in range(1, STRIPS):                             # strip markers on the right edge
        c.pixel(W - 3, s * 8)
    return c


def knob_page(c, side, knobs, title, highlight=None):
    """Four knob columns under the screen: name, value and a bar (0..1000)."""
    c.clear()
    c.text(2, 1, title)
    c.hline(0, W - 1, 10)
    base = 0 if side == "left" else 4
    for col in range(4):
        k = base + col + 1
        x0 = col * 64
        v = knobs.get("Knob %d" % k)
        c.text(x0 + 4, 15, "KNOB %d" % k)
        c.text(x0 + 4, 26, "-" if v is None else str(v))
        c.rect(x0 + 4, 40, 56, 10)
        if v is not None:
            c.fill(x0 + 6, 42, max(0, min(52, v * 52 // 1000)), 6)
        if col:
            c.vline(x0, 13, H - 1)
        if highlight == k:
            c.invert(x0 + 1, 12, 63, H - 12)
    return c


def status_page(c, side, lines, title):
    c.clear()
    c.fill(0, 0, W, 10)
    c.text(2, 1, title, on=False)
    for i, ln in enumerate(lines[:6]):
        c.text(2, 13 + i * 8, str(ln)[:42])
    return c
