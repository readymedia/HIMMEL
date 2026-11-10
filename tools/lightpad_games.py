# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Lightpad games (easter eggs on the 15 x 15 LED surface)
=======================================================

Small games drawn by Himmel on the Lightpad, the cousins of the MK2 screen
games in mk2_fun.py. They run in the host (LocalHost in lightpad_sim.py), so
they work on the simulated block and on the real one alike.

    Rally       paddle duel. Bottom paddle = a finger in the lower half, top paddle =
                a finger in the upper half (two players), or Himmel plays the top
                when nobody touches it. First to 5.
    Brick Burst a paddle at the bottom follows your finger; keep the ball up and
                clear the bricks. Press harder = wider paddle. Tap or Mode = launch.
    Star Swarm  your ship follows your finger along the bottom and fires while you
                touch (press harder = faster fire). The swarm sways and dives.

On the block: hold the Mode button ~1 s to go to the next game (touch glow ->
Rally -> Brick Burst -> Star Swarm -> touch glow). A short Mode press = launch,
fire or restart. In the live view there are buttons for each game.

Each game: update(dt, inp) and draw() -> 225 (r, g, b), row-major from the top
left. inp.fingers = {index: (x, y, z)} with x, y, z in 0..1 (y = 0 at the top,
the USB-C side); inp.taps = [(x, y, z)] new touches since the last frame;
inp.mode = True if Mode was short-pressed.

