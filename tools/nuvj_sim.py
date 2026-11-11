# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
NuVJ weapon (weapon no. 5): simulator, hardware link, map learning, LED host
============================================================================

The NuVJ (2008) is a two-deck VJ controller: 2 x 9 pads, 2 jog wheels, 2 speed
faders, a crossfader, 12 knobs, BANK encoders, a blue LCD and LED buttons.
It is a class-compliant USB-MIDI device.

Everything here speaks the device's real MIDI bytes:

- ``SOURCE_TABLE``: the note / CC table. A NuVJ owner's public forum post,
  confirmed and completed by the ``mappings.xml`` inside the owner's NuVJ 1.0
  software (see THIRD_PARTY_NOTICES.md). The map verified on the owner's unit
  lives in ``nuvj_map.json`` (repo root), learned with the live view (click a
  control, then move the real one).
- The **feedback protocol** (LEDs, LCD text, LCD segments, contrast), read
  from the owner's NuVJ 1.0 software by static analysis (never run, no code
  copied): ``F0 00 01 3F 7F 20 <cmd> 00 <len> <data> F7``. See ``CMD_*``.
- ``SimNuVJ``: a pretend device. The drawn panel drives it; it sends the same
  MIDI the map says the real one sends, and decodes the host's feedback SysEx
  into LEDs and LCD contents.
- ``HwNuVJ``: the real device over WinMM (ctypes, no new dependency), with
  the same interface. It sends only feedback SysEx with the NuVJ header and the
  MIDI standard identity request.
- ``NuvjHost``: the live view standing in for Himmel. It turns MIDI into
  Himmel events (button, knob value, relative delta, fader, switch) and keeps
  a small VJ state (decks, latch / auto / sync, master FX, insta black, tap
  tempo) that it shows with the LEDs.
- ``NuvjLive``: what tools/mk2_live.py serves for ?weapon=nuvj.

