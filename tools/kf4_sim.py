#!/usr/bin/env python3
"""Simulated KF4 4-input video mixer (Himmel weapon no. 6).

The real unit (a 2005 SD video mixer) has no MIDI, USB, RS-232 or other data
port, so there are no wire bytes to reproduce. This simulator is therefore the
mixer itself: its front-panel state machine (input keys with the blink state,
HOLD, monitor select, crossfader with 4 curves, BK / WH / C-K / L-K modes,
FINE, NTSC/PAL read at power-on, power switch), its LEDs, and the mix and key
parameters that the live page renders as pictures. All of it follows the
owner's manual and EasyStart guide; guesses are marked GUESS and listed in
weapons/kf4.md so they can be checked on the owner's unit.

Every control change is also sent as a Himmel event (a JSON datagram) to
UDP 127.0.0.1:8771, with a full state heartbeat every 250 ms, so a future
engine bridge can let the simulated KF4 play Himmel as a 4-input mixer.
Nothing listens there yet; sending is harmless.

Run:   python tools/kf4_sim.py --selftest
Live:  python tools/mk2_live.py --port 8777 --weapon kf4     (then open the page)
"""
import json
import os
import socket
import sys
import threading
import time

ENGINE_PORT = 8771
CURVES = ["1", "2", "3", "4"]          # Normal, No mix, Center mix, Switcher
MODES = ["BK", "WH", "C-K", "L-K"]
FADER_MAX = 1023
FINE_MAX = 1023
BLINK_HZ = 2.0                         # GUESS: blink rate of an input key


def clamp01(v):
    return 0.0 if v < 0 else 1.0 if v > 1 else v


def curve_gains(curve, x):
    """Channel A and B gains for crossfader position x (0 = full A, 1 = full B).

    From the owner's manual (front panel, FADER CURVE):
      1 Normal     - linear, 50:50 at the centre.
      2 No mix     - towards the centre both channels dim (black at the centre),
                     towards an end only that channel comes up.
      3 Center mix - the channels blend only over the middle part of the travel.
                     GUESS: the middle half (0.25..0.75). The EasyStart guide
                     words it differently; check on the unit.
      4 Switcher   - a cut, no dimming. GUESS: the cut point is the centre.
    """
    x = clamp01(x)
    if curve == "1":
        return 1 - x, x
    if curve == "2":
        return (clamp01(1 - 2 * x), 0.0) if x < 0.5 else (0.0, clamp01(2 * x - 1))
    if curve == "3":
        t = clamp01((x - 0.25) / 0.5)
        return 1 - t, t
    return (1.0, 0.0) if x < 0.5 else (0.0, 1.0)


class Channel:
    def __init__(self, sel):
        self.sel = sel        # input 1..4
        self.blank = False    # the key blinks: input ignored (background / black)
        self.hold = False     # frozen picture

    def as_dict(self):
        return {"sel": self.sel, "blank": self.blank, "hold": self.hold}