Self-test (no block, no browser): python tools/lightpad_games.py
"""

import math
import random
import sys

N = 15
RESTART_AFTER = 4.0   # seconds on the game-over picture before a new game starts by itself


class Input:
    def __init__(self, fingers=None, taps=(), mode=False):
        self.fingers = fingers or {}
        self.taps = list(taps)
        self.mode = mode


class Canvas:
    def __init__(self):
        self.px = [[0.0, 0.0, 0.0] for _ in range(N * N)]

    def set(self, x, y, c, add=False):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < N and 0 <= y < N:
            p = self.px[y * N + x]
            for k in range(3):
                p[k] = min(255.0, p[k] + c[k]) if add else c[k]

    def fade(self, k):
        for p in self.px:
            p[0] *= k
            p[1] *= k
            p[2] *= k

    def clear(self):
        for p in self.px:
            p[0] = p[1] = p[2] = 0.0

    def out(self):
        return [(int(p[0]), int(p[1]), int(p[2])) for p in self.px]


# 3 x 5 digits and a few letters, for scores and the game title flash
FONT = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001010010010",
    "8": "111101111101111", "9": "111101111001111", "R": "110101110101101", "B": "110101110101110",
    "S": "111100111001111", "T": "111010010010010",
}


def draw_text(cv, text, x, y, c):
    for ch in text:
        g = FONT.get(ch)
        if g:
            for i, bit in enumerate(g):
                if bit == "1":
                    cv.set(x + i % 3, y + i // 3, c)
        x += 4


def hsv(h, s=1.0, v=1.0):
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return (r * 255, g * 255, b * 255)


def finger_in(inp, lower):
    """The x (0..14) of a finger in the lower (y > 0.5) or upper half, or None."""
    best = None
    for (x, y, z) in inp.fingers.values():
        if (y > 0.5) == lower:
            best = x * N - 0.5
    return best


def lowest(inp):
    """The finger nearest the bottom edge (x, y, z), or None: it steers paddles and ships."""
    return max(inp.fingers.values(), key=lambda f: f[1], default=None)


class Rally:
    name, letter = "Rally", "R"

    def __init__(self):
        self.score = [0, 0]   # bottom, top
        self.over = 0.0
        self.cv = Canvas()
        self.top_ai = True
        self.reset(serve_down=False)

    def reset(self, serve_down):
        self.pb, self.pt = 7.0, 7.0          # paddle centres
        self.bx, self.by = 7.0, 7.0
        a = random.uniform(-0.6, 0.6)
        self.speed = 7.0
        self.vx, self.vy = math.sin(a) * self.speed, (1 if serve_down else -1) * math.cos(a) * self.speed
        self.wait = 0.8
        self.flash = 0.0

    def update(self, dt, inp):
        if max(self.score) >= 5:  # game over: a tap, Mode or 4 s starts a new match
            self.over += dt
            if inp.mode or inp.taps or self.over > RESTART_AFTER:
                self.score, self.over = [0, 0], 0.0
                self.reset(False)
            return
        xb, xt = finger_in(inp, True), finger_in(inp, False)
        if xb is not None:
            self.pb += (xb - self.pb) * min(1.0, dt * 25)
        if xt is not None:
            self.pt += (xt - self.pt) * min(1.0, dt * 25)
            self.top_ai = False
        elif not any(y <= 0.5 for (_, y, _) in inp.fingers.values()):
            self.top_ai = True
        if self.top_ai:  # Himmel plays the top: follows the ball, a little lazy
            target = self.bx if self.vy < 0 else 7.0
            self.pt += max(-dt * 9, min(dt * 9, target - self.pt))
        self.pb = max(1.5, min(12.5, self.pb))
        self.pt = max(1.5, min(12.5, self.pt))
        if max(self.score) >= 5:
            return
        if self.wait > 0:
            self.wait -= dt
            return
        self.bx += self.vx * dt
        self.by += self.vy * dt
        if self.bx < 0 or self.bx > N - 1:
            self.vx = -self.vx
            self.bx = max(0, min(N - 1, self.bx))
        for paddle_y, centre, sign in ((13, self.pb, 1), (1, self.pt, -1)):
            if (sign > 0 and self.vy > 0 and paddle_y - 0.5 <= self.by <= paddle_y + 0.5) or \
               (sign < 0 and self.vy < 0 and paddle_y - 0.5 <= self.by <= paddle_y + 0.5):
                off = self.bx - centre
                if abs(off) <= 2.2:
                    self.speed = min(16.0, self.speed * 1.06)
                    a = max(-1.0, min(1.0, off / 2.2)) * 1.0
                    self.vx = math.sin(a) * self.speed
                    self.vy = -sign * math.cos(a) * self.speed
                    self.by = paddle_y - sign * 0.6
        if self.by > N - 0.5 or self.by < -0.5:
            winner = 1 if self.by > N - 0.5 else 0
            self.score[winner] += 1
            self.flash = 1.0
            self.reset(serve_down=winner == 0)
            self.flash = 1.0

    def draw(self):
        cv = self.cv
        cv.fade(0.55)
        if self.flash > 0:
            self.flash -= 0.05
        if max(self.score) >= 5:
            w = 0 if self.score[0] >= 5 else 1
            c = (40, 220, 255) if w == 0 else (255, 60, 200)
            cv.clear()
            draw_text(cv, str(self.score[0]), 1, 9, (40, 220, 255))
            draw_text(cv, str(self.score[1]), 11, 1, (255, 60, 200))
            for x in range(N):
                cv.set(x, 14 if w == 0 else 0, c)
            return cv.out()
        for x in range(N):  # the centre line, faint
            if x % 2 == 0:
                cv.set(x, 7, (25, 25, 35))
        for i in range(self.score[0]):  # score dots on the right edge
            cv.set(14, 13 - i, (20, 110, 130))
        for i in range(self.score[1]):
            cv.set(14, 1 + i, (130, 30, 100))
        for d in (-1.5, -0.5, 0.5, 1.5):
            cv.set(self.pb + d, 14, (40, 220, 255))
            cv.set(self.pt + d, 0, (255, 60, 200))
        cv.set(self.bx, self.by, (255, 255, 255))
        return cv.out()


class BrickBurst:
    name, letter = "Brick Burst", "B"

    def __init__(self):
        self.cv = Canvas()
        self.level = 1
        self.lives = 3
        self.new_level()

    def new_level(self):
        self.bricks = {}
        for row in range(4):
            for col in range(N):
                self.bricks[(col, 1 + row)] = hsv((row * 0.17 + self.level * 0.11) % 1.0, 0.85, 0.9)
        self.serve()

    def serve(self):
        self.px, self.width = 7.0, 2.0
        self.stuck = True
        self.bx, self.by = 7.0, 12.5
        self.speed = 8.0 + 1.5 * self.level
        self.vx, self.vy = 0.0, 0.0
        self.boom = []

    def update(self, dt, inp):
        if self.lives <= 0:
            self.over = getattr(self, "over", 0.0) + dt
            if inp.mode or inp.taps or self.over > RESTART_AFTER:
                self.level, self.lives, self.over = 1, 3, 0.0
                self.new_level()
            return
        z = 0.0
        x = None
        low = lowest(inp)
        if low:
            x, z = low[0] * N - 0.5, low[2]
        if x is not None:
            self.px += (x - self.px) * min(1.0, dt * 25)
        self.width = 1.5 + 1.5 * z              # half-width: harder press = wider paddle
        self.px = max(self.width, min(N - 1 - self.width, self.px))
        if self.stuck:
            self.bx, self.by = self.px, 12.5
            if inp.mode or inp.taps:
                a = random.uniform(-0.5, 0.5)
                self.vx, self.vy = math.sin(a) * self.speed, -math.cos(a) * self.speed
                self.stuck = False
            return
        steps = max(1, int(self.speed * dt / 0.4) + 1)   # small steps: never tunnel through a brick
        for _ in range(steps):
            self.bx += self.vx * dt / steps
            self.by += self.vy * dt / steps
            if self.bx < 0 or self.bx > N - 1:
                self.vx = -self.vx
                self.bx = max(0, min(N - 1, self.bx))
            if self.by < 0:
                self.vy = abs(self.vy)
                self.by = 0
            key = (int(round(self.bx)), int(round(self.by)))
            if key in self.bricks:
                self.boom.append([key[0], key[1], self.bricks.pop(key), 1.0])
                self.vy = -self.vy
                if not self.bricks:
                    self.level += 1
                    self.new_level()
                    return
            if self.vy > 0 and 12.6 <= self.by <= 13.6 and abs(self.bx - self.px) <= self.width + 0.6:
                a = max(-1.0, min(1.0, (self.bx - self.px) / (self.width + 0.6))) * 1.05
                self.vx, self.vy = math.sin(a) * self.speed, -math.cos(a) * self.speed
                self.by = 12.6
        if self.by > N:
            self.lives -= 1
            if self.lives > 0:
                self.serve()

    def draw(self):
        cv = self.cv
        cv.fade(0.4)
        if self.lives <= 0:
            cv.clear()
            draw_text(cv, str(min(9, self.level)), 6, 5, (255, 120, 40))
            return cv.out()
        for (x, y), c in self.bricks.items():
            cv.set(x, y, (c[0] * 0.55, c[1] * 0.55, c[2] * 0.55))
        for b in self.boom:
            cv.set(b[0], b[1], tuple(v * b[3] for v in b[2]), add=True)
            b[3] -= 0.12
        self.boom = [b for b in self.boom if b[3] > 0]
        for i in range(self.lives - 1):
            cv.set(14 - i, 14, (60, 0, 0))
        w = int(round(self.width))
        for d in range(-w, w + 1):
            cv.set(self.px + d, 13, (230, 230, 255))
        cv.set(self.bx, self.by, (255, 255, 120))
        return cv.out()


class StarSwarm:
    name, letter = "Star Swarm", "S"

    def __init__(self):
        self.cv = Canvas()
        self.wave = 1
        self.lives = 3
        self.score = 0
        self.stars = [[random.uniform(0, N), random.uniform(0, N), random.uniform(1, 4)] for _ in range(12)]
        self.new_wave()

    def new_wave(self):
        self.enemies = []
        for row in range(3):
            for col in range(5):
                self.enemies.append({"hx": 2 + col * 2.5, "hy": 1 + row * 1.6, "x": 0, "y": -3,
                                     "dive": None, "c": hsv((row * 0.12 + self.wave * 0.2) % 1, 0.8, 1)})
        self.t = 0.0
        self.ship = 7.0
        self.shots, self.bombs, self.boom = [], [], []
        self.cool = 0.0
        self.hit_flash = 0.0

    def update(self, dt, inp):
        if self.lives <= 0:
            self.over = getattr(self, "over", 0.0) + dt
            if inp.mode or inp.taps or self.over > RESTART_AFTER:
                self.__init__()
            return
        self.t += dt
        for s in self.stars:
            s[1] += s[2] * dt
            if s[1] > N:
                s[0], s[1] = random.uniform(0, N), 0
        low = lowest(inp)
        touch = (low[0] * N - 0.5, low[2]) if low else None
        if touch:
            self.ship += (touch[0] - self.ship) * min(1.0, dt * 20)
        self.ship = max(1, min(N - 2, self.ship))
        self.cool -= dt
        firing = touch is not None or inp.mode
        if firing and self.cool <= 0:
            self.shots.append([self.ship, 12.0])
            self.cool = 0.32 - 0.18 * (touch[1] if touch else 0.5)
        for s in self.shots:
            s[1] -= 18 * dt
        sway = math.sin(self.t * 1.3) * 1.5
        for e in self.enemies:
            if e["dive"] is None:
                tx, ty = e["hx"] + sway, e["hy"]
                e["x"] += (tx - e["x"]) * min(1.0, dt * 4)
                e["y"] += (ty - e["y"]) * min(1.0, dt * 3)
            else:
                d = e["dive"]
                d[0] += dt
                e["y"] += (5 + self.wave) * dt
                e["x"] += math.sin(d[0] * 4) * 6 * dt + (self.ship - e["x"]) * 0.8 * dt
                if random.random() < dt * 1.5:
                    self.bombs.append([e["x"], e["y"] + 0.6])
                if e["y"] > N:
                    e["dive"], e["y"] = None, -2
        if random.random() < dt * (0.5 + 0.2 * self.wave):
            idle = [e for e in self.enemies if e["dive"] is None]
            if idle:
                random.choice(idle)["dive"] = [0.0]
        for b in self.bombs:
            b[1] += (7 + self.wave) * dt
        for s in self.shots:
            for e in self.enemies:
                if abs(s[0] - e["x"]) < 0.8 and abs(s[1] - e["y"]) < 0.8:
                    e["dead"] = True
                    s[1] = -9
                    self.score += 1
                    self.boom.append([e["x"], e["y"], 1.0, e["c"]])
        self.enemies = [e for e in self.enemies if not e.get("dead")]
        self.shots = [s for s in self.shots if s[1] > -1]
        hit = any(abs(b[0] - self.ship) < 1.3 and 12.5 <= b[1] <= 14.5 for b in self.bombs) or \
            any(abs(e["x"] - self.ship) < 1.3 and e["y"] > 12.3 for e in self.enemies)
        self.bombs = [b for b in self.bombs if b[1] < N and not (abs(b[0] - self.ship) < 1.3 and 12.5 <= b[1] <= 14.5)]
        if hit and self.hit_flash <= 0:
            self.lives -= 1
            self.hit_flash = 1.2
            self.boom.append([self.ship, 13, 1.5, (255, 255, 255)])
            for e in self.enemies:
                if e["dive"] is not None and e["y"] > 10:
                    e["dive"], e["y"] = None, -2
        self.hit_flash -= dt
        if not self.enemies:
            self.wave += 1
            self.new_wave()

    def draw(self):
        cv = self.cv
        cv.fade(0.35)
        if self.lives <= 0:
            cv.clear()
            s = str(min(99, self.score))
            draw_text(cv, s, 7 - 2 * len(s) + 1, 5, (120, 200, 255))
            return cv.out()
        for s in self.stars:
            v = 20 + 15 * s[2]
            cv.set(s[0], s[1], (v, v, v * 1.3), add=True)
        for e in self.enemies:
            cv.set(e["x"], e["y"], e["c"])
        for s in self.shots:
            cv.set(s[0], s[1], (255, 255, 160))
        for b in self.bombs:
            cv.set(b[0], b[1], (255, 40, 40))
        for b in self.boom:
            r = (1.0 - b[2]) * 2.2 + 0.2
            for k in range(8):
                a = k * math.pi / 4
                cv.set(b[0] + math.cos(a) * r, b[1] + math.sin(a) * r, tuple(v * b[2] for v in b[3]), add=True)
            b[2] -= 0.1
        self.boom = [b for b in self.boom if b[2] > 0]
        blink = self.hit_flash > 0 and int(self.hit_flash * 10) % 2
        if not blink:
            c = (60, 200, 255)
            cv.set(self.ship, 13, c)
            cv.set(self.ship - 1, 14, c)
            cv.set(self.ship, 14, c)
            cv.set(self.ship + 1, 14, c)
        for i in range(self.lives - 1):
            cv.set(i, 0, (0, 70, 120))
        return cv.out()


GAMES = {"rally": Rally, "brick": BrickBurst, "swarm": StarSwarm}
RING = ["touch", "rally", "brick", "swarm"]    # the order a long Mode press walks through


def title(letter, colour=(255, 255, 255)):
    """A big-ish letter in the middle, shown for a moment when a game starts (the easter egg's 'hello')."""
    cv = Canvas()
    g = FONT.get(letter, "")
    for i, bit in enumerate(g):
        if bit == "1":
            x, y = i % 3, i // 3
            for dx in range(2):
                for dy in range(2):
                    cv.set(4 + x * 2 + dx, 2 + y * 2 + dy, colour)
    return cv.out()


def selftest(png=False):
    """Plays every game for 30 simulated seconds with a scripted finger; checks it never crashes,
    things move, and the frames look sane."""
    random.seed(1)
    ok = True
    for key, cls in GAMES.items():
        g = cls()
        frames, lit, t = [], set(), 0.0
        for i in range(30 * 60):
            t += 1 / 60
            fx = 0.5 + 0.45 * math.sin(t * 1.7)
            inp = Input({1: (fx, 0.92, 0.6)}, taps=[(fx, 0.92, 0.6)] if i % 120 == 0 else (),
                        mode=(i % 300 == 0))
            g.update(1 / 60, inp)
            f = g.draw()
            assert len(f) == N * N and all(0 <= v <= 255 for p in f for v in p)
            lit.add(sum(1 for p in f if sum(p) > 30))
            if i % 30 == 0:
                frames.append(f)
        good = len(lit) > 3
        print(("ok   " if good else "FAIL ") + "%-12s 30 s, lit pixels per frame %d..%d" % (cls.name, min(lit), max(lit)))
        ok = ok and good
        if png:
            _save_strip(frames[:24], "tools/_screens/lightpad_%s.png" % key)
    print("games selftest", "passed" if ok else "FAILED")
    return ok


def _save_strip(frames, path):
    """Writes frames side by side as a PNG (stdlib only), 8 px per LED, for a quick look."""
    import os
    import struct
    import zlib
    s, gap = 8, 4
    w = len(frames) * (N * s + gap)
    h = N * s
    rows = []
    for y in range(h):
        row = bytearray([0])
        for f in frames:
            for x in range(N * s):
                r, g, b = f[(y // s) * N + x // s] if (x % s and y % s) else (10, 10, 12)
                row += bytes([r, g, b])
            row += bytes(3 * gap)
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                 chunk(b"IDAT", raw) + chunk(b"IEND", b""))
    print("     wrote", path)


if __name__ == "__main__":
    sys.exit(0 if selftest(png="--png" in sys.argv[1:]) else 1)