Run:   python tools/mk2_live.py --port 8777 --weapon nuvj     then open http://127.0.0.1:8777
Check: python tools/nuvj_sim.py --selftest
Ports: python tools/nuvj_sim.py --ports
"""

import ctypes
import json
import os
import shutil
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROFILE = os.path.join(ROOT, "weapons", "nuvj.weapon.json")
MAP_FILE = os.path.join(ROOT, "nuvj_map.json")
PORT_HINTS = ("nuvj", "numark")

# ---------------------------------------------------------------------------
# The input table: a public forum post, confirmed by mappings.xml in the NuVJ
# software ("note+" = a toggle in software, "cc*" = endless/relative). type: note | cc | pb.
# enc: "abs" (0..127), "rel" (endless: small numbers one way, 127.. the other),
# "switch" (fixed values per position), "toggle" (note with on/off values).
# Pad 9 of deck B is note 17 (mappings.xml; the forum post had a typo).
# ---------------------------------------------------------------------------
SOURCE_TABLE = {
    "A Bank": ("cc", 14, "rel"), "B Bank": ("cc", 15, "rel"),
    "A PMTR1": ("cc", 0, "relabs"), "B PMTR1": ("cc", 1, "relabs"),
    "A PMTR2": ("cc", 2, "relabs"), "B PMTR2": ("cc", 3, "relabs"),
    "A Latch": ("note", 8), "B Latch": ("note", 9),
    "A Auto": ("note", 4), "B Auto": ("note", 5),
    "A Sync": ("note", 6), "B Sync": ("note", 7),
    "A Jog": ("cc", 17, "rel"), "B Jog": ("cc", 16, "rel"),
    "A Scratch": ("note", 0), "B Scratch": ("note", 1),
    "A Bank Push": ("note", 2), "B Bank Push": ("note", 3),   # ID_DECKx_ENTER in mappings.xml
    "Insta A": ("note", 10), "Insta B": ("note", 11),
    "A Speed": ("pb", 0), "B Speed": ("pb", 1), "Crossfader": ("pb", 2),
    "FX Level": ("cc", 8, "relabs"), "Master PMTR1": ("cc", 4, "relabs"), "Master PMTR2": ("cc", 5, "relabs"),
    "Master FX Select": ("cc", 9, "rel"),                 # "cc*" in mappings.xml: endless
    "Contrast": ("cc", 6, "relabs"), "Bright": ("cc", 7, "relabs"),
    "Master FX": ("note", 30), "Insta Black": ("note", 31), "BPM Tap": ("note", 32),
    "Fader Mode": ("cc", 18, "switch", [1, 2, 0]),        # positions A, B, C
    "Auto Fade Speed": ("cc", 13, "abs"),
    "Fader Start": ("note", 33, "toggle", [0, 65]),       # OFF, ON
}
# pads 1-3 = bottom row, 7-9 = top row; deck A even notes, deck B the next odd one
_PADS_A = {1: 24, 2: 26, 3: 28, 4: 18, 5: 20, 6: 22, 7: 12, 8: 14, 9: 16}
for _p, _n in _PADS_A.items():
    SOURCE_TABLE["A Pad %d" % _p] = ("note", _n)
    SOURCE_TABLE["B Pad %d" % _p] = ("note", _n + 1)


def entry_from_source(t):
    e = {"type": t[0], "ch": 0, "num": t[1]}
    if t[0] == "cc":
        e["enc"] = t[2]
        if t[2] == "switch":
            e["values"] = t[3]
        if t[2] == "rel":
            e["cw"] = "neg"        # the post: clockwise reads as "slower" in software expecting 1 = forward
        if t[2] == "relabs":
            e["cw"], e["step"] = "pos", 2   # the 12 top knobs are endless on the real unit (2026-11-11)
    elif t[0] == "note" and len(t) > 2:
        e["enc"] = t[2]
        e["values"] = t[3]
    if t[0] == "pb":
        e["ch"], e["num"] = t[1], None
    return e


def seg_bits(v):
    """The bits of one LCD part: [[byte, mask], ...] (also reads the older single [byte, mask])."""
    if not v:
        return []
    return [list(v)] if isinstance(v[0], int) else [list(x) for x in v]


def load_profile():
    with open(PROFILE, encoding="utf-8") as f:
        return json.load(f)


def default_map():
    return {"format": "himmel-map/1", "weapon": "nuvj", "verified": False,
            "source": "starting table from a public forum post (unverified); entries change when learned on hardware",
            "controls": {n: dict(entry_from_source(t), learned=False, seen=False) for n, t in SOURCE_TABLE.items()},
            "leds": {"checked": False}, "lcd_segments": {}}


def load_map():
    try:
        with open(MAP_FILE, encoding="utf-8") as f:
            m = json.load(f)
        base = default_map()
        for n, e in base["controls"].items():   # a control added later to the profile keeps the forum guess
            m["controls"].setdefault(n, e)
        if not isinstance(m.get("leds"), dict) or "how" in m["leds"]:
            m["leds"] = base["leds"]
        m.setdefault("lcd_segments", {})
        return m
    except (OSError, ValueError):
        return default_map()


def save_map(m):
    if os.path.exists(MAP_FILE):
        shutil.copyfile(MAP_FILE, MAP_FILE[:-5] + ".bak.json")
    tmp = MAP_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)
    os.replace(tmp, MAP_FILE)


# ---------------------------------------------------------------------------
# MIDI codec: control value <-> bytes, using a map entry
# ---------------------------------------------------------------------------
def encode(e, value=None, down=None, delta=None):
    """Bytes the device sends for this control. value: abs 0..127 / pb 0..16383 / switch position."""
    ch = e["ch"] & 15
    t = e["type"]
    if t == "note":
        if e.get("enc") == "toggle":                 # a latching switch reported as a note
            return bytes([0x90 | ch, e["num"], e["values"][value] & 0x7F])
        return bytes([0x90 | ch, e["num"], 0x7F]) if down else bytes([0x80 | ch, e["num"], 0])
    if t == "cc":
        enc = e.get("enc", "abs")
        if enc == "relabs":         # an endless knob: the simulator gives the ticks as delta
            return encode(dict(e, enc="rel"), delta=delta) if delta else b""
        if enc == "fader":
            return bytes([0xB0 | ch, e["num"], max(0, min(127, value * 127 // 16383))])
        if enc == "rel":
            out = b""
            step = 1 if delta > 0 else -1
            for _ in range(min(abs(delta), 63)):     # one message per tick, like a real encoder
                v = step if e.get("cw", "pos") == "pos" else -step
                out += bytes([0xB0 | ch, e["num"], v & 0x7F])
            return out
        if enc == "switch":
            return bytes([0xB0 | ch, e["num"], e["values"][value] & 0x7F])
        return bytes([0xB0 | ch, e["num"], max(0, min(127, value))])
    if t == "pb":
        v = max(0, min(16383, value))
        return bytes([0xE0 | ch, v & 0x7F, v >> 7])
    return b""


def split_midi(data):
    """Split a byte string of short messages (running status not used by our encoder)."""
    out, i = [], 0
    while i < len(data):
        st = data[i]
        n = 1 if st >= 0xF8 else 2 if (st & 0xF0) in (0xC0, 0xD0) else 3
        out.append(bytes(data[i:i + n]))
        i += n
    return out


# ---------------------------------------------------------------------------
# The NuVJ feedback protocol (host -> device), from the NuVJ 1.0 software.
# Every message: F0 00 01 3F 7F 20 <cmd> 00 <len> <data...> F7
#   00 01 3F = Numark's manufacturer id, 7F = all devices, 20 = the NuVJ.
# ---------------------------------------------------------------------------
NUMARK_HEADER = bytes([0x00, 0x01, 0x3F, 0x7F, 0x20])
CMD_TEXT = 0x01            # 8 ASCII characters on the LCD's dot-matrix row (longer: 7 + ".")
CMD_SEGMENTS = 0x02        # 64 bytes: the LCD's segment memory (digits, bar, icons)
CMD_LEDS = 0x03            # 8 bytes: one bit per LED
CMD_INIT = 0x60            # sent once when the software opens the controller: data 01 01 01 00
CMD_CONTRAST = 0x7A        # 1 byte 0..99: LCD contrast, not stored (the software's slider preview)
CMD_CONTRAST_STORE = 0x7B  # the same, stored in the controller (the software's OK button). Himmel never sends it.
INIT_DATA = bytes([0x01, 0x01, 0x01, 0x00])
CMD_NAMES = {CMD_TEXT: "LCD text", CMD_SEGMENTS: "LCD segments", CMD_LEDS: "LEDs", CMD_INIT: "init",
             CMD_CONTRAST: "LCD contrast", CMD_CONTRAST_STORE: "LCD contrast (stored)",
             0x50: "challenge?", 0x51: "challenge reply?"}

# LED bits: control -> (byte 0..7, bit mask). Deck A uses bytes 0-3, deck B bytes 4-7.
LED_BITS = {
    "A Latch": (0, 0x01), "A Auto": (0, 0x02), "A Sync": (0, 0x04),
    "B Latch": (4, 0x01), "B Auto": (4, 0x02), "B Sync": (4, 0x04),
    "A Scratch": (3, 0x01), "Insta A": (3, 0x02), "Master FX": (3, 0x04), "Insta Black": (3, 0x08),
    "B Scratch": (7, 0x01), "Insta B": (7, 0x02), "BPM Tap": (7, 0x04),
}
# The software's cell 1..9 is the top-left pad first (cell 1 = PAD 7, note 12).
PAD_OF_CELL = (7, 8, 9, 4, 5, 6, 1, 2, 3)
for _i, (_byte, _bit) in enumerate(zip((0, 1, 1, 1, 1, 2, 2, 2, 2), (3, 0, 1, 2, 3, 0, 1, 2, 3))):
    LED_BITS["A Pad %d" % PAD_OF_CELL[_i]] = (_byte, 1 << _bit)
    LED_BITS["B Pad %d" % PAD_OF_CELL[_i]] = (_byte + 4, 1 << _bit)
LED_ALWAYS = (7, 0x08)     # the software always sets this bit; what it lights is not known yet

# LCD segments the software uses: a 2-digit number (0..99) and two fixed marks.
# A digit is two bytes: the first holds b (0x02) and c (0x08), the second a (0x01),
# f (0x02), g (0x04), e (0x08), d (0x10).
SEG_BITS = {"a": (1, 0x01), "b": (0, 0x02), "c": (0, 0x08), "d": (1, 0x10), "e": (1, 0x08), "f": (1, 0x02), "g": (1, 0x04)}
DIGIT_SEGMENTS = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd",
                  "6": "afgedc", "7": "abc", "8": "abcdefg", "9": "abcdfg", "-": "g", " ": ""}
SEG16 = {
    " ": "", "0": "a1 a2 b c d1 d2 e f j k", "1": "b c j", "2": "a1 a2 b g1 g2 e d1 d2", "3": "a1 a2 b g2 c d1 d2",
    "4": "f g1 g2 b c", "5": "a1 a2 f g1 g2 c d1 d2", "6": "a1 a2 f e d1 d2 c g1 g2", "7": "a1 a2 b c",
    "8": "a1 a2 b c d1 d2 e f g1 g2", "9": "a1 a2 b c d1 d2 f g1 g2", "A": "e f a1 a2 b c g1 g2", "B": "a1 a2 b c d1 d2 i l g2",
    "C": "a1 a2 f e d1 d2", "D": "a1 a2 b c d1 d2 i l", "E": "a1 a2 f e d1 d2 g1", "F": "a1 a2 f e g1", "G": "a1 a2 f e d1 d2 c g2",
    "H": "f e b c g1 g2", "I": "a1 a2 i l d1 d2", "J": "b c d1 d2 e", "K": "f e g1 j m", "L": "f e d1 d2", "M": "f e b c h j",
    "N": "f e b c h m", "O": "a1 a2 b c d1 d2 e f", "P": "a1 a2 b f e g1 g2", "Q": "a1 a2 b c d1 d2 e f m",
    "R": "a1 a2 b f e g1 g2 m", "S": "a1 a2 f g1 g2 c d1 d2", "T": "a1 a2 i l", "U": "f e d1 d2 c b", "V": "f e k j",
    "W": "f e b c k m", "X": "h j k m", "Y": "h j l", "Z": "a1 a2 j k d1 d2", "-": "g1 g2", "+": "g1 g2 i l", "/": "j k",
    "*": "g1 g2 h i j k l m", ".": "d2", "_": "d1 d2", "=": "g1 g2 d1 d2", "'": "i", ":": "i l", "%": "a1 f g1 i j k l g2 c d2",
    "?": "a1 a2 b g2 l", "#": "b c e f g1 g2 i l",
}
ALPHA_CELLS, RAMP_STEPS = 11, 7
NUMBER_DIGITS = ((0x3D, 0x3E), (0x3B, 0x3C))   # the 2-digit number: tens, units (first byte, second byte)
SEG_ALWAYS = (0x2E, 0x36)                      # set to 1 by the software on every update


def feedback(cmd, data=b""):
    """One NuVJ feedback SysEx."""
    data = bytes(data)
    return bytes([0xF0]) + NUMARK_HEADER + bytes([cmd, 0x00, len(data)]) + data + bytes([0xF7])


def parse_feedback(msg):
    """(cmd, data) for a NuVJ feedback SysEx, else None."""
    if len(msg) >= 10 and msg[0] == 0xF0 and msg[-1] == 0xF7 and bytes(msg[1:6]) == NUMARK_HEADER:
        return msg[6], bytes(msg[9:-1])
    return None


def led_bytes(names):
    b = bytearray(8)
    b[LED_ALWAYS[0]] |= LED_ALWAYS[1]
    for n in names:
        if n in LED_BITS:
            i, m = LED_BITS[n]
            b[i] |= m
    return bytes(b)


def leds_from_bytes(b):
    return {n for n, (i, m) in LED_BITS.items() if i < len(b) and b[i] & m}


def text_bytes(text):
    t = str(text)
    if len(t) > 8:
        t = t[:7] + "."
    return t.ljust(8)[:8].encode("ascii", "replace")


def number_into(seg, value):
    """Write a 2-digit number (int, or a 2-char string; "" = blank) into a 64-byte segment buffer."""
    t = ("%02d" % value) if isinstance(value, int) else str(value).rjust(2)[-2:]
    for (b0, b1), ch in zip(NUMBER_DIGITS, t):
        seg[b0] &= ~0x0A & 0x7F
        seg[b1] &= ~0x1F & 0x7F
        for sname in DIGIT_SEGMENTS.get(ch, ""):
            half, mask = SEG_BITS[sname]
            seg[b1 if half else b0] |= mask


def number_from(seg):
    out = ""
    for b0, b1 in NUMBER_DIGITS:
        lit = "".join(k for k, (half, mask) in SEG_BITS.items() if seg[b1 if half else b0] & mask)
        out += next((ch for ch, ss in DIGIT_SEGMENTS.items() if sorted(ss) == sorted(lit)), "?")
    return out


def describe(msg):
    if not msg:
        return ""
    st = msg[0]
    hi, ch = st & 0xF0, (st & 15) + 1
    if st == 0xF0:
        fb = parse_feedback(msg)
        if fb:
            cmd, data = fb
            name = CMD_NAMES.get(cmd, "command %02X" % cmd)
            if cmd == CMD_TEXT:
                return 'NuVJ %s "%s"' % (name, data.decode("ascii", "replace"))
            if cmd == CMD_LEDS:
                return "NuVJ %s: %s" % (name, ", ".join(sorted(leds_from_bytes(data))) or "all off")
            if cmd == CMD_SEGMENTS:
                return "NuVJ %s: number %s, %d bits lit" % (name, number_from(data), sum(bin(x).count("1") for x in data))
            return "NuVJ %s %s" % (name, data.hex(" "))
        if len(msg) >= 6 and msg[1] == 0x7E and msg[3] == 0x06 and msg[4] == 0x01:
            return "SysEx: universal identity request"
        if len(msg) >= 10 and msg[1] == 0x7E and msg[3] == 0x06 and msg[4] == 0x02:
            man = msg[5:8].hex(" ") if msg[5] == 0 else "%02x" % msg[5]
            return "SysEx: identity reply, manufacturer %s, rest %s" % (man, msg[8 if msg[5] == 0 else 6:-1].hex(" "))
        return "SysEx (%d bytes)" % len(msg)
    if hi == 0x90 and len(msg) > 2:
        return "note on  ch%d  %3d vel %3d" % (ch, msg[1], msg[2]) if msg[2] else "note on  ch%d  %3d vel   0 (= off)" % (ch, msg[1])
    if hi == 0x80 and len(msg) > 2:
        return "note off ch%d  %3d" % (ch, msg[1])
    if hi == 0xB0 and len(msg) > 2:
        return "CC       ch%d  %3d = %3d" % (ch, msg[1], msg[2])
    if hi == 0xE0 and len(msg) > 2:
        return "pitch    ch%d  %5d" % (ch, msg[1] | msg[2] << 7)
    return "status %02X" % st


class Decoder:
    """MIDI from the device -> (control name, event) using the current map."""

    def __init__(self, m):
        self.set_map(m)

    def set_map(self, m):
        self.m = m
        self.acc = getattr(self, "acc", {})     # relabs knobs: name -> 0..127
        self.idx = {}
        for name, e in m["controls"].items():
            if e.get("type") == "pb":
                self.idx[("pb", e["ch"], None)] = name
            elif e.get("type"):
                self.idx[(e["type"], e["ch"], e["num"])] = name
        # After the host's init (cmd 60) the NuVJ sends its faders as pitch bend on channels 1-3, as in
        # the NuVJ software; before it (plain MIDI mode) as CC 10-12. Both reach the same control.
        for name, t in SOURCE_TABLE.items():
            if t[0] == "pb":
                self.idx.setdefault(("pb", t[1], None), name)

    def decode(self, msg):
        """Returns (name or None, event dict). Event kinds: down, up, value, delta, fader, switch."""
        if len(msg) < 3:
            return None, None
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        if hi in (0x80, 0x90):
            name = self.idx.get(("note", ch, msg[1]))
            e = self.m["controls"].get(name) if name else None
            if e and e.get("enc") == "toggle":
                vals = e["values"]
                pos = vals.index(msg[2]) if msg[2] in vals else (1 if msg[2] else 0)
                return name, {"kind": "switch", "pos": pos}
            down = hi == 0x90 and msg[2] > 0
            return name, {"kind": "down" if down else "up", "vel": msg[2] if down else 0}
        if hi == 0xB0:
            name = self.idx.get(("cc", ch, msg[1]))
            e = self.m["controls"].get(name) if name else None
            enc = e.get("enc", "abs") if e else "abs"
            if enc in ("rel", "relabs"):
                v = msg[2] - 128 if msg[2] >= 64 else msg[2]
                if e.get("cw", "pos") == "neg":
                    v = -v
                if enc == "relabs":     # an endless knob used as a 0..127 knob: Himmel keeps the value
                    cur = self.acc.get(name, 64)
                    cur = max(0, min(127, cur + v * e.get("step", 2)))
                    self.acc[name] = cur
                    return name, {"kind": "value", "value": cur, "delta": v}
                return name, {"kind": "delta", "delta": v}
            if enc == "fader":          # a fader sent as a 7-bit CC: scaled to the 14-bit fader range
                return name, {"kind": "fader", "value": msg[2] * 16383 // 127, "raw": msg[2]}
            if enc == "switch":
                vals = e["values"]
                return name, {"kind": "switch", "pos": vals.index(msg[2]) if msg[2] in vals else -1, "raw": msg[2]}
            return name, {"kind": "value", "value": msg[2]}
        if hi == 0xE0:
            name = self.idx.get(("pb", ch, None))
            return name, {"kind": "fader", "value": msg[1] | msg[2] << 7}
        return None, None


# ---------------------------------------------------------------------------
# Device base: what the live view needs from both the simulated and the real one
# ---------------------------------------------------------------------------
class DeviceBase:
    kind = "sim"

    def __init__(self):
        self.lock = threading.RLock()
        self.listeners = []          # f(msg) for MIDI from the device
        self.log = []
        self.counts = {"to_host": 0, "to_device": 0}
        self.led_state = bytes(8)    # the last LED bytes the host sent (CMD_LEDS)
        self.lcd_text = ""           # the last LCD text (CMD_TEXT)
        self.lcd_segs = bytes(64)    # the last segment memory (CMD_SEGMENTS)
        self.contrast = None
        self.inited = False
        self.connected = True
        self.notes = []

    def _log(self, direction, msg, noisy=False):
        self.log.append((time.strftime("%H:%M:%S"), direction, bytes(msg).hex(" "), describe(msg), noisy))
        del self.log[:-400]

    def emit(self, data):
        for msg in ([bytes(data)] if data[:1] == b"\xF0" else split_midi(data)):
            with self.lock:
                self.counts["to_host"] += 1
                self._log("device → host", msg)
            for f in list(self.listeners):
                try:
                    f(msg)
                except Exception:
                    pass

    def note_out(self, msg):
        """Remember what the host's feedback asked the device to show (both the sim and the real one)."""
        fb = parse_feedback(msg)
        if not fb:
            return
        cmd, data = fb
        if cmd == CMD_LEDS and len(data) == 8:
            self.led_state = data
        elif cmd == CMD_TEXT:
            self.lcd_text = data.decode("ascii", "replace")
        elif cmd == CMD_SEGMENTS and len(data) == 64:
            self.lcd_segs = data
        elif cmd in (CMD_CONTRAST, CMD_CONTRAST_STORE) and data:
            self.contrast = data[0]
        elif cmd == CMD_INIT:
            self.inited = True

    def close(self):
        pass


