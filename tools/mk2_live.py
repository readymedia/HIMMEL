#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Maschine MK2 live view and map editor (browser UI)
==================================================

Shows the MK2 front panel in a browser and lights up every control you touch,
using maschine_mk2_map.json. Click a control on screen and then press / turn /
hit the real one to (re)learn it. Every control you touch is ticked off as
"seen"; when all are seen the map is marked verified.

The panel is drawn from the weapon profile weapons/mk2.weapon.json (layout,
labels, colours); the byte/bit map stays in maschine_mk2_map.json.

No MK2? Press "Simulate" on the page (or start with --sim): a virtual MK2
(tools/mk2_sim.py) takes the place of the hardware and you play the drawn panel
with the mouse. Simulated input never marks controls as seen and never changes
the map or the measurements.

Other weapons: every weapons/<id>.weapon.json is listed in the picker at the top
of the page (or open http://127.0.0.1:8765/?weapon=<id>). A weapon other than the
MK2 is drawn from its profile and played by its own simulator, e.g. the
Lightpad (tools/lightpad_sim.py).

Run:   python tools/mk2_live.py [--sim] [--port N] [--weapon ID]
       then open http://127.0.0.1:8765
       --weapon ID (not mk2) opens that weapon by default and leaves the MK2
       hardware alone (no device thread, no screens), handy when another
       MK2 Live already holds the MK2.
Needs: pip install hidapi  (same as maschine_mk2_test.py). Standard library otherwise.
The device is reopened automatically if the USB cable is pulled.
"""

import json
import os
import shutil
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import maschine_mk2_test as mk  # noqa: E402
import mk2_screens as scr  # noqa: E402
import mk2_fun as fun  # noqa: E402
import mk2_measure  # noqa: E402
import mk2_sim  # noqa: E402
import base64  # noqa: E402

PORT = 8765
HTML = os.path.join(HERE, "mk2_live.html")
WEAPON = os.path.join(os.path.dirname(HERE), "weapons", "mk2.weapon.json")
BUTTONS = list(mk.BUTTON_STEPS) + ["Master <", "Master >", "Enter"]
KNOBS = list(mk.KNOB_STEPS)
PADS = list(mk.PAD_STEPS)
ALL = BUTTONS + KNOBS + PADS
BUTTON_AREA = 8          # bytes 0..7 of report 0x01 hold buttons + encoder nibble
KNOB_LEARN_CHANGES = 4


class Live:
    def __init__(self):
        self.lock = threading.Lock()
        self.saved = self.load()
        self.t = None
        self.m = None
        self.notes = []
        self.prev = None
        self.held = set()
        self.seen = set(self.saved.get("seen", []))
        self.last = []
        self.unmapped = []
        self.learn = None
        self.message = ""
        self.save_due = 0.0
        self.backed_up = False
        self.err_check = (time.time(), 0)
        # screens: mode = status | test | knobs; format switches are saved in the map
        sc = self.saved.setdefault("screen", {})
        sc.setdefault("msb_left", True)
        sc.setdefault("invert", False)
        sc.setdefault("checked", False)
        self.screen_mode = "splash"           # boot splash, then status
        self.boot_until = time.time() + 14.0  # one full logo animation
        self.screen_bits = {"left": bytes(2048), "right": bytes(2048)}
        self.sender = scr.ScreenSender()
        self.knob_touched = None   # last knob turned, highlighted on the knob page
        self.press_q = set()       # button/pad names that went down, drained by the screen loop
        self.pads_down = set()
        self.games = {}            # fun modes, created on first use
        self.fps = 0.0
        self.meas = mk2_measure.Measure()   # Measure card (controller spec numbers)
        self.enc_last = None       # master encoder menu
        self.menu_idx = 0
        self.menu_until = 0.0
        self.sim = False           # True: use the simulated MK2 instead of the hardware
        self.swap = False          # set when sim is toggled: device loop drops the current device

    def simulated(self):
        return isinstance(self.t, mk2_sim.SimMk2)

    def engine_active(self):
        """The simulated MK2 is played by Himmel (mk2_controller --sim answers): the engine
        owns the screens and LEDs, the live view only shows them."""
        t = self.t
        return isinstance(t, mk2_sim.SimMk2) and t.engine_active()

    # ---------- map file ----------
    def load(self):
        try:
            with open(mk.MAP_PATH, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        for k in ("buttons", "knobs", "pads"):
            d.setdefault(k, {})
        d.setdefault("skipped", [])
        d["pads"].setdefault("_id_to_slot", {})
        return d

    def save(self):
        d = self.saved
        d["seen"] = sorted(self.seen)
        d["verified"] = all(n in self.seen for n in ALL)
        d["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
        if os.path.exists(mk.MAP_PATH) and not self.backed_up:
            shutil.copyfile(mk.MAP_PATH, mk.MAP_PATH.replace(".json", ".bak.json"))
            self.backed_up = True
        tmp = mk.MAP_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2)
        os.replace(tmp, mk.MAP_PATH)

    # ---------- device ----------
    def device_loop(self):
        while True:
            if self.swap:
                self.swap = False
                if self.m is not None:
                    self.drop_device(["Switched to the simulated MK2" if self.sim else "Switched to the hardware"])
            if self.m is None:
                t = mk2_sim.SimMk2(self.saved) if self.sim else mk.Transport()
                if not t.open():
                    self.notes = (t.notes or ["MK2 not found"]) + ["No MK2? Press Simulate to play the panel with the mouse."]
                    for _ in range(10):  # wait 1 s, but react to Simulate at once
                        if self.swap:
                            break
                        time.sleep(0.1)
                    continue
                m = mk.Mapper(t)
                m.result = {"buttons": self.saved["buttons"], "knobs": self.saved["knobs"],
                            "pads": self.saved["pads"], "skipped": self.saved["skipped"]}
                m.step = len(mk.STEPS) - 1  # "done": no step-learning inside Mapper
                if not self.sim:  # never measure the simulator
                    self.meas.attach(m, {pid: int(n.split()[1]) for n, pid in self.pad_ids().items()})
                    self.meas.event("connected")
                threading.Thread(target=m.reader, daemon=True).start()
                threading.Thread(target=m.led_writer, daemon=True).start()
                with self.lock:
                    self.t, self.m, self.prev = t, m, None
                    self.notes = t.notes
                    self.err_check = (time.time(), 0)
                    self.sender.reset()  # new connection: resend every strip
            try:
                with self.lock:
                    self.tick()
            except Exception as e:  # never let a bug in tick kill the device thread
                self.message = "tick error: %s" % e
            self.check_errors()
            if self.save_due and time.time() > self.save_due:
                self.save_due = 0.0
                try:
                    with self.lock:
                        self.save()
                except OSError as e:
                    self.message = "save failed: %s" % e
            time.sleep(0.004)

    # ---------- screens ----------
    def screen_loop(self):
        """Draws both screens at ~30 Hz and sends only changed strips. Never touches input.

        Fun modes (demo/snake/maze) draw one 512x64 image outside the lock, so a slow
        frame can never hold up the device thread.
        """
        c = scr.Canvas()
        wide = scr.Canvas(fun.WW, fun.HH)
        last = time.perf_counter()
        while True:
            t0 = time.perf_counter()
            dt, last = min(0.1, t0 - last), t0
            if self.engine_active():
                time.sleep(0.1)
                last = time.perf_counter()
                continue
            try:
                with self.lock:
                    if self.boot_until and time.time() > self.boot_until:
                        self.boot_until = 0.0
                        if self.screen_mode == "splash":
                            self.screen_mode = "status"
                    menu = self.menu_tick()
                    mode, t, sc = self.screen_mode, self.t, self.saved["screen"]
                    if mode in fun.MODES:
                        pads = [0] * 16
                        if self.m:
                            for name, pid in self.pad_ids().items():
                                pads[int(name.split()[1]) - 1] = self.m.pads[pid]
                        inp = fun.Input(self.held | self.pads_down, self.press_q, pads, self.knob_values())
                        self.press_q = set()
                    else:
                        self.press_q = set()
                        pages = {side: self.draw_screen(c, side) for side in ("left", "right")}
                if mode in fun.MODES:
                    game = self.games.get(mode) or self.games.setdefault(mode, fun.MODES[mode]())
                    game.update(dt, inp)
                    game.draw(wide)
                    left, right = wide.split()
                    pages = {"left": left, "right": right}
                if menu is not None:  # master encoder menu on top of any mode
                    wide.buf[:] = scr.join(pages["left"], pages["right"])
                    fun.menu_overlay(wide, menu)
                    left, right = wide.split()
                    pages = {"left": left, "right": right}
                self.fps = 0.9 * self.fps + 0.1 / max(dt, 1e-3)
                for side, buf in pages.items():
                    self.screen_bits[side] = buf
                    if t is not None:
                        self.sender.send(t, side, scr.pack_buf(buf, sc["msb_left"], sc["invert"]))
            except Exception as e:  # a screen problem must never stop the device thread
                self.message = "screen: %s" % e
                time.sleep(0.5)
            time.sleep(max(0.0, 1 / 30 - (time.perf_counter() - t0)))

    def set_mode(self, mode):
        """Switch screen mode (from the page or the encoder menu). Under lock."""
        m = self.m
        self.screen_mode = mode
        self.boot_until = 0.0
        self.press_q = set()
        if mode == "splash" and "splash" in self.games:
            self.games["splash"].t = 0.0  # restart the logo animation
        if m:  # modes that use pads as controls light those pads
            m.leds_off()
            if mode in fun.MODES:
                game = self.games.get(mode) or self.games.setdefault(mode, fun.MODES[mode]())
                leds = game.pad_leds()
                if leds:
                    m.pad_feedback = False
                    ids = self.pad_ids()
                    for n, rgb in leds.items():
                        if "Pad %d" % n in ids:
                            m.set80(ids["Pad %d" % n], *rgb)

    def menu_tick(self):
        """Shift + master encoder = mode menu on the device: Shift + turn opens it, turn to choose,
        push to start. Without Shift the encoder is free (in Himmel it is the browse/select wheel).
        Returns the highlighted menu index while the menu is open, else None. Under lock."""
        now = time.time()
        enc = self.knob_values().get("Master encoder (turn)")
        if enc is not None:
            if self.enc_last is not None and enc != self.enc_last:
                d = enc - self.enc_last
                d = d - 16 if d > 8 else d + 16 if d < -8 else d
                if now > self.menu_until and "Shift" not in self.held:
                    self.enc_last = enc  # menu closed and no Shift: not ours
                    return None
                if now > self.menu_until:  # opening: start on the current mode
                    self.menu_idx = fun.MENU.index(self.screen_mode) if self.screen_mode in fun.MENU else 0
                else:
                    self.menu_idx = (self.menu_idx + (1 if d > 0 else -1)) % len(fun.MENU)
                self.menu_until = now + 4.0
            self.enc_last = enc
        if now > self.menu_until:
            return None
        if "Master encoder (push)" in self.press_q:
            self.press_q.discard("Master encoder (push)")
            self.menu_until = 0.0
            self.set_mode(fun.MENU[self.menu_idx])
            return None
        return self.menu_idx

    def draw_screen(self, c, side):
        m = self.m
        if self.screen_mode == "test":
            scr.test_pattern(c, side)
        elif self.screen_mode == "knobs":
            scr.knob_page(c, side, self.knob_values(), "HIMMEL  KNOBS" if side == "left" else "PAGE 1/1",
                          self.knob_touched)
        else:
            if side == "left":
                r = m.reports if m else {}
                lines = [("MK2 SIMULATED" if self.simulated() else "MK2 CONNECTED") if m else "MK2 NOT CONNECTED",
                         "CONTROLS %d  PADS %d" % (r.get("0x01", 0), r.get("0x20", 0)),
                         "SEEN %d / %d" % (len(self.seen), len(ALL)),
                         "SCREEN WRITE %.2f MS" % self.sender.write_ms]
                scr.status_page(c, side, lines, "HIMMEL  MK2 LIVE")
            else:
                if self.learn:
                    lines = ["LEARN: " + self.learn["name"], "%s IT ON THE MK2" % {"button": "PRESS", "knob": "TURN", "pad": "HIT"}[self.learn["kind"]]]
                else:
                    lines = ["LAST PRESSED:"] + self.last[:4]
                scr.status_page(c, side, lines, "INPUT")
        return bytes(c.buf)

    def knob_values(self):
        st = self.m.state if self.m else None
        out = {}
        if st is None:
            return out
        for name, v in self.saved["knobs"].items():
            b = v["byte"]
            if b >= len(st):
                continue
            if v["nibble"] == "both":
                hi = st[b + 1] if b + 1 < len(st) and b >= BUTTON_AREA else 0
                out[name] = st[b] | hi << 8
            else:
                out[name] = st[b] & 0x0F if v["nibble"] == "low" else st[b] >> 4
        return out

    def check_errors(self):
        t0, n0 = self.err_check
        if time.time() - t0 < 1.0:
            return
        n = len(self.m.errors)
        self.err_check = (time.time(), n)
        if n - n0 >= 4:  # reader keeps failing: device gone, reopen
            m = self.m
            self.drop_device(["Lost the MK2 (%s) - reconnecting" % (m.errors[-1] if m.errors else "?")])

    def drop_device(self, notes):
        """Stop and close the current device (real or simulated). Called from the device thread."""
        m, t = self.m, self.t
        was_sim = self.simulated()
        m.running = False
        m.chase_stop.set()
        time.sleep(0.25)
        t.close()
        with self.lock:
            self.m = self.t = None
            self.held, self.pads_down, self.prev = set(), set(), None
            if not was_sim:
                self.meas.event("disconnected")
            self.notes = notes

    # ---------- per-tick logic (under lock) ----------
    def tick(self):
        m = self.m
        if not self.learn:
            self.pad_tick(m)
        st = m.state
        if st is None:
            return
        self.meas.knob_tick(self.knob_values())
        prev = self.prev or st
        self.prev = st
        rising = {}
        for i in range(min(len(st), len(prev), BUTTON_AREA)):
            r = st[i] & ~prev[i] & 0xFF
            if r:
                rising[i] = r
        changed = {i for i in range(min(len(st), len(prev))) if st[i] != prev[i]}

        if self.learn:
            self.learn_tick(st, rising, changed)
            return

        # held buttons, seen marks, unmapped bits
        held = set()
        for name, v in self.saved["buttons"].items():
            if v["byte"] < len(st) and st[v["byte"]] >> v["bit"] & 1:
                held.add(name)
        for name in held - self.held:
            self.mark_seen(name)
            self.last = ([name] + self.last)[:6]
            self.press_q.add(name)
        if held != self.held:
            self.mirror_leds(held)
            self.held = held
        masks = m.knob_masks()
        used = m.used_bits()
        for i, r in rising.items():
            for bit in range(8):
                if r >> bit & 1 and (i, bit) not in used and not (masks.get(i, 0) >> bit & 1):
                    tag = "byte %d bit %d" % (i, bit)
                    if tag not in self.unmapped:
                        self.unmapped = (self.unmapped + [tag])[-8:]
        for name, v in self.saved["knobs"].items():
            if v["byte"] in changed:
                self.mark_seen(name)
                if name.startswith("Knob "):
                    self.knob_touched = int(name.split()[1])

    def pad_tick(self, m):
        """Pad hit edges and seen marks. Runs even before the first controls report arrives."""
        for name, pid in self.pad_ids().items():
            down = m.pads[pid] > mk.PAD_HIT
            if down and name not in self.pads_down:
                self.pads_down.add(name)
                self.press_q.add(name)
                self.mark_seen(name)
            elif not down and m.pads[pid] < mk.PAD_HIT // 2:  # hysteresis
                self.pads_down.discard(name)

    def pad_ids(self):
        return {n: v["id"] for n, v in self.saved["pads"].items() if not n.startswith("_")}

    def mark_seen(self, name):
        if self.simulated():  # "seen" means checked on the real device
            return
        if name not in self.seen:
            self.seen.add(name)
            self.save_due = time.time() + 1.0

    def mirror_leds(self, held):
        m = self.m
        m.leds81 = [0] * 56
        m.leds82 = [0] * 31
        for name in held:
            m.light_button(name)
        m.dirty = True

    def learn_tick(self, st, rising, changed):
        L = self.learn
        kind, name = L["kind"], L["name"]
        if time.time() - L["started"] > 30:
            self.learn, self.message = None, "Learn timed out for %s" % name
            return
        if kind == "button":
            masks = self.m.knob_masks()
            for i, r in sorted(rising.items()):
                r &= ~masks.get(i, 0)
                for bit in range(8):
                    if r >> bit & 1:
                        btns = self.saved["buttons"]
                        for other in [n for n, v in btns.items() if (v["byte"], v["bit"]) == (i, bit) and n != name]:
                            del btns[other]
                            self.seen.discard(other)
                        btns[name] = {"byte": i, "bit": bit}
                        return self.learned(name, "byte %d bit %d" % (i, bit))
        elif kind == "knob":
            for i in changed:
                L["counts"][i] = L["counts"].get(i, 0) + 1
                if (st[i] ^ L["base"][i]) & 0xF0:
                    L["hi"].add(i)
            hits = [i for i, c in L["counts"].items() if c >= KNOB_LEARN_CHANGES]
            if hits:
                i = max(hits, key=lambda b: L["counts"][b])
                if i < BUTTON_AREA:
                    entry = {"byte": i, "nibble": "both" if i in L["hi"] else "low", "next_byte_moves": False}
                else:
                    i &= ~1  # knobs are 16-bit little-endian words starting at byte 8
                    entry = {"byte": i, "nibble": "both", "next_byte_moves": True}
                knobs = self.saved["knobs"]
                for other in [n for n, v in knobs.items() if v["byte"] == i and n != name]:
                    del knobs[other]
                    self.seen.discard(other)
                knobs[name] = entry
                return self.learned(name, "byte %d (%s)" % (i, entry["nibble"]))
        elif kind == "pad":
            p = self.m.pads
            pid = max(range(16), key=lambda k: p[k])
            if p[pid] > mk.PAD_HIT:
                pads = self.saved["pads"]
                slot = int(name.split()[1]) - 1
                for other in [n for n, v in self.pad_ids().items() if v == pid and n != name]:
                    del pads[other]
                    self.seen.discard(other)
                pads[name] = {"id": pid}
                ids = {str(v): int(n.split()[1]) - 1 for n, v in self.pad_ids().items()}
                pads["_id_to_slot"] = ids
                self.m.all_off()
                return self.learned(name, "pad id %d -> slot %d" % (pid, slot + 1))

    def learned(self, name, what):
        self.learn = None
        self.seen.add(name)
        self.message = "Learned %s = %s" % (name, what)
        self.save_due = time.time() + 0.2

    # ---------- state for the browser ----------
    def snapshot(self):
        with self.lock:
            m = self.m
            st = m.state if m else None
            knobs = {}
            if st is not None:
                for name, v in self.saved["knobs"].items():
                    b = v["byte"]
                    if b >= len(st):
                        continue
                    if v["nibble"] == "both":
                        hi = st[b + 1] if b + 1 < len(st) and b >= BUTTON_AREA else 0
                        knobs[name] = st[b] | hi << 8
                    else:
                        knobs[name] = st[b] & 0x0F if v["nibble"] == "low" else st[b] >> 4
            pads = [0] * 16
            if m:
                for name, pid in self.pad_ids().items():
                    pads[int(name.split()[1]) - 1] = m.pads[pid]
            return {
                "connected": m is not None,
                "sim": self.sim,
                "simulated": self.simulated(),
                "engine": self.engine_active(),
                "leds": self.led_state(),
                "notes": self.notes[-2:],
                "reports": dict(m.reports) if m else {},
                "errors": (m.errors[-2:] if m else []),
                "raw": st.hex(" ") if st is not None else "",
                "held": sorted(self.held),
                "knobs": knobs,
                "pads": pads,
                "mapped": sorted(set(self.saved["buttons"]) | set(self.saved["knobs"]) | set(self.pad_ids())),
                "seen": sorted(self.seen),
                "total": len(ALL),
                "verified": all(n in self.seen for n in ALL),
                "last": self.last,
                "unmapped": self.unmapped,
                "learn": {"kind": self.learn["kind"], "name": self.learn["name"]} if self.learn else None,
                "message": (m.message if m and m.chase_running() else self.message),
                "chase": bool(m and m.chase_running()),
                "measure": self.meas.snapshot(),
                "screen": dict(self.saved["screen"], mode=self.screen_mode,
                               write_ms=round(self.sender.write_ms, 2), writes=self.sender.writes,
                               fps=round(self.fps, 1),
                               left=base64.b64encode(bytes(self.shown_screen("left"))).decode(),
                               right=base64.b64encode(bytes(self.shown_screen("right"))).decode()),
            }

    def led_state(self):
        """What the LEDs show right now (what was last sent), by control name: [r, g, b] 0..127.
        Lets the drawn panel glow like the device, real or simulated. Under lock."""
        m = self.m
        if m is None:
            return {}
        if self.engine_active():
            rx = self.t.rx_leds
            leds80, leds81, leds82 = rx[0x80], rx[0x81], rx[0x82]
        else:
            leds80, leds81, leds82 = m.leds80, m.leds81, m.leds82
        out = {}
        for i, name in enumerate(mk.LED_82):
            v = leds82[i]
            if v:
                out[name.rstrip("*")] = [v, v, v]
        for g, name in enumerate(mk.GROUPS):
            rgb = leds81[g * 6:g * 6 + 3]
            if any(rgb):
                out[name] = list(rgb)
        for i, name in enumerate(mk.LED_81_TRANSPORT):
            v = leds81[48 + i]
            if v:
                out[name.rstrip("*")] = [v, v, v]
        for name, pid in self.pad_ids().items():
            rgb = leds80[pid * 3:pid * 3 + 3]
            if any(rgb):
                out[name] = list(rgb)
        return out

    def shown_screen(self, side):
        return self.t.rx_screens[side] if self.engine_active() else self.screen_bits[side]

    def sim_input(self, d):
        """Mouse/keyboard input for the simulated MK2. Not under lock (the sim has its own)."""
        t = self.t
        if not isinstance(t, mk2_sim.SimMk2):
            return
        name = d.get("name", "")
        if "down" in d:
            t.press(name, bool(d["down"]))
        elif "delta" in d:
            t.turn(name, d["delta"])
        elif "pressure" in d:
            t.pad(name, d["pressure"])
        elif d.get("release_all"):
            t.release_all()

    def command(self, d):
        if "sim_input" in d:
            return self.sim_input(d["sim_input"])
        with self.lock:
            m = self.m
            if "sim" in d:
                self.sim = bool(d["sim"])
                self.learn = None
                self.swap = True
                return
            if "learn" in d and self.simulated():
                self.message = "Learning needs the real MK2 (turn Simulate off)"
                return
            if "learn" in d:
                name = d["learn"]
                if not name:
                    self.learn, self.message = None, "Learn cancelled"
                    return
                kind = "button" if name in BUTTONS else "knob" if name in KNOBS else "pad" if name in PADS else None
                if kind is None:
                    return
                base = bytes(m.state) if m and m.state is not None else bytes(32)
                self.learn = {"kind": kind, "name": name, "started": time.time(),
                              "counts": {}, "base": base, "hi": set()}
                verb = {"button": "Press", "knob": "Turn", "pad": "Hit"}[kind]
                self.message = "%s %s on the MK2 now (Esc cancels)" % (verb, name)
            elif d.get("led") and m:
                act = d["led"]
                if act == "chase":
                    m.toggle_chase()
                else:
                    {"pads": m.test_pads, "groups": m.test_groups, "buttons": m.test_buttons,
                     "off": m.leds_off}.get(act, lambda: None)()
            elif "measure" in d:
                self.meas.command(d)
            elif d.get("screen_mode") in fun.MENU:
                self.set_mode(d["screen_mode"])
            elif "screen_set" in d:  # {"screen_set": {"msb_left": bool} | {"invert": bool} | {"checked": bool}}
                sc = self.saved["screen"]
                for k in ("msb_left", "invert", "checked"):
                    if k in d["screen_set"]:
                        sc[k] = bool(d["screen_set"][k])
                self.sender.reset()
                self.save_due = time.time() + 0.2
                self.message = "Screen format: %s, %s%s" % (
                    "MSB left" if sc["msb_left"] else "LSB left", "inverted" if sc["invert"] else "normal",
                    ", checked" if sc["checked"] else "")
            elif d.get("reset_seen"):
                self.seen.clear()
                self.unmapped = []
                self.save_due = time.time() + 0.2
                self.message = "Seen marks cleared"


LIVE = Live()

# ---------- other weapons ----------
# The MK2 above is the hardware-backed weapon with its own map, LEDs and screens.
# Other weapons are drawn from their profile and run by a small live object with
# snapshot() and command(d), created on first use.
WEAPONS_DIR = os.path.join(os.path.dirname(HERE), "weapons")
DEFAULT_WEAPON = "mk2"


def _make_lightpad(profile):
    import lightpad_sim
    return lightpad_sim.LightpadLive(profile)


GENERIC = {"lightpad": _make_lightpad}
_generic_live = {}
_generic_lock = threading.Lock()


def weapon_path(wid):
    if not wid.replace("_", "").replace("-", "").isalnum():
        return None
    p = os.path.join(WEAPONS_DIR, wid + ".weapon.json")
    return p if os.path.exists(p) else None


def list_weapons():
    out = []
    for fn in sorted(os.listdir(WEAPONS_DIR)):
        if fn.endswith(".weapon.json"):
            try:
                with open(os.path.join(WEAPONS_DIR, fn), encoding="utf-8") as f:
                    w = json.load(f)
                out.append({"id": w.get("id", fn[:-12]), "name": w.get("name", fn[:-12]),
                            "playable": w.get("id") == "mk2" or w.get("id") in GENERIC})
            except (OSError, ValueError):
                pass
    return {"default": DEFAULT_WEAPON, "weapons": out}


def generic_live(wid):
    """The live object for a non-MK2 weapon, or None."""
    if wid not in GENERIC:
        return None
    with _generic_lock:
        if wid not in _generic_live:
            with open(weapon_path(wid), encoding="utf-8") as f:
                _generic_live[wid] = GENERIC[wid](json.load(f))
        return _generic_live[wid]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlsplit(self.path)
        wid = (parse_qs(u.query).get("id") or [""])[0]
        if u.path == "/weapons":
            self.send_json(list_weapons())
        elif u.path == "/weapon" and wid and wid != "mk2":
            p = weapon_path(wid)
            if p is None:
                self.send_error(404, "no such weapon")
                return
            with open(p, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/wevents":
            live = generic_live(wid)
            if live is None:
                self.send_error(404, "no simulator for this weapon")
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(("data: %s\n\n" % json.dumps(live.snapshot())).encode())
                    self.wfile.flush()
                    time.sleep(1 / 30)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                return
        else:  # the page itself and the MK2 routes, exactly as before
            self.path = u.path
            self.do_GET_mk2()

    def do_GET_mk2(self):
        if self.path in ("/", "/index.html"):
            with open(HTML, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/weapon":
            try:
                with open(WEAPON, "rb") as f:
                    body = f.read()
            except OSError:
                self.send_error(404, "weapon profile missing")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(("data: %s\n\n" % json.dumps(LIVE.snapshot())).encode())
                    self.wfile.flush()
                    time.sleep(1 / 30)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                return
        else:
            self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        u = urlsplit(self.path)
        if u.path == "/wcmd":
            live = generic_live((parse_qs(u.query).get("id") or [""])[0])
            try:
                if live is not None:
                    live.command(json.loads(self.rfile.read(n) or b"{}"))
            except ValueError:
                self.send_error(400)
                return
            self.send_response(204)
            self.end_headers()
            return
        try:
            LIVE.command(json.loads(self.rfile.read(n) or b"{}"))
        except ValueError:
            self.send_error(400)
            return
        self.send_response(204)
        self.end_headers()


def _arg(name, default):
    a = sys.argv[1:]
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else default


def main():
    global DEFAULT_WEAPON
    LIVE.sim = "--sim" in sys.argv[1:]
    port = int(_arg("--port", PORT))
    DEFAULT_WEAPON = _arg("--weapon", "mk2")
    if DEFAULT_WEAPON == "mk2":
        threading.Thread(target=LIVE.device_loop, daemon=True).start()
        threading.Thread(target=LIVE.screen_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print("%s live view on http://127.0.0.1:%d  (Ctrl-C to stop)" % (
        "MK2" if DEFAULT_WEAPON == "mk2" else DEFAULT_WEAPON, port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        with LIVE.lock:
            if LIVE.save_due:
                LIVE.save()
            if LIVE.m:
                LIVE.m.running = False
        time.sleep(0.15)
        if LIVE.t:
            try:  # blank both screens on exit
                sc = LIVE.saved["screen"]
                blank = scr.pack_buf(bytes(2048), sc["msb_left"], sc["invert"])
                for side in ("left", "right"):
                    for i in range(scr.STRIPS):
                        LIVE.t.write(scr.report(side, i, blank[i]))
            except Exception:
                pass
            mk.leds_off_and_close(LIVE.t)


if __name__ == "__main__":
    main()
