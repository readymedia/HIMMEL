# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Jog Pong: a small game on the NuVJ's own LCD (an easter egg)
============================================================

A three-lane pong played with the two jog wheels, drawn with the NuVJ's
segment LCD. It shows that Himmel drives every part of the controller.

- The field is the LCD's 11 16-segment characters. The ball is a bar in
  one of three lanes: top, middle or bottom. The paddles are brackets in
  the end characters (A on the left, B on the right).
- Turn the left jog to move A, the right jog to move B. Spin while you hit
  to put an angle on the ball.
- Serve: push your BANK knob or press your SCRATCH button ("PRESS ENTER"
  blinks for the server).
- Scores in the big digits (A over BPM, B under PRESET), first to 5.
- The dot-matrix row says what happens. The wedges show the ball speed.
  The LEDs flash on hits, and the A / B boxes show who serves.
- A side whose jog is not touched for 5 s is played by Himmel, so one
  person can play alone.
- Start / stop: hold both SCRATCH buttons, or the live view's button.

The game only produces Himmel's LCD model and a set of LEDs; the NuVJ host
turns them into the device's own SysEx, exactly as for everything else.
"""

import random
import time

ROWS = 3                 # lanes: 0 top (a), 1 middle (g1 g2), 2 bottom (d1 d2)
CELLS = 11
FIELD = range(1, CELLS - 1)
WIN = 5
JOG_TICKS_PER_LANE = 6   # jog ticks to move a paddle one lane
AI_IDLE = 5.0            # s without jog input -> Himmel plays that side

BALL = {0: ["a1", "a2"], 1: ["g1", "g2"], 2: ["d1", "d2"]}
PADDLE = {"A": {0: ["a1", "a2", "b"], 1: ["g1", "g2", "b", "c"], 2: ["d1", "d2", "c"]},
          "B": {0: ["a1", "a2", "f"], 1: ["g1", "g2", "f", "e"], 2: ["d1", "d2", "e"]}}
PADS = ["%s Pad %d" % (s, n) for s in "AB" for n in range(1, 10)]


class JogPong:
    name = "Jog Pong"

    def __init__(self):
        now = time.time()
        self.t0 = now
        self.score = {"A": 0, "B": 0}
        self.pad = {"A": 1, "B": 1}
        self.acc = {"A": 0.0, "B": 0.0}
        self.spin = {"A": 0.0, "B": 0.0}       # recent jog speed, for angle on hits
        self.touched = {"A": now, "B": now}
        self.ai_next = {"A": 0.0, "B": 0.0}
        self.server = random.choice("AB")
        self.state = "serve"                    # serve / play / point / over
        self.state_t = now
        self.ball = [2, 1]                      # [cell, lane]
        self.dx, self.dy = 1, 0
        self.step_s = 0.30
        self.next_step = now
        self.rally = 0
        self.msg = ("JOG PONG", now + 2.5)
        self.flash = {}                         # led name -> until
        self._place_serve()

    # ---- helpers ----
    def _place_serve(self):
        s = self.server
        self.ball = [1 if s == "A" else CELLS - 2, self.pad[s]]
        self.dx = 1 if s == "A" else -1
        self.dy = 0

    def _ai(self, side, now):
        return now - self.touched[side] > AI_IDLE

    def _say(self, text, secs=1.6):
        self.msg = (text, time.time() + secs)

    def _flash(self, names, secs=0.18):
        until = time.time() + secs
        for n in names:
            self.flash[n] = until

    def _serve(self):
        self.state = "play"
        self.rally = 0
        self.step_s = 0.30
        self.dy = random.choice([-1, 0, 1])
        self.next_step = time.time() + self.step_s
        self._say("SERVE " + self.server, 0.8)

    # ---- input: the same Himmel events the NuVJ host gets ----
    def on_event(self, name, ev):
        now = time.time()
        k = ev.get("kind")
        side = name[0] if name and name[:2] in ("A ", "B ") else None
        if side and name.endswith("Jog") and k == "delta":
            self.touched[side] = now
            self.spin[side] = self.spin[side] * 0.6 + ev["delta"]
            self.acc[side] += ev["delta"]
            while abs(self.acc[side]) >= JOG_TICKS_PER_LANE:   # clockwise = down
                step = 1 if self.acc[side] > 0 else -1
                self.acc[side] -= step * JOG_TICKS_PER_LANE
                self.pad[side] = max(0, min(ROWS - 1, self.pad[side] + step))
            if self.state == "serve" and side == self.server:
                self.ball[1] = self.pad[side]
        elif side and k == "down" and (name.endswith("Bank Push") or name.endswith("Scratch")):
            self.touched[side] = now
            if self.state == "serve" and side == self.server:
                self._serve()
            elif self.state == "over":
                self.__init__()

    # ---- time ----
    def tick(self):
        now = time.time()
        for s in "AB":                                     # Himmel plays an idle side
            if self._ai(s, now) and now >= self.ai_next[s]:
                self.ai_next[s] = now + 0.22
                target = self.ball[1] if (self.dx < 0) == (s == "A") else 1
                if self.pad[s] != target:
                    self.pad[s] += 1 if target > self.pad[s] else -1
                if self.state == "serve" and s == self.server:
                    self.ball[1] = self.pad[s]
            self.spin[s] *= 0.9
        if self.state == "serve" and self._ai(self.server, now) and now - self.state_t > 1.5:
            self._serve()
        elif self.state == "point" and now - self.state_t > 1.4:
            self.state, self.state_t = "serve", now
            self._place_serve()
        elif self.state == "over" and now - self.state_t > 6:
            self.__init__()
        elif self.state == "play" and now >= self.next_step:
            self.next_step = now + self.step_s
            self._step(now)

    def _step(self, now):
        x, y = self.ball
        ny = y + self.dy
        if ny < 0 or ny >= ROWS:                         # bounce off the top / bottom
            self.dy = -self.dy
            ny = y + self.dy
        nx = x + self.dx
        if nx <= 0 or nx >= CELLS - 1:                   # at a paddle
            side = "A" if nx <= 0 else "B"
            if self.pad[side] in (y, ny):
                self.dx = -self.dx
                spin = self.spin[side]
                self.dy = max(-1, min(1, (1 if spin > 2 else -1 if spin < -2 else random.choice([-1, 0, 0, 1]))))
                self.rally += 1
                self.step_s = max(0.08, self.step_s * 0.93)
                self._flash([n for n in PADS if n.startswith(side)] + ["Insta " + side])
                if self.rally % 5 == 0:
                    self._say("RALLY %d" % self.rally, 1.0)
                self.ball = [x, max(0, min(ROWS - 1, ny if self.dy else y))]
                return
            winner = "B" if side == "A" else "A"            # missed
            self.score[winner] += 1
            self._flash(["BPM Tap", "Insta Black", "Master FX"], 0.5)
            if self.score[winner] >= WIN:
                self.state, self.state_t = "over", now
                self._say("%s WINS!" % winner, 6)
            else:
                self.state, self.state_t = "point", now
                self.server = side                          # the one who missed serves
                self._say("%s SCORES" % winner, 1.4)
            return
        self.ball = [nx, ny]

    # ---- output: Himmel's LCD model and LEDs ----
    def lcd(self):
        now = time.time()
        lit = {"rule", "lbl_bpm", "lbl_preset", "lbl_last", "lbl_data", "box_" + self.server.lower()}
        for side, cell in (("A", 0), ("B", CELLS - 1)):
            lit |= {"alpha.%d.%s" % (cell, s) for s in PADDLE[side][self.pad[side]]}
        if self.state in ("play", "serve") or (self.state == "point" and int(now * 6) % 2):
            x, y = self.ball
            lit |= {"alpha.%d.%s" % (x, s) for s in BALL[y]}
        if self.state == "serve" and int(now * 3) % 2:
            lit.add("enter")                                # PRESS ENTER: push BANK (or SCRATCH) to serve
        if self.state == "over":
            lit.add("box_" + ("a" if self.score["A"] > self.score["B"] else "b"))
        text = self.msg[0] if now < self.msg[1] else ("SERVE " + self.server if self.state == "serve" else
                                                       "A%d    B%d" % (self.score["A"], self.score["B"]))
        speed = round((0.30 - self.step_s) / (0.30 - 0.08) * 7) if self.state == "play" else 0
        return {"text": text, "alpha": "", "bpm": "%3d" % self.score["A"], "preset": "%2d" % self.score["B"],
                "last": max(0, min(7, speed)), "data": min(7, self.rally % 8), "lit": sorted(lit)}

    def leds(self, held):
        now = time.time()
        on = {n for n, t in self.flash.items() if now < t}
        on |= {"A Scratch", "B Scratch"} if self.state != "over" else set()
        if self.state == "serve":
            on.add("Insta " + self.server)
        if self.state == "over":
            names = PADS + ["A Latch", "A Auto", "A Sync", "B Latch", "B Auto", "B Sync"]
            on |= {names[int(now * 12) % len(names)], names[int(now * 12 + len(names) // 2) % len(names)]}
        if self.state == "play" and (self.next_step - now) > self.step_s / 2:
            on.add("BPM Tap")                               # ticks with the ball
        return on | {n for n in held}


GAMES = {"pong": JogPong}


def selftest():
    g = JogPong()
    g.server = "A"
    g._place_serve()
    g.on_event("A Jog", {"kind": "delta", "delta": JOG_TICKS_PER_LANE})
    assert g.pad["A"] == 2 and g.ball == [1, 2]
    g.on_event("A Bank Push", {"kind": "down"})
    assert g.state == "play"
    m = g.lcd()
    assert "alpha.0.d1" in m["lit"] and any(k.startswith("alpha.1.") for k in m["lit"])
    for _ in range(200):                                    # let it run: the game never breaks
        g.next_step = 0
        g.touched = {"A": 0, "B": 0}                        # both sides played by Himmel
        g.ai_next = {"A": 0, "B": 0}
        g.state_t -= 2
        g.tick()
    print("jog pong selftest ok: score %d-%d, rally %d, state %s" % (g.score["A"], g.score["B"], g.rally, g.state))


if __name__ == "__main__":
    selftest()
