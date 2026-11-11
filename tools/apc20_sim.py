# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
APC20 weapon (weapon no. 9, a secondary weapon): simulator, probe, map learning, LED host
========================================================================================

The APC20 (2010) is a clip-launch controller: an 8 x 5 grid of tri-colour
buttons, 5 scene buttons, 8 clip stop buttons, a transport / track selection
row, three rows of 8 track buttons, a Cue Level encoder, 8 track faders and a
master fader. It is a class-compliant USB-MIDI device (09e8:007b) and needs its
12 V DC 2 A adapter: USB carries MIDI only.

Everything here speaks the device's real MIDI bytes. The wire facts come from
Ableton's APC20 MIDI Remote Script (as decompiled on GitHub; read for protocol
facts only, no code copied) and Akai's APC20 Quickstart Guide. See
THIRD_PARTY_NOTICES.md. The wire table lives in the profile
(weapons/apc20.weapon.json, ``midi`` / ``led`` per control); a map learned on
the owner's unit (``apc20_map.json``, repo root) overrides it.

- ``SimAPC20``: a pretend device. The drawn panel drives it. It answers the
  identity request, takes the mode SysEx, keeps the LEDs the host sends
  (blinking is done by the device, as on the real one) and sends note / CC.
  Every message it sends also goes to UDP 127.0.0.1:8782 (the engine link for
  the generic weapon driver); datagrams sent back count as host -> device.
- ``HwAPC20``: the real device over WinMM (ctypes, no new dependency). It is
  a **read-only probe** until you press a button: it sends nothing by itself.
  Only the identity request, the APC mode SysEx and note on/off go out; any
  other SysEx is refused.
- ``Apc20Host``: the live view standing in for Himmel, with the APC20 as the
  **secondary weapon**: clip / scene launcher, deck mixer and deck focus,
  while the primary weapon (the MK2) keeps its pages and screens.
- ``Apc20Live``: what tools/mk2_live.py serves for ?weapon=apc20.

Run:   python tools/mk2_live.py --port 8787 --weapon apc20     then open http://127.0.0.1:8787
Check: python tools/apc20_sim.py --selftest
Ports: python tools/apc20_sim.py --ports
"""

import ctypes
import json
import os
import random
import shutil
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROFILE = os.path.join(ROOT, "weapons", "apc20.weapon.json")
MAP_FILE = os.path.join(ROOT, "apc20_map.json")
PORT_HINTS = ("apc20", "apc 20")
ENGINE_ADDR = ("127.0.0.1", 8782)   # MK2 8766, Lightpad 8767, LUMI 8769, ... APC20 8782
HOST_TIMEOUT = 1.0                  # s without a datagram from the engine: the local host drives again

# ---------------------------------------------------------------------------
# Protocol facts (Ableton's APC20 / _APC scripts, Akai's Quickstart Guide)
# ---------------------------------------------------------------------------
MANUFACTURER = 0x47                 # Akai
MODEL = 0x7B                        # APC20 (APC40 = 0x73)
MODES = {0x40: "generic", 0x41: "ableton", 0x42: "ableton-alt", 0x43: "note"}
MODE_BYTE = {v: k for k, v in MODES.items()}
IDENTITY_REQUEST = bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])
NOTE_MODE_CH = 9                    # in note mode the grid plays notes 36..75 on channel 10

# clip grid LED velocities (bi-colour LEDs) and single-colour LEDs
TRI = {0: "off", 1: "green", 2: "green blink", 3: "red", 4: "red blink", 5: "amber", 6: "amber blink"}
TRI_VALUE = {v: k for k, v in TRI.items()}
MONO = {0: "off", 1: "on", 2: "blink"}


def mode_sysex(mode, dev=0, version=(1, 0, 0)):
    """F0 47 <dev> 7B 60 00 04 <mode> <maj> <min> <bug> F7: tells the APC20 which mode to be in.
    Ableton sends this after the identity reply, with its own version numbers."""
    m = MODE_BYTE[mode] if isinstance(mode, str) else mode
    return bytes([0xF0, MANUFACTURER, dev & 0x7F, MODEL, 0x60, 0x00, 0x04, m] +
                 [v & 0x7F for v in version] + [0xF7])


def parse_mode(msg):
    """(device id, mode byte) for an APC20 mode SysEx, else None."""
    if len(msg) == 12 and msg[0] == 0xF0 and msg[1] == MANUFACTURER and msg[3] == MODEL \
            and msg[4] == 0x60 and msg[5] == 0x00 and msg[6] == 0x04 and msg[-1] == 0xF7:
        return msg[2], msg[7]
    return None


def identity_reply(dev=0, version=(0, 1, 0, 5), serial=(0, 0, 0, 0)):
    """What an APC answers to F0 7E 7F 06 01 F7. Ableton reads bytes 5 (manufacturer), 6 (model),
    9..12 (version, 4 nibbles) and 13 (device id). The rest is filled with zeros in the sim."""
    return bytes([0xF0, 0x7E, dev & 0x7F, 0x06, 0x02, MANUFACTURER, MODEL, 0x00, 0x19, 0x00] +
                 list(version) + [dev & 0x7F] + list(serial) + [0] * 16 + [0xF7])


def parse_identity(msg):
    """dict(dev, version, model) for an identity reply, else None."""
    if len(msg) >= 15 and msg[0] == 0xF0 and msg[1] == 0x7E and msg[3] == 0x06 and msg[4] == 0x02:
        return {"manufacturer": msg[5], "model": msg[6], "is_apc20": msg[5] == MANUFACTURER and msg[6] == MODEL,
                "version": "%d.%d" % ((msg[9] << 4) + msg[10], (msg[11] << 4) + msg[12]), "dev": msg[13]}
    return None


def is_identity_request(msg):
    return len(msg) == 6 and msg[0] == 0xF0 and msg[1] == 0x7E and msg[3] == 0x06 and msg[4] == 0x01


def rel2c(v):
    """Cue Level: relative two's complement. 1..63 = +1..+63 (clockwise), 65..127 = -63..-1."""
    return v - 128 if v >= 64 else v


def to_rel2c(d):
    return max(-63, min(63, d)) & 0x7F