class KF4:
    """The mixer's panel logic. Thread-safe; listeners get Himmel events."""

    def __init__(self):
        self.lock = threading.RLock()
        self.listeners = []
        # physical controls keep their position through a power cycle
        self.power_switch = 1          # 0 STBY, 1 ON
        self.std_switch = 1            # 0 NTSC, 1 PAL (Norway)
        self.fader = 0                 # 0 = A end
        self.curve = "1"
        self.mode = "BK"
        self.fine = 512
        # what is plugged in (the page can pull cables)
        self.plugged = {"in1s": False, "in1": True, "in2": True, "in3": True, "in4": True}
        self.source_std = "PAL"        # standard of the simulated sources
        self.held = set()              # buttons the mouse holds down (for the drawing)
        self.log = []                  # (t, text) event monitor, newest last
        self.power_on(initial=True)

    # ---------- power ----------
    def power_on(self, initial=False):
        with self.lock:
            self.on = self.power_switch == 1
            self.std = "NTSC" if self.std_switch == 0 else "PAL"   # latched at power-on
            # GUESS: power-on state (the manual does not say). Check on the unit.
            self.a = Channel(1)
            self.b = Channel(2)
            self.monitor = "OUT"
            if not initial:
                self._emit({"t": "power", "on": self.on, "std": self.std})

    # ---------- input from the panel ----------
    def button(self, name, down):
        with self.lock:
            if down:
                self.held.add(name)
            else:
                self.held.discard(name)
            self._emit({"t": "button", "id": name, "down": bool(down)})
            if not down or not self.on:
                return
            if name.startswith("Monitor "):
                self.monitor = name.split(" ", 1)[1]
            elif name in ("Hold A", "Hold B"):
                ch = self.a if name == "Hold A" else self.b
                ch.hold = not ch.hold
            elif name[:2] in ("A ", "B ") and name[2:].isdigit():
                ch = self.a if name[0] == "A" else self.b
                n = int(name[2:])
                if n == ch.sel:
                    ch.blank = not ch.blank   # second press: blink (GUESS: third press = solid again)
                else:
                    ch.sel, ch.blank = n, False

    def set_fader(self, v):
        with self.lock:
            v = max(0, min(FADER_MAX, int(round(v))))
            if v != self.fader:
                self.fader = v
                self._emit({"t": "fader", "id": "Crossfader", "v": v})

    def set_switch(self, name, pos):
        with self.lock:
            pos = int(pos)
            if name == "Fader curve":
                self.curve = CURVES[max(0, min(3, pos))]
            elif name == "Mode":
                self.mode = MODES[max(0, min(3, pos))]
            elif name == "NTSC/PAL":
                self.std_switch = max(0, min(1, pos))      # takes effect at the next power-on
            elif name == "Power":
                new = max(0, min(1, pos))
                if new != self.power_switch:
                    self.power_switch = new
                    self._emit({"t": "switch", "id": name, "pos": new})
                    self.power_on()
                return
            else:
                return
            self._emit({"t": "switch", "id": name, "pos": pos})

    def set_knob(self, name, v):
        with self.lock:
            if name == "Fine":
                v = max(0, min(FINE_MAX, int(round(v))))
                if v != self.fine:
                    self.fine = v
                    self._emit({"t": "knob", "id": "Fine", "v": v})

    def plug(self, jack, connected):
        with self.lock:
            if jack in self.plugged:
                self.plugged[jack] = bool(connected)
                self._emit({"t": "cable", "id": jack, "plugged": bool(connected)})

    # ---------- what the mixer does ----------
    def signal(self, n):
        """Does input n carry a picture? IN 1: S-Video wins over composite."""
        if n == 1:
            return self.plugged["in1s"] or self.plugged["in1"]
        return self.plugged.get("in%d" % n, False)

    def channel_source(self, ch):
        """What one channel shows before mixing: ('input', n), ('solid', colour) or ('none',)."""
        keying = self.mode in ("C-K", "L-K")
        if ch.blank:
            if not keying:
                return ("solid", "black" if self.mode == "BK" else "white")
            # key modes: a blinking A gives a black background; a blinking B
            # drops channel B (only A is output), unless HOLD B is pressed in
            # C-K mode, which gives a black channel B.
            if ch is self.a:
                return ("solid", "black")
            if self.mode == "C-K" and ch.hold:
                return ("solid", "black")
            return ("none",)
        if not self.signal(ch.sel):
            return ("solid", "blue")        # no signal: blue background
        return ("input", ch.sel)

    def mix(self):
        """Everything the page needs to draw program and monitor out."""
        with self.lock:
            x = self.fader / FADER_MAX
            ga, gb = curve_gains(self.curve, x)
            keying = self.mode in ("C-K", "L-K")
            if keying:
                # the fader only fades the channel B overlay; A stays as background
                ga = 1.0
            key = None
            if self.mode == "C-K":
                # FINE picks the colour that is keyed out. GUESS: hue 180..300 deg
                # (cyan..blue..magenta), blue (240) at the centre.
                key = {"kind": "chroma", "hue": 180 + 120 * self.fine / FINE_MAX, "width": 40}
            elif self.mode == "L-K":
                # FINE moves the keyed-out brightness from white through grey to black.
                key = {"kind": "luma", "level": 1 - self.fine / FINE_MAX, "width": 0.18}
            a = self.channel_source(self.a)
            b = self.channel_source(self.b)
            if b == ("none",):
                gb = 0.0
            return {
                "on": self.on,
                "garbled": self.on and self.std != self.source_std,
                "std": self.std,
                "a": {"src": list(a), "gain": round(ga, 4), "hold": self.a.hold},
                "b": {"src": list(b), "gain": round(gb, 4), "hold": self.b.hold},
                "key": key,
                "monitor": self.monitor,
                "signal": {str(n): self.signal(n) for n in range(1, 5)},
                "svideo_in1": self.plugged["in1s"],
            }

    def leds(self):
        """Control name -> 'off' / 'on' / 'blink'."""
        with self.lock:
            out = {}
            for side, ch in (("A", self.a), ("B", self.b)):
                for n in range(1, 5):
                    st = "off"
                    if self.on and n == ch.sel:
                        st = "blink" if ch.blank else "on"
                    out["%s %d" % (side, n)] = st
                out["Hold " + side] = "on" if self.on and ch.hold else "off"
            for m in ("1", "2", "3", "4", "OUT"):
                out["Monitor " + m] = "on" if self.on and self.monitor == m else "off"
            out["power_led"] = "on" if self.on else "off"
            return out

    def state(self):
        with self.lock:
            return {
                "power": self.power_switch, "std_switch": self.std_switch, "std": self.std,
                "fader": self.fader, "curve": self.curve, "mode": self.mode, "fine": self.fine,
                "a": self.a.as_dict(), "b": self.b.as_dict(), "monitor": self.monitor,
                "plugged": dict(self.plugged), "source_std": self.source_std,
            }

    # ---------- events ----------
    def _emit(self, ev):
        ev = dict(ev, w="kf4")
        self.log.append((time.time(), json.dumps(ev, separators=(",", ":"))))
        del self.log[:-60]
        for fn in list(self.listeners):
            try:
                fn(ev)
            except Exception:
                pass


