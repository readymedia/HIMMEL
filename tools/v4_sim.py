# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Simulated V-4 video mixer ("weapon no. 7")
==========================================

A pretend Roland / EDIROL V-4: a four-channel SD video mixer with a T-bar, two
input selectors (A and B), four effect buttons and a CONTROL dial per channel,
MIX / WIPE / EFX transitions, transformer buttons, output fade, BPM and memory.
It talks MIDI only (5-pin DIN IN / OUT, no USB), so Himmel would reach it through
any USB-MIDI interface.

Everything here follows the MIDI implementation in the V-4 owner's manual
(pp. 89-96). See weapons/README.md, "Weapon no. 7", for the summary and sources.

What the panel sends (factory MIDI setup, channel 1):
  A / B input 1-4  -> Bank Select MSB 00 (A) / 01 (B), LSB 00, Program Change 0-3
  Memory 1-8       -> Bank Select 50H / 00H, Program Change 0-7
  Video fader      -> CC#11 (Expression), 0 = A ... 127 = B
  everything else  -> nothing, until a MIDI setup assigns it a CC (SysEx DT1)
What it receives: the same messages, CC#7 = transition time (0.0-4.0 s),
Note Mode notes (24H 26H 28H 29H = A 1-4, 2BH 2DH 2FH 30H = B 1-4), Timing Clock
F8 (BPM sync), and SysEx DT1 to model 00 5B for the MIDI setup.

The live view is the host (a sketch of Himmel's future driver): it decodes the
V-4's messages, can drive the mixer (inputs, T-bar, auto take, output fade,
MIDI clock) and can send "Himmel's MIDI setup", which gives every panel
function its own CC so Himmel sees all of it.

Engine link: every MIDI message the V-4 sends also goes to UDP 127.0.0.1:8773;
a datagram sent back to the sender is MIDI into the V-4.

