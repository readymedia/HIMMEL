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

# ---------------------------------------------------------------------------
# Start-up shows: short light shows on the NuVJ's LEDs and LCD, played when
# Himmel takes the controller (or from the live view). Any button skips.
# ---------------------------------------------------------------------------
SEG16_ALL = ["a1", "a2", "b", "c", "d1", "d2", "e", "f", "g1", "g2", "h", "i", "j", "k", "l", "m"]
RAYS = ["i", "j", "g2", "m", "l", "k", "g1", "h"]          # the spokes of a 16-segment cell, clockwise from the top
RING = [7, 8, 9, 6, 3, 2, 1, 4]                             # the pads around the centre pad, clockwise
DECK_COLS = [[7, 4, 1], [8, 5, 2], [9, 6, 3]]               # pad columns, left to right
SIDE_LEDS = {"A": ["A Latch", "A Auto", "A Sync", "A Scratch", "Insta A"],
             "B": ["B Latch", "B Auto", "B Sync", "B Scratch", "Insta B"]}
CENTRE_LEDS = ["Master FX", "Insta Black", "BPM Tap"]
ALL_LEDS = PADS + SIDE_LEDS["A"] + SIDE_LEDS["B"] + CENTRE_LEDS
WORDS = ["lbl_edit", "lbl_global", "lbl_program", "lbl_change", "lbl_preset", "lbl_page", "lbl_midich", "lbl_prog",
         "lbl_bankma", "lbl_banklb", "lbl_ctrl", "lbl_aft", "lbl_note", "lbl_bpm", "lbl_full", "lbl_16levels",
         "lbl_last", "lbl_data", "rule"]


def seg16_keys(cell, ch):
    from nuvj_sim import SEG16
    return {"alpha.%d.%s" % (cell, s) for s in SEG16.get(ch.upper(), "").split()}


class Intro:
    """Base: a show that runs for `length` seconds, then ends (done = True)."""
    name, length = "Intro", 4.0

    def __init__(self):
        self.t0 = time.time()
        self.done = False

    def t(self):
        return time.time() - self.t0

    def tick(self):
        if self.t() > self.length:
            self.done = True

    def on_event(self, name, ev):
        if ev.get("kind") == "down":
            self.done = True                                    # any button skips the show

    def frame(self, t):
        return {}, set()

    def lcd(self):
        model, _ = self.frame(self.t())
        return model

    def leds(self, held):
        _, leds = self.frame(self.t())
        return set(leds) | set(held)


class BootSweep(Intro):
    """Light sweeps A -> centre -> B, the 16-segment cells light up one by one and settle into
    HIMMEL VJ, the numbers count up, HIMMEL is typed on the dot row, the wedges fill."""
    name, length = "Boot sweep", 4.2

    def frame(self, t):
        lit, leds = {"rule"}, set()
        # LEDs: a bright column sweeping across deck A, the centre, deck B, twice, then all on and out
        cols = [("A", c) for c in DECK_COLS] + [("C", None)] + [("B", c) for c in DECK_COLS]
        if t < 2.4:
            i = int(t / 0.12) % len(cols)
            side, col = cols[i]
            leds |= {"%s Pad %d" % (side, n) for n in col} if col else set(CENTRE_LEDS)
            if side in "AB":
                leds |= set(SIDE_LEDS[side][:3]) if i % 3 == 1 else set()
        elif t < 3.3:
            leds |= set(ALL_LEDS) if int(t * 8) % 2 == 0 else set()
        elif t < 3.9:
            leds |= set(ALL_LEDS)
        # LCD: cells fill left to right with every segment, then become the letters
        word = "HIMMEL VJ 5"
        for cell in range(CELLS):
            on_at = 0.15 + cell * 0.12
            if t >= on_at + 0.9:
                lit |= seg16_keys(cell, word[cell])
            elif t >= on_at:
                lit |= {"alpha.%d.%s" % (cell, s) for s in SEG16_ALL}
        text = "HIMMEL"[:max(0, int((t - 0.3) / 0.16))] if t > 0.3 else ""
        bpm = min(120, int(t * 60)) if t > 0.5 else None
        preset = min(5, int(t * 2)) if t > 1.0 else None
        n = min(7, int(t * 3))
        lit |= {w for i, w in enumerate(WORDS) if t > 0.6 + i * 0.08}
        lit |= {"box_" + "abcd"[int(t * 6) % 4]} if t < 3.0 else {"box_a", "box_b", "box_c", "box_d"}
        if t > 3.0 and int(t * 4) % 2:
            lit.add("enter")
        model = {"text": text, "alpha": "", "bpm": "" if bpm is None else "%3d" % bpm,
                 "preset": "" if preset is None else "%02d" % preset, "last": n, "data": n, "lit": sorted(lit)}
        return model, leds