class SimNuVJ(DeviceBase):
    """A pretend NuVJ: the drawn panel drives it, it sends the map's MIDI bytes."""

    kind = "sim"

    def __init__(self, live):
        super().__init__()
        self.live = live
        self.notes = ["Simulated NuVJ: the drawing is the controller. Its LEDs and LCD show the feedback SysEx the host sends."]

    def receive(self, msg):
        """MIDI from the host to the device."""
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=True)
            self.note_out(msg)


# ---------------------------------------------------------------------------
# The real NuVJ over WinMM (reuses the ctypes bindings of lightpad_hw.py)
# ---------------------------------------------------------------------------
class HwNuVJ(DeviceBase):
    kind = "hw"

    def __init__(self, port_hint=None):
        super().__init__()
        import lightpad_hw as W
        self.W = W
        self.mm = W.winmm()
        self.mm.midiOutShortMsg.argtypes = [W.wintypes.HANDLE, W.wintypes.DWORD]
        self.port_hint = port_hint
        self.connected = False
        self.closed = False
        self.port_names = ("", "")
        self.hin = W.wintypes.HANDLE()
        self.hout = W.wintypes.HANDLE()
        self.headers, self.bufs, self.readd = [], [], []
        self.out_lock = threading.Lock()
        self.cb = W.MIDIINPROC(self._callback)
        self.wake = threading.Event()
        self.stats = {"msgs": 0, "rate": 0.0, "first": None}
        self._rn, self._rt = 0, time.time()
        self.notes = ["Real NuVJ: looking for its MIDI ports…"]
        threading.Thread(target=self._loop, daemon=True).start()

    def _pick(self, ports):
        if self.port_hint is not None:
            if isinstance(self.port_hint, int):
                return self.port_hint if any(k == self.port_hint for k, _ in ports) else None
            return next((k for k, n in ports if self.port_hint.lower() in n.lower()), None)
        return next((k for k, n in ports if any(h in n.lower() for h in PORT_HINTS)), None)

    def _open(self):
        W, mm = self.W, self.mm
        ins, outs = W.list_ports()
        i = self._pick(ins)
        if isinstance(self.port_hint, int) and i is not None:
            name = dict(ins)[i]
            o = next((k for k, n in outs if n == name), None)
        else:
            o = self._pick(outs)
        if i is None:
            self.notes = ["No NuVJ MIDI port found. Plug it in with USB and close the NuVJ / GrandVJ software. "
                          "Ports seen: in %s, out %s. Pick one below if it has another name." % (
                              [n for _, n in ins], [n for _, n in outs])]
            return False
        r = mm.midiInOpen(ctypes.byref(self.hin), i, ctypes.cast(self.cb, ctypes.c_void_p), 0, W.CALLBACK_FUNCTION)
        if r:
            self.notes = ["Could not open MIDI in %d (error %d). Close other MIDI software: "
                          "Windows lets only one program use the port." % (i, r)]
            return False
        if o is not None and mm.midiOutOpen(ctypes.byref(self.hout), o, 0, 0, 0):
            o = None
        self.has_out = o is not None
        self.headers, self.bufs = [], []
        for _ in range(8):   # buffers for SysEx (the device may send some, e.g. an identity reply)
            b = ctypes.create_string_buffer(512)
            h = W.MIDIHDR()
            h.lpData = ctypes.cast(b, ctypes.c_void_p)
            h.dwBufferLength = 512
            mm.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            self.headers.append(h)
            self.bufs.append(b)
        mm.midiInStart(self.hin)
        self.port_names = (dict(ins)[i], dict(outs).get(o, "-") if o is not None else "-")
        self.connected = True
        self.inited = False          # a (re)opened port: the host sends init and a full refresh
        self.notes = ["Real NuVJ on MIDI in %d (%s), out %s. Touch every control: the drawing follows." % (
            i, self.port_names[0], "%d" % o if o is not None else "not opened")]
        with self.lock:
            self._log("probe", b"")
            self.log[-1] = (time.strftime("%H:%M:%S"), "probe", "", "ports opened: " + " / ".join(self.port_names), False)
        return True

    def _close_ports(self):
        if not self.connected:
            return
        mm = self.mm
        self.connected = False
        try:
            mm.midiInStop(self.hin)
            mm.midiInReset(self.hin)
            for h in self.headers:
                mm.midiInUnprepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            mm.midiInClose(self.hin)
            if self.has_out:
                mm.midiOutReset(self.hout)
                mm.midiOutClose(self.hout)
        except OSError:
            pass

    def close(self):
        self.closed = True
        self.wake.set()
        self._close_ports()

    def _callback(self, handle, msg, inst, p1, p2):
        W = self.W
        try:
            if msg == W.MIM_DATA:
                st = p1 & 0xFF
                n = 1 if st >= 0xF8 else 2 if (st & 0xF0) in (0xC0, 0xD0) else 3
                self._in(bytes([(p1 >> (8 * k)) & 0xFF for k in range(n)]))
            elif msg in (W.MIM_LONGDATA, W.MIM_LONGERROR):
                h = W.MIDIHDR.from_address(p1)
                data = ctypes.string_at(h.lpData, h.dwBytesRecorded)
                if data:
                    self._in(bytes(data))
                if self.connected:
                    self.readd.append(p1)
                    self.wake.set()
        except Exception:
            pass

    def _in(self, msg):
        with self.lock:
            self.stats["msgs"] += 1
            self._rn += 1
            if self.stats["first"] is None:
                self.stats["first"] = time.strftime("%H:%M:%S")
        self.emit(msg)

    IDENTITY_REQUEST = bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])

    def identity(self):
        """Send the MIDI standard's Universal Identity Request (the NuVJ software sends it too).
        A device that supports it answers F0 7E <id> 06 02 <manufacturer> <family> <model> <version> F7."""
        self._long(self.IDENTITY_REQUEST)

    def _long(self, msg, noisy=False):
        """Send one SysEx with midiOutLongMsg."""
        if not self.connected or not self.has_out:
            return
        W, mm = self.W, self.mm
        with self.out_lock:
            b = ctypes.create_string_buffer(msg, len(msg))
            h = W.MIDIHDR()
            h.lpData = ctypes.cast(b, ctypes.c_void_p)
            h.dwBufferLength = h.dwBytesRecorded = len(msg)
            mm.midiOutPrepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))
            r = mm.midiOutLongMsg(self.hout, ctypes.byref(h), ctypes.sizeof(h))
            t_end = time.time() + 0.2
            while r == 0 and not (h.dwFlags & W.MHDR_DONE) and time.time() < t_end:
                time.sleep(0.001)
            mm.midiOutUnprepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=noisy)
            self.note_out(msg)
        if r:
            self.connected = False

    def receive(self, msg):
        """MIDI from the host to the real device. Only NuVJ feedback SysEx (our header) and short
        messages go out; any other SysEx is refused, so nothing unknown can reach the controller."""
        msg = bytes(msg)
        if not self.connected or not self.has_out or not msg:
            return
        if msg[0] == 0xF0:
            fb = parse_feedback(msg)
            if fb and fb[0] != CMD_CONTRAST_STORE:   # never store settings in the controller
                self._long(msg, noisy=fb[0] in (CMD_LEDS, CMD_SEGMENTS, CMD_TEXT))
            return
        w = msg[0] | (msg[1] << 8 if len(msg) > 1 else 0) | (msg[2] << 16 if len(msg) > 2 else 0)
        with self.out_lock:
            r = self.mm.midiOutShortMsg(self.hout, w)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=True)
        if r:
            self.connected = False

    def _loop(self):
        mm, W = self.mm, self.W
        while not self.closed:
            if not self.connected:
                self._close_ports()
                if not self._open():
                    for _ in range(10):
                        if self.closed:
                            return
                        time.sleep(0.1)
                    continue
            self.wake.wait(0.1)
            self.wake.clear()
            while self.readd and self.connected:
                p = self.readd.pop(0)
                h = W.MIDIHDR.from_address(p)
                h.dwBytesRecorded = 0
                mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            now = time.time()
            with self.lock:
                if now - self._rt >= 1.0:
                    self.stats["rate"] = round(self._rn / (now - self._rt), 1)
                    self._rn, self._rt = 0, now
            # a pulled cable: the port disappears from the list
            if self.connected and int(now * 2) % 4 == 0:
                ins, _ = W.list_ports()
                if not any(n == self.port_names[0] for _, n in ins):
                    with self.lock:
                        self.log.append((time.strftime("%H:%M:%S"), "probe", "", "port gone (cable pulled?)", False))
                    self.connected = False