Self-test:  python tools/v4_sim.py
"""

import socket
import sys
import threading
import time

ENGINE_ADDR = ("127.0.0.1", 8773)   # MK2 8766, Lightpad 8767, Entrancer 8768, LUMI 8769, KF4 8771, V-4 8773
HOST_TIMEOUT = 1.5

ROLAND = 0x41
MODEL = (0x00, 0x5B)                # V-4 model ID
DT1 = 0x12
DEVICE_ID = 0x10                    # "17" in Roland's numbering; the manual's example uses it

# Panel functions that the MIDI setup can assign (manual 3-1-2 / 3-1-3), in address order.
# The address is 3 bytes; the value is a "#" parameter: two bytes, high nibble then low nibble.
ASSIGNABLE = [
    ("Transition time", (0x01, 0x10, 0x00)),
    ("Video fader", (0x01, 0x10, 0x02)),
    ("Transition select", (0x01, 0x10, 0x04)),
    ("Transformer A", (0x01, 0x10, 0x06)),
    ("Transformer B", (0x01, 0x10, 0x08)),
    ("Output fade", (0x01, 0x10, 0x0A)),
    ("BPM sync", (0x01, 0x10, 0x0C)),
    ("A effect 1", (0x02, 0x10, 0x00)),
    ("A effect 2", (0x02, 0x10, 0x02)),
    ("A effect 3", (0x02, 0x10, 0x04)),
    ("A effect 4", (0x02, 0x10, 0x06)),
    ("B effect 1", (0x02, 0x10, 0x08)),
    ("B effect 2", (0x02, 0x10, 0x0A)),
    ("B effect 3", (0x02, 0x10, 0x0C)),
    ("B effect 4", (0x02, 0x10, 0x0E)),
]
ADDR_FUNC = {a: n for n, a in ASSIGNABLE}
PRESSURE, BEND, OFF = 0xD0, 0xE0, 0xEF       # assignment values that are not a CC number

FACTORY_SETUP = {n: OFF for n, _ in ASSIGNABLE}
FACTORY_SETUP.update({"Transition time": 7, "Video fader": 11})      # manual p. 95-96
# Himmel's setup: one CC per function, all from the allowed set (1-5, 7-31, 64-95)
HIMMEL_SETUP = {"Transition time": 7, "Video fader": 11, "Transition select": 20,
                "Transformer A": 21, "Transformer B": 22, "Output fade": 23, "BPM sync": 24,
                "A effect 1": 16, "A effect 2": 17, "A effect 3": 18, "A effect 4": 19,
                "B effect 1": 80, "B effect 2": 81, "B effect 3": 82, "B effect 4": 83}

BANK_A, BANK_B, BANK_MEMORY = 0x00, 0x01, 0x50
NOTE_INPUT = {0x24: ("A", 1), 0x26: ("A", 2), 0x28: ("A", 3), 0x29: ("A", 4),
              0x2B: ("B", 1), 0x2D: ("B", 2), 0x2F: ("B", 3), 0x30: ("B", 4)}
EFFECTS = {"A": ["STROBE", "NEGATIVE", "COLORIZE", "MULTI"],       # factory assignment, top to bottom
           "B": ["MIRROR", "CHROMA KEY", "LUMI KEY", "P in P"]}
TRANSITIONS = ["MIX", "WIPE", "EFX"]


def valid_cc(v):
    return 1 <= v <= 5 or 7 <= v <= 31 or 64 <= v <= 95


def assign_name(v):
    if v == OFF:
        return "off"
    if v == PRESSURE:
        return "channel pressure"
    if v == BEND:
        return "pitch bend"
    return "CC#%d" % v if valid_cc(v) else "?%02X" % v


# ---------------------------------------------------------------- SysEx
def roland_sum(body):
    """Roland checksum: address + data + sum must be 0 in the low 7 bits."""
    return (128 - sum(body) % 128) % 128


def dt1(addr, data, dev=DEVICE_ID):
    body = list(addr) + list(data)
    return bytes([0xF0, ROLAND, dev, MODEL[0], MODEL[1], DT1] + body + [roland_sum(body), 0xF7])


def nibbles(v):
    return (v >> 4) & 0x0F, v & 0x0F


def setup_messages(setup):
    """SysEx DT1 messages that give each function its controller (send >= 20 ms apart)."""
    return [dt1(addr, nibbles(setup[name])) for name, addr in ASSIGNABLE if name in setup]


def parse_dt1(msg):
    """-> (addr tuple, data bytes) for a V-4 DT1 with a good checksum, else None."""
    if len(msg) < 12 or msg[0] != 0xF0 or msg[-1] != 0xF7 or msg[1] != ROLAND:
        return None
    if tuple(msg[3:5]) != MODEL or msg[5] != DT1:
        return None
    body = msg[6:-2]
    if roland_sum(body) != msg[-2]:
        return None
    return tuple(body[:3]), bytes(body[3:])


def describe(msg, setup=None):
    """A line of English for the MIDI monitor."""
    if not msg:
        return "-"
    st = msg[0]
    if st == 0xF8:
        return "timing clock"
    if st in (0xFA, 0xFB, 0xFC):
        return {0xFA: "start", 0xFB: "continue", 0xFC: "stop"}[st]
    if st == 0xFE:
        return "active sensing"
    if st == 0xF0:
        p = parse_dt1(msg)
        if p is None:
            return "SysEx (not a V-4 DT1, or bad checksum)"
        addr, data = p
        if addr == (0, 0, 0) and data:
            return "DT1 MIDI receive channel = %s" % ("off" if data[0] >= 16 else data[0] + 1)
        name = ADDR_FUNC.get(addr)
        if name and len(data) >= 2:
            return "DT1 setup: %s <- %s" % (name, assign_name((data[0] << 4) | data[1]))
        return "DT1 %s = %s" % (" ".join("%02X" % a for a in addr), data.hex(" "))
    kind, ch = st & 0xF0, (st & 0x0F) + 1
    if kind == 0xB0 and len(msg) >= 3:
        cc, v = msg[1], msg[2]
        if cc == 0:
            return "ch%d bank select MSB %02XH" % (ch, v)
        if cc == 32:
            return "ch%d bank select LSB %02XH" % (ch, v)
        names = [n for n, a in (setup or {}).items() if a == cc]
        return "ch%d CC#%d = %d%s" % (ch, cc, v, ("  (" + ", ".join(names) + ")") if names else "")
    if kind == 0xC0 and len(msg) >= 2:
        return "ch%d program change %d" % (ch, msg[1])
    if kind == 0x90 and len(msg) >= 3:
        hit = NOTE_INPUT.get(msg[1])
        return "ch%d note on %02XH vel %d%s" % (ch, msg[1], msg[2], ("  (%s input %d)" % hit) if hit else "")
    if kind == 0x80 and len(msg) >= 3:
        return "ch%d note off %02XH" % (ch, msg[1])
    if kind == 0xD0 and len(msg) >= 2:
        return "ch%d channel pressure %d" % (ch, msg[1])
    if kind == 0xE0 and len(msg) >= 3:
        return "ch%d pitch bend %d" % (ch, ((msg[2] << 7) | msg[1]) - 8192)
    return "MIDI " + msg.hex(" ")


# ---------------------------------------------------------------- the simulated mixer
class SimV4:
    """The mixer's state, its panel, and its MIDI IN / OUT."""

    kind = "sim"

    def __init__(self, engine=True):
        self.lock = threading.RLock()
        self.t0 = time.time()
        self.tx_ch = 0                    # MIDI channel 1 (factory) for sending...
        self.rx_ch = 0                    # ...and receiving
        self.note_mode = False            # factory: off
        self.midi_thru = False            # on = the V-4's own messages are not sent
        self.setup = dict(FACTORY_SETUP)
        self.sel = {"A": 1, "B": 2}       # input on each channel
        self.tbar = 0                     # 0..127, 0 = A
        self.transition = 0               # 0 MIX, 1 WIPE, 2 EFX
        self.trans_time = 0               # 0..127 = 0.0..4.0 s
        self.fade = 64                    # 0 black, 64 centre, 127 white
        self.fx = {"A": [False] * 4, "B": [False] * 4}
        self.fx_val = {"A": [64] * 4, "B": [64] * 4}
        self.fx_sel = {"A": None, "B": None}   # the blinking effect the CONTROL dial adjusts
        self.transformer = {"A": False, "B": False}
        self.bpm = 120.0
        self.bpm_sync = False
        self.memory = 1
        self.preview = "out"
        self.menu_open, self.menu_sel = False, 0
        self.held = set()
        self.taps = []
        self.clock_ticks = []             # times of received F8, for BPM from MIDI clock
        self.beat0 = time.time()
        self.log = []                     # (time, dir, hex, text, noisy, seq)
        self.noise_log = []
        self.log_seq = 0
        self.counts = {"to_host": 0, "to_device": 0, "bad": 0}
        self.listeners = []
        self.closed = False
        self.engine_time = 0.0
        self.engine_addr = None
        self.engine_out = ENGINE_ADDR
        self.notes = ["Simulated V-4 - no hardware. Play the drawn panel with the mouse."]
        self.udp = None
        if engine:
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

    def close(self):
        self.closed = True
        if self.udp is not None:
            try:
                self.udp.close()
            except OSError:
                pass

    def engine_active(self):
        return time.time() - self.engine_time < HOST_TIMEOUT

    # ---- MIDI OUT ----
    def _log(self, direction, msg):
        # MIDI clock (24 a beat) and active sensing go to their own short list, so they never push
        # the interesting messages out of the monitor
        noisy = msg[0] in (0xF8, 0xFE)
        self.log_seq += 1
        e = (time.strftime("%H:%M:%S"), direction, bytes(msg[:40]).hex(" ") + (" …" if len(msg) > 40 else ""),
             describe(msg, self.setup), noisy, self.log_seq)
        lst = self.noise_log if noisy else self.log
        lst.append(e)
        del lst[:-(40 if noisy else 300)]

    def send(self, msg):
        msg = bytes(msg)
        if self.midi_thru:
            return                        # manual p. 91: with MIDI Thru on, the V-4's own messages are not sent
        with self.lock:
            self.counts["to_host"] += 1
            self._log("V-4 → host", msg)
        for f in list(self.listeners):
            try:
                f(msg)
            except Exception:
                pass
        if self.udp is not None:
            try:
                self.udp.sendto(msg, self.engine_out)
            except OSError:
                pass

    def _cc(self, cc, v):
        self.send(bytes([0xB0 | self.tx_ch, cc, max(0, min(127, int(v)))]))

    def _out(self, func, v):
        """Send a panel function the way the MIDI setup says (CC, pressure, bend or nothing)."""
        a = self.setup.get(func, OFF)
        v = max(0, min(127, int(v)))
        if a == PRESSURE:
            self.send(bytes([0xD0 | self.tx_ch, v]))
        elif a == BEND:
            b = v << 7
            self.send(bytes([0xE0 | self.tx_ch, b & 0x7F, b >> 7]))
        elif valid_cc(a):
            self._cc(a, v)

    def _program(self, bank, prog):
        self._cc(0, bank)
        self._cc(32, 0)
        self.send(bytes([0xC0 | self.tx_ch, prog]))

    # ---- the panel (mouse in the live view) ----
    def button(self, name, down):
        with self.lock:
            if (name in self.held) == down:
                return
            (self.held.add if down else self.held.discard)(name)
        if name.startswith("Preview") and down:
            with self.lock:
                self.preview = "out" if name.endswith("Out") else int(name[-1])
        elif name == "Tap" and down:
            self._tap()
        elif name[1:].startswith(" input ") and down:            # INPUT SELECT buttons, A 1-4 and B 1-4
            self.select(name[0], int(name[-1]))
        elif name in ("Menu", "Up", "Down", "Enter") and down:
            self._menu(name)
        elif name in ("Mix", "Wipe", "EFX") and down:
            with self.lock:
                self.transition = ("Mix", "Wipe", "EFX").index(name)
            self._out("Transition select", self.transition)
        elif name == "BPM sync" and down:
            with self.lock:
                self.bpm_sync = not self.bpm_sync
            self._out("BPM sync", 127 if self.bpm_sync else 0)
        elif name.endswith("transformer"):
            ch = name[0]
            with self.lock:
                self.transformer[ch] = down       # factory: an instant cut to that channel while held
            self._out("Transformer " + ch, 127 if down else 0)
        elif " effect " in name and down:
            ch, i = name[0], int(name[-1]) - 1
            with self.lock:
                if not self.fx[ch][i]:
                    self.fx[ch][i], self.fx_sel[ch] = True, i
                elif self.fx_sel[ch] != i:
                    self.fx_sel[ch] = i              # a lit effect: select it for the CONTROL dial
                    return
                else:
                    self.fx[ch][i], self.fx_sel[ch] = False, None
                on, v = self.fx[ch][i], self.fx_val[ch][i]
            self._out(name, max(1, v) if on else 0)  # 0 = off, 1..127 = on with the parameter

    def knob(self, name, delta):
        delta = int(delta)
        if not delta:
            return
        if name == "Memory":
            with self.lock:
                n = max(1, min(8, self.memory + (1 if delta > 0 else -1)))
                if n == self.memory:
                    return
                self.memory = n
            self._program(BANK_MEMORY, n - 1)
        elif name == "BPM control":
            with self.lock:
                self.bpm = max(40.0, min(250.0, round(self.bpm) + delta / 4))
        elif name == "Output fade":
            with self.lock:
                self.fade = max(0, min(127, self.fade + delta))
                v = self.fade
            self._out("Output fade", v)
        elif name in ("A control", "B control"):
            ch = name[0]
            with self.lock:
                i = self.fx_sel[ch]
                if i is None:
                    return                           # no blinking effect: the dial does nothing
                self.fx_val[ch][i] = max(1, min(127, self.fx_val[ch][i] + delta))
                v = self.fx_val[ch][i]
            self._out("%s effect %d" % (ch, i + 1), v)

    def select(self, ch, n):
        with self.lock:
            if self.sel[ch] == n:
                return
            self.sel[ch] = n
        self._program(BANK_A if ch == "A" else BANK_B, n - 1)

    # The on-screen menu on the preview output. The real V-4's menu pages are not documented in what we read;
    # this one holds the MIDI settings the manual names (p. 95), so they can be changed on the drawn panel.
    MENU = ("MIDI TX CH", "MIDI RX CH", "NOTE MODE", "MIDI THRU", "TRANSITION TIME")

    def _menu(self, name):
        with self.lock:
            if name == "Menu":
                if not self.menu_open:
                    self.menu_open, self.menu_sel = True, 0
                else:
                    self.menu_sel = (self.menu_sel + 1) % len(self.MENU)
                return
            if not self.menu_open:
                return
            if name == "Enter":
                self.menu_open = False
                return
            d = 1 if name == "Up" else -1
            item = self.MENU[self.menu_sel]
            if item == "MIDI TX CH":
                self.tx_ch = (self.tx_ch + d) % 16
            elif item == "MIDI RX CH":
                self.rx_ch = (self.rx_ch + d) % 17          # 16 = off
            elif item == "NOTE MODE":
                self.note_mode = not self.note_mode
            elif item == "MIDI THRU":
                self.midi_thru = not self.midi_thru
            else:
                self.trans_time = max(0, min(127, self.trans_time + 4 * d))

    def menu(self):
        with self.lock:
            vals = [self.tx_ch + 1, "OFF" if self.rx_ch >= 16 else self.rx_ch + 1, "ON" if self.note_mode else "OFF",
                    "ON" if self.midi_thru else "OFF", "%.1f s" % (self.trans_time * 4 / 127)]
            return {"open": self.menu_open, "sel": self.menu_sel, "items": [[n, v] for n, v in zip(self.MENU, vals)]}

    def fader(self, name, value):
        if name != "Video fader":
            return
        v = max(0, min(127, round(float(value) * 127)))
        with self.lock:
            if v == self.tbar:
                return
            self.tbar = v
        self._out("Video fader", v)

    def release_all(self):
        for n in list(self.held):
            self.button(n, False)

    def _tap(self):
        now = time.time()
        with self.lock:
            self.taps = [t for t in self.taps if now - t < 2.5] + [now]
            if len(self.taps) >= 2:
                d = (self.taps[-1] - self.taps[0]) / (len(self.taps) - 1)
                self.bpm = max(40.0, min(250.0, round(60 / d, 1)))
            self.beat0 = now

    def beat(self):
        """Beat phase 0..1 and beat count, from the BPM (or the MIDI clock)."""
        t = (time.time() - self.beat0) * self.bpm / 60
        return t % 1.0, int(t)

    # ---- MIDI IN ----
    def _rx_loop(self):
        while not self.closed:
            try:
                r, addr = self.udp.recvfrom(4096)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue                  # Windows: queued "port unreachable"; drain, no sleep
            except OSError:
                if self.closed:
                    return
                time.sleep(0.05)
                continue
            if r:
                self.engine_time = time.time()
                self.engine_addr = addr
                self.receive(r, "engine")

    def receive(self, msg, source="host"):
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log(source + " → V-4", msg)
            self._receive(msg)

    def _receive(self, msg):
        st = msg[0]
        if st == 0xF8:
            now = time.time()
            self.clock_ticks = [t for t in self.clock_ticks if now - t < 2] + [now]
            if len(self.clock_ticks) >= 25:   # 24 clocks per beat
                d = (self.clock_ticks[-1] - self.clock_ticks[0]) / (len(self.clock_ticks) - 1)
                self.bpm = float(max(40, min(250, round(60 / (24 * d)))))   # clock jitter: whole BPM
            return
        if st in (0xFA, 0xFB):
            self.beat0 = time.time()
            return
        if st == 0xF0:
            p = parse_dt1(msg)
            if p is None or msg[2] not in (DEVICE_ID, 0x7F):
                self.counts["bad"] += 1
                return
            addr, data = p
            if addr == (0, 0, 0) and data:
                self.rx_ch = data[0]                   # 16 = off
            elif addr in ADDR_FUNC and len(data) >= 2:  # "#" value: high nibble, low nibble
                v = (data[0] << 4) | data[1]
                if v in (OFF, PRESSURE, BEND) or valid_cc(v):
                    self.setup[ADDR_FUNC[addr]] = v
                else:
                    self.counts["bad"] += 1
            return
        if st >= 0xF0 or (st & 0x0F) != self.rx_ch:
            return
        kind = st & 0xF0
        if kind == 0xB0 and len(msg) >= 3:
            cc, v = msg[1], msg[2]
            if cc == 0:
                self._bank = v
            elif cc == 32:
                pass                                   # LSB ignored (manual p. 89)
            else:
                for func, a in self.setup.items():
                    if a == cc:
                        self._apply(func, v)
        elif kind == 0xC0 and len(msg) >= 2:
            bank, prog = getattr(self, "_bank", 0), msg[1]
            if bank in (BANK_A, BANK_B) and prog <= 3:
                self.sel["A" if bank == BANK_A else "B"] = prog + 1
            elif bank == BANK_MEMORY and prog <= 7:
                self.memory = prog + 1
        elif kind == 0x90 and len(msg) >= 3 and msg[2] > 0 and self.note_mode:
            hit = NOTE_INPUT.get(msg[1])
            if hit:
                self.sel[hit[0]] = hit[1]
        elif kind == 0xD0 and len(msg) >= 2:
            for func, a in self.setup.items():
                if a == PRESSURE:
                    self._apply(func, msg[1])
        elif kind == 0xE0 and len(msg) >= 3:
            for func, a in self.setup.items():
                if a == BEND:
                    self._apply(func, msg[2])

    def _apply(self, func, v):
        """A received controller value for a panel function (manual p. 95-96)."""
        if func == "Video fader":
            self.tbar = v
        elif func == "Transition time":
            self.trans_time = v
        elif func == "Transition select":
            self.transition = v if v <= 2 else min(2, v * 3 // 128)
        elif func.startswith("Transformer"):
            self.transformer[func[-1]] = v >= 64
        elif func == "Output fade":
            self.fade = v
        elif func == "BPM sync":
            self.bpm_sync = v >= 64
        elif " effect " in func:
            ch, i = func[0], int(func[-1]) - 1
            self.fx[ch][i] = v > 0
            if v > 0:
                self.fx_val[ch][i] = v

    # ---- what the live view shows ----
    def state(self):
        phase, n = self.beat()
        with self.lock:
            return {"a": self.sel["A"], "b": self.sel["B"], "tbar": round(self.tbar / 127, 3),
                    "trans": TRANSITIONS[self.transition], "trans_time": round(self.trans_time * 4 / 127, 2),
                    "fade": round(max(-1.0, min(1.0, (self.fade - 64) / 63.5)), 3),   # -1 black .. +1 white
                    "fx_a": [EFFECTS["A"][i] if on else "" for i, on in enumerate(self.fx["A"])],
                    "fx_b": [EFFECTS["B"][i] if on else "" for i, on in enumerate(self.fx["B"])],
                    "fx_val_a": list(self.fx_val["A"]), "fx_val_b": list(self.fx_val["B"]),
                    "transformer_a": self.transformer["A"], "transformer_b": self.transformer["B"],
                    "bpm": round(self.bpm, 1), "bpm_sync": self.bpm_sync, "beat": n, "phase": round(phase, 3),
                    "memory": self.memory, "preview": self.preview}


# ---------------------------------------------------------------- the host (Himmel's driver sketch)
class V4Host:
    """The live view standing in for Himmel: reads the V-4 and can drive it."""

    def __init__(self, sim):
        self.sim = sim
        self.lock = threading.Lock()
        self.seen = {"a": None, "b": None, "tbar": None, "memory": None, "cc": {}}
        self.events = []
        self.bank = 0
        self.setup = dict(FACTORY_SETUP)     # what Himmel believes the V-4's MIDI setup is
        self.clock = False
        self.take_busy = False
        self.closed = False
        sim.listeners.append(self.on_midi)
        threading.Thread(target=self._clock_loop, daemon=True).start()

    def to_v4(self, msg):
        self.sim.receive(msg, "host")

    def on_midi(self, msg):
        st = msg[0] & 0xF0
        with self.lock:
            if st == 0xB0 and msg[1] == 0:
                self.bank = msg[2]
            elif st == 0xB0 and msg[1] != 32:
                self.seen["cc"][msg[1]] = msg[2]
                names = [n for n, a in self.setup.items() if a == msg[1]]
                if "Video fader" in names:
                    self.seen["tbar"] = msg[2]
                self._event("%s = %d" % (" / ".join(names) or "CC#%d" % msg[1], msg[2]))
            elif st == 0xC0:
                if self.bank in (BANK_A, BANK_B):
                    ch = "a" if self.bank == BANK_A else "b"
                    self.seen[ch] = msg[1] + 1
                    self._event("input %s = %d" % (ch.upper(), msg[1] + 1))
                elif self.bank == BANK_MEMORY:
                    self.seen["memory"] = msg[1] + 1
                    self._event("memory %d" % (msg[1] + 1))

    def _event(self, text):
        self.events.append(time.strftime("%H:%M:%S ") + text)
        del self.events[:-8]

    # ---- Himmel drives the mixer ----
    def select(self, ch, n):
        self.to_v4(bytes([0xB0, 0, BANK_A if ch == "A" else BANK_B]))
        self.to_v4(bytes([0xB0, 32, 0]))
        self.to_v4(bytes([0xC0, int(n) - 1]))

    def tbar(self, v01):
        cc = self.setup.get("Video fader", 11)
        if valid_cc(cc):
            self.to_v4(bytes([0xB0, cc, max(0, min(127, round(v01 * 127)))]))

    def fade(self, v01):                      # -1 black .. 0 .. +1 white
        cc = self.setup.get("Output fade", OFF)
        if valid_cc(cc):
            self.to_v4(bytes([0xB0, cc, max(0, min(127, round(64 + v01 * 63.5)))]))

    def take(self, seconds=1.5):
        """Auto take: sweep the T-bar to the other side over `seconds` (Himmel's own timing)."""
        if self.take_busy:
            return
        start = self.sim.tbar
        end = 0 if start >= 64 else 127
        cc = self.setup.get("Video fader", 11)

        def run():
            self.take_busy = True
            t0 = time.time()
            last = None
            while not self.closed:
                k = min(1.0, (time.time() - t0) / max(0.05, seconds))
                v = round(start + (end - start) * k)
                if v != last:
                    self.to_v4(bytes([0xB0, cc, v]))
                    last = v
                if k >= 1:
                    break
                time.sleep(0.02)
            self.take_busy = False
        threading.Thread(target=run, daemon=True).start()

    def send_setup(self, which):
        setup = HIMMEL_SETUP if which == "himmel" else FACTORY_SETUP
        msgs = setup_messages(setup)

        def run():
            for m in msgs:
                self.to_v4(m)
                time.sleep(0.025)             # manual: at least 20 ms between DT1 packets
        with self.lock:
            self.setup = dict(setup)
        threading.Thread(target=run, daemon=True).start()

    def _clock_loop(self):
        nxt = time.time()
        while not self.closed:
            if not self.clock:
                time.sleep(0.05)
                nxt = time.time()
                continue
            bpm = getattr(self, "clock_bpm", 128.0)
            nxt += 60 / (bpm * 24)
            self.to_v4(b"\xF8")
            time.sleep(max(0.0, nxt - time.time()))

    def close(self):
        self.closed = True


# ---------------------------------------------------------------- the live view object
class V4Live:
    """What tools/mk2_live.py talks to for ?weapon=v4: snapshot() and command()."""

    weapon = "v4"

    def __init__(self, profile):
        self.profile = profile
        self.sim = SimV4()
        self.host = V4Host(self.sim)
        self.show_noise = False

    def snapshot(self):
        s, h = self.sim, self.host
        st = s.state()
        with s.lock:
            log = sorted(s.log[-18:] + (s.noise_log[-18:] if self.show_noise else []), key=lambda e: e[5])
            log = [e[:4] for e in log[-18:]]
            counts, held, setup = dict(s.counts), sorted(s.held), dict(s.setup)
            fx_sel = dict(s.fx_sel)
            rx_ch, note_mode, thru = s.rx_ch, s.note_mode, s.midi_thru
        phase, blink = st["phase"], int(time.time() * 3) % 2 == 0
        lit = {}
        for ch, col in (("A", "#ff6b6b"), ("B", "#4dabf7")):
            for i in range(4):
                if (st["fx_" + ch.lower()][i]) and (fx_sel[ch] != i or blink):
                    lit["%s effect %d" % (ch, i + 1)] = col
        lit[("Mix", "Wipe", "EFX")[TRANSITIONS.index(st["trans"])]] = "#ffd166"
        lit["A input %d" % st["a"]] = lit["B input %d" % st["b"]] = "#ff5a1f"
        menu = s.menu()
        if menu["open"]:
            for n in ("Menu", "Up", "Down", "Enter"):
                lit[n] = "#ffffff"
        p = st["preview"]
        lit["Preview Out" if p == "out" else "Preview %d" % p] = "#3ddc84"
        if st["bpm_sync"] or phase < 0.15:
            lit["Tap" if not st["bpm_sync"] else "BPM sync"] = "#ffd166"
        if st["bpm_sync"] and phase < 0.15:
            lit["Tap"] = "#ffd166"
        for ch in "AB":
            if st["transformer_" + ch.lower()]:
                lit[ch + " transformer"] = "#ffd166"
        fade_txt = "centre" if st["fade"] == 0 else ("white %d%%" % round(st["fade"] * 100) if st["fade"] > 0
                                                       else "black %d%%" % round(-st["fade"] * 100))
        ctrl = {}
        for ch in "AB":
            i = fx_sel[ch]
            ctrl[ch] = (i, st["fx_val_" + ch.lower()][i]) if i is not None else (None, None)
        knobs = {"Output fade": s.fade, "Memory": st["memory"] - 1, "BPM control": int(st["bpm"]) % 128,
                 "A control": ctrl["A"][1] if ctrl["A"][1] is not None else 0,
                 "B control": ctrl["B"][1] if ctrl["B"][1] is not None else 0}
        knob_text = {"Output fade": fade_txt, "Memory": "MEM %d" % st["memory"], "BPM control": "%g" % st["bpm"],
                     "A control": ("%s %d" % (EFFECTS["A"][ctrl["A"][0]], ctrl["A"][1])) if ctrl["A"][0] is not None else "-",
                     "B control": ("%s %d" % (EFFECTS["B"][ctrl["B"][0]], ctrl["B"][1])) if ctrl["B"][0] is not None else "-"}
        with h.lock:
            seen = {k: v for k, v in h.seen.items() if k != "cc"}
            hsetup_name = "himmel" if h.setup == HIMMEL_SETUP else "factory" if h.setup == FACTORY_SETUP else "custom"
            events = list(h.events)
        return {
            "weapon": self.weapon,
            "mixer": True,
            "simulated": True,
            "hardware": False,
            "connected": True,
            "engine": s.engine_active(),
            "engine_port": ENGINE_ADDR[1],
            "mode_name": "MIDI (DIN), channel %d" % (s.tx_ch + 1),
            "knobs": knobs,
            "knob_text": knob_text,
            "faders": {"Video fader": st["tbar"]},
            "held": held,
            "lit": lit,
            "displays": {"bpm": "%03d" % round(st["bpm"])},          # three red digits, e.g. 042
            "menu": menu,
            "mix": st,
            "setup": [[n, assign_name(setup[n])] for n, _ in ASSIGNABLE],
            "device": {"rx_ch": "off" if rx_ch >= 16 else rx_ch + 1, "note_mode": note_mode, "midi_thru": thru},
            "host": {"seen": seen, "setup": hsetup_name, "clock": h.clock, "clock_bpm": getattr(h, "clock_bpm", 128.0),
                     "events": events, "take_busy": h.take_busy},
            "counts": counts,
            "log": log,
            "notes": s.notes,
        }

    def command(self, d):
        s, h = self.sim, self.host
        i = d.get("sim_input")
        if i:
            if i.get("release_all"):
                s.release_all()
            elif "down" in i:
                s.button(i.get("name", ""), bool(i["down"]))
            elif "delta" in i:
                s.knob(i.get("name", ""), i["delta"])
            elif "value" in i:
                s.fader(i.get("name", ""), i["value"])
            return
        if "show_noise" in d:
            self.show_noise = bool(d["show_noise"])
        if "host_select" in d:
            ch, n = d["host_select"]
            if ch in ("A", "B") and 1 <= int(n) <= 4:
                h.select(ch, int(n))
        if "host_tbar" in d:
            h.tbar(float(d["host_tbar"]))
        if "host_fade" in d:
            h.fade(float(d["host_fade"]))
        if d.get("host_take"):
            h.take(float(d.get("seconds", 1.5)))
        if d.get("host_setup") in ("himmel", "factory"):
            h.send_setup(d["host_setup"])
        if "host_clock" in d:
            h.clock = bool(d["host_clock"])
            if "bpm" in d:
                h.clock_bpm = max(40.0, min(250.0, float(d["bpm"])))
        if "note_mode" in d:
            with s.lock:
                s.note_mode = bool(d["note_mode"])
        if "midi_thru" in d:
            with s.lock:
                s.midi_thru = bool(d["midi_thru"])
        if "host_note" in d:                 # Note Mode test: A/B input by note number
            note = int(d["host_note"])
            h.to_v4(bytes([0x90, note, 100]))
            h.to_v4(bytes([0x80, note, 0]))

    def close(self):
        self.host.close()
        self.sim.close()


# ---------------------------------------------------------------- self-test
def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond

    # the manual's own checksum example (p. 95): transition time <- Modulation
    ex = dt1((0x01, 0x10, 0x00), (0x01,))
    check("manual DT1 example " + ex.hex(" "), ex == bytes.fromhex("F0 41 10 00 5B 12 01 10 00 01 6E F7"))
    check("parse it back", parse_dt1(ex) == ((0x01, 0x10, 0x00), b"\x01"))
    bad = bytearray(ex)
    bad[-2] ^= 1
    check("bad checksum rejected", parse_dt1(bytes(bad)) is None)

    s = SimV4(engine=False)
    h = V4Host(s)
    out = []
    s.listeners.append(out.append)
    s.button("A input 2", True)
    s.button("A input 2", False)
    check("A input button 2 sends bank 00/00 + program 1",
          out[-3:] == [b"\xB0\x00\x00", b"\xB0\x20\x00", b"\xC0\x01"] and h.seen["a"] == 2)
    s.knob("Memory", +1)
    check("memory 2 sends bank 50H + program 1", out[-3:] == [b"\xB0\x00\x50", b"\xB0\x20\x00", b"\xC0\x01"])
    s.fader("Video fader", 1.0)
    check("T-bar to B sends CC#11 = 127 (factory)", out[-1] == b"\xB0\x0B\x7F" and h.seen["tbar"] == 127)
    n = len(out)
    s.button("A effect 1", True)
    s.button("A effect 1", False)
    check("factory setup: an effect button sends nothing", len(out) == n and s.fx["A"][0])
    h.select("B", 4)
    check("host selects B input 4", s.sel["B"] == 4)
    for m in setup_messages(HIMMEL_SETUP):
        s.receive(m)
    check("Himmel's MIDI setup lands (A effect 1 -> CC#16)", s.setup["A effect 1"] == 16 and s.setup["Output fade"] == 23)
    h.setup = dict(HIMMEL_SETUP)
    s.knob("A control", 10)
    check("CONTROL dial on the blinking effect sends CC#16 = 74", out[-1] == b"\xB0\x10\x4A")
    s.button("A effect 1", True)
    check("second press turns it off: CC#16 = 0", out[-1] == b"\xB0\x10\x00" and not s.fx["A"][0])
    h.fade(-1.0)
    check("host output fade to black (CC#23 = 0)", s.fade == 0)
    s.receive(b"\x90\x29\x64")
    check("Note Mode off: note 29H is ignored", s.sel["A"] == 2)
    s.note_mode = True
    s.receive(b"\x90\x29\x64")
    check("Note Mode on: note 29H = A input 4", s.sel["A"] == 4)
    base, dt = time.time(), 60 / (150 * 24)  # 2 beats of MIDI clock at 150 BPM, timestamps faked
    s.clock_ticks = [base + k * dt for k in range(47)]
    real_time = time.time
    time.time = lambda: base + 47 * dt
    try:
        s.receive(b"\xF8")
    finally:
        time.time = real_time
    check("MIDI clock sets BPM 150 (got %s)" % s.bpm, abs(s.bpm - 150) < 1)
    s.button("Menu", True); s.button("Menu", False)
    s.button("Up", True); s.button("Up", False)
    check("menu: MIDI TX CH up = channel 2", s.tx_ch == 1)
    s.button("Enter", True); s.button("Enter", False)
    s.tx_ch = 0
    s.midi_thru = True
    n = len(out)
    s.fader("Video fader", 0.0)
    check("MIDI Thru on: the V-4's own messages are not sent", len(out) == n)
    h.close()
    s.close()
    print("v4 selftest", "passed" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    sys.exit(0 if selftest() else 1)