def split_midi(data):
    """Split a byte string with several short MIDI messages into messages (running status aware)."""
    out, i, st = [], 0, 0
    data = bytes(data)
    while i < len(data):
        b = data[i]
        if b >= 0x80:
            st = b
            i += 1
        if st == 0:
            i += 1
            continue
        n = 1 if (st & 0xF0) in (0xC0, 0xD0) else 2
        if i + n > len(data):
            break
        out.append(bytes([st]) + data[i:i + n])
        i += n
    return out


def describe(msg):
    msg = bytes(msg)
    if not msg:
        return ""
    if msg[0] == 0xF0:
        if is_identity_request(msg):
            return "identity request"
        idn = parse_identity(msg)
        if idn:
            return "identity reply: %s, version %s, device id %d" % (
                "APC20" if idn["is_apc20"] else "manufacturer %02X model %02X" % (idn["manufacturer"], idn["model"]),
                idn["version"], idn["dev"])
        pm = parse_mode(msg)
        if pm:
            return "mode SysEx: %s (device id %d)" % (MODES.get(pm[1], "%02X" % pm[1]), pm[0])
        if len(msg) > 4 and msg[1] == MANUFACTURER and msg[3] == MODEL and msg[4] in (0x50, 0x51):
            return "Ableton's dongle challenge / reply (Himmel does not use it)"
        return "SysEx (%d bytes)" % len(msg)
    s, ch = msg[0] & 0xF0, (msg[0] & 0x0F) + 1
    if s == 0x90 and len(msg) >= 3:
        return "ch%d note on %d vel %d" % (ch, msg[1], msg[2])
    if s == 0x80 and len(msg) >= 3:
        return "ch%d note off %d vel %d" % (ch, msg[1], msg[2])
    if s == 0xB0 and len(msg) >= 3:
        return "ch%d CC%d = %d" % (ch, msg[1], msg[2])
    return "MIDI " + msg.hex(" ")


def note_mode_note(track, scene):
    """Note mode (Quickstart Guide): the grid splits in two halves of 4 x 5. Left half: left to right,
    bottom to top, C1 (36) .. G2 (55); right half G#2 (56) .. D#3 (75). scene = 0 for the top row."""
    half, col, row_up = track // 4, track % 4, 4 - scene
    return 36 + half * 20 + row_up * 4 + col


# ---------------------------------------------------------------------------
# Profile and map
# ---------------------------------------------------------------------------
def load_profile():
    with open(PROFILE, encoding="utf-8") as f:
        return json.load(f)


def load_map():
    try:
        with open(MAP_FILE, encoding="utf-8") as f:
            m = json.load(f)
        if m.get("format") == "himmel-apc20-map/1":
            return m
    except (OSError, ValueError):
        pass
    return None


def new_map():
    return {"format": "himmel-apc20-map/1", "device": "Akai APC20", "identity": None, "note_off": None,
            "controls": {}, "seen": [], "led_colors": {}, "note_mode": {}, "updated": None}


def save_map(m):
    m["updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    if os.path.exists(MAP_FILE):
        shutil.copyfile(MAP_FILE, MAP_FILE[:-5] + ".bak.json")
    tmp = MAP_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=1)
        f.write("\n")
    os.replace(tmp, MAP_FILE)


class Wire:
    """Control index <-> MIDI address, from the profile with the learned map on top."""

    def __init__(self, profile, m=None):
        self.controls = profile["controls"]
        self.index = {c["name"]: i for i, c in enumerate(self.controls)}
        self.set_map(m)

    def set_map(self, m):
        self.addr, self.led = {}, {}
        learned = (m or {}).get("controls", {})
        for i, c in enumerate(self.controls):
            w = learned.get(c["name"]) or c.get("midi")
            if w:
                self.addr[i] = (w["type"], w["ch"], w["num"], c.get("midi", {}).get("enc", "abs"))
            if c.get("led"):
                self.led[i] = (c["led"]["ch"], c["led"]["num"], c["led"]["palette"])
        self.by_addr = {(t, ch, n): i for i, (t, ch, n, _) in self.addr.items()}
        self.by_led = {(ch, n): i for i, (ch, n, _) in self.led.items()}
        self.note_mode = {}
        for i, c in enumerate(self.controls):
            if c.get("group") == "clip":
                self.note_mode[note_mode_note(c["track"] - 1, c["scene"] - 1)] = i
        self.note_mode_of = {i: n for n, i in self.note_mode.items()}

    def encode(self, i, down=None, value=None, delta=None, note_mode=False):
        """The bytes the device sends for control i."""
        if note_mode and i in self.note_mode_of:
            n = self.note_mode_of[i]
            return bytes([0x90 | NOTE_MODE_CH, n, 127]) if down else bytes([0x80 | NOTE_MODE_CH, n, 127])
        t, ch, n, enc = self.addr[i]
        if t == "note":
            # Ableton treats note off and note on vel 0 the same; 0x80 vel 127 as on the APC40 (to be checked)
            return bytes([0x90 | ch, n, 127]) if down else bytes([0x80 | ch, n, 127])
        if enc == "rel2c":
            return bytes([0xB0 | ch, n, to_rel2c(delta or 0)])
        return bytes([0xB0 | ch, n, max(0, min(127, int(value)))])

    def decode(self, msg, note_mode=False):
        """(index, what, value) for a message from the device, or None.
        what = down | up | value | delta. note_mode: channel-10 notes are grid buttons."""
        if len(msg) < 3 or msg[0] >= 0xF0:
            return None
        s, ch = msg[0] & 0xF0, msg[0] & 0x0F
        if s in (0x80, 0x90):
            down = s == 0x90 and msg[2] > 0
            i = self.by_addr.get(("note", ch, msg[1]))
            if i is None and ch == NOTE_MODE_CH:
                i = self.note_mode.get(msg[1])
            return None if i is None else (i, "down" if down else "up", msg[2])
        if s == 0xB0:
            i = self.by_addr.get(("cc", ch, msg[1]))
            if i is None:
                return None
            if self.addr[i][3] == "rel2c":
                return i, "delta", rel2c(msg[2])
            return i, "value", msg[2]
        return None