# ---------------------------------------------------------------------------
# The host: Himmel's side. Events -> a small VJ state -> LEDs (+ LCD text in the sim)
# ---------------------------------------------------------------------------
import vj_classic                      # the classic two-deck show model (tools/vj_classic.py)

FX_NAMES = tuple(e["name"].upper() for e in vj_classic.EFFECTS)   # Himmel's effect set, as on the LCD
TOGGLES = ("A Latch", "B Latch", "A Auto", "B Auto", "A Sync", "B Sync", "Master FX", "Insta Black")


class NuvjHost:
    MODES = ("himmel", "follow", "all", "chase", "off")

    def __init__(self, live):
        self.live = live
        self.mode = "himmel"
        self.state = {"A": {"pad": None, "bank": 0, "jog": 0}, "B": {"pad": None, "bank": 0, "jog": 0},
                      "toggles": {n: False for n in TOGGLES}, "bpm": 120.0, "taps": [], "xf": 8192,
                      "insta": None, "focus": "A", "fx": 0}
        for n in ("A Latch", "B Latch"):
            self.state["toggles"][n] = True
        self.events = []           # the last Himmel events, newest last
        self.sent = {}             # "leds" / "text" / "segs" -> the bytes we last sent
        self.sent_t = 0.0          # when we last sent everything (a full refresh every 2 s)
        self.t0 = time.time()
        self.flash = None          # (until, text, bar 0..1 or None): a value shown for a moment
        self.seg_hunt = None       # segment hunt: {"byte", "bit", "auto", "t"} lights one LCD segment bit
        self.contrast = None       # a contrast value (0..99) to send once (not stored in the controller)

    # ---- the LCD: Himmel's page for the NuVJ's segment display ----
    def show(self, text, bar=None, secs=1.5):
        self.flash = (time.time() + secs, text, bar)

    def lcd(self):
        """The LCD as a model that the drawing renders and segments() turns into the device's bytes.
        The glass (owner's photo): text = 8 dot-matrix characters (cmd 01); alpha = 11 16-segment
        characters; bpm = 3 digits ("188") over the word BPM; preset = 2 digits under PRESET (Himmel: the
        deck's bank); last / data = two ramps of 8 steps; lit = words and boxes that are on."""
        st, now = self.state, time.time()
        f = st["focus"]
        if self.flash and now < self.flash[0]:
            text, bar = self.flash[1], self.flash[2]
        elif now - self.t0 < 3.0:
            text, bar = " HIMMEL ", (now - self.t0) / 3.0            # start-up: name + a filling ramp
        else:
            text, bar = "%5.1fBPM" % st["bpm"] if st["bpm"] < 1000 else "BPM", None
        beat = ((now - self.t0) * st["bpm"] / 60.0) % 1.0
        show = getattr(self.live, "show", None)
        alpha = "HIMMEL VJ"
        if show:
            c = show.playing_cell(f)
            if c:
                alpha = (c["effect"] + " " if c.get("effect") else "") + c["name"].rsplit("/", 1)[-1].rsplit(".", 1)[0]
            else:
                alpha = "DECK %s  --" % f
        lit = {"rule", "lbl_bpm", "lbl_preset", "lbl_data", "box_" + f.lower()}
        if st["toggles"]["Master FX"]:
            lit.add("box_c")
        if st["toggles"]["Insta Black"]:
            lit.add("box_d")
        if st["toggles"][f + " Sync"] and beat < 0.5:
            lit.add("lbl_bpm")
        elif st["toggles"][f + " Sync"]:
            lit.discard("lbl_bpm")
        if bar is not None:
            lit.add("lbl_last")
        if now < getattr(self, "enter_until", 0):
            lit.add("enter")                     # "PRESS ENTER" after turning BANK, until it is pushed
        return {
            "text": text,          # text_bytes() makes it 8 characters (7 + "." when longer), as the NuVJ software did
            "alpha": alpha.upper()[:ALPHA_CELLS],
            "bpm": "%3d" % round(st["bpm"]) if st["bpm"] < 200 else "",
            "preset": "%02d" % (st[f]["bank"] + 1),
            "last": None if bar is None else max(0, min(RAMP_STEPS, round(bar * RAMP_STEPS))),
            "data": round(st["xf"] / 16383 * RAMP_STEPS),
            "lit": sorted(lit),
        }

    # ---- input ----
    def on_event(self, name, ev):
        st = self.state
        self.events.append((time.strftime("%H:%M:%S"), name or "?", ev))
        del self.events[:-60]
        if not name:
            return
        k = ev["kind"]
        side = name[0] if name[:2] in ("A ", "B ") else None
        if side:
            st["focus"] = side
        if k == "down":
            if name in TOGGLES:
                st["toggles"][name] = not st["toggles"][name]
                if name == "Insta Black":
                    self.show("BLACKOUT" if st["toggles"][name] else "LIGHTS ON")
                elif name == "Master FX":
                    self.show(FX_NAMES[st["fx"]] if st["toggles"][name] else "FX OFF")
                else:
                    self.show("%s %s" % (name.split()[1].upper(), "ON" if st["toggles"][name] else "OFF"))
            if " Pad " in name:
                n = int(name.split()[-1])
                st[side]["pad"] = n if (st[side]["pad"] != n or not st["toggles"][side + " Latch"]) else None
                self.show("CLIP %s%d-%d" % (side, st[side]["bank"] + 1, n))
            if name == "BPM Tap":
                now = time.time()
                taps = [t for t in st["taps"] if now - t < 3.0] + [now]
                st["taps"] = taps[-6:]
                if len(taps) >= 2:
                    iv = (taps[-1] - taps[0]) / (len(taps) - 1)
                    if iv > 0.2:
                        st["bpm"] = round(60.0 / iv, 1)
                self.show("TAP %5.1f" % st["bpm"])
            if name in ("Insta A", "Insta B"):
                st["insta"] = name[-1]
                st["focus"] = name[-1]
                self.show("ONLY " + name[-1], 0.0 if name[-1] == "A" else 1.0)
            if name.endswith("Scratch"):
                self.show("SCRATCH " + side)
        elif k == "up":
            if " Pad " in name and not st["toggles"][side + " Latch"]:
                st[side]["pad"] = None           # latch off: the clip plays while the pad is held
            if name in ("Insta A", "Insta B"):
                st["insta"] = None
        elif k == "delta":
            if name == "Master FX Select":
                st["fx"] = (st["fx"] + (1 if ev["delta"] > 0 else -1)) % len(FX_NAMES)
                self.show(FX_NAMES[st["fx"]], st["fx"] / (len(FX_NAMES) - 1))
            elif name.endswith("Bank"):
                self.enter_until = time.time() + 3.0
                st[side]["bank"] = (st[side]["bank"] + (1 if ev["delta"] > 0 else -1)) % 16
                self.show("BANK %s%2d" % (side, st[side]["bank"] + 1), st[side]["bank"] / 15)
            elif name.endswith("Jog"):
                st[side]["jog"] += ev["delta"]
                scr = self.live.held_now(side + " Scratch")
                self.show(("SCR%+5d" if scr else "NUDG%+4d") % (st[side]["jog"] if scr else max(-99, min(99, ev["delta"] * 10))),
                          (st[side]["jog"] % 128) / 127)
        elif k == "value":
            v = ev["value"] / 127
            if name == "Master FX Select":
                st["fx"] = min(len(FX_NAMES) - 1, int(v * len(FX_NAMES)))
                self.show(FX_NAMES[st["fx"]], v)
            else:
                short = {"FX Level": "FX LEVEL", "Master PMTR1": "FX PAR 1", "Master PMTR2": "FX PAR 2",
                         "Contrast": "CONTRAST", "Bright": "BRIGHT", "Auto Fade Speed": "FADE SPD"}.get(name)
                self.show(short or (name.split()[1].replace("PMTR", "PAR ") + " " + name[0]), v)
        elif k == "fader":
            v = ev["value"] / 16383
            if name == "Crossfader":
                st["xf"] = ev["value"]
                self.show("XF %3d%%" % round(v * 100), v, secs=0.8)
            else:
                self.show("SPD%s%+.1f" % (name[0], (v - 0.5) * 4), v)
        elif k == "switch":
            c = self.live.control(name)
            pos = ev["pos"]
            p = c["positions"][pos] if c and 0 <= pos < len(c["positions"]) else "?"
            self.show(("MODE " if name == "Fader Mode" else "FSTART ") + p)
        if name.endswith("Bank Push") and k == "down":
            self.enter_until = 0
            self.show("LOAD %s%02d" % (side, st[side]["bank"] + 1))

    # ---- LEDs and LCD out: feedback SysEx ----
    def wanted(self, held):
        """The controls whose LED should be on now."""
        st = self.state
        names = sorted(LED_BITS)
        t = time.time() - self.t0
        beat = (t * st["bpm"] / 60.0) % 1.0
        if self.mode == "off":
            return set()
        if self.mode == "all":
            return set(names)
        if self.mode == "chase":
            return {names[int(t * 6) % len(names)]}
        if self.mode == "follow":
            return {n for n in names if n in held}
        on = {n for n in names if n in held}                 # himmel: what Himmel would show
        on |= {n for n, v in st["toggles"].items() if v and n in LED_BITS}
        for d in ("A", "B"):
            if st[d]["pad"]:
                on.add("%s Pad %d" % (d, st[d]["pad"]))
        if beat < 0.2:
            on.add("BPM Tap")
        if st["insta"]:
            on.add("Insta " + st["insta"])
        return on

    @staticmethod
    def lit_keys(model):
        """Every LCD part the model lights, by the keys the segment hunt learns:
        <digits id>.<i>.<a..g>, alpha.<i>.<16-seg name>, last.<i> / data.<i>, word and box ids."""
        keys = set(model.get("lit") or [])
        for did, n in (("bpm", 3), ("preset", 2)):
            v = model.get(did)
            if v:
                t = str(v).rjust(n)[-n:]
                for i, ch in enumerate(t):
                    keys |= {"%s.%d.%s" % (did, i, sg) for sg in DIGIT_SEGMENTS.get(ch, "")}
        for i, ch in enumerate(str(model.get("alpha") or "").upper()[:ALPHA_CELLS]):
            keys |= {"alpha.%d.%s" % (i, sg) for sg in (SEG16.get(ch, SEG16["?"]).split())}
        for rid in ("last", "data"):
            n = model.get(rid)
            if n is not None:
                keys |= {"%s.%d" % (rid, i) for i in range(n)}
        return keys

    def segments(self, model, m):
        """64 bytes of segment memory for the model: the 2-digit number where the software puts it
        (shown as "big"), the software's two fixed marks, and every element learned by the segment hunt."""
        seg = bytearray(64)
        if self.seg_hunt is not None:
            h = self.seg_hunt
            if h.get("all"):                     # every segment on: a photo shows the whole glass
                return bytes([0x7F] * 64)
            seg[h["byte"]] = 0x7F if h.get("whole") else 1 << h["bit"]
            return bytes(seg)
        learned = m.get("lcd_segments") or {}
        if not any(k.startswith("preset.") for k in learned):
            for b in SEG_ALWAYS:        # until the hunt says otherwise: what the NuVJ software sent
                seg[b] = 1
            number_into(seg, model.get("preset") or "")
        lit = self.lit_keys(model)
        for key, v in learned.items():
            if key in lit or key.split(".")[0] + ".text" == key and key[:-5] in lit:
                for b, mask in seg_bits(v):
                    seg[b] |= mask
        return bytes(seg)

    def tick(self, dev, m, held, force=False):
        """Send what changed: LEDs (8 bytes), LCD text, LCD segments. Everything again every 2 s."""
        if not getattr(dev, "connected", True):
            self.sent.clear()
            return
        now = time.time()
        if not dev.inited:
            dev.receive(feedback(CMD_INIT, INIT_DATA))
            self.sent.clear()
        if force or now - self.sent_t > 2.0:
            self.sent.clear()
            self.sent_t = now
        if self.seg_hunt is not None and self.seg_hunt["auto"] and now - self.seg_hunt["t"] > 1.2:
            h = self.seg_hunt
            h["bit"] += 7 if h.get("whole") else 1
            if h["bit"] > 6:
                h["bit"], h["byte"] = 0, (h["byte"] + 1) % 64
            h["t"] = now
        model = self.lcd()
        if self.seg_hunt is not None:
            h = self.seg_hunt
            model = dict(model, text="ALL SEGS" if h.get("all") else
                         "SEG%02d.%s" % (h["byte"], "*" if h.get("whole") else h["bit"]))
        out = {"leds": led_bytes(self.wanted(held)), "text": text_bytes(model["text"]), "segs": self.segments(model, m)}
        cmds = {"leds": CMD_LEDS, "text": CMD_TEXT, "segs": CMD_SEGMENTS}
        for k, data in out.items():
            if self.sent.get(k) != data:
                dev.receive(feedback(cmds[k], data))
                self.sent[k] = data
        if self.contrast is not None:
            dev.receive(feedback(CMD_CONTRAST, [max(0, min(99, int(self.contrast)))]))
            self.contrast = None

    def let_go(self, dev):
        """LEDs off, the LCD back to the device's own start-up look (as far as we can)."""
        if getattr(dev, "connected", False):
            dev.receive(feedback(CMD_LEDS, bytes(8)))
            dev.receive(feedback(CMD_SEGMENTS, bytes(64)))
            dev.receive(feedback(CMD_TEXT, text_bytes("  NuVJ")))


