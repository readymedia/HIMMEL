# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Ohm64 weapon (weapon no. 8): simulator, hardware link, map readback, LED host
=============================================================================

The Ohm64 (first version, 2009) is a USB-MIDI controller: an 8 x 8 button grid,
16 knobs, 8 faders, a crossfader, 8 mute buttons, XL / XR crossfader buttons,
F1-F6 and a BPM button. 75 of the buttons have a single-colour (blue) LED.
It is class-compliant USB-MIDI and bus powered.

Everything here speaks the device's real MIDI bytes, from the Ohm64 user guide
(see THIRD_PARTY_NOTICES.md):

- SysEx ``F0 00 01 61 02 <cmd> ... F7`` (00 01 61 = Livid, 02 = Ohm64).
  ``04`` sets all 75 LEDs at once (6 LED columns x 14 rows, a 1 bit = off).
  ``07.xx`` are requests: the device reports its own button map (``0B``),
  analog map (``0A``), basic channel, MIDI settings, LED state. So Himmel can
  **read the map from the device** instead of guessing it.
- Buttons send notes (or CCs), analog inputs send 7-bit CCs (or 14-bit CCs or
  pitch bend), all on the basic channel. LEDs also follow incoming notes / CCs
  through the device's LED input map, unless Local Control is on.
- ``BUTTON_INDEX`` / ``ANALOG_INDEX`` / ``LED_CODE``: where each control sits in
  the device's tables (the guide's appendix). Not yet checked on the unit; the
  LED hunt in the live view checks the LED codes one by one.
- ``SimOhm``: a pretend Ohm64 with the device's tables, LEDs, Local Control,
  dimmers and the request/reply SysEx. Its factory note / CC numbers are a guess
  (the guide does not list them); the real ones come from the readback.
- ``HwOhm``: the real device over WinMM (ctypes via lightpad_hw, no new
  dependency). An allow-list: it only ever sends LED SysEx (04), requests (07.xx),
  notes, and CC 27 / 31 / 63 / 122 / 123. Never save (02), factory reset (06),
  remapping (09-0F) or channel-mode changes (omni / mono / poly).
- ``OhmHost``: the live view standing in for Himmel: MIDI -> Himmel events and a
  small 8-layer state shown on the LEDs. On the real device it listens only,
  until the owner lets it drive the LEDs.
- ``OhmLive``: what tools/mk2_live.py serves for ?weapon=ohm64.

Run:   python tools/mk2_live.py --port 8783 --weapon ohm64     then open http://127.0.0.1:8783
Check: python tools/ohm64_sim.py --selftest
Ports: python tools/ohm64_sim.py --ports
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
PROFILE = os.path.join(ROOT, "weapons", "ohm64.weapon.json")
MAP_FILE = os.path.join(ROOT, "ohm64_map.json")
PORT_HINTS = ("ohm64", "ohm 64", "ohm")

# ---------------------------------------------------------------------------
# The device's own tables (Ohm64 user guide, appendix A-C)
# ---------------------------------------------------------------------------
HDR = bytes([0xF0, 0x00, 0x01, 0x61, 0x02])
CMD_SAVE, CMD_LEDS, CMD_RECALL, CMD_FACTORY, CMD_REQUEST = 0x02, 0x04, 0x05, 0x06, 0x07
CMD_SETTINGS, CMD_MAP_LED, CMD_MAP_ANALOG, CMD_MAP_BUTTONS = 0x08, 0x09, 0x0A, 0x0B
CMD_CHANNEL, CMD_MERGE, CMD_ERASE_LED_MAP, CMD_FLIP, CMD_NAK, CMD_ACK = 0x0C, 0x0D, 0x0E, 0x0F, 0x7E, 0x7F
REQ_LEDS, REQ_SNAPSHOT, REQ_SETTINGS, REQ_LED_MAP = 0x04, 0x06, 0x08, 0x09
REQ_ANALOG, REQ_BUTTONS, REQ_CHANNEL, REQ_MERGE, REQ_FLIP = 0x0A, 0x0B, 0x0C, 0x0D, 0x0F
SAFE_REQUESTS = (REQ_LEDS, REQ_SNAPSHOT, REQ_SETTINGS, REQ_ANALOG, REQ_BUTTONS, REQ_CHANNEL, REQ_MERGE, REQ_FLIP)
CMD_NAMES = {0x02: "save settings", 0x04: "LEDs", 0x05: "recall settings", 0x06: "factory reset", 0x07: "request",
             0x08: "MIDI settings", 0x09: "map one LED", 0x0A: "analog map", 0x0B: "button map", 0x0C: "basic channel",
             0x0D: "MIDI merge", 0x0E: "erase LED map", 0x0F: "crossfader flip", 0x7E: "NAK", 0x7F: "ACK"}
CC_BACKLIGHT, CC_LOGO, CC_LOGO_LSB = 27, 31, 63          # 7-bit backlight dimmer, 14-bit (10-bit effective) logo dimmer
CC_LOCAL, CC_ALL_NOTES_OFF = 122, 123
N_BUTTON_SLOTS, N_ANALOG = 88, 25

BUTTON_INDEX = {}            # control name -> index in the 0B button map (88 slots, 81 used)
for _r in range(8):
    for _c in range(8):
        BUTTON_INDEX["Grid %d" % (_r * 8 + _c + 1)] = _c * 8 + _r      # column-major on the wire
BUTTON_INDEX.update({"XL": 64, "XR": 72, "BPM": 87})
for _i, _x in enumerate((65, 73, 66, 74, 67, 75, 68, 76)):
    BUTTON_INDEX["Mute %d" % (_i + 1)] = _x
for _i, _x in enumerate((77, 78, 79, 69, 70, 71)):                        # F1-F6: which F is which is a guess (row-major)
    BUTTON_INDEX["F%d" % (_i + 1)] = _x

ANALOG_INDEX = {"Crossfader": 24}   # control name -> index in the 0A analog map (25 inputs)
for _i, _x in enumerate((23, 22, 15, 14, 5, 7, 6, 4)):
    ANALOG_INDEX["Fader %d" % (_i + 1)] = _x
for _i, _x in enumerate((17, 16, 9, 8, 19, 18, 11, 10, 21, 20, 13, 12, 3, 1, 0, 2)):
    ANALOG_INDEX["Knob %d" % (_i + 1)] = _x