# ---------------------------------------------------------------------------
# Devices: the simulated one and the real one, same interface
# ---------------------------------------------------------------------------
class DeviceBase:
    kind = "sim"

    def __init__(self):
        self.lock = threading.RLock()
        self.listeners = []          # f(msg) for MIDI from the device
        self.log = []
        self.counts = {"to_host": 0, "to_device": 0}
        self.leds = {}               # (ch, note) -> velocity, as last sent by the host
        self.mode = None             # what the host last asked for (None = unknown / power-on)
        self.connected = True
        self.notes = []

    def _log(self, direction, msg, noisy=False):
        self.log.append((time.strftime("%H:%M:%S"), direction, bytes(msg[:24]).hex(" ") +
                         (" …" if len(msg) > 24 else ""), describe(msg), noisy))
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
        """Remember what the host asked the device to show (both the sim and the real one)."""
        if msg[0] == 0xF0:
            pm = parse_mode(msg)
            if pm:
                self.mode = MODES.get(pm[1], "%02X" % pm[1])
                self.leds = {}           # a mode change clears the LEDs (sim; assumed for the real unit)
            return
        s, ch = msg[0] & 0xF0, msg[0] & 0x0F
        if s == 0x90 and len(msg) >= 3:
            self.leds[(ch, msg[1])] = msg[2]
        elif s == 0x80 and len(msg) >= 3:
            self.leds[(ch, msg[1])] = 0

    def close(self):
        pass


class SimAPC20(DeviceBase):
    """A pretend APC20: the drawn panel drives it; it sends what the wire table says the real one sends."""

    kind = "sim"

    def __init__(self):
        super().__init__()
        self.mode = "generic"        # the APC powers up in generic mode
        self.dev_id = 0
        self.engine_time = 0.0
        self.closed = False
        self.notes = ["Simulated APC20: the drawing is the controller. Its LEDs show what the host sends "
                      "(blinking is done by the device). Engine link: UDP 127.0.0.1:%d." % ENGINE_ADDR[1]]
        self.udp = None
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.bind(("127.0.0.1", 0))
            u.settimeout(0.2)
            if hasattr(socket, "SIO_UDP_CONNRESET"):
                u.ioctl(socket.SIO_UDP_CONNRESET, False)
            self.udp = u
            threading.Thread(target=self._rx_loop, daemon=True).start()
        except OSError as e:
            self.notes.append("No link to the engine: %s" % e)

    def engine_active(self):
        return time.time() - self.engine_time < HOST_TIMEOUT

    def emit(self, data):
        super().emit(data)
        if self.udp is not None:
            try:
                self.udp.sendto(bytes(data), ENGINE_ADDR)
            except OSError:
                pass

    def _rx_loop(self):
        while not self.closed:
            try:
                r, _ = self.udp.recvfrom(4096)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue     # Windows: queued "port unreachable" for datagrams nobody read; drain, no sleep
            except OSError:
                if self.closed:
                    return
                time.sleep(0.05)
                continue
            if r:
                self.engine_time = time.time()
                for m in ([r] if r[:1] == b"\xF0" else split_midi(r)):
                    self.receive(m, "engine")

    def receive(self, msg, source="host"):
        """MIDI from the host to the device."""
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("%s → device" % source, msg, noisy=msg[0] != 0xF0)
            if msg[0] == 0xF0:
                pm = parse_mode(msg)
                if pm and pm[0] not in (self.dev_id, 0x7F):
                    return               # addressed to another APC
            self.note_out(msg)
        if is_identity_request(msg):
            threading.Timer(0.002, self.emit, (identity_reply(self.dev_id),)).start()

    def close(self):
        self.closed = True
        if self.udp is not None:
            try:
                self.udp.close()
            except OSError:
                pass