# ---------------------------------------------------------------------------
# The live object for tools/mk2_live.py
# ---------------------------------------------------------------------------
class NuvjLive:
    weapon = "nuvj"

    def __init__(self, profile):
        self.profile = profile
        self.controls = {c["name"]: c for c in profile["controls"]}
        self.lock = threading.RLock()
        self.m = load_map()
        self.dec = Decoder(self.m)
        self.values = {}           # name -> current value (knob 0..127, fader 0..16383, wheel position, switch pos)
        self.held = set()
        self.last = []
        self.unmapped = []
        self._unmapped = {}
        self.learn = None
        self.learn_msg = ""
        self.show_noise = False
        self.host = NuvjHost(self)
        self.drive = False                     # send init / LEDs / LCD to the real NuVJ (off = listen only)
        self.show = vj_classic.ClassicShow()   # the classic two-deck view's model, played by the GUI and the NuVJ
        self.dev = None
        self.ranges = {}           # name -> [min, max] seen on hardware
        self._use(SimNuVJ(self))
        for n, c in self.controls.items():
            if c["kind"] == "knob":
                self.values[n] = 64 if "PMTR" in n or n in ("Contrast", "Bright") else 0
            elif c["kind"] == "fader":
                self.values[n] = 8192
            elif c["kind"] == "switch":
                self.values[n] = 0
            elif c["kind"] in ("wheel", "encoder"):
                self.values[n] = 0
        threading.Thread(target=self._loop, daemon=True).start()

    def control(self, name):
        return self.controls.get(name)

    def held_now(self, name):
        return name in self.held

    # ---- device plumbing ----
    def _use(self, dev):
        old = self.dev
        self.dev = dev
        dev.listeners.append(self._from_device)
        self.held.clear()
        self.host.sent.clear()
        if old is not None:
            old.listeners.clear()
            try:
                self.host.let_go(old)
            except Exception:
                pass
            old.close()

    def _from_device(self, msg):
        if msg[:1] == b"\xF0":
            return
        with self.lock:
            hw = self.dev.kind == "hw"
            if self.learn and hw and self._learn_from(msg):
                return
            name, ev = self.dec.decode(msg)
            if ev is None:
                return
            if name is None:        # one line per unmapped note / CC / pitch, with how often and which values
                hi = msg[0] & 0xF0
                key = ("note %d" % msg[1] if hi in (0x80, 0x90) else "CC %d" % msg[1] if hi == 0xB0 else "pitch") +                       " ch%d" % ((msg[0] & 15) + 1)
                val = msg[2] if hi != 0xE0 else msg[1] | msg[2] << 7
                u = self._unmapped.setdefault(key, [0, val, val])
                u[0] += 1
                u[1], u[2] = min(u[1], val), max(u[2], val)
                self.unmapped = ["%s ×%d (%d..%d)" % (k, *v) for k, v in self._unmapped.items()][-16:]
                self.host.on_event(None, ev)
                return
            k = ev["kind"]
            if k == "down":
                self.held.add(name)
            elif k == "up":
                self.held.discard(name)
            elif k == "value":
                self.values[name] = ev["value"]
            elif k == "fader":
                self.values[name] = ev["value"]
            elif k == "delta":
                self.values[name] = self.values.get(name, 0) + ev["delta"]
            elif k == "switch":
                self.values[name] = ev["pos"]
            if hw:
                raw = ev.get("raw", msg[2]) if k in ("value", "delta", "switch") else                     ev.get("raw", msg[1] | msg[2] << 7) if k == "fader" else None
                if raw is not None:
                    lo, hi = self.ranges.get(name, [raw, raw])
                    self.ranges[name] = [min(lo, raw), max(hi, raw)]
                e = self.m["controls"].get(name)
                if e and not e.get("seen"):
                    e["seen"] = True
                    self._save_soon()
            self.last = ([name] + [x for x in self.last if x != name])[:6]
            self.host.on_event(name, ev)
            self.show.on_event(name, ev, bpm=self.host.state["bpm"])
            self._sync_host()

    # ---- map learning: click a control, then move the real one ----
    def _learn_from(self, msg):
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        name = self.learn
        c = self.controls[name]
        e = None
        if hi == 0x90 and msg[2] > 0 and c["kind"] in ("button", "pad", "push"):
            e = {"type": "note", "ch": ch, "num": msg[1]}
        elif c["kind"] == "switch" and hi in (0x80, 0x90, 0xB0):
            e = self._learn_switch(name, c, msg)
            if e is None:
                return True
        elif hi == 0xB0 and c["kind"] in ("knob", "encoder", "wheel"):
            e = {"type": "cc", "ch": ch, "num": msg[1]}
            endless = c["kind"] != "knob" or msg[2] in (1, 2, 3, 125, 126, 127)
            if not endless:
                e["enc"] = "abs"
            else:   # endless: the owner turns it clockwise; the first value tells the direction
                e["enc"] = "relabs" if c["kind"] == "knob" else "rel"
                e["cw"] = "pos" if 0 < msg[2] < 64 else "neg"
        elif hi == 0xE0 and c["kind"] == "fader":
            e = {"type": "pb", "ch": ch, "num": None}
        elif hi == 0xB0 and c["kind"] == "fader":
            e = {"type": "cc", "ch": ch, "num": msg[1], "enc": "fader"}
        if e is None:
            self.learn_msg = "ignored %s while learning %s (wrong kind of message)" % (describe(msg), name)
            return True
        # a message can belong to one control only
        for other, oe in self.m["controls"].items():
            if other != name and oe.get("type") == e["type"] and oe.get("ch") == e["ch"] and oe.get("num") == e["num"]:
                oe.update({"type": None, "learned": False, "seen": False})
                self.learn_msg = "%s moved from %s" % (describe(msg), other)
        e.update(learned=True, seen=True)
        self.m["controls"][name] = e
        self.dec.set_map(self.m)
        self.learn_msg = (self.learn_msg + "; " if self.learn_msg.startswith("note") or "moved" in self.learn_msg else "") + \
            "learned %s = %s" % (name, describe(msg))
        self.learn = None
        self.m["verified"] = False
        save_map(self.m)
        return True

    def _learn_switch(self, name, c, msg):
        """Switches: step through every position; each position's value is recorded."""
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        sw = self._switch_learn
        typ = "cc" if hi == 0xB0 else "note"
        val = msg[2] if hi != 0x80 else 0
        if sw.get("name") != name:
            sw.clear()
            sw.update(name=name, type=typ, ch=ch, num=msg[1], vals=[])
        if (typ, ch, msg[1]) != (sw["type"], sw["ch"], sw["num"]):
            return None
        if val not in sw["vals"]:
            sw["vals"].append(val)
        n = len(c["positions"])
        self.learn_msg = "%s: %d of %d positions seen (values %s). Move it %s." % (
            name, len(sw["vals"]), n, sw["vals"], "on" if len(sw["vals"]) < n else "")
        if len(sw["vals"]) < n:
            return None
        e = {"type": typ, "ch": ch, "num": sw["num"], "enc": "switch" if typ == "cc" else "toggle", "values": sw["vals"]}
        sw.clear()
        return e

    _switch_learn = {}

    def _save_soon(self):
        self._save_due = time.time() + 1.0

    _save_due = 0

    # ---- housekeeping: LED host + saving ----
    def _loop(self):
        while True:
            time.sleep(1 / 30)
            try:
                with self.lock:
                    if self._save_due and time.time() > self._save_due:
                        self._save_due = 0
                        save_map(self.m)
                    self.show.tick()
                    self._sync_host()
                    if self.dev.kind == "sim" or self.drive:   # the real NuVJ: listen only until the owner says so
                        self.host.tick(self.dev, self.m, set(self.held))
            except Exception as ex:   # never let the LED loop die
                self.learn_msg = "host error: %s" % ex

    def _sync_host(self):
        """The show model is the truth for decks and master; the host shows it on the LEDs and LCD."""
        st, sh = self.host.state, self.show
        for d in "AB":
            dk = sh.decks[d]
            st[d]["pad"], st[d]["bank"] = dk["playing"], dk["bank"]
            for key in ("Latch", "Auto", "Sync"):
                st["toggles"]["%s %s" % (d, key)] = dk[key.lower()]
        m = sh.master
        st["toggles"]["Master FX"], st["toggles"]["Insta Black"] = m["on"], m["black"]
        st["xf"], st["insta"] = int(m["xf"] * 16383), m["insta"]
        st["bpm"] = m["bpm"]
        names = [e["name"] for e in vj_classic.EFFECTS]
        if m["fx"] in names:
            st["fx"] = names.index(m["fx"])

    # ---- the live view API ----
    def snapshot(self):
        with self.lock:
            dev = self.dev
            hw = dev.kind == "hw"
            mc = self.m["controls"]
            mapped = [n for n, e in mc.items() if e.get("type")]
            seen = [n for n, e in mc.items() if e.get("seen")]
            log = [e[:4] for e in dev.log if self.show_noise or not e[4]][-22:]
            leds = {n: 127 for n in leds_from_bytes(dev.led_state)}   # what was really sent
            st = self.host.state
            return {
                "weapon": "nuvj",
                "simulated": not hw, "hardware": hw,
                "connected": getattr(dev, "connected", True),
                "ports": list(getattr(dev, "port_names", ("", ""))),
                "port_list": self._ports() if hw else None,
                "notes": dev.notes,
                "held": sorted(self.held),
                "values": dict(self.values),
                "ranges": dict(self.ranges),
                "leds": leds,
                "led_preview": hw,
                "lcd": self.host.lcd(),
                "lcd_wire": {"text": dev.lcd_text, "number": number_from(dev.lcd_segs), "segs": dev.lcd_segs.hex(),
                             "contrast": dev.contrast, "inited": dev.inited},
                "seg_hunt": self.host.seg_hunt,
                "drive": self.drive,
                "lcd_segments": {k: seg_bits(v) for k, v in (self.m.get("lcd_segments") or {}).items()},
                "lcd_bytes_lit": list(self.m.get("lcd_bytes_lit") or []),
                "host": {"mode": self.host.mode, "modes": list(NuvjHost.MODES), "bpm": st["bpm"],
                         "A": dict(st["A"]), "B": dict(st["B"]), "toggles": dict(st["toggles"]),
                         "xf": st["xf"]},
                "events": [(t, n, ev) for t, n, ev in self.host.events[-14:]],
                "show": self.show.snapshot(),
                "library": self.show.library(),
                "map": {"mapped": mapped, "seen": seen, "total": len(self.controls), "verified": self.m.get("verified", False),
                        "learned": [n for n, e in mc.items() if e.get("learned")],
                        "wire": {n: self._wire(e) for n, e in mc.items()}, "leds": self.m.get("leds")},
                "learn": self.learn, "learn_msg": self.learn_msg,
                "last": self.last, "unmapped": self.unmapped,
                "counts": dict(dev.counts),
                "stats": dict(getattr(dev, "stats", {})),
                "log": log,
            }

    @staticmethod
    def _wire(e):
        t = e.get("type")
        if t == "note":
            return "note %d ch%d" % (e["num"], e["ch"] + 1) + (" (%s)" % "/".join(map(str, e["values"])) if e.get("values") else "")
        if t == "cc":
            return "CC %d ch%d %s" % (e["num"], e["ch"] + 1, e.get("enc", "abs") +
                                     (" cw=" + e.get("cw", "pos") if e.get("enc") == "rel" else "") +
                                     (" " + "/".join(map(str, e["values"])) if e.get("values") else ""))
        if t == "pb":
            return "pitch ch%d" % (e["ch"] + 1)
        return "-"

    def _ports(self):
        try:
            import lightpad_hw as W
            ins, outs = W.list_ports()
            return {"in": ins, "out": outs}
        except Exception:
            return None

    def sim_input(self, i):
        """Mouse input on the drawn panel -> the bytes the device would send."""
        name = i.get("name")
        if i.get("release_all"):
            for n in list(self.held):
                e = self.m["controls"].get(n)
                if e and e.get("type") == "note":
                    self.dev.emit(encode(e, down=False))
            return
        c = self.controls.get(name)
        e = self.m["controls"].get(name)
        if not c or not e or not e.get("type") or self.dev.kind != "sim":
            return
        k = c["kind"]
        if k in ("button", "pad", "push") and "down" in i:
            if bool(i["down"]) != (name in self.held):
                self.dev.emit(encode(e, down=bool(i["down"])))
        elif k == "knob":
            v = self.values.get(name, 0)
            nv = max(0, min(127, int(i["value"]) if "value" in i else v + int(i.get("delta", 0))))
            if nv != v and e.get("enc") == "relabs":     # endless knob: send ticks
                n = int(round((nv - v) / e.get("step", 2))) or (1 if nv > v else -1)
                self.dev.emit(encode(e, delta=n))
            elif nv != v:
                self.dev.emit(encode(e, value=nv))
        elif k in ("encoder", "wheel") and i.get("delta"):
            self.dev.emit(encode(e, delta=int(i["delta"])))
        elif k == "fader" and "value" in i:
            self.dev.emit(encode(e, value=int(i["value"])))
        elif k == "switch" and "pos" in i:
            self.dev.emit(encode(e, value=int(i["pos"]) % len(c["positions"])))

    def command(self, d):
        with self.lock:
            if "sim_input" in d:
                self.sim_input(d["sim_input"])
            elif "drive" in d:         # the real NuVJ: start / stop sending feedback
                self.drive = bool(d["drive"])
                self.host.sent.clear()
                if not self.drive:
                    self.host.let_go(self.dev)
            elif "show" in d:          # the classic two-deck view
                self.show.command(d["show"] or {})
                self._sync_host()
            elif d.get("led_mode") in NuvjHost.MODES:
                self.host.mode = d["led_mode"]
            elif "seg_hunt" in d:      # light one LCD segment bit at a time
                h = d["seg_hunt"]
                if h is None:
                    self.host.seg_hunt = None
                else:
                    old = self.host.seg_hunt or {"byte": 0, "bit": 0, "auto": False}
                    pos = old["byte"] * 7 + old["bit"]
                    if "byte" in h:
                        pos = int(h["byte"]) * 7 + int(h.get("bit", 0))
                    pos = (pos + int(h.get("step", 0))) % (64 * 7)
                    whole = bool(h.get("whole", old.get("whole", False)))
                    if whole and "step" in h:          # whole bytes: step a byte at a time
                        pos = (old["byte"] + (1 if int(h["step"]) > 0 else -1)) % 64 * 7
                    self.host.seg_hunt = {"byte": pos // 7, "bit": 0 if whole else pos % 7, "whole": whole,
                                          "all": bool(h.get("all", False)),
                                          "auto": bool(h.get("auto", old["auto"])), "t": time.time()}
            elif "seg_assign" in d:    # the owner clicked the element that lit: learn its bit
                key, h = str(d["seg_assign"])[:40], self.host.seg_hunt
                if not h:
                    pass
                elif h.get("whole"):
                    lit = self.m.setdefault("lcd_bytes_lit", [])
                    if h["byte"] not in lit:
                        lit.append(h["byte"])
                        lit.sort()
                    self.learn_msg = "byte %d lights something (%s); go through it bit by bit later" % (h["byte"], key)
                    if self.dev.kind == "hw":
                        save_map(self.m)
                elif self.dev.kind != "hw":
                    self.learn_msg = "segment hunt on the simulator: nothing is saved (only the real NuVJ teaches segments)"
                else:
                    segs = self.m.setdefault("lcd_segments", {})
                    pair = [h["byte"], 1 << h["bit"]]
                    for k2 in list(segs):                  # this bit belongs to one part only
                        bits = [b for b in seg_bits(segs[k2]) if b != pair]
                        if bits:
                            segs[k2] = bits
                        else:
                            del segs[k2]
                    segs[key] = seg_bits(segs.get(key)) + [pair]   # a part may light from several bits
                    h["auto"] = False
                    self.learn_msg = "LCD part %s += byte %d bit %d (%d bit(s) now)" % (key, h["byte"], h["bit"], len(segs[key]))
                    save_map(self.m)
            elif d.get("seg_forget"):  # the hunt: forget what the current bit is saved as
                h = self.host.seg_hunt
                if h and self.dev.kind == "hw":
                    segs = self.m.setdefault("lcd_segments", {})
                    pair, gone = [h["byte"], 1 << h["bit"]], []
                    for k in list(segs):
                        bits = seg_bits(segs[k])
                        if pair in bits:
                            gone.append(k)
                            bits.remove(pair)
                            if bits:
                                segs[k] = bits
                            else:
                                del segs[k]
                    self.learn_msg = "forgot %s" % (", ".join(gone) or "nothing")
                    save_map(self.m)
            elif "contrast" in d:      # LCD contrast 0..99, not stored in the controller
                self.host.contrast = max(0, min(99, int(d["contrast"])))
            elif d.get("identity"):
                if hasattr(self.dev, "identity"):
                    self.dev.identity()
                    self.learn_msg = "identity request sent; an answer shows in the MIDI monitor (SysEx)"
            elif d.get("leds_ok"):
                self.m["leds"] = {"checked": time.strftime("%Y-%m-%d")}
                save_map(self.m)
            elif "show_noise" in d:
                self.show_noise = bool(d["show_noise"])
            elif "hardware" in d:
                self.drive = False
                self._use(HwNuVJ(d.get("port")) if d["hardware"] else SimNuVJ(self))
                self.ranges.clear()
            elif "learn" in d:
                n = d["learn"]
                self.learn = n if n in self.controls and self.dev.kind == "hw" else None
                self._switch_learn.clear()
                c = self.controls.get(n or "")
                self.learn_msg = ("LEARN %s: %s it on the NuVJ" % (n, {
                    "knob": "turn", "encoder": "turn (clockwise)", "wheel": "turn (clockwise)", "fader": "move",
                    "switch": "move through every position of", "pad": "hit"}.get(c["kind"], "press"))) if self.learn else ""
            elif d.get("reset_seen"):
                for e in self.m["controls"].values():
                    e["seen"] = False
                self.m["verified"] = False
                save_map(self.m)
            elif d.get("verify"):
                if all(e.get("seen") for e in self.m["controls"].values()):
                    self.m["verified"] = time.strftime("%Y-%m-%d")
                    save_map(self.m)

    def close(self):
        with self.lock:
            try:
                self.host.seg_hunt = None
                self.host.let_go(self.dev)
            except Exception:
                pass
            if self._save_due:
                save_map(self.m)
            self.dev.close()