def _led_pos(p):
    """LED position 0.. in the guide's order -> (column 1..6, row 1..14). Row 11 does not exist."""
    return (p % 6 + 1, p // 6 + 1)


LED_CODE = {}                # control name -> (column, row) of its LED; F1-F6 have none
for _k in range(64):
    LED_CODE["Grid %d" % (_k + 1)] = _led_pos(_k if _k < 60 else _k + 6)
LED_CODE["XL"], LED_CODE["XR"] = (5, 12), (6, 12)
for _i in range(8):          # the guide's drawing: mutes 1-8 in order (its 04 text lists another order; the hunt decides)
    LED_CODE["Mute %d" % (_i + 1)] = _led_pos(72 + _i)
LED_CODE["BPM"] = (3, 14)
LED_ORDER = sorted(LED_CODE, key=lambda n: (LED_CODE[n][1], LED_CODE[n][0]))


def cr_byte(c, r):
    return ((c - 1) << 4) | (r - 1)


def sysex(cmd, data=b""):
    return HDR + bytes([cmd]) + bytes(data) + b"\xF7"


def request(sub, data=b""):
    return sysex(CMD_REQUEST, bytes([sub]) + bytes(data))


def parse_sysex(msg):
    """(cmd, data) for an Ohm64 SysEx, else None."""
    msg = bytes(msg)
    if len(msg) >= 7 and msg[:5] == HDR and msg[-1] == 0xF7:
        return msg[5], msg[6:-1]
    return None


ALL_OFF = (0x3FFF,) * 6


def led_words(lit):
    """{(c, r)} lit -> 6 column words, bit r-1 = row r, 1 = off (as the device wants)."""
    w = [0x3FFF] * 6
    for c, r in lit:
        w[c - 1] &= ~(1 << (r - 1))
    return tuple(w)


def words_to_data(words):
    out = bytearray()
    for w in words:
        out += bytes([w & 0x7F, (w >> 7) & 0x7F])
    return bytes(out)


def data_to_words(data):
    if len(data) < 12:
        return None
    return tuple(data[2 * i] | data[2 * i + 1] << 7 for i in range(6))


def lit_codes(words):
    return {(c + 1, r + 1) for c in range(6) for r in range(14) if r != 10 and not (words[c] >> r) & 1}


def lit_names(words, codes=None):
    codes = codes or LED_CODE
    on = lit_codes(words)
    return {n for n, cr in codes.items() if tuple(cr) in on}


# ---------------------------------------------------------------------------
# Maps: the device's 0B / 0A tables <-> Himmel's per-control wire entries
# ---------------------------------------------------------------------------
def guess_button_table():
    """The simulator's factory buttons (a guess: the guide does not list them): grid = notes 0..63 row-major
    (the guide's LED example puts note 10 on the 11th button), the other buttons = their index as note."""
    t = [(0x7F, 0x7F)] * N_BUTTON_SLOTS
    for n, i in BUTTON_INDEX.items():
        t[i] = (int(n.split()[1]) - 1, 0) if n.startswith("Grid") else (i, 0)
    return t


def guess_analog_table():
    """The simulator's factory analog map (a guess): input index = CC number + 1 (CC 0 is bank select)."""
    return [(i + 1, 0) for i in range(N_ANALOG)]


def entry_from_button(ll, hh, ch):
    if hh == 0:
        return {"type": "note", "ch": ch, "num": ll}
    if hh == 1 and ll <= 0x78:
        return {"type": "cc", "ch": ch, "num": ll}
    if hh == 1 and ll in (0x7A, 0x7B, 0x7C):
        return {"type": "mmc", "ch": ch, "num": ll}
    return {"type": None, "ch": ch, "num": None}


def entry_from_analog(ll, hh, ch):
    if hh == 0 and ll <= 0x78:
        return {"type": "cc", "ch": ch, "num": ll}
    if hh == 1 and 0x60 <= ll <= 0x6F:
        return {"type": "pb", "ch": ll - 0x60, "num": None}
    if hh == 1 and ll <= 0x1F:
        return {"type": "cc14", "ch": ch, "num": ll}
    return {"type": None, "ch": ch, "num": None}


def controls_from_tables(buttons, analog, ch):
    out = {}
    for n, i in BUTTON_INDEX.items():
        out[n] = entry_from_button(buttons[i][0], buttons[i][1], ch)
    for n, i in ANALOG_INDEX.items():
        out[n] = entry_from_analog(analog[i][0], analog[i][1], ch)
    return out


def table_pairs(data, n):
    return [(data[2 * i], data[2 * i + 1]) for i in range(n)] if len(data) >= 2 * n else None


def load_profile():
    with open(PROFILE, encoding="utf-8") as f:
        return json.load(f)


def default_map():
    ctl = controls_from_tables(guess_button_table(), guess_analog_table(), 0)
    return {"format": "himmel-map/1", "weapon": "ohm64", "verified": False,
            "source": "the simulator's guess; replaced by the device's own map (Read the map) and by learning",
            "controls": {n: dict(e, learned=False, seen=False) for n, e in ctl.items()},
            "device": None,
            "leds": {"codes": {n: list(cr) for n, cr in LED_CODE.items()}, "hunted": [], "checked": False}}


def load_map():
    try:
        with open(MAP_FILE, encoding="utf-8") as f:
            m = json.load(f)
        base = default_map()
        for n, e in base["controls"].items():
            m["controls"].setdefault(n, e)
        m.setdefault("device", None)
        if not isinstance(m.get("leds"), dict) or "codes" not in m["leds"]:
            m["leds"] = base["leds"]
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
# MIDI codec
# ---------------------------------------------------------------------------
def encode(e, down=None, value=None):
    """Bytes the device sends for one control. Buttons: down True/False. Analog: value 0..127."""
    ch = e["ch"] & 15
    t = e.get("type")
    if t == "note":
        return bytes([0x90 | ch, e["num"], 0x7F if down else 0])     # release = note on, velocity 0 (unverified)
    if t == "cc":
        if down is not None:
            return bytes([0xB0 | ch, e["num"], 0x7F if down else 0])
        return bytes([0xB0 | ch, e["num"], max(0, min(127, value))])
    if t == "cc14":
        v = max(0, min(127, value)) << 7
        return bytes([0xB0 | ch, e["num"], v >> 7, 0xB0 | ch, e["num"] + 32, v & 0x7F])
    if t == "pb":
        v = max(0, min(127, value)) << 7
        return bytes([0xE0 | ch, v & 0x7F, v >> 7])
    if t == "mmc" and down:
        return bytes([0xF0, 0x7F, 0x7F, 0x06, {0x7A: 2, 0x7B: 3, 0x7C: 1}[e["num"]], 0xF7])
    return b""


def split_midi(data):
    out, i = [], 0
    while i < len(data):
        st = data[i]
        if st == 0xF0:
            j = data.index(0xF7, i) + 1
            out.append(bytes(data[i:j]))
            i = j
            continue
        n = 1 if st >= 0xF8 else 2 if (st & 0xF0) in (0xC0, 0xD0) else 3
        out.append(bytes(data[i:i + n]))
        i += n
    return out


def describe(msg):
    if not msg:
        return ""
    st = msg[0]
    if st == 0xF0:
        p = parse_sysex(msg)
        if p:
            cmd, data = p
            name = CMD_NAMES.get(cmd, "cmd %02X" % cmd)
            if cmd == CMD_REQUEST and data:
                return "Ohm64 request %02X (%s)" % (data[0], CMD_NAMES.get(data[0], "?"))
            if cmd == CMD_LEDS:
                w = data_to_words(data)
                return "Ohm64 LEDs: %d lit" % len(lit_codes(w)) if w else "Ohm64 LEDs (short)"
            return "Ohm64 %s (%d bytes)" % (name, len(data))
        return "SysEx %d bytes" % len(msg)
    hi, ch = st & 0xF0, (st & 15) + 1
    if hi == 0x90 and len(msg) > 2:
        return "note on  ch%d  %3d vel %3d" % (ch, msg[1], msg[2])
    if hi == 0x80 and len(msg) > 2:
        return "note off ch%d  %3d" % (ch, msg[1])
    if hi == 0xB0 and len(msg) > 2:
        return "CC       ch%d  %3d = %3d" % (ch, msg[1], msg[2])
    if hi == 0xE0 and len(msg) > 2:
        return "pitch    ch%d  %5d" % (ch, msg[1] | msg[2] << 7)
    return "status %02X" % st


class Decoder:
    """MIDI from the device -> (control name, event) using the current map."""

    def __init__(self, m, kinds):
        self.kinds = kinds
        self.set_map(m)

    def set_map(self, m):
        self.m = m
        self.idx = {}
        self.lsb = set()
        for name, e in m["controls"].items():
            t = e.get("type")
            if t == "pb":
                self.idx[("pb", e["ch"], None)] = name
            elif t == "cc14":
                self.idx[("cc", e["ch"], e["num"])] = name
                self.lsb.add((e["ch"], e["num"] + 32))
            elif t:
                self.idx[(t, e["ch"], e["num"])] = name

    def decode(self, msg):
        """(name or None, event). Events: down, up (buttons), value 0..127 (knobs, faders), lsb (ignored)."""
        if len(msg) < 3:
            return None, None
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        if hi in (0x80, 0x90):
            name = self.idx.get(("note", ch, msg[1]))
            down = hi == 0x90 and msg[2] > 0
            return name, {"kind": "down" if down else "up", "vel": msg[2] if down else 0}
        if hi == 0xB0:
            if (ch, msg[1]) in self.lsb:
                return None, {"kind": "lsb"}
            name = self.idx.get(("cc", ch, msg[1]))
            if name and self.kinds.get(name) == "button":
                return name, {"kind": "down" if msg[2] else "up", "vel": msg[2]}
            return name, {"kind": "value", "value": msg[2]}
        if hi == 0xE0:
            name = self.idx.get(("pb", ch, None))
            v = msg[1] | msg[2] << 7
            return name, {"kind": "value", "value": v >> 7, "raw": v}
        return None, None


# ---------------------------------------------------------------------------
# Devices
# ---------------------------------------------------------------------------
class DeviceBase:
    kind = "sim"

    def __init__(self):
        self.lock = threading.RLock()
        self.listeners = []
        self.log = []
        self.counts = {"to_host": 0, "to_device": 0}
        self.led_sent = ALL_OFF      # the last 04 the host sent
        self.connected = True
        self.notes = []

    def _log(self, direction, msg, noisy=False, text=None):
        self.log.append((time.strftime("%H:%M:%S"), direction, bytes(msg).hex(" "), text or describe(msg), noisy))
        del self.log[:-400]

    def emit(self, data):
        for msg in split_midi(bytes(data)):
            with self.lock:
                self.counts["to_host"] += 1
                self._log("device → host", msg, noisy=msg[:1] == b"\xF0" and parse_sysex(msg) is not None
                          and parse_sysex(msg)[0] == CMD_ACK)
            for f in list(self.listeners):
                try:
                    f(msg)
                except Exception:
                    pass

    def note_out(self, msg):
        p = parse_sysex(msg)
        if p and p[0] == CMD_LEDS:
            w = data_to_words(p[1])
            if w:
                self.led_sent = w

    def shown(self):
        """The LED words the device shows (the sim knows; for the real one: what was sent)."""
        return self.led_sent

    def close(self):
        pass


class SimOhm(DeviceBase):
    """A pretend Ohm64: the drawn panel drives it. It keeps the device's tables, LEDs and settings and
    answers the guide's requests, so Himmel's readback and LED code paths run as on the real one."""

    kind = "sim"

    def __init__(self):
        super().__init__()
        self.buttons = guess_button_table()
        self.analog = guess_analog_table()
        self.channel = 0
        self.local = True            # Local Control: LEDs light under pressed buttons, incoming LED MIDI ignored
        self.omni = True
        self.merge = True
        self.flip = False            # factory: the crossfader value grows towards the LEFT
        self.leds = ALL_OFF
        self.backlight = 127
        self.logo = 1023
        self._logo_msb = 127
        self.values = [0] * N_ANALOG
        self.notes = ["Simulated Ohm64: the drawing is the controller. Its LEDs show what the device would light."]

    def led_map(self):
        """The LED input map: each button's own message lights its own LED (the editor's 'Lock')."""
        m = {}
        for n, i in BUTTON_INDEX.items():
            ll, hh = self.buttons[i]
            if n in LED_CODE and hh in (0, 1):
                m[("note" if hh == 0 else "cc", ll)] = LED_CODE[n]
        return m

    def _set_led(self, cr, on):
        w = list(self.leds)
        if on:
            w[cr[0] - 1] &= ~(1 << (cr[1] - 1))
        else:
            w[cr[0] - 1] |= 1 << (cr[1] - 1)
        self.leds = tuple(w)

    def shown(self):
        return self.leds

    def press(self, name, down):
        """A button on the drawing: the device's own bytes, and its LED under Local Control."""
        i = BUTTON_INDEX[name]
        e = entry_from_button(self.buttons[i][0], self.buttons[i][1], self.channel)
        if self.local and name in LED_CODE:
            with self.lock:
                self._set_led(LED_CODE[name], down)
        self.emit(encode(e, down=down))

    def move(self, name, value):
        i = ANALOG_INDEX[name]
        e = entry_from_analog(self.analog[i][0], self.analog[i][1], self.channel)
        self.values[i] = value
        self.emit(encode(e, value=value))

    def receive(self, msg):
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            p = parse_sysex(msg)
            self._log("host → device", msg, noisy=bool(p and p[0] == CMD_LEDS))
            self.note_out(msg)
        if p:
            self._sysex(*p)
            return
        if len(msg) < 3:
            return
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        if not self.omni and ch != self.channel:
            return
        if hi == 0xB0 and msg[1] == CC_BACKLIGHT:
            self.backlight = msg[2]
        elif hi == 0xB0 and msg[1] == CC_LOGO:
            self._logo_msb = msg[2]
            self.logo = msg[2] << 3
        elif hi == 0xB0 and msg[1] == CC_LOGO_LSB:
            self.logo = (self._logo_msb << 7 | msg[2]) >> 4
        elif hi == 0xB0 and msg[1] == CC_LOCAL:
            self.local = msg[2] >= 64
        elif hi == 0xB0 and msg[1] == CC_ALL_NOTES_OFF:
            self.leds = ALL_OFF
        elif hi in (0x80, 0x90, 0xB0) and not self.local:
            cr = self.led_map().get(("cc" if hi == 0xB0 else "note", msg[1]))
            if cr:
                with self.lock:
                    self._set_led(cr, hi != 0x80 and msg[2] > 0)

    def _sysex(self, cmd, data):
        if cmd == CMD_LEDS:
            w = data_to_words(data)
            if w:
                self.leds = w
            self.emit(sysex(CMD_ACK))
        elif cmd == CMD_REQUEST and data:
            sub = data[0]
            if sub == REQ_LEDS:
                self.emit(sysex(CMD_LEDS, words_to_data(self.leds)))
            elif sub == REQ_BUTTONS:
                self.emit(sysex(CMD_MAP_BUTTONS, b"".join(bytes(p) for p in self.buttons)))
            elif sub == REQ_ANALOG:
                self.emit(sysex(CMD_MAP_ANALOG, b"".join(bytes(p) for p in self.analog)))
            elif sub == REQ_CHANNEL:
                self.emit(sysex(CMD_CHANNEL, bytes([self.channel])))
            elif sub == REQ_MERGE:
                self.emit(sysex(CMD_MERGE, bytes([int(self.merge)])))
            elif sub == REQ_FLIP:
                self.emit(sysex(CMD_FLIP, bytes([int(self.flip)])))
            elif sub == REQ_SETTINGS:
                self.emit(sysex(CMD_SETTINGS, bytes([(2 if self.omni else 0) | (8 if self.local else 0)])))
            elif sub == REQ_SNAPSHOT:
                for n, i in ANALOG_INDEX.items():
                    self.emit(encode(entry_from_analog(*self.analog[i], self.channel), value=self.values[i]))
        else:
            self.emit(sysex(CMD_ACK))


# The real Ohm64 over WinMM ---------------------------------------------------
def allowed_to_device(msg):
    """The allow-list for the real device. Returns (ok, why)."""
    if not msg:
        return False, "empty"
    if msg[0] == 0xF0:
        p = parse_sysex(msg)
        if not p:
            return False, "SysEx that is not an Ohm64 message"
        cmd, data = p
        if cmd == CMD_LEDS and len(data) == 12:
            return True, ""
        if cmd == CMD_REQUEST and data and data[0] in SAFE_REQUESTS:
            return True, ""
        return False, "Ohm64 %s: Himmel never changes the device's stored settings or map" % CMD_NAMES.get(cmd, "cmd %02X" % cmd)
    hi = msg[0] & 0xF0
    if hi in (0x80, 0x90) and len(msg) == 3:
        return True, ""
    if hi == 0xB0 and len(msg) == 3 and msg[1] <= 0x78:
        return True, ""
    if hi == 0xB0 and len(msg) == 3 and msg[1] in (CC_LOCAL, CC_ALL_NOTES_OFF):
        return True, ""
    return False, "not on the allow-list"


class HwOhm(DeviceBase):
    kind = "hw"

    def __init__(self, port_hint=None):
        super().__init__()
        import lightpad_hw as W
        self.W = W
        self.mm = W.winmm()
        self.mm.midiOutShortMsg.argtypes = [W.wintypes.HANDLE, W.wintypes.DWORD]
        self.port_hint = port_hint
        self.connected = False
        self.has_out = False
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
        self.notes = ["Real Ohm64: looking for its MIDI port…"]
        threading.Thread(target=self._loop, daemon=True).start()

    def _pick(self, ports):
        if self.port_hint is not None:
            if isinstance(self.port_hint, int):
                return self.port_hint if any(k == self.port_hint for k, _ in ports) else None
            return next((k for k, n in ports if self.port_hint.lower() in n.lower()), None)
        ok = [(k, n) for k, n in ports if any(h in n.lower() for h in PORT_HINTS) and "external" not in n.lower()]
        return ok[0][0] if ok else None    # "Ohm64 External" is the DIN MIDI jack, not the controls

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
            self.notes = ["No Ohm64 MIDI port found. Plug it in with USB and close the Livid Editor. "
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
        for _ in range(8):           # SysEx replies: the button map is 183 bytes
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
        self.notes = ["Real Ohm64 on MIDI in %d (%s), out %s. Listening only: touch every control." % (
            i, self.port_names[0], "%d" % o if o is not None else "not opened")]
        with self.lock:
            self._log("probe", b"", text="ports opened: " + " / ".join(self.port_names))
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

    def _long(self, msg, noisy=False):
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
        return r

    def receive(self, msg):
        msg = bytes(msg)
        if not self.connected or not self.has_out or not msg:
            return
        ok, why = allowed_to_device(msg)
        if not ok:
            with self.lock:
                self._log("refused", msg, text="refused: " + why)
            return
        if msg[0] == 0xF0:
            r = self._long(msg)
        else:
            w = msg[0] | msg[1] << 8 | msg[2] << 16
            with self.out_lock:
                r = self.mm.midiOutShortMsg(self.hout, w)
        with self.lock:
            self.counts["to_device"] += 1
            p = parse_sysex(msg)
            self._log("host → device", msg, noisy=bool(p and p[0] == CMD_LEDS) or msg[0] & 0xF0 in (0x80, 0x90))
            self.note_out(msg)
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
            if self.connected and int(now * 2) % 4 == 0:     # a pulled cable: the port disappears
                ins, _ = W.list_ports()
                if not any(n == self.port_names[0] for _, n in ins):
                    with self.lock:
                        self._log("probe", b"", text="port gone (cable pulled?)")
                    self.connected = False


# ---------------------------------------------------------------------------
# The host: Himmel's side. Events -> an 8-layer VJ state -> LEDs (one 04 SysEx per change)
# ---------------------------------------------------------------------------
FONT5 = {"H": "7f0808087f", "I": "00417f4100", "M": "7f020c027f", "E": "7f49494941", "L": "7f40404040",
         " ": "0000000000", "·": "0000180000"}


def scroll_columns(text):
    cols = []
    for ch in text:
        h = FONT5.get(ch, FONT5[" "])
        cols += [int(h[i:i + 2], 16) for i in range(0, 10, 2)] + [0]
    return cols


class OhmHost:
    MODES = ("himmel", "follow", "all", "chase", "text", "off", "hunt")

    def __init__(self, live):
        self.live = live
        self.mode = "himmel"
        self.drive = True            # False = listen only (the read-only probe on the real device)
        self.sent = None
        self.sent_at = 0
        self.events = []
        self.page = 1
        self.clip = [None] * 8       # per layer (grid column): playing clip row, or None
        self.live_layer = [True] * 8
        self.bump = None
        self.taps = []
        self.bpm = 120.0
        self.t0 = time.time()
        self.hunt = {"pos": 0, "auto": False, "t": 0}
        self.saved = {"leds": None, "local": None}   # what the real device had before Himmel drove it
        self.backlight = None
        self.logo = None

    def log(self, name, ev):
        self.events.append((time.strftime("%H:%M:%S"), name, ev))
        del self.events[:-60]

    def on_event(self, name, ev):
        k = ev["kind"]
        if name is None or k == "lsb":
            return
        if name.startswith("Grid") and k == "down":
            n = int(name.split()[1]) - 1
            r, c = divmod(n, 8)
            self.clip[c] = None if self.clip[c] == r else r
            self.log(name, {"himmel": "ClipTrigger", "layer": c + 1, "clip": r + 1, "stop": self.clip[c] is None})
        elif name.startswith("Mute") and k == "down":
            i = int(name.split()[1]) - 1
            self.live_layer[i] = not self.live_layer[i]
            self.log(name, {"himmel": "LayerMute", "layer": i + 1, "live": self.live_layer[i]})
        elif name in ("XL", "XR"):
            self.bump = (name if k == "down" else None)
            self.log(name, {"himmel": "CrossfadeBump", "side": "A" if name == "XL" else "B", "held": k == "down"})
        elif name == "BPM" and k == "down":
            now = time.time()
            self.taps = [t for t in self.taps if now - t < 2.5] + [now]
            if len(self.taps) >= 2:
                gaps = [b - a for a, b in zip(self.taps, self.taps[1:])]
                self.bpm = round(60 / (sum(gaps) / len(gaps)), 1)
            self.t0 = now
            self.log(name, {"himmel": "TapTempo", "bpm": self.bpm})
        elif name.startswith("F") and k == "down":
            self.page = int(name[1:])
            self.log(name, {"himmel": "KnobPage", "page": self.page})
        elif name.startswith("Fader") and k == "value":
            i = int(name.split()[1])
            self.log(name, {"himmel": "LayerOpacity", "layer": i, "value": round(ev["value"] / 127, 3)})
        elif name == "Crossfader" and k == "value":
            ab = ev["value"] / 127
            if not self.live.xf_flip():
                ab = 1 - ab              # factory: the value grows towards the left; Himmel wants 0 = A (left)
            self.log(name, {"himmel": "Crossfade", "ab": round(ab, 3)})
        elif name.startswith("Knob") and k == "value":
            i = int(name.split()[1])
            self.log(name, {"himmel": "Param", "page": self.page, "slot": i, "value": round(ev["value"] / 127, 3)})
        elif k in ("down", "up"):
            self.log(name, {"himmel": "ButtonDown" if k == "down" else "ButtonUp"})

    def xf_side(self):
        v = self.live.values.get("Crossfader", 0)
        ab = v / 127 if self.live.xf_flip() else 1 - v / 127
        return "A" if ab < 1 / 3 else "B" if ab > 2 / 3 else None

    def wanted(self, held, codes):
        mode, now = self.mode, time.time()
        if mode == "off":
            return set()
        if mode == "all":
            return set(codes)
        if mode == "follow":
            return {n for n in held if n in codes}
        if mode == "chase":
            return {LED_ORDER[int(now * 12) % len(LED_ORDER)]}
        if mode == "text":
            cols = scroll_columns("  HIMMEL · ")
            off = int(now * 8)
            on = set()
            for c in range(8):           # 5x7 letters on grid rows 1-7, scrolling right to left
                bits = cols[(off + c) % len(cols)]
                on |= {"Grid %d" % (r * 8 + c + 1) for r in range(7) if bits >> r & 1}
            return on
        if mode == "hunt":
            return set()
        # himmel: the playing clip per layer, live layers on their mute LEDs, crossfader side, BPM on the beat
        on = {"Grid %d" % (r * 8 + c + 1) for c, r in enumerate(self.clip) if r is not None}
        on |= {"Mute %d" % (i + 1) for i, v in enumerate(self.live_layer) if v}
        side = self.bump[1] if self.bump else {"A": "L", "B": "R"}.get(self.xf_side())
        if side:
            on.add("X" + side)
        beat = (now - self.t0) * self.bpm / 60
        if beat % 1 < 0.22:
            on.add("BPM")
        return on

    def frame_words(self, held, codes):
        if self.mode == "hunt":
            c, r = _hunt_code(self.hunt["pos"])
            return led_words({(c, r)})
        return led_words({tuple(codes[n]) for n in self.wanted(held, codes) if n in codes})

    def tick(self, dev, codes, held):
        if not self.drive or not dev.connected:
            return
        now = time.time()
        if self.mode == "hunt" and self.hunt["auto"] and now - self.hunt["t"] > 1.2:
            self.hunt["pos"] = (self.hunt["pos"] + 1) % HUNT_LEN
            self.hunt["t"] = now
        w = self.frame_words(held, codes)
        if w != self.sent or now - self.sent_at > 2.0:      # resend now and then: a replugged device starts dark
            dev.receive(sysex(CMD_LEDS, words_to_data(w)))
            self.sent, self.sent_at = w, now

    def start_driving(self, dev):
        """Before the first LED frame on the real device: ask what it shows and whether Local Control is on,
        then switch Local Control off so only Himmel lights the LEDs (not stored; let_go puts it back)."""
        dev.receive(request(REQ_SETTINGS))
        dev.receive(request(REQ_LEDS))
        dev.receive(bytes([0xB0 | self.live.channel(), CC_LOCAL, 0]))
        self.sent = None
        self.drive = True

    def let_go(self, dev):
        if not self.drive or dev.kind != "hw" or not dev.connected:
            self.drive = dev.kind != "hw"
            return
        dev.receive(sysex(CMD_LEDS, words_to_data(self.saved["leds"] or ALL_OFF)))
        if self.saved["local"] is not False:      # it was on, or unknown: the factory behaviour
            dev.receive(bytes([0xB0 | self.live.channel(), CC_LOCAL, 127]))
        self.drive = False
        self.sent = None


HUNT_CODES = [(c, r) for r in range(1, 15) if r != 11 for c in range(1, 7)]
HUNT_LEN = len(HUNT_CODES)


def _hunt_code(pos):
    return HUNT_CODES[pos % HUNT_LEN]


# ---------------------------------------------------------------------------
class OhmLive:
    weapon = "ohm64"

    def __init__(self, profile):
        self.profile = profile
        self.controls = {c["name"]: c for c in profile["controls"]}
        self.kinds = {n: ("button" if c["kind"] == "button" else "analog") for n, c in self.controls.items()}
        self.lock = threading.RLock()
        self.m = load_map()
        self.dec = Decoder(self.m, self.kinds)
        self.values = {n: 0 for n, c in self.controls.items() if c["kind"] in ("knob", "fader")}
        self.held = set()
        self.last = []
        self.unmapped = []
        self.learn = None
        self.learn_msg = ""
        self.show_noise = False
        self.ranges = {}
        self.readback = {}           # what the device answered to the requests this session
        self.host = OhmHost(self)
        self.dev = None
        self._use(SimOhm())
        threading.Thread(target=self._loop, daemon=True).start()

    def channel(self):
        d = self.m.get("device") or {}
        return d.get("channel", 0) & 15

    def xf_flip(self):
        d = self.readback if "flip" in self.readback else (self.m.get("device") or {})
        return bool(d.get("flip", False))

    def codes(self):
        return {n: tuple(cr) for n, cr in self.m["leds"]["codes"].items()}

    # ---- device plumbing ----
    def _use(self, dev):
        old = self.dev
        if old is not None:
            try:
                self.host.let_go(old)
            except Exception:
                pass
            old.listeners.clear()
            old.close()
        self.dev = dev
        self.readback = {}
        self.host.saved = {"leds": None, "local": None}
        self.host.drive = dev.kind == "sim"      # the real device: listen only until the owner says so
        self.host.sent = None
        dev.listeners.append(self._from_device)
        self.held.clear()

    def _from_device(self, msg):
        with self.lock:
            if msg[:1] == b"\xF0":
                self._sysex_in(msg)
                return
            hw = self.dev.kind == "hw"
            if self.learn and hw and self._learn_from(msg):
                return
            name, ev = self.dec.decode(msg)
            if ev is None or ev["kind"] == "lsb":
                return
            if name is None:
                self.unmapped = ([describe(msg)] + [u for u in self.unmapped if u != describe(msg)])[:12]
                return
            k = ev["kind"]
            if k == "down":
                self.held.add(name)
            elif k == "up":
                self.held.discard(name)
            elif k == "value":
                self.values[name] = ev["value"]
            if hw:
                if k == "value":
                    lo, hi = self.ranges.get(name, [ev["value"], ev["value"]])
                    self.ranges[name] = [min(lo, ev["value"]), max(hi, ev["value"])]
                e = self.m["controls"].get(name)
                if e and not e.get("seen"):
                    e["seen"] = True
                    self._save_soon()
            self.last = ([name] + [x for x in self.last if x != name])[:6]
            self.host.on_event(name, ev)

    def _sysex_in(self, msg):
        p = parse_sysex(msg)
        if not p:
            return
        cmd, data = p
        rb = self.readback
        if cmd == CMD_MAP_BUTTONS:
            rb["buttons"] = table_pairs(data, N_BUTTON_SLOTS)
        elif cmd == CMD_MAP_ANALOG:
            rb["analog"] = table_pairs(data, N_ANALOG)
        elif cmd == CMD_CHANNEL and data:
            rb["channel"] = data[0]
        elif cmd == CMD_MERGE and data:
            rb["merge"] = bool(data[0])
        elif cmd == CMD_FLIP and data:
            rb["flip"] = bool(data[0])
        elif cmd == CMD_SETTINGS and data:
            rb["omni"], rb["local"] = bool(data[0] & 2), bool(data[0] & 8)
            if self.host.saved["local"] is None:
                self.host.saved["local"] = rb["local"]
        elif cmd == CMD_LEDS:
            w = data_to_words(data)
            if w:
                rb["leds"] = w
                if self.host.saved["leds"] is None:
                    self.host.saved["leds"] = w
        elif cmd in (CMD_ACK, CMD_NAK):
            rb["acks" if cmd == CMD_ACK else "naks"] = rb.get("acks" if cmd == CMD_ACK else "naks", 0) + 1
            return
        rb["at"] = time.strftime("%H:%M:%S")
        if self.pending_adopt and rb.get("buttons") and rb.get("analog") and "channel" in rb:
            self.pending_adopt = False
            self._adopt()

    pending_adopt = False

    def read_device(self, adopt=True):
        """Ask the device for its own tables (requests only: nothing on the device changes)."""
        self.pending_adopt = adopt
        for sub in (REQ_CHANNEL, REQ_BUTTONS, REQ_ANALOG, REQ_SETTINGS, REQ_MERGE, REQ_FLIP):
            self.dev.receive(request(sub))

    def _adopt(self):
        """Use the device's answer as the map. Learned entries stay learned; 'seen' stays where the wire is unchanged."""
        rb = self.readback
        new = controls_from_tables(rb["buttons"], rb["analog"], rb["channel"])
        changed = 0
        for n, e in new.items():
            old = self.m["controls"].get(n, {})
            if old.get("learned"):
                continue
            same = all(old.get(k) == e.get(k) for k in ("type", "ch", "num"))
            changed += not same
            self.m["controls"][n] = dict(e, learned=False, seen=old.get("seen", False) and same)
        self.m["device"] = {"read": time.strftime("%Y-%m-%d %H:%M"), "from": self.dev.kind, "channel": rb["channel"],
                            "buttons": [list(p) for p in rb["buttons"]], "analog": [list(p) for p in rb["analog"]],
                            "omni": rb.get("omni"), "local": rb.get("local"), "merge": rb.get("merge"), "flip": rb.get("flip")}
        self.m["source"] = "read from the device (SysEx 07.0B / 07.0A)" + (" of the simulator" if self.dev.kind == "sim" else "")
        self.dec.set_map(self.m)
        if self.dev.kind == "hw":
            self.m["verified"] = False
            save_map(self.m)
        self.learn_msg = "map read from the %s: %d of %d controls differ from before" % (
            "Ohm64" if self.dev.kind == "hw" else "simulator", changed, len(new))

    # ---- map learning: click a control, then move the real one ----
    def _learn_from(self, msg):
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        name = self.learn
        c = self.controls[name]
        e = None
        if hi == 0x90 and msg[2] > 0 and c["kind"] == "button":
            e = {"type": "note", "ch": ch, "num": msg[1]}
        elif hi == 0xB0 and c["kind"] == "button" and msg[2] > 0:
            e = {"type": "cc", "ch": ch, "num": msg[1]}
        elif hi == 0xB0 and c["kind"] in ("knob", "fader"):
            e = {"type": "cc", "ch": ch, "num": msg[1]}
        elif hi == 0xE0 and c["kind"] in ("knob", "fader"):
            e = {"type": "pb", "ch": ch, "num": None}
        if e is None:
            if not (hi in (0x80, 0x90) and len(msg) > 2 and msg[2] == 0):
                self.learn_msg = "ignored %s while learning %s (wrong kind of message)" % (describe(msg), name)
            return True
        self.learn_msg = ""
        for other, oe in self.m["controls"].items():
            if other != name and oe.get("type") == e["type"] and oe.get("ch") == e["ch"] and oe.get("num") == e["num"]:
                oe.update({"type": None, "learned": False, "seen": False})
                self.learn_msg = "%s moved from %s; " % (describe(msg), other)
        e.update(learned=True, seen=True)
        self.m["controls"][name] = e
        self.dec.set_map(self.m)
        self.learn_msg += "learned %s = %s" % (name, describe(msg))
        self.learn = None
        self.m["verified"] = False
        save_map(self.m)
        return True

    def _save_soon(self):
        self._save_due = time.time() + 1.0

    _save_due = 0

    def _loop(self):
        while True:
            time.sleep(1 / 30)
            try:
                with self.lock:
                    if self._save_due and time.time() > self._save_due:
                        self._save_due = 0
                        save_map(self.m)
                    self.host.tick(self.dev, self.codes(), set(self.held))
            except Exception as ex:   # never let the LED loop die
                self.learn_msg = "host error: %s" % ex

    # ---- the live view API ----
    def snapshot(self):
        with self.lock:
            dev = self.dev
            hw = dev.kind == "hw"
            mc = self.m["controls"]
            log = [e[:4] for e in dev.log if self.show_noise or not e[4]][-24:]
            leds = {n: 127 for n in lit_names(dev.shown(), self.codes())}
            h = self.host
            hunt = None
            if h.mode == "hunt":
                c, r = _hunt_code(h.hunt["pos"])
                hunt = {"pos": h.hunt["pos"], "of": HUNT_LEN, "c": c, "r": r, "auto": h.hunt["auto"],
                        "owner": next((n for n, cr in self.codes().items() if cr == (c, r)), None)}
            rb = {k: v for k, v in self.readback.items() if k not in ("buttons", "analog", "leds")}
            rb["buttons"] = bool(self.readback.get("buttons"))
            rb["analog"] = bool(self.readback.get("analog"))
            return {
                "weapon": "ohm64",
                "simulated": not hw, "hardware": hw,
                "connected": dev.connected,
                "ports": list(getattr(dev, "port_names", ("", ""))),
                "port_list": self._ports() if hw else None,
                "notes": dev.notes,
                "held": sorted(self.held),
                "values": dict(self.values),
                "xf_flip": self.xf_flip(),
                "ranges": dict(self.ranges),
                "leds": leds,
                "backlight": getattr(dev, "backlight", h.backlight),
                "logo": getattr(dev, "logo", h.logo),
                "local": getattr(dev, "local", self.readback.get("local")),
                "host": {"mode": h.mode, "modes": list(OhmHost.MODES), "drive": h.drive, "page": h.page,
                         "clips": h.clip, "live": h.live_layer, "bpm": h.bpm, "side": h.xf_side(),
                         "saved_local": h.saved["local"], "saved_leds": h.saved["leds"] is not None},
                "hunt": hunt,
                "events": h.events[-14:],
                "readback": rb,
                "map": {"mapped": [n for n, e in mc.items() if e.get("type")],
                        "seen": [n for n, e in mc.items() if e.get("seen")],
                        "total": len(self.controls), "verified": self.m.get("verified", False),
                        "learned": [n for n, e in mc.items() if e.get("learned")],
                        "source": self.m.get("source", ""), "device_read": (self.m.get("device") or {}).get("read"),
                        "wire": {n: self._wire(e) for n, e in mc.items()},
                        "leds": {"hunted": self.m["leds"].get("hunted", []), "checked": self.m["leds"].get("checked")}},
                "learn": self.learn, "learn_msg": self.learn_msg,
                "last": self.last, "unmapped": self.unmapped,
                "counts": dict(dev.counts), "stats": dict(getattr(dev, "stats", {})),
                "log": log,
            }

    @staticmethod
    def _wire(e):
        t = e.get("type")
        if t == "note":
            return "note %d ch%d" % (e["num"], e["ch"] + 1)
        if t == "cc":
            return "CC %d ch%d" % (e["num"], e["ch"] + 1)
        if t == "cc14":
            return "CC %d/%d ch%d (14-bit)" % (e["num"], e["num"] + 32, e["ch"] + 1)
        if t == "pb":
            return "pitch ch%d" % (e["ch"] + 1)
        if t == "mmc":
            return "MMC %s" % {0x7A: "start", 0x7B: "continue", 0x7C: "stop"}.get(e["num"], "?")
        return "-"

    def _ports(self):
        try:
            import lightpad_hw as W
            ins, outs = W.list_ports()
            return {"in": ins, "out": outs}
        except Exception:
            return None

    def sim_input(self, i):
        dev = self.dev
        if dev.kind != "sim":
            return
        if i.get("release_all"):
            for n in list(self.held):
                dev.press(n, False)
            return
        name = i.get("name")
        c = self.controls.get(name)
        if not c:
            return
        if c["kind"] == "button" and "down" in i:
            if bool(i["down"]) != (name in self.held):
                dev.press(name, bool(i["down"]))
        elif c["kind"] in ("knob", "fader"):
            v = dev.values[ANALOG_INDEX[name]]
            if "pos" in i and name == "Crossfader":     # the drawing gives the position (0 = left)
                p = int(i["pos"])
                nv = p if dev.flip else 127 - p
            else:
                nv = int(i["value"]) if "value" in i else v + int(i.get("delta", 0))
            nv = max(0, min(127, nv))
            if nv != v:
                dev.move(name, nv)

    def command(self, d):
        with self.lock:
            if "sim_input" in d:
                self.sim_input(d["sim_input"])
            elif d.get("led_mode") in OhmHost.MODES:
                self.host.mode = d["led_mode"]
                if d["led_mode"] == "hunt":
                    self.host.hunt["t"] = time.time()
            elif "hunt" in d:          # {"step": n} / {"auto": bool} / {"pos": n}
                h = self.host.hunt
                if "pos" in d["hunt"]:
                    h["pos"] = int(d["hunt"]["pos"]) % HUNT_LEN
                h["pos"] = (h["pos"] + int(d["hunt"].get("step", 0))) % HUNT_LEN
                if "auto" in d["hunt"]:
                    h["auto"] = bool(d["hunt"]["auto"])
                h["t"] = time.time()
            elif "hunt_assign" in d:   # the owner clicked the button whose LED lit
                name = d["hunt_assign"]
                if self.host.mode != "hunt" or name not in LED_CODE and name not in self.controls:
                    return
                if self.dev.kind != "hw":
                    self.learn_msg = "LED hunt on the simulator: nothing is saved (only the real Ohm64 teaches LED codes)"
                    return
                cr = list(_hunt_code(self.host.hunt["pos"]))
                codes = self.m["leds"]["codes"]
                for other in [n for n, v in codes.items() if v == cr and n != name]:
                    del codes[other]
                codes[name] = cr
                hunted = self.m["leds"].setdefault("hunted", [])
                if name not in hunted:
                    hunted.append(name)
                self.learn_msg = "LED of %s = column %d row %d" % (name, cr[0], cr[1])
                self.host.hunt["pos"] = (self.host.hunt["pos"] + 1) % HUNT_LEN
                save_map(self.m)
            elif "drive" in d:         # the real device: let Himmel light the LEDs, or give them back
                if d["drive"] and not self.host.drive:
                    self.host.start_driving(self.dev)
                elif not d["drive"]:
                    self.host.let_go(self.dev)
            elif d.get("read_map"):
                self.read_device(adopt=True)
            elif "backlight" in d and self.host.drive:
                self.host.backlight = max(0, min(127, int(d["backlight"])))
                self.dev.receive(bytes([0xB0 | self.channel(), CC_BACKLIGHT, self.host.backlight]))
            elif "logo" in d and self.host.drive:
                v = max(0, min(1023, int(d["logo"]))) << 4          # 10 bits effective, sent as 14
                self.host.logo = v >> 4
                self.dev.receive(bytes([0xB0 | self.channel(), CC_LOGO, v >> 7]))
                self.dev.receive(bytes([0xB0 | self.channel(), CC_LOGO_LSB, v & 0x7F]))
            elif d.get("leds_ok"):
                self.m["leds"]["checked"] = time.strftime("%Y-%m-%d")
                save_map(self.m)
            elif "show_noise" in d:
                self.show_noise = bool(d["show_noise"])
            elif "hardware" in d:
                self._use(HwOhm(d.get("port")) if d["hardware"] else SimOhm())
                self.ranges.clear()
            elif "learn" in d:
                n = d["learn"]
                self.learn = n if n in self.controls and self.dev.kind == "hw" else None
                c = self.controls.get(n or "")
                self.learn_msg = ("LEARN %s: %s it on the Ohm64" % (n, {"knob": "turn", "fader": "move"}.get(c["kind"], "press"))
                                  ) if self.learn else ""
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
    assert names == set(BUTTON_INDEX) | set(ANALOG_INDEX), names ^ (set(BUTTON_INDEX) | set(ANALOG_INDEX))
    assert len(BUTTON_INDEX) == 81 and len(set(BUTTON_INDEX.values())) == 81 and max(BUTTON_INDEX.values()) < N_BUTTON_SLOTS
    assert sorted(ANALOG_INDEX.values()) == list(range(25))
    assert len(LED_CODE) == 75 and len(set(LED_CODE.values())) == 75
    assert all(r != 11 and 1 <= c <= 6 and 1 <= r <= 14 for c, r in LED_CODE.values())
    with_led = {c["name"] for c in prof["controls"] if c.get("led")}
    assert with_led == set(LED_CODE), with_led ^ set(LED_CODE)
    # the guide's tables: grid LED codes (first line = first button column), BPM 2Dh, note 10 -> 41h
    col1 = [LED_CODE["Grid %d" % (r * 8 + 1)] for r in range(8)]
    assert col1 == [(1, 1), (3, 2), (5, 3), (1, 5), (3, 6), (5, 7), (1, 9), (3, 10)], col1
    assert [LED_CODE["Grid %d" % (r * 8 + 8)] for r in range(8)][-1] == (4, 12)
    assert cr_byte(*LED_CODE["BPM"]) == 0x2D and cr_byte(*LED_CODE["Grid 11"]) == 0x41
    assert cr_byte(*LED_CODE["XL"]) == 0x4B and cr_byte(*LED_CODE["XR"]) == 0x5B
    assert [BUTTON_INDEX["Grid %d" % n] for n in range(1, 9)] == [0, 8, 16, 24, 32, 40, 48, 56]
    # LED bytes: 1 = off, round trip
    some = {LED_CODE[n] for n in ("Grid 1", "Grid 64", "Mute 8", "BPM", "XL")}
    w = led_words(some)
    data = words_to_data(w)
    assert len(data) == 12 and max(data) <= 0x7F and data_to_words(data) == w and lit_codes(w) == some
    assert words_to_data(ALL_OFF) == bytes([0x7F, 0x7F] * 6)
    assert sysex(CMD_LEDS, data)[:6].hex(" ") == "f0 00 01 61 02 04" and len(sysex(CMD_LEDS, data)) == 19
    assert request(REQ_BUTTONS).hex(" ") == "f0 00 01 61 02 07 0b f7"
    # the allow-list for the real device
    for bad in (sysex(CMD_SAVE), sysex(CMD_FACTORY), sysex(CMD_MAP_BUTTONS, bytes(176)), sysex(CMD_CHANNEL, b"\x01"),
                sysex(CMD_ERASE_LED_MAP), bytes([0xB0, 124, 0]), bytes([0xB0, 121, 0]), bytes([0xFF]), bytes([0xF0, 0x7E, 0xF7])):
        assert not allowed_to_device(bad)[0], bad.hex(" ")
    for good in (sysex(CMD_LEDS, data), request(REQ_BUTTONS), bytes([0x90, 10, 127]), bytes([0xB0, CC_LOCAL, 0]),
                 bytes([0xB0, CC_BACKLIGHT, 64])):
        assert allowed_to_device(good)[0], good.hex(" ")
    # the default map: no wire key twice, and every message round-trips
    m = default_map()
    kinds = {c["name"]: ("button" if c["kind"] == "button" else "analog") for c in prof["controls"]}
    dec = Decoder(m, kinds)
    keys = [(e["type"], e["ch"], e["num"]) for e in m["controls"].values()]
    assert len(keys) == len(set(keys))
    for n, e in m["controls"].items():
        if kinds[n] == "button":
            assert dec.decode(encode(e, down=True)) == (n, {"kind": "down", "vel": 127}), n
            assert dec.decode(encode(e, down=False))[1]["kind"] == "up", n
        else:
            assert dec.decode(encode(e, value=99)) == (n, {"kind": "value", "value": 99}), n
    for e, t in (({"type": "pb", "ch": 3, "num": None}, "pb"), ({"type": "cc14", "ch": 0, "num": 7}, "cc14")):
        mm = {"controls": {"Fader 1": e}}
        d2 = Decoder(mm, {"Fader 1": "analog"})
        msgs = split_midi(encode(e, value=77))
        assert d2.decode(msgs[0])[1]["value"] == 77, t
        if t == "cc14":
            assert d2.decode(msgs[1])[1]["kind"] == "lsb"
    # the live object on the simulator: readback, presses, LEDs, crossfader direction
    live = OhmLive(prof)
    live.m = default_map()
    live.dec.set_map(live.m)
    live.dev.buttons[BUTTON_INDEX["Grid 5"]] = (100, 0)          # a remapped button: readback must find it
    live.dev.analog[ANALOG_INDEX["Fader 2"]] = (0x63, 1)         # fader 2 -> pitch bend on channel 4
    live.command({"read_map": True})
    assert live.m["controls"]["Grid 5"]["num"] == 100 and live.m["controls"]["Fader 2"] == dict(
        type="pb", ch=3, num=None, learned=False, seen=False), live.m["controls"]["Fader 2"]
    live.command({"led_mode": "follow"})
    live.command({"sim_input": {"name": "Grid 5", "down": True}})
    live.command({"sim_input": {"name": "Fader 2", "value": 90}})
    live.command({"sim_input": {"name": "Crossfader", "pos": 0}})       # far left: factory value 127
    time.sleep(0.2)
    s = live.snapshot()
    assert "Grid 5" in s["held"] and s["leds"].get("Grid 5") == 127, (s["held"], s["leds"])
    assert s["values"]["Fader 2"] == 90 and s["values"]["Crossfader"] == 127 and s["host"]["side"] == "A"
    live.command({"sim_input": {"name": "Grid 5", "down": False}})
    time.sleep(0.2)
    assert "Grid 5" not in live.snapshot()["leds"]
    live.command({"led_mode": "all"})
    time.sleep(0.2)
    assert len(live.snapshot()["leds"]) == 75
    live.command({"led_mode": "himmel"})
    live.command({"sim_input": {"name": "Grid 10", "down": True}})
    live.command({"sim_input": {"name": "Grid 10", "down": False}})
    live.command({"sim_input": {"name": "Mute 3", "down": True}})
    time.sleep(0.2)
    s = live.snapshot()
    assert s["host"]["clips"][1] == 1 and "Grid 10" in s["leds"] and "Mute 3" not in s["leds"] and "Mute 4" in s["leds"]
    # local control: incoming notes light LEDs only with Local Control off
    sim = live.dev
    sim.receive(bytes([0xB0, CC_LOCAL, 0]))
    sim.receive(bytes([0x90, 0, 127]))
    assert LED_CODE["Grid 1"] in lit_codes(sim.leds)
    sim.receive(bytes([0xB0, CC_ALL_NOTES_OFF, 0]))
    assert not lit_codes(sim.leds)
    live.close()
    print("ohm64 selftest ok: %d controls, %d LEDs, the guide's tables, readback, LED bytes, allow-list" % (
        len(names), len(LED_CODE)))


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