class HwAPC20(DeviceBase):
    """The real APC20 over WinMM (reuses the ctypes bindings of lightpad_hw.py). Sends nothing by itself."""

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
        self.has_out = False
        self.port_names = ("", "")
        self.hin = W.wintypes.HANDLE()
        self.hout = W.wintypes.HANDLE()
        self.headers, self.bufs, self.readd = [], [], []
        self.out_lock = threading.Lock()
        self.cb = W.MIDIINPROC(self._callback)
        self.wake = threading.Event()
        self.opened = 0              # counts (re)opens, so the host knows to send its setup again
        self.stats = {"msgs": 0, "rate": 0.0, "first": None}
        self._rn, self._rt = 0, time.time()
        self.notes = ["Real APC20: looking for its MIDI ports…"]
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
            o = next((k for k, n in outs if n == dict(ins)[i]), None)
        else:
            o = self._pick(outs)
        if i is None:
            self.notes = ["No APC20 MIDI port found. Is the 12 V adapter in and the power switch on? Is USB in? "
                          "Close Ableton Live. Ports seen: in %s, out %s. Pick one below if it has another name." % (
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
        for _ in range(8):   # buffers for SysEx (the identity reply)
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
        self.mode = None             # a (re)opened port: unknown until the host sends the mode
        self.leds = {}
        self.opened += 1
        self.notes = ["Real APC20 on MIDI in %d (%s), out %s. Read-only until you press a button below." % (
            i, self.port_names[0], "%d" % o if o is not None else "not opened")]
        with self.lock:
            self.log.append((time.strftime("%H:%M:%S"), "probe", "", "ports opened: " + " / ".join(self.port_names), False))
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

    def _long(self, msg):
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
            self._log("host → device", msg)
            self.note_out(msg)
        if r:
            self.connected = False

    def receive(self, msg, source="host"):
        """MIDI from the host to the real device. Only the identity request, the APC mode SysEx and
        note on/off (LEDs) go out; anything else is refused, so nothing unknown reaches the controller."""
        msg = bytes(msg)
        if not self.connected or not self.has_out or not msg:
            return
        if msg[0] == 0xF0:
            if is_identity_request(msg) or parse_mode(msg):
                self._long(msg)
            return
        if (msg[0] & 0xF0) not in (0x80, 0x90) or len(msg) != 3:
            return
        w = msg[0] | (msg[1] << 8) | (msg[2] << 16)
        with self.out_lock:
            r = self.mm.midiOutShortMsg(self.hout, w)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → device", msg, noisy=True)
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
            if self.connected and int(now * 2) % 4 == 0:   # a pulled cable: the port disappears
                ins, _ = W.list_ports()
                if not any(n == self.port_names[0] for _, n in ins):
                    with self.lock:
                        self.log.append((time.strftime("%H:%M:%S"), "probe", "", "port gone (cable or power pulled?)", False))
                    self.connected = False


# ---------------------------------------------------------------------------
# The host: Himmel's side, with the APC20 as the secondary weapon
# ---------------------------------------------------------------------------
DECKS, ROWS, BANK_ROWS = 8, 5, 15          # 8 decks, 5 visible clip rows, 15 clip slots per deck
FADER_PAGES = ("OPACITY", "SPEED", "FX A", "FX B", "FX C", "USER 1", "USER 2", "USER 3")
TRANSITIONS = ("cut", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats", "8 beats")
TRANSPORT = ("PLAY", "STOP", "REC", "MIDI OVERDUB", "LEFT", "RIGHT", "UP", "DOWN")


class Apc20Host:
    """A small model of what Himmel does with the APC20 as the secondary weapon.

    Jobs it owns: clip / scene launching (quantized to the beat), the deck mixer (8 faders =
    one fader page, master fader), deck on/off / solo, and the deck focus that the primary
    weapon's knob page follows. It never touches the primary weapon's screens or pages."""

    def __init__(self, wire):
        self.w = wire
        self.ctl = wire.controls
        rnd = random.Random(20)
        self.present = [[rnd.random() < (0.85 if r < 5 else 0.5) for r in range(BANK_ROWS)] for _ in range(DECKS)]
        self.playing = [None] * DECKS             # clip row per deck
        self.queued = [None] * DECKS              # row, "stop" or None
        self.active = [True] * DECKS
        self.solo = [False] * DECKS
        self.focus = 0                            # deck the primary weapon edits ("master" = 8)
        self.offset = 0                           # first visible clip row
        self.page = 0
        self.values = [[1.0 if p == 0 else 0.5 for _ in range(DECKS)] for p in range(len(FADER_PAGES))]
        self.picked = [[True] * DECKS for _ in FADER_PAGES]   # soft takeover per page and fader
        self.fader_pos = [None] * DECKS
        self.master = 1.0
        self.transition = 0
        self.bpm = 120.0
        self.running = True
        self.beat_t0 = time.time()
        self.beat_n = 0
        self.flash = False                        # NOTE MODE: the grid = 40 one-shot flashes
        self.flash_held = set()
        self.shift = False
        self.taps = []
        self.events = []                          # Himmel events, newest last
        self.message = ""

    # ---- events ----
    def ev(self, text):
        self.events.append((time.strftime("%H:%M:%S"), text))
        del self.events[:-40]

    def name(self, i):
        return self.ctl[i]["name"]

    def clip_name(self, d, r):
        return "D%d·%02d" % (d + 1, r + 1)

    def on_event(self, i, what, value):
        c = self.ctl[i]
        g = c.get("group")
        if g == "shift":
            self.shift = what == "down"
            return
        if what == "value" and g == "fader":
            self._fader(c, value)
            return
        if what == "delta":
            if self.shift:
                self.bpm = max(40.0, min(240.0, self.bpm + value))
                self.ev("clock.bpm %.0f" % self.bpm)
            else:
                self.transition = max(0, min(len(TRANSITIONS) - 1, self.transition + (1 if value > 0 else -1)))
                self.ev("scene.transition %s" % TRANSITIONS[self.transition])
            return
        if g == "clip" and self.flash:
            n = (c["scene"] - 1) * 8 + c["track"]
            if what == "down":
                self.flash_held.add(i)
                self.ev("fx.flash %d on (velocity 127)" % n)
            else:
                self.flash_held.discard(i)
                self.ev("fx.flash %d off" % n)
            return
        if what != "down":
            return
        t = c.get("track", 0) - 1
        if g == "clip":
            r = self.offset + c["scene"] - 1
            if self.shift:
                self.focus = t
                self.ev("deck.focus %d, cue %s (no launch)" % (t + 1, self.clip_name(t, r)))
            elif self.present[t][r]:
                self._queue(t, r)
            else:
                self._queue(t, "stop")
        elif g == "scene":
            r = self.offset + c["scene"] - 1
            self.ev("scene.launch row %d (%s)" % (r + 1, TRANSITIONS[self.transition]))
            for d in range(DECKS):
                self._queue(d, r if self.present[d][r] else "stop", quiet=True)
        elif g == "stop":
            if self.shift:
                self.ev("deck.stop all (at the beat)")
                for d in range(DECKS):
                    if self.playing[d] is not None:
                        self.queued[d] = "stop"
            elif self.playing[t] is not None:
                self._queue(t, "stop")
        elif g == "select":
            if c["name"] == "Note Mode":
                if self.shift:
                    self.focus = 8
                    self.ev("deck.focus master")
                else:
                    self.flash = not self.flash
                    self.flash_held.clear()
                    self.ev("grid.mode %s" % ("flash (40 one-shot FX)" if self.flash else "clips"))
            elif self.shift:
                self.focus = t
                self.ev("deck.focus %d (the MK2 knob page follows)" % (t + 1))
            else:
                self._transport(t)
        elif g == "activator":
            self.active[t] = not self.active[t]
            self.ev("deck.%s %d" % ("on" if self.active[t] else "off", t + 1))
        elif g == "solo":
            self.solo[t] = not self.solo[t]
            self.ev("deck.solo %d %s" % (t + 1, "on" if self.solo[t] else "off"))
        elif g == "arm":
            if self.shift:
                self.page = t
                self.picked[t] = [False] * DECKS
                self.ev("faders.page %s (pick up each fader at its value)" % FADER_PAGES[t])
            else:
                self.focus = t
                self.ev("deck.focus %d (the MK2 knob page follows)" % (t + 1))

    def _transport(self, k):
        if k == 0:
            self.running = not self.running
            if self.running:
                self.beat_t0, self.beat_n = time.time(), 0
            self.ev("clock.%s" % ("run" if self.running else "pause (launches fire at once)"))
        elif k == 1:
            self.ev("deck.stop all (now)")
            self.playing = [None] * DECKS
            self.queued = [None] * DECKS
        elif k == 2:
            d = self.focus if self.focus < DECKS else 0
            rows = [r for r in range(BANK_ROWS) if self.present[d][r] and r != self.playing[d]]
            if rows:
                self.ev("smart.random deck %d" % (d + 1))
                self._queue(d, random.choice(rows), quiet=True)
        elif k == 3:
            now = time.time()
            self.taps = [t for t in self.taps if now - t < 2.0] + [now]
            if len(self.taps) >= 3:
                self.bpm = round(60.0 * (len(self.taps) - 1) / (self.taps[-1] - self.taps[0]), 1)
                self.beat_t0, self.beat_n = now, 0
                self.ev("clock.tap %.1f BPM" % self.bpm)
        elif k in (4, 5):
            self.focus = max(0, min(DECKS - 1, (self.focus if self.focus < DECKS else 0) + (1 if k == 5 else -1)))
            self.ev("deck.focus %d" % (self.focus + 1))
        else:
            step = ROWS if self.shift else 1
            self.offset = max(0, min(BANK_ROWS - ROWS, self.offset + (step if k == 7 else -step)))
            self.ev("clips.window rows %d-%d" % (self.offset + 1, self.offset + ROWS))

    def _queue(self, d, r, quiet=False):
        if not self.running:
            self._fire(d, r)
            return
        self.queued[d] = r
        if not quiet:
            self.ev("clip.launch %s (at the next beat)" % self.clip_name(d, r) if r != "stop"
                    else "deck.stop %d (at the next beat)" % (d + 1))

    def _fire(self, d, r):
        if r == "stop":
            if self.playing[d] is not None:
                self.ev("deck.stopped %d" % (d + 1))
            self.playing[d] = None
        else:
            self.playing[d] = r
            self.ev("clip.playing %s on deck %d" % (self.clip_name(d, r), d + 1))
        self.queued[d] = None

    def _fader(self, c, value):
        v = value / 127.0
        if c["name"] == "Master":
            self.master = v
            self.ev("master.level %.2f" % v)
            return
        t = c["track"] - 1
        prev, self.fader_pos[t] = self.fader_pos[t], v
        p = self.page
        cur = self.values[p][t]
        if not self.picked[p][t]:
            # soft takeover: the fader takes over once it reaches or crosses the value
            if abs(v - cur) <= 2 / 127 or (prev is not None and (prev - cur) * (v - cur) <= 0):
                self.picked[p][t] = True
            else:
                self.message = "Fader %d: move to %d%% to pick up %s" % (t + 1, round(cur * 100), FADER_PAGES[p])
                return
        self.values[p][t] = v
        self.ev("deck.%s %d %.2f" % (FADER_PAGES[p].lower().replace(" ", "_"), t + 1, v))

    def tick(self):
        """Advance the beat clock; queued launches fire on the beat. Returns True on a beat."""
        if not self.running:
            return False
        n = int((time.time() - self.beat_t0) * self.bpm / 60.0)
        if n == self.beat_n:
            return False
        self.beat_n = n
        for d in range(DECKS):
            if self.queued[d] is not None:
                self._fire(d, self.queued[d])
        return True

    # ---- LEDs: what the device should show, per control index ----
    def wanted(self):
        out = {}
        beat_on = self.running and ((time.time() - self.beat_t0) * self.bpm / 60.0) % 1.0 < 0.25
        for i, c in enumerate(self.ctl):
            g = c.get("group")
            t = c.get("track", 0) - 1
            v = 0
            if g == "clip":
                r = self.offset + c["scene"] - 1
                if self.flash:
                    v = 3 if i in self.flash_held else 0
                elif self.queued[t] == r:
                    v = 2
                elif self.playing[t] == r:
                    v = 1
                elif self.present[t][r]:
                    v = 5
            elif g == "scene":
                r = self.offset + c["scene"] - 1
                if any(q == r for q in self.queued):
                    v = 2
                elif any(self.present[d][r] for d in range(DECKS)):
                    v = 1
            elif g == "stop":
                v = 2 if self.queued[t] == "stop" else 1 if self.playing[t] is not None else 0
            elif g == "select":
                if c["name"] == "Note Mode":
                    v = 1 if (self.focus == 8 if self.shift else self.flash) else 0
                elif self.shift:
                    v = 1 if self.focus == t else 0
                else:
                    v = [1 if self.running else 0, 0, 2 if any(q is not None for q in self.queued) else 0,
                         1 if beat_on else 0, 0, 0, 1 if self.offset > 0 else 0,
                         1 if self.offset < BANK_ROWS - ROWS else 0][t]
            elif g == "activator":
                v = 1 if self.active[t] else 0
            elif g == "solo":
                v = 1 if self.solo[t] else 0
            elif g == "arm":
                v = (1 if self.page == t else 0) if self.shift else (1 if self.focus == t else 0)
            if "led" in c:
                out[i] = v
        return out

    def state(self):
        return {"decks": [{"playing": None if p is None else self.clip_name(d, p),
                           "queued": None if q is None else ("stop" if q == "stop" else self.clip_name(d, q)),
                           "active": self.active[d], "solo": self.solo[d],
                           "values": [round(self.values[k][d], 2) for k in range(len(FADER_PAGES))],
                           "picked": self.picked[self.page][d]}
                          for d, (p, q) in enumerate(zip(self.playing, self.queued))],
                "focus": "master" if self.focus == 8 else self.focus + 1, "page": FADER_PAGES[self.page],
                "pages": FADER_PAGES, "offset": self.offset, "master": round(self.master, 2),
                "transition": TRANSITIONS[self.transition], "bpm": self.bpm, "running": self.running,
                "beat": self.beat_n, "flash": self.flash, "shift": self.shift, "message": self.message,
                "events": self.events[-24:]}


# ---------------------------------------------------------------------------
# LED tests (hardware check): what each test lights
# ---------------------------------------------------------------------------
LED_TESTS = ("green", "red", "amber", "blink", "mono", "walk", "chase", "off")


def led_test_frame(wire, test, step):
    out = {}
    leds = sorted(wire.led)
    for k, i in enumerate(leds):
        pal = wire.led[i][2]
        if test == "green":
            v = 1
        elif test == "red":
            v = 3 if pal == "tri" else 0
        elif test == "amber":
            v = 5 if pal == "tri" else 0
        elif test == "blink":
            v = 2 if pal == "mono" else (2, 4, 6)[k % 3]
        elif test == "mono":
            v = 1 if pal == "mono" else 0
        elif test == "walk":
            v = (1 if pal == "mono" else (1, 3, 5)[(step // len(leds)) % 3]) if k == step % len(leds) else 0
        elif test == "chase":
            v = 1 if (k - step) % 8 == 0 else 0
            if v and pal == "tri":
                v = (1, 3, 5)[(step // 8) % 3]
        else:
            v = 0
        out[i] = v
    return out


# ---------------------------------------------------------------------------
# The live object served by mk2_live.py
# ---------------------------------------------------------------------------
class Apc20Live:
    def __init__(self, profile):
        self.profile = profile
        self.lock = threading.RLock()
        self.map = load_map()
        self.wire = Wire(profile, self.map)
        self.host = Apc20Host(self.wire)
        self.host_on = True            # Himmel drives the LEDs (sim default; off for the real unit)
        self.held = set()
        self.fader = {i: 0 for i, c in enumerate(profile["controls"]) if c["kind"] == "fader"}
        self.enc = {i: 0 for i, c in enumerate(profile["controls"]) if c["kind"] == "encoder"}
        self.dev = None
        self.sent = {}                 # control index -> velocity last sent
        self.learn = None              # control name waiting for a press / move
        self.message = ""
        self.led_test = None
        self.test_step = 0
        self.walk_name = ""
        self.identity = None
        self.dev_id = 0
        self.save_due = 0.0
        self.opened_seen = 0
        self._use(SimAPC20())
        self.closed = False
        threading.Thread(target=self._loop, daemon=True).start()

    # ---- device ----
    def _use(self, dev):
        old = self.dev
        dev.listeners.append(self._from_device)
        self.dev = dev
        self.sent = {}
        self.held.clear()
        for k in self.fader:
            self.fader[k] = 0     # the real faders are unknown until they move
        for k in self.enc:
            self.enc[k] = 0
        if old is not None:
            old.listeners.clear()
            old.close()
        if dev.kind == "sim":
            self.host_on = True
            self._setup()
        else:
            self.host_on = False       # read-only probe until the owner presses a button
            self.led_test = None

    def _setup(self):
        """What Himmel sends on connect: identity request, then Ableton mode (host owns the LEDs)."""
        self.dev.receive(IDENTITY_REQUEST)
        self.dev.receive(mode_sysex("ableton", self.dev_id))
        self.sent = {}

    def _send_leds(self, want, force=False):
        for i, v in want.items():
            if force or self.sent.get(i) != v:
                ch, n, _ = self.wire.led[i]
                self.dev.receive(bytes([0x90 | ch, n, v]))
                self.sent[i] = v

    def _from_device(self, msg):
        with self.lock:
            idn = parse_identity(msg)
            if idn:
                self.identity = idn
                self.dev_id = idn["dev"]
                if self.dev.kind == "hw":
                    m = self._map()
                    m["identity"] = idn
                    self.save_due = time.time() + 0.3
                return
            if msg[0] == 0xF0:
                return
            note_mode = self.dev.mode == "note"
            if self.learn and self.dev.kind == "hw":
                if self._learn_from(msg):
                    return
            d = self.wire.decode(msg, note_mode)
            if d is None:
                if self.dev.kind == "hw":
                    self.message = "Unmapped: %s. Click a control, then press it, to learn it." % describe(msg)
                return
            i, what, value = d
            if self.dev.kind == "hw":
                m = self._map()
                name = self.wire.controls[i]["name"]
                if name not in m["seen"]:
                    m["seen"].append(name)
                    self.save_due = time.time() + 0.5
                if what == "up" and m.get("note_off") is None:
                    m["note_off"] = describe(msg)
                    self.save_due = time.time() + 0.5
                if note_mode and msg[0] & 0x0F == NOTE_MODE_CH:
                    m["note_mode"][name] = msg[1]
            if what == "down":
                self.held.add(i)
            elif what == "up":
                self.held.discard(i)
            elif what == "value":
                self.fader[i] = value
            elif what == "delta":
                self.enc[i] = (self.enc[i] + value) % 128
            if self.host_on and not self._engine():
                self.host.on_event(i, what, value)

    def _engine(self):
        return self.dev.kind == "sim" and self.dev.engine_active()

    def _map(self):
        if self.map is None:
            self.map = new_map()
        return self.map

    def _learn_from(self, msg):
        s = msg[0] & 0xF0
        if not ((s == 0x90 and msg[2] > 0) or s == 0xB0):
            return s in (0x80, 0x90)     # swallow the release of the learning press
        name = self.learn
        i = self.wire.index[name]
        want_cc = self.wire.controls[i]["kind"] in ("fader", "encoder")
        if want_cc != (s == 0xB0):
            self.message = "%s is a %s; that was %s. Try again." % (
                name, self.wire.controls[i]["kind"], describe(msg))
            return True
        m = self._map()
        new = {"type": "cc" if s == 0xB0 else "note", "ch": msg[0] & 0x0F, "num": msg[1]}
        old_owner = self.wire.by_addr.get((new["type"], new["ch"], new["num"]))
        if old_owner is not None and old_owner != i:
            t, ch, n, _ = self.wire.addr[i]
            m["controls"][self.wire.controls[old_owner]["name"]] = {"type": t, "ch": ch, "num": n}
        m["controls"][name] = new
        if name not in m["seen"]:
            m["seen"].append(name)
        self.wire.set_map(m)
        self.learn = None
        self.message = "Learned %s = %s%s" % (name, describe(msg), "" if old_owner in (None, i) else
                                              " (swapped with %s)" % self.wire.controls[old_owner]["name"])
        self.save_due = time.time() + 0.2
        return True

    # ---- loop: beat clock, LEDs, tests, saving ----
    def _loop(self):
        last_test = 0.0
        while not self.closed:
            time.sleep(0.02)
            with self.lock:
                dev = self.dev
                if dev.kind == "hw" and getattr(dev, "opened", 0) != self.opened_seen:
                    self.opened_seen = dev.opened
                    self.sent = {}
                    if self.host_on and dev.connected:     # reconnect: send the setup again
                        self._setup()
                if self.led_test:
                    if time.time() - last_test > (0.6 if self.led_test == "walk" else 0.15):
                        last_test = time.time()
                        self.test_step += 1
                        frame = led_test_frame(self.wire, self.led_test, self.test_step)
                        if self.led_test == "walk":
                            lit = [i for i, v in frame.items() if v]
                            self.walk_name = ("%s = %s" % (self.wire.controls[lit[0]]["name"],
                                                           TRI.get(frame[lit[0]]) if self.wire.led[lit[0]][2] == "tri"
                                                           else "on")) if lit else ""
                        self._send_leds(frame)
                elif self.host_on and not self._engine() and dev.connected:
                    self.host.tick()
                    self._send_leds(self.host.wanted())
                if self.save_due and time.time() > self.save_due and self.map is not None:
                    self.save_due = 0.0
                    try:
                        save_map(self.map)
                    except OSError as e:
                        self.message = "Could not save the map: %s" % e

    # ---- page I/O ----
    def snapshot(self):
        with self.lock:
            dev = self.dev
            leds = {}
            for i, (ch, n, _) in self.wire.led.items():
                v = dev.leds.get((ch, n), 0)
                if v:
                    leds[i] = v
            if dev.mode == "note":   # in note mode the host lights the grid via channel 10 notes
                for n, i in self.wire.note_mode.items():
                    v = dev.leds.get((NOTE_MODE_CH, n), 0)
                    if v:
                        leds[i] = v
            m = self.map or {}
            out = {
                "id": "apc20", "source": dev.kind, "connected": dev.connected, "mode": dev.mode,
                "leds": leds, "held": sorted(self.held), "faders": self.fader, "enc": self.enc,
                "host_on": self.host_on, "engine": self._engine(),
                "himmel": self.host.state(), "notes": dev.notes, "counts": dev.counts,
                "log": [list(e) for e in dev.log[-80:]], "message": self.message,
                "learn": self.learn, "led_test": self.led_test, "walk": self.walk_name,
                "identity": self.identity,
                "map": {"file": os.path.basename(MAP_FILE), "exists": self.map is not None,
                        "seen": m.get("seen", []), "learned": sorted(m.get("controls", {})),
                        "led_colors": m.get("led_colors", {}), "note_off": m.get("note_off"),
                        "total": len(self.wire.controls)},
                "wire": {i: list(a[:3]) for i, a in self.wire.addr.items()},
            }
            if dev.kind == "hw":
                out["stats"] = dev.stats
                out["ports"] = dev.port_names
                try:
                    import lightpad_hw as W
                    ins, outs = W.list_ports()
                    out["port_list"] = [n for _, n in ins]
                except Exception:
                    out["port_list"] = []
            return out

    def sim_input(self, i, down=None, value=None, delta=None):
        if self.dev.kind != "sim":
            return
        c = self.wire.controls[i]
        if c["kind"] == "button":
            msg = self.wire.encode(i, down=down, note_mode=self.dev.mode == "note")
        elif c["kind"] == "fader":
            msg = self.wire.encode(i, value=value)
        else:
            msg = self.wire.encode(i, delta=delta)
        self.dev.emit(msg)
        if self.dev.mode == "generic" and c.get("led"):
            # assumed generic-mode behaviour: the button lights while held (to be checked on the unit)
            ch, n, _ = self.wire.led[i]
            self.dev.leds[(ch, n)] = 1 if down else 0

    def command(self, d):
        with self.lock:
            if "press" in d:
                self.sim_input(int(d["press"]), down=bool(d.get("down")))
            elif "fader" in d:
                self.sim_input(int(d["fader"]), value=int(d["value"]))
            elif "turn" in d:
                self.sim_input(int(d["turn"]), delta=int(d["delta"]))
            elif d.get("use") == "sim":
                self._use(SimAPC20())
                self.message = "Simulated APC20"
            elif d.get("use") == "hw":
                port = d.get("port")
                self._use(HwAPC20(port_hint=port if port not in (None, "") else None))
                self.message = "Real APC20: read-only probe. Press the buttons below to send anything."
            elif d.get("identity"):
                self.dev.receive(IDENTITY_REQUEST)
                self.message = "Identity request sent"
            elif d.get("mode") in MODE_BYTE:
                self.dev.receive(mode_sysex(d["mode"], self.dev_id))
                self.sent = {}
                self.message = "Mode SysEx sent: %s" % d["mode"]
            elif "led_test" in d:
                t = d["led_test"]
                self.led_test = t if t in LED_TESTS and t != "off" else None
                self.test_step = -1
                self.walk_name = ""
                if t == "off" or self.led_test:
                    self.host_on = False
                    self._send_leds({i: 0 for i in self.wire.led}, force=True)
                self.message = "LED test: %s" % t
            elif "led_color" in d:   # {"led_color": name, "color": "green" | ...}: what the owner saw
                self._map()["led_colors"][d["led_color"]] = d.get("color")
                self.save_due = time.time() + 0.2
            elif "host" in d:
                self.host_on = bool(d["host"])
                self.led_test = None
                if self.host_on:
                    if self.dev.kind == "hw" and self.dev.mode not in ("ableton", "ableton-alt"):
                        self._setup()
                    self.sent = {}
                self.message = "Himmel host %s" % ("on: it drives the LEDs" if self.host_on else "off")
            elif "learn" in d:
                name = d["learn"]
                if name in self.wire.index and self.dev.kind == "hw":
                    self.learn = name
                    self.message = "Press / move %s on the APC20 now (Esc cancels)" % name
                elif self.dev.kind != "hw":
                    self.message = "Learning needs the real APC20 (Use the real APC20)"
            elif d.get("learn_cancel"):
                self.learn = None
                self.message = "Learning cancelled"
            elif d.get("reset_seen") and self.map is not None:
                self.map["seen"] = []
                self.save_due = time.time() + 0.2
                self.message = "Seen marks cleared"

    def close(self):
        self.closed = True
        with self.lock:
            if self.map is not None and self.save_due:
                try:
                    save_map(self.map)
                except OSError:
                    pass
            try:
                if self.dev.kind == "hw" and self.dev.connected and (self.host_on or self.led_test):
                    self._send_leds({i: 0 for i in self.wire.led}, force=True)
            except Exception:
                pass
            self.dev.close()


# ---------------------------------------------------------------------------
def selftest():
    prof = load_profile()
    w = Wire(prof)
    names = [c["name"] for c in prof["controls"]]
    ok = True

    def check(cond, text):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + text)
        ok &= bool(cond)

    check(len(names) == len(set(names)) == 97, "97 controls, unique names")
    check(len(w.by_addr) == len(w.addr) == 97, "every control has its own MIDI address")
    check(w.addr[w.index["Clip 3.2"]][:3] == ("note", 2, 54), "Clip 3.2 = ch3 note 54")
    check(w.addr[w.index["Scene 5"]][:3] == ("note", 0, 86), "Scene 5 = ch1 note 86")
    check(w.addr[w.index["Shift"]][:3] == ("note", 0, 81), "Shift = note 81")
    check(w.addr[w.index["Master"]][:3] == ("cc", 0, 14), "Master fader = CC 14")
    check(w.addr[w.index["Fader 8"]][:3] == ("cc", 7, 7), "Fader 8 = ch8 CC 7")
    # round trips: encode -> decode for every control
    rt = True
    for i, c in enumerate(prof["controls"]):
        if c["kind"] == "button":
            rt &= w.decode(w.encode(i, down=True)) == (i, "down", 127)
            rt &= w.decode(w.encode(i, down=False))[:2] == (i, "up")
        elif c["kind"] == "fader":
            rt &= w.decode(w.encode(i, value=99)) == (i, "value", 99)
        else:
            rt &= w.decode(w.encode(i, delta=-3)) == (i, "delta", -3)
            rt &= w.decode(w.encode(i, delta=5)) == (i, "delta", 5)
    check(rt, "encode -> decode round trip for every control")
    nm = sorted(w.note_mode)
    check(nm == list(range(36, 76)), "note mode: the grid plays notes 36..75")
    check(note_mode_note(0, 4) == 36 and note_mode_note(3, 0) == 55 and note_mode_note(4, 4) == 56
          and note_mode_note(7, 0) == 75, "note mode halves: C1 bottom-left .. D#3 top-right")
    rt = all(w.decode(w.encode(i, down=True, note_mode=True), note_mode=True) == (i, "down", 127)
             for i in w.note_mode_of)
    check(rt, "note mode round trip")
    check(mode_sysex("ableton").hex(" ") == "f0 47 00 7b 60 00 04 41 01 00 00 f7", "Ableton mode SysEx")
    check(parse_mode(mode_sysex("note", 3)) == (3, 0x43), "mode SysEx parses")
    rep = identity_reply(5)
    idn = parse_identity(rep)
    check(idn and idn["is_apc20"] and idn["dev"] == 5 and rep[13] == 5, "identity reply: APC20, device id at byte 13")
    check(rel2c(1) == 1 and rel2c(127) == -1 and rel2c(65) == -63 and to_rel2c(-2) == 126, "relative two's complement")

    # the simulated device
    sim = SimAPC20()
    got = []
    sim.listeners.append(got.append)
    sim.receive(IDENTITY_REQUEST)
    time.sleep(0.05)
    check(got and parse_identity(got[0]), "sim answers the identity request")
    sim.receive(mode_sysex("ableton", 0))
    check(sim.mode == "ableton", "sim switches to Ableton mode")
    sim.receive(bytes([0x92, 54, 4]))
    check(sim.leds.get((2, 54)) == 4, "sim keeps a clip LED (red blink)")
    sim.receive(mode_sysex("generic", 9))
    check(sim.mode == "ableton", "sim ignores a mode SysEx for another device id")
    sim.close()

    # the host, as the secondary weapon
    h = Apc20Host(w)
    h.running = False
    d0 = next(r for r in range(5) if h.present[0][r])
    h.on_event(w.index["Clip 1.%d" % (d0 + 1)], "down", 127)
    check(h.playing[0] == d0, "clip press launches the clip (clock paused = at once)")
    led = h.wanted()
    check(led[w.index["Clip 1.%d" % (d0 + 1)]] == 1 and led[w.index["Stop 1"]] == 1, "playing clip green, stop LED on")
    h.running = True
    h.beat_t0, h.beat_n = time.time(), 0
    h.on_event(w.index["Stop 1"], "down", 127)
    check(h.wanted()[w.index["Stop 1"]] == 2, "stop is queued to the beat (blinks)")
    h.beat_n = -1
    h.tick()
    check(h.playing[0] is None, "stop fires on the beat")
    h.on_event(w.index["Shift"], "down", 127)
    h.on_event(w.index["Arm 2"], "down", 127)
    h.on_event(w.index["Shift"], "up", 0)
    check(h.page == 1 and not h.picked[1][0], "Shift + Arm 2 = fader page SPEED, pickup armed")
    h.on_event(w.index["Fader 1"], "value", 0)
    check(h.values[1][0] == 0.5, "soft takeover: a fader far from the value does nothing")
    h.on_event(w.index["Fader 1"], "value", 100)
    check(h.picked[1][0] and abs(h.values[1][0] - 100 / 127) < 1e-6, "soft takeover: crossing the value picks it up")
    h.on_event(w.index["Arm 4"], "down", 127)
    check(h.focus == 3, "Arm 4 = deck focus 4 (for the primary weapon)")
    h.on_event(w.index["Cue Level"], "delta", 1)
    check(h.transition == 1, "Cue Level = scene transition time")
    # map learning and the read-only probe, with a fake real device (the map goes to a temp file)
    global MAP_FILE
    import tempfile
    keep, MAP_FILE = MAP_FILE, os.path.join(tempfile.mkdtemp(), "apc20_map.json")
    try:
        class FakeHw(DeviceBase):
            kind = "hw"
            opened = 1

            def receive(self, msg, source="host"):
                self.counts["to_device"] += 1
                self.note_out(bytes(msg))
        live = Apc20Live(prof)
        fake = FakeHw()
        live._use(fake)
        time.sleep(0.1)
        check(fake.counts["to_device"] == 0 and not live.host_on, "real device: read-only until asked")
        live.command({"learn": "Clip 1.1"})
        fake.emit(bytes([0x95, 99, 127]))
        fake.emit(bytes([0x85, 99, 127]))
        check(live.wire.addr[w.index["Clip 1.1"]][:3] == ("note", 5, 99) and live.learn is None, "click-then-press learns a control")
        fake.emit(bytes([0x90, 86, 127]))
        check("Scene 5" in live.map["seen"], "touching a control on the real device marks it seen")
        live.save_due = time.time()
        time.sleep(0.15)
        check(os.path.exists(MAP_FILE), "the map is saved")
        live.command({"mode": "ableton"})
        check(fake.mode == "ableton", "mode SysEx goes out when asked")
        live.close()
    finally:
        MAP_FILE = keep

    frame = led_test_frame(w, "amber", 0)
    check(frame[w.index["Clip 1.1"]] == 5 and frame[w.index["Scene 1"]] == 0, "LED test amber lights only the grid")
    print("selftest:", "PASS" if ok else "FAIL")
    return ok


def main():
    a = sys.argv[1:]
    if "--selftest" in a:
        sys.exit(0 if selftest() else 1)
    if "--ports" in a:
        import lightpad_hw as W
        ins, outs = W.list_ports()
        print("MIDI in: ", ins)
        print("MIDI out:", outs)
        return
    print(__doc__)


if __name__ == "__main__":
    main()
