# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Simulated LUMI Keys ("virtual weapon no. 3")
============================================

A pretend ROLI LUMI Keys: 24 pressure-sensitive keys with one RGB LED under each,
and three buttons (Mode, Octave -, Octave +). It is a BLOCKS device like the
Lightpad, so it reuses the BLOCKS codec, HeapSync and the LocalHost lifecycle
from lightpad_sim.py. Only the parts that differ live here.

What the keys send (as ROLI's LUMI example program and default handler do):
  MPE, one channel per sounding key: note on with the strike velocity, channel
  pressure while held, pitch bend for glide (moving the finger along the key),
  note off on release. Key 0 = note 48 + 12 * octave (config item 4, -4..6).
Buttons: 0 = Mode, 1 = Octave -, 2 = Octave +. Without a host program the
block changes the octave itself; with Himmel's renderer loaded the buttons
come to the host as program events and Himmel changes the octave (config 4).

LEDs: Himmel's key renderer (lightpad_littlefoot.build_key_program) keeps the
block's own musical key handling and replaces only the lighting:
setUseDefaultKeyHandler(true, false), then fillPixel(colour, key, 0) for 24 keys
from a 48-byte RGB565 heap. The simulator runs that bytecode in MiniVM.

Engine link: every message the block sends also goes to UDP 127.0.0.1:8769.

Self-test:  python tools/lumi_sim.py
"""

import colorsys
import sys
import time

import lightpad_littlefoot as lf
import lightpad_sim as L

KEYS = 24
BASE_NOTE = 48
CFG_OCTAVE = 4
BUTTONS = {"Mode": 0, "Octave -": 1, "Octave +": 2}
ENGINE_ADDR = ("127.0.0.1", 8769)   # MK2 8766, Lightpad 8767, (Entrancer engine 8768), LUMI 8769


class SimLumi(L.SimLightpad):
    serial = "LKBHIMMELSIM0001"      # "LKB" = LUMI Keys Block
    engine_out = ENGINE_ADDR

    def __init__(self):
        super().__init__()
        self.notes = ["Simulated LUMI Keys - no hardware. Play the keys with the mouse."]
        self.config[CFG_OCTAVE] = [0, -4, 6]
        self.keys = {}                  # key index -> dict(vel, z, x, ch, note)
        self.buttons = set()

    # ---- input from the browser ----
    def key(self, n, phase, vel=0.6, z=0.0, x=0.0):
        """phase start/move/end; vel, z 0..1; x = glide -1..1 (finger moved along the key)."""
        n = int(n)
        if not 0 <= n < KEYS:
            return
        with self.lock:
            k = self.keys.get(n)
            octave = self.config[CFG_OCTAVE][0]
        if k is None and phase != "start":
            return   # a late move/end after the key was released (browser requests can cross)
        if phase == "start":
            if k is not None:
                self._key_out(k, "end")
            k = {"n": n, "ch": 1 + (n % 15), "note": min(127, BASE_NOTE + n + 12 * octave),
                 "vel": max(1, min(127, round(vel * 127))), "z": z, "x": x}
            with self.lock:
                self.keys[n] = k
            self._key_out(k, "start")
            return
        k["z"], k["x"] = z, x
        if phase == "end":
            with self.lock:
                self.keys.pop(n, None)
            self._key_out(k, "end")
        else:
            self._key_out(k, "move")

    def _key_out(self, k, phase):
        ch = k["ch"]
        if phase == "start":
            self.send(bytes([0xE0 | ch, 0, 64]))
            self.send(bytes([0x90 | ch, k["note"], k["vel"]]))
        elif phase == "end":
            self.send(bytes([0x80 | ch, k["note"], 64]))
        else:
            bend = max(0, min(16383, int(8192 + k["x"] * 4096)))
            self.send(bytes([0xE0 | ch, bend & 0x7F, bend >> 7]))
            self.send(bytes([0xD0 | ch, max(0, min(127, round(k["z"] * 127)))]))

    def press_button(self, idx, down):
        with self.lock:
            if (idx in self.buttons) == down:
                return
            (self.buttons.add if down else self.buttons.discard)(idx)
            self.button = 0 in self.buttons
            api = self.api
            prog = None if self.factory else lf.parse_program(self.data)
            block = bytes(self.data[:prog[0] + prog[1]]) if prog else None
        if block is not None and api:
            try:  # Himmel's renderer forwards buttons as program events
                vm = lf.MiniVM(block, width=KEYS, height=1)
                vm.call("handleButtonDown/vi" if down else "handleButtonUp/vi", idx)
            except ValueError:
                return
            for a, b, c in vm.to_host:
                self.send(self._packet([(0x28, 7), (a & 0xFFFFFFFF, 32), (b & 0xFFFFFFFF, 32), (c & 0xFFFFFFFF, 32)]))
            return
        if api:
            self.send(self._packet([(L.BUTTON_DOWN if down else L.BUTTON_UP, 7), (0, 5), (idx, 12)]))
        if down and idx in (1, 2):  # the factory program changes the octave itself
            with self.lock:
                c = self.config[CFG_OCTAVE]
                c[0] = max(c[1], min(c[2], c[0] + (1 if idx == 2 else -1)))

    def press(self, down):          # the SimLightpad name: Mode
        self.press_button(0, down)

    def release_all(self):
        for n in list(self.keys):
            self.key(n, "end")
        for b in list(self.buttons):
            self.press_button(b, False)

    def touch(self, *a):
        pass                        # no touch surface

    # ---- LEDs ----
    def leds(self):
        with self.lock:
            if self.factory:
                return self._factory_picture()
            p = lf.parse_program(self.data)
            if p is None:
                return ["000000"] * KEYS
            key = bytes(self.data[:p[0] + p[1]])
        if key != self._vm_key:
            try:
                vm = lf.MiniVM(key, width=KEYS, height=1)
                vm.call("initialise/v")
                vm.call("repaint/v")
                self._vm_leds = ["%02x%02x%02x" % c for c in vm.pixels]
            except ValueError as e:
                self._vm_leds = ["400000"] * KEYS
                self.notes = self.notes[:1] + ["LittleFoot error: %s" % e]
            self._vm_key = key
        return self._vm_leds

    def _factory_picture(self):
        """Roughly what the block shows on its own: C keys bright, a pressed key white."""
        out = []
        for n in range(KEYS):
            if n in self.keys:
                out.append("ffffff")
            elif n % 12 == 0:
                out.append("e557da")
            elif n % 12 in (1, 3, 6, 8, 10):
                out.append("1a3a6a")
            else:
                out.append("2f72b0")
        return out


class LumiHost(L.LocalHost):
    """The live view as the BLOCKS host for a LUMI (Himmel's driver sketch)."""

    MODES = ("keys", "rainbow", "test", "off")
    DEFAULT_MODE = "keys"
    WANT = {}                                  # no config changes until tested on the owner's LUMI
    CONFIG_READ = (L.CFG_PRESSURE_SENSITIVITY, L.CFG_VELOCITY_SENSITIVITY, CFG_OCTAVE)

    def build_program(self):
        return lf.build_key_program(KEYS)

    def __init__(self, sim):
        self.keys = {}                         # key index -> [velocity 0..1, pressure 0..1, glow]
        self.chan_key = {}                     # MIDI channel -> key index (MPE)
        self.glow = [[0.0, 0.0, 0.0] for _ in range(KEYS)]
        self.events = []                       # recent key events for the monitor
        super().__init__(sim)
        self.glow = [[0.0, 0.0, 0.0] for _ in range(KEYS)]

    def octave(self):
        return self.config.get(CFG_OCTAVE, [0])[0]

    def on_block(self, msg):
        if msg and msg[0] != 0xF0:             # MIDI from the block's own key handler
            self.on_midi(msg)
            return
        for e in L.decode_device(msg):
            if e["type"] == "button":
                if e["down"] and e["id"] in (1, 2):
                    v = max(-4, min(6, self.octave() + (1 if e["id"] == 2 else -1)))
                    self.set_config(CFG_OCTAVE, v)         # Himmel turns the octave (restored on let go)
                elif e["id"] == 0 and not e["down"]:
                    self.palette = (self.palette + 1) % 6
                continue
            with self.lock:
                if e["type"] == "serial":
                    self.serial = e["serial"]
                elif e["type"] == "topology" and e["devices"]:
                    self.battery = e["devices"][0]["battery"]
                elif e["type"] == "ack":
                    self.acks += 1
                    self.sync.on_ack(e["packet"])
                elif e["type"] == "config" and "item" in e:
                    self.config[e["item"]] = [e["value"], e["min"], e["max"]]
                    v = e["value"]
                    if v == self.want.get(e["item"]) and e["item"] in L.CFG_DEFAULTS:
                        v = L.CFG_DEFAULTS[e["item"]]
                    self.original.setdefault(e["item"], v)

    def on_midi(self, msg):
        st, ch = msg[0] & 0xF0, msg[0] & 0x0F
        with self.lock:
            if st == 0x90 and len(msg) >= 3 and msg[2] > 0:
                n = msg[1] - BASE_NOTE - 12 * self.octave()
                if 0 <= n < KEYS:
                    self.keys[n] = [msg[2] / 127, 0.0]
                    self.chan_key[ch] = n
                    self.z_now = 0
            elif st in (0x80, 0x90) and len(msg) >= 3:
                n = msg[1] - BASE_NOTE - 12 * self.octave()
                self.keys.pop(n, None)
                self.chan_key.pop(ch, None)
                self.z_now = 0
            elif st == 0xD0 and len(msg) >= 2:
                n = self.chan_key.get(ch)
                if n in self.keys:
                    self.keys[n][1] = msg[1] / 127
                z = msg[1] * 2
                self.z_now, self.z_max = z, max(self.z_max, z)
                self.z_hist[min(7, z // 32)] += 1

    def picture(self, now, dt=0.04):
        with self.lock:
            keys = {n: list(v) for n, v in self.keys.items()}
            pal = self.palette
        if self.mode == "off":
            return [(0, 0, 0)] * KEYS
        if self.mode == "test":     # key order check: first red, last of octave 1 green, first of octave 2 blue, last white
            out = [(0, 0, 0)] * KEYS
            out[0], out[11], out[12], out[23] = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)
            return out
        if self.mode == "rainbow":
            out = []
            for n in range(KEYS):
                r, g, b = colorsys.hsv_to_rgb((n / KEYS - now * 0.2) % 1.0, 1.0, 0.8)
                out.append((int(r * 255), int(g * 255), int(b * 255)))
            return out
        # keys: Himmel's palette per pitch class, played keys flash white and glow by pressure
        base_h = pal / 6.0
        out = []
        for n in range(KEYS):
            cell = self.glow[n]
            for k in range(3):
                cell[k] *= 0.85 ** (dt / 0.04)
            if n in keys:
                vel, z = keys[n]
                hot = 0.45 + 0.55 * max(vel * 0.6, z)
                for k in range(3):
                    cell[k] = max(cell[k], hot)
            semi = n % 12
            v = 0.55 if semi == 0 else (0.12 if semi in (1, 3, 6, 8, 10) else 0.25)
            r, g, b = colorsys.hsv_to_rgb((base_h + semi / 24.0) % 1.0, 0.8, v)
            out.append((int(255 * max(r, cell[0])), int(255 * max(g, cell[1])), int(255 * max(b, cell[2]))))
        return out


class LumiLive(L.LightpadLive):
    """What tools/mk2_live.py talks to for ?weapon=lumi."""

    weapon = "lumi"

    def make_sim(self):
        return SimLumi()

    def make_hw(self):
        import lightpad_hw
        return lightpad_hw.HwLightpad(port_name="LUMI", device_name="LUMI Keys")

    def make_host(self, sim):
        return LumiHost(sim)

    def snapshot(self):
        d = super().snapshot()
        h = self.host
        with h.lock:
            d["keys"] = {str(n): {"vel": round(v[0], 2), "z": round(v[1], 2)} for n, v in h.keys.items()}
        d["octave"] = h.octave()
        held = set()
        sb = getattr(self.sim, "buttons", None)
        if sb:
            held = {name for name, i in BUTTONS.items() if i in sb}
        d["held"] = sorted(held)
        d["mode_name"] = "API mode (BLOCKS SysEx + MPE keys)" if d["api"] else "MIDI mode (MPE)"
        return d

    def command(self, d):
        i = d.get("sim_input")
        if i and "key" in i and hasattr(self.sim, "key"):
            k = i["key"]
            self.sim.key(k.get("n", 0), k.get("phase", "move"), k.get("vel", 0.6), k.get("z", 0.0), k.get("x", 0.0))
            return
        if i and i.get("name") in BUTTONS and "down" in i and hasattr(self.sim, "press_button"):
            self.sim.press_button(BUTTONS[i["name"]], bool(i["down"]))
            return
        super().command(d)


def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond

    s = SimLumi()
    h = LumiHost(s)
    h.mode = "test"
    t_end = time.time() + 3
    while time.time() < t_end and h.sync.frames < 3:
        time.sleep(0.05)
    leds = s.leds()
    check("host uploads the key renderer; the block lights key 1 red, 12 green, 13 blue, 24 white",
          (leds[0], leds[11], leds[12], leds[23]) == ("f80000", "00fc00", "0000f8", "f8fcf8"))
    s.key(0, "start", vel=0.8)
    s.key(0, "move", z=0.5)
    time.sleep(0.05)
    check("a key strike reaches the host as MPE (key 1, velocity, pressure)",
          0 in h.keys and abs(h.keys[0][0] - 0.8) < 0.02 and abs(h.keys[0][1] - 0.5) < 0.02)
    s.key(0, "end")
    s.press_button(2, True)
    s.press_button(2, False)
    t_end = time.time() + 1
    while time.time() < t_end and h.octave() != 1:
        time.sleep(0.02)
    check("Octave + through our renderer: the host sets octave 1", h.octave() == 1)
    out = []
    s.listeners.append(lambda m: out.append(m))
    s.key(0, "start", vel=0.5)
    check("after Octave +, key 1 plays note 60", any(m[0] & 0xF0 == 0x90 and m[1] == 60 for m in out))
    s.key(0, "end")
    h.let_go()
    check("let go puts the octave back (0)", s.config[CFG_OCTAVE][0] == 0)
    s.close()
    print("lumi selftest", "passed" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    sys.exit(0 if selftest() else 1)