class EngineLink:
    """Sends the KF4's Himmel events to UDP 127.0.0.1:8771 (+ a state heartbeat)."""

    def __init__(self, kf4, port=ENGINE_PORT):
        self.kf4 = kf4
        self.addr = ("127.0.0.1", port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.sent = 0
        self.running = True
        kf4.listeners.append(self.send)
        threading.Thread(target=self._beat, daemon=True).start()

    def send(self, ev):
        try:
            self.sock.sendto(json.dumps(ev, separators=(",", ":")).encode(), self.addr)
            self.sent += 1
        except OSError:
            pass  # nobody listening (WSAECONNRESET etc.) is fine
        self._drain()

    def _drain(self):
        # Windows queues a WSAECONNRESET per datagram nobody received; read them off.
        for _ in range(64):
            try:
                self.sock.recvfrom(2048)
            except BlockingIOError:
                return
            except OSError:
                continue

    def _beat(self):
        while self.running:
            self.send({"t": "state", "w": "kf4", "state": self.kf4.state()})
            time.sleep(0.25)

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


class KF4Live:
    """What tools/mk2_live.py talks to for ?weapon=kf4."""

    weapon = "kf4"

    def __init__(self, profile, port=ENGINE_PORT):
        self.profile = profile
        self.kf4 = KF4()
        self.link = EngineLink(self.kf4, port)

    def snapshot(self):
        k = self.kf4
        with k.lock:
            return {
                "weapon": "kf4",
                "simulated": True,
                "state": k.state(),
                "mix": k.mix(),
                "leds": k.leds(),
                "held": sorted(k.held),
                "log": [t for _, t in k.log[-24:]],
                "engine_port": self.link.addr[1],
                "sent": self.link.sent,
                "t": time.time(),
            }

    def command(self, d):
        k = self.kf4
        i = d.get("sim_input") or {}
        if "release_all" in i:
            for n in list(k.held):
                k.button(n, False)
        elif "down" in i:
            k.button(i.get("name", ""), bool(i["down"]))
        elif "fader" in i:
            k.set_fader(i["fader"])
        elif "switch" in i:
            k.set_switch(i["switch"], i.get("pos", 0))
        elif "knob" in i:
            k.set_knob(i["knob"], i.get("value", 0))
        elif "plug" in i:
            k.plug(i["plug"], i.get("on", True))
        if "source_std" in d and d["source_std"] in ("NTSC", "PAL"):
            with k.lock:
                k.source_std = d["source_std"]

    def close(self):
        self.link.close()


def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond

    def near(a, b):
        return abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6

    check("curve 1 is linear (50:50 at the centre)", near(curve_gains("1", 0.5), (0.5, 0.5)))
    check("curve 2 is black at the centre", near(curve_gains("2", 0.5), (0, 0)))
    check("curve 2 at 3/4 is half B, no A", near(curve_gains("2", 0.75), (0, 0.5)))
    check("curve 3 does not blend before 1/4", near(curve_gains("3", 0.2), (1, 0)))
    check("curve 4 cuts at the centre", near(curve_gains("4", 0.49), (1, 0)) and near(curve_gains("4", 0.51), (0, 1)))

    k = KF4()
    evs = []
    k.listeners.append(evs.append)
    k.button("A 3", True); k.button("A 3", False)
    check("A 3 selects input 3 for A, its LED lights", k.a.sel == 3 and k.leds()["A 3"] == "on" and k.leds()["A 1"] == "off")
    k.button("A 3", True); k.button("A 3", False)
    check("a second press blinks", k.leds()["A 3"] == "blink")
    check("BK + blinking A = black background", k.mix()["a"]["src"] == ["solid", "black"])
    k.set_switch("Mode", 1)
    check("WH + blinking A = white background", k.mix()["a"]["src"] == ["solid", "white"])
    k.plug("in2", False)
    check("B on an empty input shows blue", k.mix()["b"]["src"] == ["solid", "blue"])
    k.plug("in2", True)
    k.button("Hold B", True); k.button("Hold B", False)
    check("HOLD B lights and freezes B", k.leds()["Hold B"] == "on" and k.mix()["b"]["hold"])
    k.set_switch("Mode", 3)
    k.set_fader(FADER_MAX)
    m = k.mix()
    check("L-K: A stays full under B, a luma key is set", m["a"]["gain"] == 1 and m["key"]["kind"] == "luma")
    k.button("B 2", True); k.button("B 2", False)   # B now blinks
    check("L-K + blinking B: only A is output", k.mix()["b"]["gain"] == 0)
    k.set_switch("Mode", 2)
    check("C-K + blinking B + HOLD B: channel B is black", k.mix()["b"]["src"] == ["solid", "black"])
    k.button("Monitor 4", True); k.button("Monitor 4", False)
    check("monitor select follows the key", k.monitor == "4" and k.leds()["Monitor 4"] == "on")
    k.set_switch("NTSC/PAL", 0)
    check("NTSC/PAL is only read at power-on", k.std == "PAL" and not k.mix()["garbled"])
    k.set_switch("Power", 0)
    check("standby: every LED off", not any(v != "off" for v in k.leds().values()))
    k.set_switch("Power", 1)
    check("power-on latches NTSC; PAL sources are garbled", k.std == "NTSC" and k.mix()["garbled"])
    check("the fader keeps its position through a power cycle", k.fader == FADER_MAX)
    check("every change became a Himmel event", any(e["t"] == "fader" for e in evs) and any(e["t"] == "button" for e in evs))

    # the engine link: a listener on a free port gets the events
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    rx.settimeout(1.0)
    live = KF4Live({}, port=rx.getsockname()[1])
    live.command({"sim_input": {"name": "B 4", "down": True}})
    got = []
    t_end = time.time() + 1
    while time.time() < t_end:
        try:
            got.append(json.loads(rx.recvfrom(4096)[0]))
        except socket.timeout:
            break
        if any(g.get("t") == "button" for g in got):
            break
    check("the engine link delivers button events over UDP", any(g.get("t") == "button" and g.get("id") == "B 4" for g in got))
    live.close()
    rx.close()
    print("kf4 selftest", "passed" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(0 if selftest() else 1)
    print(__doc__)
