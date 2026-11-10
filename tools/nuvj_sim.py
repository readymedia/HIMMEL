# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
NuVJ weapon (weapon no. 5): simulator, hardware link, map learning, LED host
============================================================================

The NuVJ (2008) is a two-deck VJ controller: 2 x 9 pads, 2 jog wheels, 2 speed
faders, a crossfader, 12 knobs, BANK encoders, a blue LCD and LED buttons.
It is a class-compliant USB-MIDI device.

Everything here speaks the device's real MIDI bytes:

- ``SOURCE_TABLE``: the starting note / CC table, from a public forum post by a
  NuVJ owner (see THIRD_PARTY_NOTICES.md). It is only a guess until checked:
  the verified map lives in ``nuvj_map.json`` (repo root), learned with the
  live view (click a control, then move the real one).
- ``SimNuVJ``: a pretend device. The drawn panel drives it; it sends the same
  MIDI the map says the real one sends, and lights its LEDs from the host's
  note-on messages.
- ``HwNuVJ``: the real device over WinMM (ctypes, no new dependency), with
  the same interface. Read-only until the host sends LED notes.
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
# The starting table (public forum post, unverified). type: note | cc | pb.
# enc: "abs" (0..127), "rel" (endless: small numbers one way, 127.. the other),
# "switch" (fixed values per position), "toggle" (note with on/off values).
# Pad 9 of deck B is listed as note 16 in the post (the same as deck A); the
# pattern (A even, B odd) says 17.
# ---------------------------------------------------------------------------
SOURCE_TABLE = {
    "A Bank": ("cc", 14, "rel"), "B Bank": ("cc", 15, "rel"),
    "A PMTR1": ("cc", 0, "abs"), "B PMTR1": ("cc", 1, "abs"),
    "A PMTR2": ("cc", 2, "abs"), "B PMTR2": ("cc", 3, "abs"),
    "A Latch": ("note", 8), "B Latch": ("note", 9),
    "A Auto": ("note", 4), "B Auto": ("note", 5),
    "A Sync": ("note", 6), "B Sync": ("note", 7),
    "A Jog": ("cc", 17, "rel"), "B Jog": ("cc", 16, "rel"),
    "A Scratch": ("note", 0), "B Scratch": ("note", 1),
    "Insta A": ("note", 10), "Insta B": ("note", 11),
    "A Speed": ("pb", 0), "B Speed": ("pb", 1), "Crossfader": ("pb", 2),
    "FX Level": ("cc", 8, "abs"), "Master PMTR1": ("cc", 4, "abs"), "Master PMTR2": ("cc", 5, "abs"),
    "Master FX Select": ("cc", 9, "abs"), "Contrast": ("cc", 6, "abs"), "Bright": ("cc", 7, "abs"),
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
    elif t[0] == "note" and len(t) > 2:
        e["enc"] = t[2]
        e["values"] = t[3]
    if t[0] == "pb":
        e["ch"], e["num"] = t[1], None
    return e


def led_key(e):
    """(channel, note) that lights this control's LED: a learned "led" entry, else the button's own note
    (the first guess; on Numark's Total Control the LED notes differ from the button notes)."""
    if e.get("led"):
        return e["led"]["ch"], e["led"]["num"]
    return e["ch"], e["num"]


def load_profile():
    with open(PROFILE, encoding="utf-8") as f:
        return json.load(f)


def default_map():
    return {"format": "himmel-map/1", "weapon": "nuvj", "verified": False,
            "source": "starting table from a public forum post (unverified); entries change when learned on hardware",
            "controls": {n: dict(entry_from_source(t), learned=False, seen=False) for n, t in SOURCE_TABLE.items()},
            "leds": {"how": "unknown", "checked": False}}


def load_map():
    try:
        with open(MAP_FILE, encoding="utf-8") as f:
            m = json.load(f)
        base = default_map()
        for n, e in base["controls"].items():   # a control added later to the profile keeps the forum guess
            m["controls"].setdefault(n, e)
        m.setdefault("leds", base["leds"])
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


def describe(msg):
    if not msg:
        return ""
    st = msg[0]
    hi, ch = st & 0xF0, (st & 15) + 1
    if st == 0xF0:
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
        self.idx = {}
        for name, e in m["controls"].items():
            if e.get("type") == "pb":
                self.idx[("pb", e["ch"], None)] = name
            elif e.get("type"):
                self.idx[(e["type"], e["ch"], e["num"])] = name

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
            if enc == "rel":
                v = msg[2] - 128 if msg[2] >= 64 else msg[2]
                if e.get("cw", "pos") == "neg":
                    v = -v
                return name, {"kind": "delta", "delta": v}
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
        self.led_raw = {}            # (ch, note) -> velocity last sent by the host
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

    def note_led(self, msg):
        """Remember what the host asked the LEDs to show (the guess: note on = LED)."""
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        if hi in (0x80, 0x90) and len(msg) > 2:
            self.led_raw[(ch, msg[1])] = msg[2] if hi == 0x90 else 0

    def close(self):
        pass


class SimNuVJ(DeviceBase):
    """A pretend NuVJ: the drawn panel drives it, it sends the map's MIDI bytes."""

    kind = "sim"

    def __init__(self, live):
        super().__init__()
        self.live = live
        self.notes = ["Simulated NuVJ: the drawing is the controller. LEDs follow the note-on messages the host sends."]

    def receive(self, msg):
        """MIDI from the host to the device."""
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=True)
            self.note_led(msg)

    def leds_for(self, m):
        """LED level per control name, read back from what the host sent (same guess as for the real one)."""
        out = {}
        for name, e in m["controls"].items():
            if e.get("type") == "note":
                v = self.led_raw.get(led_key(e), 0)
                if v:
                    out[name] = v
        return out


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
        """Send the MIDI standard's Universal Identity Request (the only SysEx sent to the NuVJ).
        A device that supports it answers F0 7E <id> 06 02 <manufacturer> <family> <model> <version> F7."""
        if not self.connected or not self.has_out:
            return
        W, mm = self.W, self.mm
        msg = self.IDENTITY_REQUEST
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
            self._log("host → device", msg)

    def receive(self, msg):
        """Short MIDI from the host to the real device (LED notes). SysEx is never sent from here."""
        msg = bytes(msg)
        if not self.connected or not self.has_out or not msg or msg[0] == 0xF0:
            return
        w = msg[0] | (msg[1] << 8 if len(msg) > 1 else 0) | (msg[2] << 16 if len(msg) > 2 else 0)
        with self.out_lock:
            r = self.mm.midiOutShortMsg(self.hout, w)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=True)
            self.note_led(msg)
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
LED_ON, LED_DIM = 127, 0
FX_NAMES = ("BLUR", "INVERT", "MIRROR", "KALEIDO", "STROBE", "RGB SPLIT", "PIXELATE", "FEEDBACK",
            "EDGES", "HUE SPIN", "ZOOM", "TUNNEL")
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
        self.sent = {}             # (ch, note) -> velocity we last sent
        self.t0 = time.time()
        self.flash = None          # (until, text, bar 0..1 or None): a value shown for a moment
        self.led_probe = None      # {"ch": c, "vel": v} lights every mapped note on that channel
        self.hunt = None           # LED hunt: {"ch", "note", "auto", "t"} lights one note at a time

    # ---- the LCD: Himmel's page for the NuVJ's segment display ----
    def show(self, text, bar=None, secs=1.5):
        self.flash = (time.time() + secs, text, bar)

    def lcd(self):
        """The LCD as a model the drawing (and, once the protocol is known, the driver) renders:
        text = 9 dot-matrix cells, big / small = 2 seven-segment digits each, bar = 0..12 segments,
        icons on/off. This is the interface between Himmel and the device, like the MK2 pages."""
        st, now = self.state, time.time()
        f = st["focus"]
        if self.flash and now < self.flash[0]:
            text, bar = self.flash[1], self.flash[2]
        elif now - self.t0 < 3.0:
            text, bar = "HIMMEL VJ", (now - self.t0) / 3.0          # start-up: name + a filling bar
        else:
            text, bar = "%5.1f BPM" % st["bpm"] if st["bpm"] < 1000 else "BPM", st["xf"] / 16383
        beat = ((now - self.t0) * st["bpm"] / 60.0) % 1.0
        return {
            "text": text[:9],
            "big": "%02d" % (st[f]["bank"] + 1),
            "small": "%02d" % (st["fx"] + 1) if st["toggles"]["Master FX"] else "",
            "bar": None if bar is None else max(0, min(12, round(bar * 12))),
            "icons": {"sync": st["toggles"][f + " Sync"] and beat < 0.5, "black": st["toggles"]["Insta Black"],
                      "deck_a": f == "A", "deck_b": f == "B"},
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
            if name.endswith("Bank"):
                st[side]["bank"] = (st[side]["bank"] + (1 if ev["delta"] > 0 else -1)) % 16
                self.show("BANK %s %02d" % (side, st[side]["bank"] + 1), st[side]["bank"] / 15)
            elif name.endswith("Jog"):
                st[side]["jog"] += ev["delta"]
                scr = self.live.held_now(side + " Scratch")
                self.show(("SCR %+5d" if scr else "NUDGE %+3d") % (st[side]["jog"] if scr else max(-99, min(99, ev["delta"] * 10))),
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
                self.show("SPD%s %+.1f" % (name[0], (v - 0.5) * 4), v)
        elif k == "switch":
            c = self.live.control(name)
            pos = ev["pos"]
            p = c["positions"][pos] if c and 0 <= pos < len(c["positions"]) else "?"
            self.show(("MODE " if name == "Fader Mode" else "FSTART ") + p)

    # ---- LEDs ----
    def wanted(self, m, held):
        """(ch, note) -> velocity for every mapped note control."""
        st = self.state
        out = {}
        notes = [(n, e) for n, e in m["controls"].items() if e.get("type") == "note" and e.get("enc") != "toggle"]
        t = time.time() - self.t0
        beat = (t * st["bpm"] / 60.0) % 1.0
        if self.hunt is not None:   # one note lit on the hunt channel, every other note on it dark
            h = self.hunt
            if h["auto"] and time.time() - h["t"] > 1.2:
                h["note"], h["t"] = (h["note"] + 1) % 128, time.time()
            return {(h["ch"], n): (LED_ON if n == h["note"] else 0) for n in range(128)}
        for i, (n, e) in enumerate(notes):
            key = led_key(e)
            if self.led_probe is not None:
                out[(self.led_probe["ch"], e["num"])] = self.led_probe["vel"]
                continue
            if self.mode == "off":
                v = 0
            elif self.mode == "all":
                v = LED_ON
            elif self.mode == "chase":
                v = LED_ON if int(t * 6) % len(notes) == i else 0
            elif self.mode == "follow":
                v = LED_ON if n in held else 0
            else:  # himmel: what Himmel would show
                if n in held:
                    v = LED_ON
                elif n in st["toggles"]:
                    v = LED_ON if st["toggles"][n] else 0
                elif " Pad " in n:
                    v = LED_ON if st[n[0]]["pad"] == int(n.split()[-1]) else 0
                elif n == "BPM Tap":
                    v = LED_ON if beat < 0.2 else 0
                elif n in ("Insta A", "Insta B"):
                    v = LED_ON if st["insta"] == n[-1] else 0
                else:
                    v = 0
            out[key] = v
        return out

    def tick(self, dev, m, held, force=False):
        if not getattr(dev, "connected", True):
            self.sent.clear()
            return
        want = self.wanted(m, held)
        n = 0
        for key, v in want.items():
            if force or self.sent.get(key) != v:
                ch, note = key
                dev.receive(bytes([0x90 | ch, note, v]))
                self.sent[key] = v
                n += 1
                if n >= 24:              # at most 24 LED messages per tick; the rest next tick
                    break


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
        self.learn = None
        self.learn_msg = ""
        self.show_noise = False
        self.host = NuvjHost(self)
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
            if name is None:
                self.unmapped = ([describe(msg)] + [u for u in self.unmapped if u != describe(msg)])[:12]
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
                raw = msg[2] if k in ("value", "delta", "switch") else (msg[1] | msg[2] << 7) if k == "fader" else None
                if raw is not None:
                    lo, hi = self.ranges.get(name, [raw, raw])
                    self.ranges[name] = [min(lo, raw), max(hi, raw)]
                e = self.m["controls"].get(name)
                if e and not e.get("seen"):
                    e["seen"] = True
                    self._save_soon()
            self.last = ([name] + [x for x in self.last if x != name])[:6]
            self.host.on_event(name, ev)

    # ---- map learning: click a control, then move the real one ----
    def _learn_from(self, msg):
        hi, ch = msg[0] & 0xF0, msg[0] & 15
        name = self.learn
        c = self.controls[name]
        e = None
        if hi == 0x90 and msg[2] > 0 and c["kind"] in ("button", "pad"):
            e = {"type": "note", "ch": ch, "num": msg[1]}
        elif c["kind"] == "switch" and hi in (0x80, 0x90, 0xB0):
            e = self._learn_switch(name, c, msg)
            if e is None:
                return True
        elif hi == 0xB0 and c["kind"] in ("knob", "encoder", "wheel"):
            e = {"type": "cc", "ch": ch, "num": msg[1]}
            if c["kind"] == "knob":
                e["enc"] = "abs"
            else:   # endless: the owner turns it clockwise; the first value tells the direction
                e["enc"] = "rel"
                e["cw"] = "pos" if 0 < msg[2] < 64 else "neg"
        elif hi == 0xE0 and c["kind"] == "fader":
            e = {"type": "pb", "ch": ch, "num": None}
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
                    self.host.tick(self.dev, self.m, set(self.held))
            except Exception as ex:   # never let the LED loop die
                self.learn_msg = "host error: %s" % ex

    # ---- the live view API ----
    def snapshot(self):
        with self.lock:
            dev = self.dev
            hw = dev.kind == "hw"
            mc = self.m["controls"]
            mapped = [n for n, e in mc.items() if e.get("type")]
            seen = [n for n, e in mc.items() if e.get("seen")]
            log = [e[:4] for e in dev.log if self.show_noise or not e[4]][-22:]
            leds = {}
            for name, e in mc.items():
                if e.get("type") == "note" and e.get("enc") != "toggle":
                    v = dev.led_raw.get(led_key(e), 0)
                    if v:
                        leds[name] = v
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
                "host": {"mode": self.host.mode, "modes": list(NuvjHost.MODES), "bpm": st["bpm"],
                         "A": dict(st["A"]), "B": dict(st["B"]), "toggles": dict(st["toggles"]),
                         "xf": st["xf"], "probe": self.host.led_probe, "hunt": self.host.hunt},
                "events": [(t, n, ev) for t, n, ev in self.host.events[-14:]],
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
            return "note %d ch%d" % (e["num"], e["ch"] + 1) + (" (%s)" % "/".join(map(str, e["values"])) if e.get("values") else "") +                 ("  LED note %d ch%d" % (e["led"]["num"], e["led"]["ch"] + 1) if e.get("led") else "")
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
        if k in ("button", "pad") and "down" in i:
            if bool(i["down"]) != (name in self.held):
                self.dev.emit(encode(e, down=bool(i["down"])))
        elif k == "knob":
            v = self.values.get(name, 0)
            nv = max(0, min(127, int(i["value"]) if "value" in i else v + int(i.get("delta", 0))))
            if nv != v:
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
            elif d.get("led_mode") in NuvjHost.MODES:
                self.host.mode = d["led_mode"]
                self.host.led_probe = None
                self.host.hunt = None
            elif "hunt" in d:
                h = d["hunt"]
                if h is None:
                    self.host.hunt = None
                else:
                    old = self.host.hunt or {"ch": 0, "note": 0, "auto": False}
                    self.host.hunt = {"ch": int(h.get("ch", old["ch"])) & 15,
                                      "note": (int(h["note"]) if "note" in h else old["note"] + int(h.get("step", 0))) % 128,
                                      "auto": bool(h.get("auto", old["auto"])), "t": time.time()}
                    self.host.led_probe = None
            elif "led_assign" in d:   # LED hunt: the control the owner sees lit gets this note as its LED
                n, h = d["led_assign"], self.host.hunt
                if h and n in self.m["controls"] and self.dev.kind != "hw":
                    self.learn_msg = "LED hunt on the simulator: nothing is saved (only the real NuVJ teaches LED notes)"
                elif h and n in self.m["controls"]:
                    for other, oe in self.m["controls"].items():
                        if oe.get("led") == {"ch": h["ch"], "num": h["note"]}:
                            oe.pop("led")
                    self.m["controls"][n]["led"] = {"ch": h["ch"], "num": h["note"]}
                    self.learn_msg = "LED of %s = note %d ch%d" % (n, h["note"], h["ch"] + 1)
                    h["auto"] = False
                    save_map(self.m)
            elif d.get("identity"):
                if hasattr(self.dev, "identity"):
                    self.dev.identity()
                    self.learn_msg = "identity request sent; an answer shows in the MIDI monitor (SysEx)"
            elif "led_probe" in d:
                p = d["led_probe"]
                self.host.led_probe = None if p is None else {"ch": int(p.get("ch", 0)) & 15, "vel": int(p.get("vel", 127)) & 127}
            elif "show_noise" in d:
                self.show_noise = bool(d["show_noise"])
            elif "hardware" in d:
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
            elif "led_result" in d:
                self.m["leds"] = {"how": str(d["led_result"])[:200], "checked": time.strftime("%Y-%m-%d")}
                save_map(self.m)

    def close(self):
        with self.lock:
            try:
                self.host.mode = "off"
                self.host.led_probe = None
                self.host.hunt = None
                self.host.tick(self.dev, self.m, set(), force=True)
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
        if k in ("button", "pad"):
            assert dec.decode(encode(e, down=True)) == (n, {"kind": "down", "vel": 127}), n
            assert dec.decode(encode(e, down=False))[1]["kind"] == "up", n
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
    print("nuvj selftest ok: %d controls, every message round-trips, LEDs follow the host" % len(names))


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