# ---------------------------------------------------------------------------
def selftest():
    prof = load_profile()
    names = {c["name"] for c in prof["controls"]}
    assert names == set(SOURCE_TABLE), ("profile and table differ", names ^ set(SOURCE_TABLE))
    m = default_map()
    dec = Decoder(m)
    seen_keys = set()
    for n, e in m["controls"].items():
        key = (e["type"], e["ch"], e["num"])
        assert key not in seen_keys, ("duplicate wire key", n, key)
        seen_keys.add(key)
    for n, e in m["controls"].items():
        c = next(c for c in prof["controls"] if c["name"] == n)
        k = c["kind"]
        if k in ("button", "pad", "push"):
            assert dec.decode(encode(e, down=True)) == (n, {"kind": "down", "vel": 127}), n
            assert dec.decode(encode(e, down=False))[1]["kind"] == "up", n
        elif k == "knob" and e.get("enc") == "relabs":     # endless knob: ticks add up to a 0..127 value
            msgs = split_midi(encode(e, delta=3))
            assert [dec.decode(x)[1]["value"] for x in msgs] == [66, 68, 70], n
            assert dec.decode(encode(e, delta=-1))[1]["value"] == 68, n
        elif k == "knob":
            assert dec.decode(encode(e, value=99)) == (n, {"kind": "value", "value": 99}), n
        elif k in ("encoder", "wheel"):
            msgs = split_midi(encode(e, delta=3))
            assert len(msgs) == 3 and all(dec.decode(x) == (n, {"kind": "delta", "delta": 1}) for x in msgs), n
            assert dec.decode(split_midi(encode(e, delta=-1))[0]) == (n, {"kind": "delta", "delta": -1}), n
        elif k == "fader":
            assert dec.decode(encode(e, value=12345)) == (n, {"kind": "fader", "value": 12345}), n
        elif k == "switch":
            for p in range(len(c["positions"])):
                assert dec.decode(encode(e, value=p))[1]["pos"] == p, (n, p)
    # the live object: simulated presses come back as held controls and light LEDs in "follow" mode
    live = NuvjLive(prof)
    live.command({"led_mode": "follow"})
    live.command({"sim_input": {"name": "A Pad 5", "down": True}})
    live.command({"sim_input": {"name": "Crossfader", "value": 16383}})
    live.command({"sim_input": {"name": "B Jog", "delta": 4}})
    time.sleep(0.15)
    s = live.snapshot()
    assert "A Pad 5" in s["held"] and s["leds"].get("A Pad 5") == 127, s["held"]
    assert s["values"]["Crossfader"] == 16383 and s["values"]["B Jog"] == 4
    live.command({"sim_input": {"name": "A Pad 5", "down": False}})
    time.sleep(0.15)
    assert "A Pad 5" not in live.snapshot()["leds"]
    # the feedback protocol: the exact bytes the NuVJ software sends, and round trips
    assert feedback(CMD_INIT, INIT_DATA).hex(" ") == "f0 00 01 3f 7f 20 60 00 04 01 01 01 00 f7"
    assert parse_feedback(feedback(CMD_TEXT, text_bytes("HELLO"))) == (CMD_TEXT, b"HELLO   ")
    assert text_bytes("BRIGHTNESS") == b"BRIGHTN."
    assert len(LED_BITS) == 31 and len(set(LED_BITS.values())) == 31
    assert leds_from_bytes(led_bytes({"A Pad 7", "B Sync", "Insta Black"})) == {"A Pad 7", "B Sync", "Insta Black"}
    assert led_bytes({"A Pad 7"})[0] == 0x08 and led_bytes({"B Pad 3"})[6] == 0x08
    for v in range(100):
        seg = bytearray(64)
        number_into(seg, v)
        assert number_from(seg) == "%02d" % v, v
    seg = bytearray(64)
    number_into(seg, 7)        # the software's own bytes for 07: tens 0 = 0a 1b, units 7 = 0a 01
    assert (seg[0x3D], seg[0x3E], seg[0x3B], seg[0x3C]) == (0x0A, 0x1B, 0x0A, 0x01)
    # the simulated device decodes what the host sends: LEDs and LCD
    s = live.snapshot()
    assert s["lcd_wire"]["inited"] and s["lcd_wire"]["text"] == text_bytes(s["lcd"]["text"]).decode()
    live.command({"led_mode": "all"})
    time.sleep(0.15)
    assert len(live.snapshot()["leds"]) == 31
    live.close()
    print("nuvj selftest ok: %d controls, every message round-trips, feedback SysEx (LEDs, LCD) matches the NuVJ software" % len(names))


def main():
    if "--selftest" in sys.argv:
        selftest()
    elif "--ports" in sys.argv:
        import lightpad_hw as W
        ins, outs = W.list_ports()
        print("MIDI in: ", ins)
        print("MIDI out:", outs)
    else:
        print(__doc__)


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    main()
