# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""A simulated Entrancer-style X/Y effects unit (weapon no. 3), played with the
mouse on the drawn panel or with a real Lightpad Block as its touch pad.

It behaves like the real unit (owner's manual; docs/touch-fx-entrancer.md):
program 00-99, HOLD, FX BALANCE, PAD MOTION (about 6 s of finger movement,
looped), MUTE/FREEZE, TAP/BPM, 8 program memories (FUNCTION + key = store),
the AUDIO / COMBI / VIDEO lever. It sends what the real unit sends on its MIDI
out (CC 12 = X, CC 13 = Y, CC 92 = touch, program change), plus a few
Himmel-only controllers for the panel controls the real unit keeps to itself
(src/engine/include/himmel/engine/entrancer.h). The bytes go to the engine over
UDP 127.0.0.1:8768; the engine answers with a status line every 250 ms.

Run the page:   python tools/entrancer_live.py      (http://127.0.0.1:8770)
Self-test:      python tools/entrancer_sim.py --selftest
"""
import colorsys
import json
import math
import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

ENGINE_PORT = 8768
CC_X, CC_Y, CC_TOUCH = 12, 13, 92
CC_FX_BALANCE, CC_FREEZE, CC_VIDEO_OFF, CC_BPM_MSB, CC_BPM_LSB, CC_PRESSURE = 102, 104, 105, 106, 107, 108
ACTIVE_SENSING = 0xFE
MOTION_MAX_S = 6.0          # the unit records about 6 s of pad motion
TAP_MODE_S = 5.0            # the display shows BPM this long after TAP
STORE_HOLD_S = 2.0          # FUNCTION + memory key held this long = store (manual: ~2 s)
MEMORY_FILE = os.path.join(HERE, "_entrancer", "memories.json")
# The real block's touch range (owner's Lightpad M): x and y about 146..3953 of 4095.
LP_MIN, LP_MAX = 146 / 4095, 3953 / 4095
LIGHTPAD_CURVE = 1.5        # the same soft pressure curve as Touch FX uses for the Lightpad

# Which Himmel effect each program plays: a copy of the table in
# src/engine/src/entrancer.cpp, for the page only (the engine's answer is the truth).
EFFECTS = ["HUE TURN", "DUOTONE", "GRID", "KALEIDO", "BULGE", "SLIDE", "WAVES", "STROBE", "TRAILS"]


def clamp01(v):
    return 0.0 if v < 0 else 1.0 if v > 1 else v


class Entrancer:
    """The unit's own logic. Thread-safe: the web server and the loop share it."""

    def __init__(self, engine_port=ENGINE_PORT, send=True):
        self.lock = threading.RLock()
        self.program = 0
        self.bpm = 120
        self.fx_balance = 1.0
        self.input_volume = 0.7          # drawn, no audio in Himmel
        self.control = "video"           # audio | combi | video
        self.lever = "center"            # motion (latched up) | center | freeze (held down)
        self.hold = False
        self.function = False
        self.memories = self._load_memories()
        self.mem_down = {}               # key -> time pressed (FUNCTION held: store after STORE_HOLD_S)
        self.mem_active = None           # the memory key that is lit (last recalled / stored)
        self.flash = ""                  # a short message on the display area
        self.flash_until = 0.0
        self.tap_times = []
        self.tap_mode_until = 0.0
        # pad: two sources, the mouse on the drawing and the real Lightpad
        self.mouse = None                # (x, y, z) or None
        self.lightpad = None
        self.touch = False               # what the unit currently "sends" as touched
        self.x, self.y, self.z = 0.5, 0.5, 1.0
        self.motion = []                 # [(t, x, y, touching)] recorded
        self.motion_rec_start = None
        self.motion_play_start = None
        self.motion_play_xy = None       # (x, y, touching) while playing back
        # engine link
        self.sent = {}                   # last value sent per CC (send only changes)
        self.sent_touch = None
        self.sent_program = None
        self.engine = None               # last status line from the engine (dict)
        self.engine_at = 0.0
        self.out_count = 0
        self.sock = None
        self.addr = ("127.0.0.1", engine_port)
        if send:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(("127.0.0.1", 0))
            self.sock.setblocking(False)
        self.closed = False
        self.next_sense = 0.0
        self.resend_all = True

    # ------------------------------------------------------------ memories
    def _load_memories(self):
        try:
            with open(MEMORY_FILE, encoding="utf-8") as f:
                m = json.load(f)
            return [m[i] if i < len(m) else None for i in range(8)]
        except (OSError, ValueError):
            return [None] * 8

    def _save_memories(self):
        try:
            os.makedirs(os.path.dirname(MEMORY_FILE), exist_ok=True)
            with open(MEMORY_FILE, "w", encoding="utf-8") as f:
                json.dump(self.memories, f, indent=1)
        except OSError:
            pass

    def show(self, text, seconds=1.2):
        self.flash, self.flash_until = text, time.time() + seconds

    # ------------------------------------------------------------ panel input
    def command(self, d):
        """One panel action from the page: {"control": name, ...}."""
        now = time.time()
        with self.lock:
            c = d.get("control")
            if c == "program_turn":
                steps = int(d.get("steps", 0))
                if now < self.tap_mode_until:
                    self.bpm = max(40, min(300, self.bpm + steps))
                    self.tap_mode_until = now + TAP_MODE_S
                else:
                    self.program = max(0, min(99, self.program + steps))
                    self.mem_active = None
            elif c == "fx_balance":
                self.fx_balance = clamp01(float(d.get("value", 1)))
            elif c == "input_volume":
                self.input_volume = clamp01(float(d.get("value", 0.7)))
            elif c == "control_select" and d.get("value") in ("audio", "combi", "video"):
                self.control = d["value"]
            elif c == "lever" and d.get("value") in ("motion", "center", "freeze"):
                self._lever(d["value"], now)
            elif c == "hold" and d.get("down"):
                self.hold = not self.hold
            elif c == "function":
                self.function = bool(d.get("down"))
                if self.function and self.lever == "motion":
                    self.motion = []          # FUNCTION + PAD MOTION = erase (manual)
                    self.show("CLR")
            elif c == "tap" and d.get("down"):
                self._tap(now)
            elif c == "memory":
                self._memory(int(d.get("n", 1)) - 1, bool(d.get("down")), now)
            elif c in ("sample1", "sample2", "rec") and d.get("down"):
                self.show("n/a", 1.5)         # sampling is not in Himmel yet
            elif c == "pad":
                if d.get("phase") == "end":
                    self.mouse = None
                else:
                    self.mouse = (clamp01(float(d.get("x", 0.5))), clamp01(float(d.get("y", 0.5))),
                                  clamp01(float(d.get("z", 1.0))))
            elif c == "resend":
                self.resend_all = True

    def _lever(self, v, now):
        if v == "motion" and self.function:
            self.motion = []
            self.show("CLR")
        if v != "motion":
            self.motion_play_start = self.motion_play_xy = None
            self.motion_rec_start = None
        self.lever = v

    def _tap(self, now):
        self.tap_times = [t for t in self.tap_times if now - t < 2.5] + [now]
        if len(self.tap_times) >= 2:
            gaps = [b - a for a, b in zip(self.tap_times, self.tap_times[1:]) if b - a > 0.05]
            if gaps:
                self.bpm = max(40, min(300, int(round(60.0 / (sum(gaps) / len(gaps))))))
        self.tap_mode_until = now + TAP_MODE_S

    def _memory(self, i, down, now):
        if not 0 <= i < 8:
            return
        if down:
            self.mem_down[i] = now
            if not self.function:
                m = self.memories[i]
                if m:
                    self.program = m["program"]
                    self.fx_balance = m["fx_balance"]
                    self.bpm = m["bpm"]
                    self.hold = m["hold"]
                    if m["hold"]:
                        self.x, self.y = m["x"], m["y"]
                    self.mem_active = i
                    self.show("P-%d" % (i + 1), 0.6)
                else:
                    self.show("---", 0.6)
        else:
            self.mem_down.pop(i, None)

    # ------------------------------------------------------------ the unit's loop
    def step(self, now):
        with self.lock:
            for i, t in list(self.mem_down.items()):     # FUNCTION + key held 2 s = store
                if self.function and now - t >= STORE_HOLD_S:
                    self.memories[i] = {"program": self.program, "fx_balance": self.fx_balance, "bpm": self.bpm,
                                        "hold": self.hold, "x": self.x, "y": self.y}
                    self._save_memories()
                    self.mem_down.pop(i)
                    self.mem_active = i
                    self.show("STO", 1.0)
            finger = self.lightpad or self.mouse
            touching = finger is not None
            # PAD MOTION: touching records, letting go loops the recording
            if self.lever == "motion":
                if touching:
                    if self.motion_rec_start is None:
                        self.motion_rec_start, self.motion, self.motion_play_start = now, [], None
                    t = now - self.motion_rec_start
                    if t <= MOTION_MAX_S:
                        self.motion.append((t, finger[0], finger[1], True))
                else:
                    if self.motion_rec_start is not None and self.motion:
                        t = now - self.motion_rec_start
                        if t <= MOTION_MAX_S:
                            self.motion.append((t, self.motion[-1][1], self.motion[-1][2], False))  # the lift is recorded too
                        self.motion_rec_start = None
                        self.motion_play_start = now
                    if self.motion and self.motion_play_start is not None:
                        self.motion_play_xy = self._motion_at(now - self.motion_play_start)
            else:
                self.motion_play_xy = None
            if touching:
                self.x, self.y, self.z = finger
                self.touch = True
            elif self.motion_play_xy is not None:
                self.x, self.y, on = self.motion_play_xy
                self.z = 1.0
                self.touch = on or self.hold
            else:
                self.touch = self.hold and self._was_touched()  # HOLD keeps the last position
            self._send(now)

    def _was_touched(self):
        return self.touch or self.sent_touch

    def _motion_at(self, t):
        length = self.motion[-1][0] if self.motion else 0
        if length <= 0:
            return None
        t = t % max(length, 0.05)
        last = self.motion[0]
        for s in self.motion:
            if s[0] > t:
                break
            last = s
        return last[1], last[2], last[3]

    # ------------------------------------------------------------ MIDI out
    def _cc(self, out, cc, v):
        v = max(0, min(127, int(round(v))))
        if self.resend_all or self.sent.get(cc) != v:
            out += bytes([0xB0, cc, v])
            self.sent[cc] = v

    def midi_now(self, now):
        """The bytes the unit would send right now (only changes, unless resend_all)."""
        out = bytearray()
        if self.resend_all or self.sent_program != self.program:
            out += bytes([0xC0, self.program])
            self.sent_program = self.program
        self._cc(out, CC_FX_BALANCE, self.fx_balance * 127)
        self._cc(out, CC_VIDEO_OFF, 127 if self.control == "audio" else 0)
        if self.resend_all or self.sent.get(CC_BPM_MSB) != self.bpm >> 7 or self.sent.get(CC_BPM_LSB) != self.bpm & 127:
            out += bytes([0xB0, CC_BPM_MSB, self.bpm >> 7, 0xB0, CC_BPM_LSB, self.bpm & 127])
            self.sent[CC_BPM_MSB], self.sent[CC_BPM_LSB] = self.bpm >> 7, self.bpm & 127
        self._cc(out, CC_FREEZE, 127 if self.lever == "freeze" else 0)
        self._cc(out, CC_X, self.x * 127)
        self._cc(out, CC_Y, self.y * 127)
        self._cc(out, CC_PRESSURE, self.z * 127)
        if self.resend_all or self.sent_touch != self.touch:
            out += bytes([0xB0, CC_TOUCH, 127 if self.touch else 0])
            self.sent_touch = self.touch
        if now >= self.next_sense:
            self.next_sense = now + 0.25
            out.append(ACTIVE_SENSING)
        self.resend_all = False
        return bytes(out)

    def _send(self, now):
        data = self.midi_now(now)
        if self.sock and data:
            try:
                self.sock.sendto(data, self.addr)
                self.out_count += len(data)
            except OSError:
                pass
        while self.sock:                  # the engine's status lines (and Windows' resets: drain, never sleep)
            try:
                pkt, _ = self.sock.recvfrom(512)
            except BlockingIOError:
                break
            except OSError:
                continue
            line = pkt.decode("ascii", "replace").strip()
            if line.startswith("HIMMEL"):
                if not self.engine:
                    self.resend_all = True    # an engine just appeared: give it the whole state
                self.engine = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
                self.engine_at = now
        if self.engine and now - self.engine_at > 1.0:
            self.engine = None

    # ------------------------------------------------------------ for the page and the LEDs
    def display(self, now):
        with self.lock:
            if now < self.flash_until:
                return self.flash
            if now < self.tap_mode_until:
                return "%3d" % self.bpm
            finger = self.lightpad or self.mouse
            if finger:                      # the unit shows the touched position as 1.1 .. 9.9
                return "%d.%d" % (1 + int(finger[0] * 8.999), 1 + int(finger[1] * 8.999))
            return " %02d" % self.program

    def snapshot(self, now):
        with self.lock:
            fx = self.engine.get("effect") if self.engine else None
            return {
                "program": self.program, "bpm": self.bpm, "fx_balance": self.fx_balance,
                "input_volume": self.input_volume, "control": self.control, "lever": self.lever,
                "hold": self.hold, "function": self.function, "touch": self.touch,
                "x": self.x, "y": self.y, "z": self.z,
                "mouse": self.mouse is not None, "lightpad_finger": self.lightpad is not None,
                "motion_len": round(self.motion[-1][0], 2) if self.motion else 0,
                "motion_playing": self.motion_play_xy is not None,
                "memories": [m is not None for m in self.memories],
                "mem_active": self.mem_active,
                "display": self.display(now),
                "tap_mode": now < self.tap_mode_until,
                "engine": self.engine, "engine_effect": fx,
                "out_bytes": self.out_count,
            }

    def pad_colour(self, x, y, lit):
        """The pad's light: colour follows the finger and the program (the unit's 'Light')."""
        h = (self.program * 0.071 + x * 0.35) % 1.0
        v = (0.35 + 0.65 * y) if lit else 0.25
        return colorsys.hsv_to_rgb(h, 0.85, v)

    def close(self):
        self.closed = True
        if self.sock:
            self.sock.close()


# ---------------------------------------------------------------- the Lightpad as the pad
def make_lightpad_host(unit):
    """A Lightpad host (tools/lightpad_sim.py LocalHost) on the real block that feeds
    its first finger to the unit and draws the unit's pad light on the 15 x 15 LEDs.
    Returns (block, host) or raises if the Lightpad code is missing."""
    import lightpad_hw
    import lightpad_sim as ls

    class EntrancerHost(ls.LocalHost):
        def on_block(self, msg):
            super().on_block(msg)
            for e in ls.decode_device(msg):
                if e["type"] == "button" and e["down"]:
                    unit.command({"control": "hold", "down": True})   # Mode = HOLD

        def picture(self, now, dt=0.04):
            with self.lock:
                fingers = dict(self.fingers)
            if fingers:
                fx, fy, fz = fingers[min(fingers)]
                x = clamp01((fx - LP_MIN) / (LP_MAX - LP_MIN))
                y = 1.0 - clamp01((fy - LP_MIN) / (LP_MAX - LP_MIN))     # the block's y = 0 is the top
                with unit.lock:
                    unit.lightpad = (x, y, clamp01(fz) ** LIGHTPAD_CURVE)
            else:
                with unit.lock:
                    unit.lightpad = None
            return lightpad_picture(unit, now)

    block = lightpad_hw.HwLightpad()
    host = EntrancerHost(block)
    return block, host


def lightpad_picture(unit, now):
    """15 x 15 RGB tuples, row 0 = the block's top row."""
    g = 15
    with unit.lock:
        lit, x, y, hold, lever, ctrl = unit.touch, unit.x, unit.y, unit.hold, unit.lever, unit.control
        playing = unit.motion_play_xy is not None
        finger = unit.lightpad or unit.mouse
    if ctrl == "audio":                       # the pad does not touch the video
        return [(10, 10, 14)] * (g * g)
    if lever == "freeze":
        k = 0.5 + 0.5 * math.sin(now * 12)
        return [(int(90 + 120 * k), int(110 + 120 * k), 255)] * (g * g)
    r, gg, b = unit.pad_colour(x, y, lit)
    if not lit:                               # idle: the program's colour breathing slowly
        k = 0.6 + 0.4 * math.sin(now * 2.0)
        r, gg, b = r * k, gg * k, b * k
    px = []
    cx, cy = x * g - 0.5, (1 - y) * g - 0.5   # LED centres; row 0 is the top
    for row in range(g):
        for col in range(g):
            c = [r, gg, b]
            if lit:
                d = math.hypot(col - cx, row - cy)
                k = max(0.0, 1 - d / 2.2)
                if finger is None and (hold or playing):   # held / played-back point: a cross
                    k = max(k, 1.0 if (col == round(cx) or row == round(cy)) and d < 3.5 else 0)
                c = [v + (1 - v) * k for v in c]
            px.append(tuple(int(255 * max(0.0, min(1.0, v))) for v in c))
    return px


# ---------------------------------------------------------------- self-test
def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + name)
        ok = ok and cond

    u = Entrancer(send=False)
    now = 100.0
    first = u.midi_now(now)
    check("first send carries program, touch off, active sensing",
          first[:2] == bytes([0xC0, 0]) and bytes([0xB0, CC_TOUCH, 0]) in first and first[-1] == ACTIVE_SENSING)
    check("nothing changed: only active sensing later", u.midi_now(now + 0.3) == bytes([ACTIVE_SENSING]))
    u.command({"control": "pad", "phase": "start", "x": 1.0, "y": 0.0, "z": 1})
    u.step(now + 0.31)
    out = u.midi_now(now + 0.32)
    u.step(now + 0.33)
    check("touch sends X / Y / touch on", u.sent.get(CC_X) == 127 and u.sent.get(CC_Y) == 0 and u.sent_touch)
    u.command({"control": "pad", "phase": "end"})
    u.step(now + 0.4)
    check("lift without HOLD = touch off", u.sent_touch is False)
    u.command({"control": "hold", "down": True})
    u.command({"control": "pad", "phase": "start", "x": 0.25, "y": 0.75, "z": 1})
    u.step(now + 0.5)
    u.command({"control": "pad", "phase": "end"})
    u.step(now + 0.6)
    check("lift with HOLD keeps touch on at the last position",
          u.sent_touch is True and u.sent.get(CC_X) == 32 and u.sent.get(CC_Y) == 95)
    u.command({"control": "hold", "down": True})
    u.step(now + 0.7)
    check("HOLD off = touch off", u.sent_touch is False)
    u.command({"control": "program_turn", "steps": 150})
    u.step(now + 0.8)
    check("program clamps to 99 and sends a program change", u.program == 99 and u.sent_program == 99)
    u.command({"control": "tap", "down": True})
    u.command({"control": "tap", "down": True})
    check("TAP shows BPM on the display", u.display(time.time()).strip().isdigit())
    u.tap_times = [10.0, 10.5, 11.0]
    u._tap(11.5)
    check("taps every 0.5 s = 120 BPM", u.bpm == 120)
    # pad motion: record 0.3 s, lift, playback loops
    u.command({"control": "lever", "value": "motion"})
    t = 200.0
    for i in range(4):
        u.command({"control": "pad", "phase": "move", "x": i / 3, "y": 0.5, "z": 1})
        u.step(t + i * 0.1)
    u.command({"control": "pad", "phase": "end"})
    u.step(t + 0.35)
    u.step(t + 0.36)
    check("PAD MOTION recorded and plays back", len(u.motion) >= 4 and u.motion_play_xy is not None)
    u.step(t + 0.36 + 0.21)
    x_play = u.x
    u.step(t + 0.36 + 0.21 + u.motion[-1][0])   # one loop (the recording's length) later: the same point
    check("playback loops", abs(u.x - x_play) < 1e-6)
    u.command({"control": "lever", "value": "center"})
    u.step(t + 1.0)
    check("lever back to centre stops the loop (touch off)", u.motion_play_xy is None and u.sent_touch is False)
    u.command({"control": "lever", "value": "freeze"})
    u.step(t + 1.1)
    check("MUTE/FREEZE sends CC 104 on", u.sent.get(CC_FREEZE) == 127)
    u.command({"control": "control_select", "value": "audio"})
    u.step(t + 1.2)
    check("lever on AUDIO sends CC 105 on", u.sent.get(CC_VIDEO_OFF) == 127)
    px = lightpad_picture(u, 1.0)
    check("LED picture is 15 x 15", len(px) == 225 and all(len(p) == 3 for p in px))
    u.control = "video"
    u.lever = "center"
    u.touch, u.x, u.y = True, 0.0, 1.0
    px = lightpad_picture(u, 1.0)
    check("the finger glows at its place (top-left = row 0, col 0)", sum(px[0]) > sum(px[224]))
    print("selftest", "passed" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(0 if selftest() else 1)
    print(__doc__)