class Starburst(Intro):
    """Every 16-segment cell is a spinning star (phase shifted along the row), the pads run round
    each deck, the A-D boxes chase, the wedges breathe."""
    name, length = "Starburst", 4.0

    def frame(self, t):
        lit, leds = {"rule"}, set()
        speed = 10 + 14 * min(1.0, t / 2.0)                    # spins up
        for cell in range(CELLS):
            k = int(t * speed + cell * 1.5)
            lit |= {"alpha.%d.%s" % (cell, RAYS[(k + j) % 8]) for j in (0, 4)}     # two opposite spokes
            if t > 2.8:
                lit |= {"alpha.%d.%s" % (cell, r) for r in RAYS}                  # burst: every spoke
        step = int(t * 14)
        for side, d in (("A", 1), ("B", -1)):
            n = RING[(step * d) % 8]
            leds |= {"%s Pad %d" % (side, n), "%s Pad %d" % (side, RING[(step * d + 4) % 8])}
            if t > 2.8:
                leds |= {"%s Pad %d" % (side, p) for p in range(1, 10)}
        leds |= {CENTRE_LEDS[step % 3]}
        lit.add("box_" + "abdc"[step % 4])
        breathe = int(3.5 + 3.5 * __import__("math").sin(t * 6))
        model = {"text": "  READY " if t > 2.8 else " " * int(t * 3) + "*", "alpha": "",
                 "bpm": "", "preset": "", "last": breathe, "data": 7 - breathe, "lit": sorted(lit | {"lbl_last", "lbl_data"})}
        return model, leds


class MadeBy(Intro):
    """MADE BY READYMEDIA.NO scrolls along the 16-segment row; the LEDs beat like a heart."""
    name, length = "Made by", 5.4
    TEXT = " " * 11 + "MADE BY READYMEDIA.NO" + " " * 4

    def frame(self, t):
        lit, leds = {"rule", "lbl_bpm"}, set()
        off = min(len(self.TEXT) - 11, int(t / 0.14))
        window = self.TEXT[off:off + 11]
        for cell, ch in enumerate(window):
            lit |= seg16_keys(cell, ch)
        beat = (t * 1.6) % 1.0                                 # a double beat, about 96 BPM
        thump = beat < 0.08 or 0.18 < beat < 0.26
        if thump:
            leds |= {"A Pad 5", "B Pad 5", "BPM Tap"} | ({"%s Pad %d" % (s, n) for s in "AB" for n in RING} if beat < 0.08 else set())
        model = {"text": "HIMMEL" if t < 4.5 else " HIMMEL ", "alpha": "", "bpm": " 96" if thump else "",
                 "preset": "", "last": 7 if thump else 0, "data": 7 if thump else 0, "lit": sorted(lit)}
        return model, leds


INTROS = {"boot": BootSweep, "starburst": Starburst, "madeby": MadeBy}



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


def intro_selftest():
    import sys
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    for key, cls in INTROS.items():
        show = cls()
        for k in range(0, int(cls.length * 20) + 5):
            show.t0 = time.time() - k / 20.0
            m = show.lcd()
            assert set(m) >= {"text", "lit"} and isinstance(show.leds(set()), set), key
            show.tick()
        assert show.done, key
    print("intro selftest ok:", ", ".join(c.name for c in INTROS.values()))


if __name__ == "__main__":
    selftest()
    intro_selftest()
