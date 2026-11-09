"""
Fun modes for the two MK2 screens (one 512 x 64 image across both)
==================================================================

  Demo  - parallax starfield, a big wavy "HIMMEL" scroller panning across both
          screens, pad hits spawn rings (bigger hit = bigger ring).
  Snake - a snake game on a 4-px grid (128 x 14 cells). Pads 10/5/6/7 are a
          d-pad (up/left/down/right), Play restarts.
  Maze  - an original first-person 3D maze (raycaster). Pad 10 = forward,
          Pad 6 = back, Pad 5 / 7 = turn. Pad pressure sets the speed.
          Walls are shaded by distance with ordered dithering.

Every mode has update(dt, inp) and draw(canvas). inp is an Input with:
  held    - names of held buttons and pads ("Play", "Pad 10", ...)
  pressed - names that went down since the last frame (edges)
  pads    - pressure 0..4095 per pad number (index 0 = Pad 1)
  knobs   - knob values by name ("Knob 1": 0..1000)
pad_leds() returns {pad number: (r, g, b)} for modes that light pads as controls.
"""

import math
import random

import mk2_screens as scr

WW, HH = 512, 64
BAYER = scr.BAYER
UP, LEFT, DOWN, RIGHT = "Pad 10", "Pad 5", "Pad 6", "Pad 7"
DPAD_LEDS = {10: (0, 90, 127), 5: (0, 90, 127), 6: (0, 90, 127), 7: (0, 90, 127)}


class Input:
    def __init__(self, held=(), pressed=(), pads=None, knobs=None):
        self.held = set(held)
        self.pressed = set(pressed)
        self.pads = pads or [0] * 16
        self.knobs = knobs or {}


def set_px(buf, rb, x, y):
    if 0 <= x < WW and 0 <= y < HH:
        buf[y * rb + (x >> 3)] |= 0x80 >> (x & 7)


def circle(c, cx, cy, r):
    steps = max(12, int(r * 6))
    for i in range(steps):
        a = 2 * math.pi * i / steps
        c.pixel(int(cx + r * math.cos(a)), int(cy + r * math.sin(a)))


# ---------------------------------------------------------------- demo
class Demo:
    TEXT = "  HIMMEL  *  MASCHINE MK2  *  512 X 64 PIXELS ACROSS TWO SCREENS  *  HIT THE PADS  *  MADE BY READYMEDIA.NO  "

    def __init__(self):
        rnd = random.Random(7)
        self.stars = [[rnd.uniform(0, WW), rnd.randrange(HH), rnd.choice((1, 2, 3))] for _ in range(90)]
        self.t = 0.0
        self.scroll = 0.0
        self.rings = []

    def pad_leds(self):
        return None  # pads glow by pressure

    def update(self, dt, inp):
        self.t += dt
        speed = 0.4 + inp.knobs.get("Knob 1", 300) / 1000 * 2.5  # Knob 1 = speed
        self.scroll += 60 * speed * dt
        for s in self.stars:
            s[0] -= s[2] * 25 * speed * dt
            if s[0] < 0:
                s[0] += WW
                s[1] = random.randrange(HH)
        for name in inp.pressed:
            if name.startswith("Pad "):
                n = int(name.split()[1])
                p = inp.pads[n - 1]
                x = ((n - 1) % 4) * 128 + 64 + random.randint(-30, 30)
                self.rings.append([x, random.randint(16, 48), 2.0, 20 + p / 4095 * 60])
        for r in self.rings:
            r[2] += 70 * dt
        self.rings = [r for r in self.rings if r[2] < r[3]]

    def draw(self, c):
        c.clear()
        buf, rb = c.buf, c.rb
        for x, y, layer in self.stars:
            set_px(buf, rb, int(x), y)
            if layer == 3:
                set_px(buf, rb, int(x) + 1, y)
        scale, gw = 5, scr.GLYPH_W * 5
        total = len(self.TEXT) * gw
        off = self.scroll % total
        first = int(off // gw)
        x = -(off % gw)
        i = first
        while x < WW:
            ch = self.TEXT[i % len(self.TEXT)]
            y = 14 + int(8 * math.sin(self.t * 3 + (x + off) * 0.02))
            c.fill(int(x) - 1, y - 1, gw, 37, False)  # keep stars out from behind the letters
            c.text(int(x), y, ch, scale=scale)
            x += gw
            i += 1
        for rx, ry, r, _ in self.rings:
            circle(c, rx, ry, r)


# ---------------------------------------------------------------- snake
class Snake:
    CELL, TOP = 4, 8
    COLS, ROWS = WW // 4, (HH - 8) // 4

    def __init__(self):
        self.reset()
        self.best = 0

    def pad_leds(self):
        return DPAD_LEDS

    def reset(self):
        y = self.ROWS // 2
        self.body = [(10, y), (9, y), (8, y), (7, y)]
        self.dir = (1, 0)
        self.next_dir = (1, 0)
        self.food = self.place_food()
        self.acc = 0.0
        self.dead = 0.0
        self.score = 0

    def place_food(self):
        while True:
            f = (random.randrange(1, self.COLS - 1), random.randrange(1, self.ROWS - 1))
            if f not in getattr(self, "body", []):
                return f

    def update(self, dt, inp):
        want = {UP: (0, -1), DOWN: (0, 1), LEFT: (-1, 0), RIGHT: (1, 0)}
        if self.dead:
            self.dead += dt
            if self.dead > 0.8 and (inp.pressed & (set(want) | {"Play"})):
                self.reset()
            return
        for name in inp.pressed:
            d = want.get(name)
            if d and (d[0] != -self.dir[0] or d[1] != -self.dir[1]):
                self.next_dir = d
        if "Play" in inp.pressed:
            self.reset()
            return
        self.acc += dt
        step = 1 / (9 + len(self.body) / 3)
        while self.acc >= step and not self.dead:
            self.acc -= step
            self.dir = self.next_dir
            hx, hy = self.body[0]
            head = (hx + self.dir[0], hy + self.dir[1])
            if not (0 <= head[0] < self.COLS and 0 <= head[1] < self.ROWS) or head in self.body[:-1]:
                self.dead = 0.001
                self.best = max(self.best, self.score)
                return
            self.body.insert(0, head)
            if head == self.food:
                self.score += 1
                self.food = self.place_food()
            else:
                self.body.pop()

    def draw(self, c):
        c.clear()
        c.fill(0, 0, WW, 7)
        c.text(2, 0, "SNAKE   SCORE %d   BEST %d" % (self.score, self.best), on=False)
        c.text(WW - scr.text_width("PADS 10/5/6/7 STEER  PLAY RESTART") - 2, 0, "PADS 10/5/6/7 STEER  PLAY RESTART", on=False)
        c.rect(0, self.TOP - 1, WW, HH - self.TOP + 1)
        for i, (x, y) in enumerate(self.body):
            px, py = x * self.CELL, self.TOP + y * self.CELL
            c.fill(px, py, 3, 3)
            if i == 0:
                c.fill(px, py, 4, 4)
        fx, fy = self.food
        c.rect(fx * self.CELL - 1, self.TOP + fy * self.CELL - 1, 5, 5)
        if self.dead:
            msg = "GAME OVER - HIT A PAD"
            w = scr.text_width(msg, 2)
            c.fill((WW - w) // 2 - 6, 22, w + 12, 22, False)
            c.rect((WW - w) // 2 - 6, 22, w + 12, 22)
            c.text((WW - w) // 2, 26, msg, scale=2)


# ---------------------------------------------------------------- 3D maze
MAZE = [
    "################",
    "#..#.......#...#",
    "#..#.###.#.#.#.#",
    "#....#...#...#.#",
    "####.#.#####.#.#",
    "#....#.....#.#.#",
    "#.####.###.#.#.#",
    "#.#....#...#...#",
    "#.#.####.#####.#",
    "#...#....#.....#",
    "###.#.####.###.#",
    "#...#......#...#",
    "#.#######.##.###",
    "#.......#....#E#",
    "#.#####...##...#",
    "################",
]


class Maze:
    FOV = math.radians(100)
    COLW = 2  # pixel width per ray (256 rays)

    def __init__(self):
        self.reset()
        # floor template: dithered gradient on the lower half, built once
        c = scr.Canvas(WW, HH)
        for y in range(HH // 2 + 2, HH):
            v = (y - HH / 2) / (HH / 2) * 0.35
            for x in range(WW):
                if v > BAYER[y & 3][x & 3]:
                    c.pixel(x, y)
        self.floor = bytes(c.buf)
        self.msg_t = 0.0

    def pad_leds(self):
        return DPAD_LEDS

    def reset(self):
        self.x, self.y, self.a = 1.5, 1.5, 0.0

    def wall(self, x, y):
        return MAZE[int(y)][int(x)] if 0 <= int(y) < len(MAZE) and 0 <= int(x) < len(MAZE[0]) else "#"

    def update(self, dt, inp):
        def amount(pad):
            p = inp.pads[int(pad.split()[1]) - 1]
            return 0.0 if p < 300 else 0.4 + min(1.0, p / 2500)
        turn = amount(RIGHT) - amount(LEFT)
        turn += (1 if "Transport >" in inp.held else 0) - (1 if "Transport <" in inp.held else 0)
        move = amount(UP) - amount(DOWN)
        self.a += turn * 2.2 * dt
        nx = self.x + math.cos(self.a) * move * 2.6 * dt
        ny = self.y + math.sin(self.a) * move * 2.6 * dt
        r = 0.2
        if self.wall(nx + math.copysign(r, nx - self.x), self.y) != "#":
            self.x = nx
        if self.wall(self.x, ny + math.copysign(r, ny - self.y)) != "#":
            self.y = ny
        if self.wall(self.x, self.y) == "E":
            self.msg_t = 2.5
            self.reset()
        self.msg_t = max(0.0, self.msg_t - dt)
        if "Play" in inp.pressed:
            self.reset()

    def draw(self, c):
        buf, rb = c.buf, c.rb
        buf[:] = self.floor
        half = HH // 2
        for col in range(0, WW, self.COLW):
            ra = self.a + (col / WW - 0.5) * self.FOV
            dx, dy = math.cos(ra), math.sin(ra)
            mx, my = int(self.x), int(self.y)
            ddx = abs(1 / dx) if dx else 1e30
            ddy = abs(1 / dy) if dy else 1e30
            sx, sdx = (-1, (self.x - mx) * ddx) if dx < 0 else (1, (mx + 1 - self.x) * ddx)
            sy, sdy = (-1, (self.y - my) * ddy) if dy < 0 else (1, (my + 1 - self.y) * ddy)
            side, cell = 0, "#"
            for _ in range(64):
                if sdx < sdy:
                    sdx += ddx; mx += sx; side = 0
                else:
                    sdy += ddy; my += sy; side = 1
                cell = self.wall(mx, my)
                if cell != ".":
                    break
            dist = (sdx - ddx) if side == 0 else (sdy - ddy)
            dist *= math.cos(ra - self.a)  # no fish-eye
            dist = max(dist, 0.05)
            h = min(HH, int(56 / dist))
            top, bot = half - h // 2, half + h // 2
            bright = max(0.06, min(1.0, 1.15 - dist / 7)) * (0.62 if side else 1.0)
            exit_wall = cell == "E"
            for x in range(col, col + self.COLW):
                bi, m = x >> 3, 0x80 >> (x & 7)
                nm = ~m & 0xFF
                for y in range(max(0, top), min(HH, bot)):
                    i = y * rb + bi
                    lit = bright > BAYER[y & 3][x & 3]
                    if exit_wall:
                        lit = ((y >> 2) + (x >> 2)) & 1 == 0  # checkerboard exit door
                    if lit or y == top or y == bot - 1:
                        buf[i] |= m
                    else:
                        buf[i] &= nm
        # minimap (top-left), 2 px per cell
        c.fill(0, 0, 34, 34, False)
        for yy, row in enumerate(MAZE):
            for xx, ch in enumerate(row):
                if ch == "#":
                    c.fill(1 + xx * 2, 1 + yy * 2, 2, 2)
        c.fill(1 + int(self.x * 2), 1 + int(self.y * 2), 1, 1, False)
        c.pixel(1 + int(self.x * 2 + math.cos(self.a) * 2), 1 + int(self.y * 2 + math.sin(self.a) * 2))
        c.rect(0, 0, 34, 34)
        if self.msg_t:
            msg = "EXIT FOUND!"
            w = scr.text_width(msg, 3)
            c.fill((WW - w) // 2 - 6, 16, w + 12, 32, False)
            c.text((WW - w) // 2, 22, msg, scale=3)


# ---------------------------------------------------------------- knobs
class KnobTracker:
    """Turns absolute, wrapping knob values into deltas.

    knobs = KnobTracker()            # in __init__
    d = knobs.deltas(inp)            # in update(): {"Knob 1": +12, ...} only for knobs that moved
    The 8 knobs wrap at 1000, the master encoder ("Master encoder (turn)") at 16.
    """

    def __init__(self):
        self.last = {}

    def deltas(self, inp):
        out = {}
        for name, v in inp.knobs.items():
            span = 16 if name.startswith("Master") else 1000
            p = self.last.get(name)
            self.last[name] = v
            if p is None or v == p:
                continue
            d = v - p
            if d > span // 2:
                d -= span
            elif d < -span // 2:
                d += span
            out[name] = d
        return out


# ---------------------------------------------------------------- splash
def _italic_points(s, scale, y, width, shear=3):
    tmp = scr.Canvas(WW, HH)
    x0 = (width - scr.text_width(s, scale)) // 2 - (7 * scale) // (2 * shear)
    tmp.text(x0, y, s, scale=scale)
    bottom = y + 7 * scale
    pts, rb = set(), tmp.rb
    for yy in range(HH):
        for xx in range(width):
            if tmp.buf[yy * rb + (xx >> 3)] & (0x80 >> (xx & 7)):
                pts.add((xx + (bottom - yy) // shear, yy))
    return pts


class _Logo:
    """Pre-rendered chrome logo as point lists (cheap to move, dissolve and shine)."""

    def __init__(self, width, big_scale, big_y, small_scale, small_y, line_y):
        letters = _italic_points("ReadyMedia.no", big_scale, big_y, width)
        small = _italic_points("Made by", small_scale, small_y, width, shear=4)
        h = 7 * big_scale
        dil = {(x + dx, y + dy) for (x, y) in letters for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
        lit = set(dil - letters) | small                                  # outline + small text
        for (x, y) in letters:                                            # gradient fill + chrome cut
            v = 1.0 - (y - big_y) / h * 0.75
            if (y - big_y) not in (h * 3 // 7, h * 3 // 7 + 1) and v > BAYER[y & 3][x & 3]:
                lit.add((x, y))
        for x in range(width // 16, width - width // 16):                 # swash underline
            lit.add((x, int(line_y + 1.5 * math.sin(x / 40))))
        rnd = random.Random(5)
        self.pts = [(x, y, rnd.random()) for (x, y) in lit if 0 <= y < HH]   # (x, y, dissolve rank)
        self.glint = sorted(dil | small, key=lambda p: p[0] + p[1] // 2)
        self.edge = sorted(dil - letters)


class Splash:
    """'Made by ReadyMedia.no', animated: slides in on the left screen, glides over to the
    right screen, dissolves into a big logo across both, shines and sparkles, fades, loops.
    Pads add sparkles."""

    LOOP = 14.0

    def __init__(self):
        self.t = 0.0
        self.small = _Logo(256, 3, 22, 1, 9, 50)
        self.big = _Logo(WW, 5, 22, 2, 3, 61)
        self.sparks = []

    def pad_leds(self):
        return None

    def _state(self):
        """(logo, x offset, alpha) for the current point of the timeline."""
        t = self.t % self.LOOP
        ease = lambda u: 0.5 - 0.5 * math.cos(math.pi * max(0.0, min(1.0, u)))
        if t < 0.9:
            return [(self.small, int(-256 + 256 * ease(t / 0.9)), 1.0)]
        if t < 3.5:
            return [(self.small, 0, 1.0)]
        if t < 4.6:
            return [(self.small, int(256 * ease((t - 3.5) / 1.1)), 1.0)]
        if t < 7.0:
            return [(self.small, 256, 1.0)]
        if t < 8.2:
            u = (t - 7.0) / 1.2
            return [(self.small, 256, 1.0 - u), (self.big, 0, u)]
        if t < 12.6:
            return [(self.big, 0, 1.0)]
        if t < 13.6:
            return [(self.big, 0, 1.0 - (t - 12.6))]
        return []

    def update(self, dt, inp):
        self.t += dt
        layers = self._state()
        if layers:
            logo, ox, a = layers[-1]
            n = 6 if logo is self.big else 3
            if a > 0.5 and random.random() < dt * n and logo.edge:
                x, y = random.choice(logo.edge)
                self.sparks.append([x + ox, y, 0.0])
            for name in inp.pressed:
                if name.startswith("Pad ") and logo.edge:
                    for _ in range(6):
                        x, y = random.choice(logo.edge)
                        self.sparks.append([x + ox, y, 0.0])
        for s in self.sparks:
            s[2] += dt
        self.sparks = [s for s in self.sparks if s[2] < 0.6]

    def draw(self, c):
        c.clear()
        buf, rb = c.buf, c.rb
        for logo, ox, alpha in self._state():
            for x, y, rank in logo.pts:
                x += ox
                if rank < alpha and 0 <= x < WW:
                    buf[y * rb + (x >> 3)] |= 0x80 >> (x & 7)
            if alpha >= 1.0:                                  # shine band sweeps across
                w = 256 if logo is self.small else WW
                pos = (self.t * 220) % (w + 200) - 60
                for (x, y) in logo.glint:
                    k = x + y // 2 - pos
                    if 0 <= k < 7:
                        x += ox
                        if 0 <= x < WW and 0 <= y < HH:
                            buf[y * rb + (x >> 3)] ^= 0x80 >> (x & 7)
        for x, y, age in self.sparks:                         # 4-point twinkle
            r = 1 + int(3 * math.sin(age / 0.6 * math.pi))
            for d in range(-r, r + 1):
                c.pixel(x + d, y)
                c.pixel(x, y + d)


# ---------------------------------------------------------------- Star Swarm (fixed shooter)
SHIP = ["##......", ".###....", ".######.", "########", ".######.", ".###....", "##......"]
ALIENS = [
    (["..#.#..", ".#####.", "##.#.##", "#######", ".#...#.", "#.....#"],
     ["..#.#..", ".#####.", "##.#.##", "#######", ".#...#.", ".#...#."]),
    (["#.....#", ".#####.", "##.#.##", "#######", "..#.#..", ".#...#."],
     [".#...#.", ".#####.", "##.#.##", "#######", "..#.#..", "#.....#"]),
    (["...#...", "..###..", ".#.#.#.", "#######", "#.#.#.#", ".#...#."],
     ["...#...", "..###..", ".#.#.#.", "#######", ".#.#.#.", "#.....#"]),
]
FIRE_PADS = {"Pad 4", "Pad 8", "Pad 12", "Pad 16"}          # right-hand pad column
SHOOTER_LEDS = {10: (0, 90, 127), 6: (0, 90, 127), 5: (0, 90, 127), 7: (0, 90, 127),
                4: (127, 0, 0), 8: (127, 0, 0), 12: (127, 0, 0), 16: (127, 0, 0)}
SHIP_X_MAX = 240.0                                            # the ship may fly over the whole left screen


class StarSwarm:
    """Original horizontal shooter: a swaying alien formation on the right, aliens peel off
    and swoop at you. D-pad (pads 10/5/6/7) flies up/back/down/forward anywhere on the left
    screen, Knob 1 = up/down and Knob 2 = back/forward as paddles, right-column pads fire."""

    def __init__(self):
        self.best = 0
        rnd = random.Random(3)
        self.stars = [[rnd.uniform(0, WW), rnd.randrange(HH), rnd.choice((1, 2, 3))] for _ in range(50)]
        self.reset()

    def pad_leds(self):
        return SHOOTER_LEDS

    def reset(self):
        self.score, self.lives, self.wave = 0, 3, 0
        self.x, self.y = 2.0, 30.0
        self.shots, self.bombs, self.booms = [], [], []
        self.dead_t, self.over, self.cool = 0.0, False, 0.0
        self.knobs = KnobTracker()
        self.new_wave()

    def new_wave(self):
        self.wave += 1
        self.t = 0.0
        self.dive_t = 1.5
        self.aliens = []
        for col in range(6):
            for row in range(5):
                kind = 2 if col == 5 else (1 if col >= 3 else 0)
                hx, hy = 330 + col * 26, 4 + row * 11
                self.aliens.append({"hx": hx, "hy": hy, "x": hx + 200 + col * 30, "y": hy, "s": "enter",
                                    "k": kind, "ph": random.random() * 6, "fired": False})

    def update(self, dt, inp):
        self.t += dt
        for s in self.stars:
            s[0] -= s[2] * 20 * dt
            if s[0] < 0:
                s[0] += WW
        if self.over:
            if inp.pressed & (FIRE_PADS | {"Play"}) and self.dead_t > 1.0:
                self.reset()
            self.dead_t += dt
            return
        if "Play" in inp.pressed:
            self.reset()
            return
        # move: d-pad pads (pressure = speed), Knob 1 = up/down and Knob 2 = back/forward paddles
        def pr(n):
            p = inp.pads[n - 1]
            return 0.0 if p < 300 else 0.5 + min(1.0, p / 2500)
        self.y += (pr(6) - pr(10)) * 70 * dt
        self.x += (pr(7) - pr(5)) * 90 * dt
        kd = self.knobs.deltas(inp)
        self.y += kd.get("Knob 1", 0) * 0.15
        self.x += kd.get("Knob 2", 0) * 0.4
        self.y = max(0.0, min(HH - 7.0, self.y))
        self.x = max(0.0, min(SHIP_X_MAX, self.x))
        # fire
        self.cool -= dt
        firing = (inp.pressed & FIRE_PADS) or (inp.held & FIRE_PADS and self.cool <= 0)
        if firing and len(self.shots) < 3 and self.dead_t <= 0:
            self.shots.append([self.x + 12, self.y + 3])
            self.cool = 0.22
        for s in self.shots:
            s[0] += 340 * dt
        self.shots = [s for s in self.shots if s[0] < WW]
        # formation and dives
        sway_x, sway_y = 6 * math.sin(self.t * 0.7), 3 * math.sin(self.t * 1.6)
        self.dive_t -= dt
        if self.dive_t <= 0:
            self.dive_t = max(0.45, 1.8 - 0.15 * self.wave) * random.uniform(0.6, 1.3)
            idle = [a for a in self.aliens if a["s"] == "form"]
            for a in random.sample(idle, min(len(idle), 1 + (self.wave > 2))):
                a.update(s="dive", x0=a["x"], y0=a["y"], ty=self.y, fired=False)
        speed = 70 + 12 * self.wave
        for a in self.aliens:
            if a["s"] in ("form", "enter"):
                tx, ty = a["hx"] + sway_x, a["hy"] + sway_y
                if a["s"] == "enter":
                    a["x"] += (tx - a["x"]) * min(1.0, 3 * dt)
                    a["y"] += (ty - a["y"]) * min(1.0, 3 * dt)
                    if abs(a["x"] - tx) < 1:
                        a["s"] = "form"
                else:
                    a["x"], a["y"] = tx, ty
            elif a["s"] == "dive":
                a["x"] -= speed * dt
                prog = max(0.0, min(1.0, (a["x0"] - a["x"]) / max(1.0, a["x0"])))
                a["y"] = a["y0"] + (a["ty"] - a["y0"]) * prog + 12 * math.sin(a["ph"] + a["x"] / 25)
                if not a["fired"] and a["x"] < 300 and random.random() < dt * 2.5:
                    self.bombs.append([a["x"], a["y"] + 3])
                    a["fired"] = True
                if a["x"] < -10:
                    a.update(s="enter", x=WW + 10, y=a["hy"])
        for b in self.bombs:
            b[0] -= (130 + 10 * self.wave) * dt
        self.bombs = [b for b in self.bombs if b[0] > -2]
        # hits
        for s in self.shots[:]:
            for a in self.aliens:
                if a["x"] - 1 <= s[0] <= a["x"] + 7 and a["y"] - 1 <= s[1] <= a["y"] + 6:
                    self.aliens.remove(a)
                    if s in self.shots:
                        self.shots.remove(s)
                    self.score += 30 if a["s"] == "dive" else 10
                    self.booms.append([a["x"] + 3, a["y"] + 3, 0.0])
                    break
        if self.dead_t > 0:
            self.dead_t -= dt
        else:
            sx = self.x
            hit = any(sx - 1 <= b[0] <= sx + 8 and self.y - 1 <= b[1] <= self.y + 7 for b in self.bombs)
            hit = hit or any(a["x"] - 7 < sx + 8 and sx < a["x"] + 7 and abs(a["y"] - self.y) < 6 for a in self.aliens)
            if hit:
                self.lives -= 1
                self.booms.append([self.x + 4, self.y + 3, 0.0])
                self.bombs = []
                self.dead_t = 2.0
                if self.lives <= 0:
                    self.over, self.dead_t = True, 0.0
                    self.best = max(self.best, self.score)
        for bm in self.booms:
            bm[2] += dt
        self.booms = [bm for bm in self.booms if bm[2] < 0.5]
        if not self.aliens:
            self.new_wave()

    def draw(self, c):
        c.clear()
        buf, rb = c.buf, c.rb
        for x, y, _ in self.stars:
            set_px(buf, rb, int(x), y)
        frame = int(self.t * 3) & 1
        for a in self.aliens:
            c.blit(int(a["x"]), int(a["y"]), ALIENS[a["k"]][frame])
        if not self.over and (self.dead_t <= 0 or int(self.t * 10) & 1):
            c.blit(int(self.x), int(self.y), SHIP)
        for x, y in self.shots:
            c.hline(int(x), int(x) + 4, int(y))
        for x, y in self.bombs:
            c.fill(int(x), int(y), 2, 2)
        for x, y, age in self.booms:
            r = int(2 + age * 16)
            for i in range(8):
                a = i * math.pi / 4 + age * 3
                c.pixel(int(x + r * math.cos(a)), int(y + r * math.sin(a)))
        c.text(150, 0, "SCORE %d" % self.score)
        c.text(230, 0, "WAVE %d" % self.wave)
        for i in range(max(0, self.lives - 1)):
            c.blit(20 + i * 10, HH - 7, SHIP)
        if self.over:
            msg = "GAME OVER  BEST %d" % self.best
            w = scr.text_width(msg, 2)
            c.fill((WW - w) // 2 - 6, 20, w + 12, 24, False)
            c.rect((WW - w) // 2 - 6, 20, w + 12, 24)
            c.text((WW - w) // 2, 25, msg, scale=2)


# ---------------------------------------------------------------- Munch Maze (maze chase)
_HALF = [
    "################################",
    "#o..............................",
    "#.###.#.#.#####.#.#.#####.####..",
    "#.#...#.#...#.....#.....#.......",
    "..#.#.#.###.#.###.#####.#.####..",
    "#...#.......#...#.......#.......",
    "#o#...#####...#...#####...####..",
    "################################",
]
CHASE_MAP = [r + r[::-1] for r in _HALF]   # 64 x 8 tiles, 8 px each, row 4 wraps around
MUNCHER = [
    ["..###..", ".#####.", "##.#.##", "#######", "#.###.#", ".#...#.", "..###.."],
    ["..###..", ".#####.", "##.#.##", "#######", "#######", ".#.#.#.", "..###.."],
]
HUNTER = ["...#...", "..###..", ".#####.", "##.#.##", "#######", "#.###.#", "#.#.#.#"]
SCARED = ["...#...", "..#.#..", ".#...#.", "#.#.#.#", "#.....#", "#.#.#.#", "#.#.#.#"]
DIRS = {(1, 0), (-1, 0), (0, 1), (0, -1)}


class _Actor:
    def __init__(self, x, y, d=(0, 0)):
        self.x, self.y, self.d = float(x), float(y), d
        self.home = (x, y)


class MunchMaze:
    """Original maze chase: eat every dot, avoid the hunters, big dots make them scared and
    edible for a while. Pads 10/5/6/7 steer, Play restarts."""

    W, H, T = 64, 8, 8

    def __init__(self):
        self.best = 0
        c = scr.Canvas(WW, HH)
        for ty in range(self.H):
            for tx in range(self.W):
                if CHASE_MAP[ty][tx] != "#":
                    continue
                x0, y0 = tx * self.T, ty * self.T
                for (dx, dy) in DIRS:  # draw a line on wall edges that face a corridor
                    ny, nx = ty + dy, (tx + dx) % self.W
                    if 0 <= ny < self.H and CHASE_MAP[ny][nx] != "#":
                        if dx == 1:
                            c.vline(x0 + 5, y0, y0 + 7)
                        elif dx == -1:
                            c.vline(x0 + 2, y0, y0 + 7)
                        elif dy == 1:
                            c.hline(x0, x0 + 7, y0 + 5)
                        else:
                            c.hline(x0, x0 + 7, y0 + 2)
        self.walls = bytes(c.buf)
        self.reset()

    def pad_leds(self):
        return DPAD_LEDS

    def reset(self):
        self.score, self.lives, self.level = 0, 3, 0
        self.over = False
        self.next_level()

    def next_level(self):
        self.level += 1
        self.dots = {(x, y) for y in range(self.H) for x in range(self.W) if CHASE_MAP[y][x] == "."}
        self.power = {(x, y) for y in range(self.H) for x in range(self.W) if CHASE_MAP[y][x] == "o"}
        self.restart_round()

    def restart_round(self):
        self.p = _Actor(31, 5, (0, 0))
        self.want = (0, 0)
        self.hunters = [_Actor(5, 3, (0, -1)), _Actor(58, 3, (0, -1)), _Actor(20, 1, (1, 0))]
        for h in self.hunters:
            h.scared = 0.0
        self.t = 0.0
        self.pause = 1.2
        self.dying = 0.0

    def open(self, tx, ty):
        return 0 <= ty < self.H and CHASE_MAP[ty][tx % self.W] != "#"

    def _move(self, a, speed, dt, choose):
        left = speed * dt
        for _ in range(4):
            if left <= 0:
                break
            if a.d == (0, 0):
                choose(a)
                if a.d == (0, 0):
                    break
            dx, dy = a.d
            cur = a.x if dx else a.y
            step = dx or dy
            nxt = (math.floor(cur + 1e-6) + 1) if step > 0 else (math.ceil(cur - 1e-6) - 1)
            dist = abs(nxt - cur)
            if left >= dist:
                if dx:
                    a.x = float(nxt)
                else:
                    a.y = float(nxt)
                left -= dist
                a.x %= self.W
                choose(a)
            else:
                if dx:
                    a.x = (a.x + step * left) % self.W
                else:
                    a.y += step * left
                left = 0

    def _choose_player(self, a):
        tx, ty = int(round(a.x)) % self.W, int(round(a.y))
        if self.want != (0, 0) and self.open(tx + self.want[0], ty + self.want[1]):
            a.d = self.want
        elif not self.open(tx + a.d[0], ty + a.d[1]):
            a.d = (0, 0)

    def _choose_hunter(self, h, i):
        tx, ty = int(round(h.x)) % self.W, int(round(h.y))
        opts = [d for d in DIRS if self.open(tx + d[0], ty + d[1]) and d != (-h.d[0], -h.d[1])]
        if not opts:
            opts = [(-h.d[0], -h.d[1])]
        px, py = self.p.x, self.p.y
        if i == 1:  # aims ahead of the player
            px, py = px + 4 * self.p.d[0], py + 4 * self.p.d[1]
        dist = lambda d: abs((tx + d[0]) - px) + abs((ty + d[1]) - py) * 2
        if h.scared > 0:
            h.d = max(opts, key=dist)
        elif i == 2 and random.random() < 0.4:
            h.d = random.choice(opts)
        else:
            h.d = min(opts, key=dist)

    def update(self, dt, inp):
        self.t += dt
        if self.over:
            if inp.pressed & ({UP, DOWN, LEFT, RIGHT, "Play"}):
                self.reset()
            return
        if "Play" in inp.pressed:
            self.reset()
            return
        for name, d in ((UP, (0, -1)), (DOWN, (0, 1)), (LEFT, (-1, 0)), (RIGHT, (1, 0))):
            if name in inp.pressed or name in inp.held:
                self.want = d
                if d == (-self.p.d[0], -self.p.d[1]):
                    self.p.d = d  # instant reverse
        if self.pause > 0:
            self.pause -= dt
            return
        if self.dying:
            self.dying -= dt
            if self.dying <= 0:
                self.dying = 0.0
                if self.lives <= 0:
                    self.over = True
                    self.best = max(self.best, self.score)
                else:
                    self.restart_round()
            return
        lv = 1 + 0.08 * (self.level - 1)
        self._move(self.p, 6.0 * lv, dt, self._choose_player)
        tile = (int(round(self.p.x)) % self.W, int(round(self.p.y)))
        if tile in self.dots:
            self.dots.discard(tile)
            self.score += 10
        if tile in self.power:
            self.power.discard(tile)
            self.score += 50
            for h in self.hunters:
                h.scared = 6.0
                h.d = (-h.d[0], -h.d[1])
        for i, h in enumerate(self.hunters):
            h.scared = max(0.0, h.scared - dt)
            sp = (2.8 if h.scared else 5.2) * lv
            self._move(h, sp, dt, lambda a, i=i: self._choose_hunter(a, i))
            dx = min(abs(h.x - self.p.x), self.W - abs(h.x - self.p.x))
            if dx < 0.7 and abs(h.y - self.p.y) < 0.7:
                if h.scared:
                    self.score += 200
                    h.x, h.y = float(h.home[0]), float(h.home[1])
                    h.scared = 0.0
                else:
                    self.lives -= 1
                    self.dying = 1.5
                    return
        if not self.dots and not self.power:
            self.next_level()

    def draw(self, c):
        buf, rb = c.buf, c.rb
        buf[:] = self.walls
        T = self.T
        for x, y in self.dots:
            c.fill(x * T + 3, y * T + 3, 2, 2)
        if int(self.t * 4) & 1:
            for x, y in self.power:
                c.fill(x * T + 2, y * T + 2, 4, 4)
        if not (self.dying and int(self.t * 12) & 1):
            frame = int(self.t * 8) & 1 if self.p.d != (0, 0) else 0
            c.blit(int(self.p.x * T), int(self.p.y * T), MUNCHER[frame])
        for h in self.hunters:
            spr = SCARED if h.scared and (h.scared > 1.5 or int(self.t * 8) & 1) else HUNTER
            c.blit(int(h.x * T), int(h.y * T), spr)
        c.fill(0, 0, WW, 7)
        c.text(2, 0, "MUNCH MAZE  SCORE %d  LIVES %d  LEVEL %d" % (self.score, self.lives, self.level), on=False)
        c.text(WW - scr.text_width("BEST %d" % self.best) - 2, 0, "BEST %d" % self.best, on=False)
        if self.over or self.pause > 0:
            msg = "GAME OVER - HIT A PAD" if self.over else "READY!"
            w = scr.text_width(msg, 2)
            c.fill((WW - w) // 2 - 6, 22, w + 12, 22, False)
            c.rect((WW - w) // 2 - 6, 22, w + 12, 22)
            c.text((WW - w) // 2, 26, msg, scale=2)


# ---------------------------------------------------------------- Sky Shield (missile defence)
# Turret x positions = where the 8 physical knobs sit under the two screens.
TURRET_X = [10, 87, 163, 240, 266, 343, 419, 496]
CITY_X = [48, 125, 201, 304, 381, 457]           # between the turrets
CITY = ["..#....", ".##.#..", ".####.#", "#######", "#######"]
RUBBLE = [".......", ".......", ".......", "..#.#..", "#######"]
DOME = ["..###..", ".#####.", "#######"]
SHIELD_LEDS = {n: (90, 70, 0) for n in range(1, 9)}
SHIELD_LEDS.update({n: (127, 0, 0) for n in (13, 14, 15, 16)})
GROUND = HH - 1


class SkyShield:
    """Original missile-defence game. Eight turrets sit right under the eight knobs:
    each knob aims its own turret. Pads 1-8 fire turret 1-8; the top-row pads
    (13-16) fire the turret you touched last. Hold Shift and turn a knob to set how far
    that turret's shell flies; a crosshair shows where it will burst (always on screen).
    Shells burst into a blast that destroys incoming missiles (and chains).
    Protect the six cities. Play restarts."""

    def __init__(self):
        self.best = 0
        self.knobs = KnobTracker()
        self.reset()

    def pad_leds(self):
        return SHIELD_LEDS

    def reset(self):
        self.score, self.wave = 0, 0
        self.cities = [True] * 6
        self.angle = [0.0] * 8              # degrees, 0 = straight up, - = left
        self.range = [30.0] * 8             # px from the turret to the burst point (Shift + knob)
        self.active = 0
        self.aim_t = 0.0
        self.over = False
        self.next_wave()

    def next_wave(self):
        self.wave += 1
        self.t = 0.0
        self.ammo = [6] * 8
        self.alive = [True] * 8
        self.shells, self.blasts, self.missiles = [], [], []
        self.to_spawn = 8 + 4 * self.wave
        self.spawn_t = 1.5
        self.banner = 2.0

    def _targets(self):
        t = [(CITY_X[i] + 3, GROUND - 4) for i in range(6) if self.cities[i]]
        t += [(TURRET_X[i], GROUND - 3) for i in range(8) if self.alive[i]]
        return t

    def _spawn(self, x=None, y=0.0):
        tg = self._targets()
        if not tg:
            return
        tx, ty = random.choice(tg)
        if x is None:  # start above the target so the paths stay steep on this wide, low screen
            x = max(4.0, min(WW - 4.0, tx + random.uniform(-90, 90)))
        speed = 9 + 2.5 * self.wave + random.uniform(0, 4)
        d = math.hypot(tx - x, ty - y)
        self.missiles.append({"x0": x, "y0": y, "x": x, "y": y, "vx": (tx - x) / d * speed,
                              "vy": (ty - y) / d * speed, "split": self.wave >= 3 and random.random() < 0.25})

    def burst_point(self, i):
        """Where turret i's shell will burst: angle + range, clamped to stay on screen."""
        a = math.radians(self.angle[i])
        x0, y0 = TURRET_X[i], GROUND - 4
        r = self.range[i]
        if math.cos(a) > 0.01:
            r = min(r, (y0 - 10) / math.cos(a))          # never above y = 10 (under the score line)
        tx = x0 + math.sin(a) * r
        if tx < 2 or tx > WW - 3:                         # nor off the sides
            r = min(r, abs(((2 if tx < 2 else WW - 3) - x0) / (math.sin(a) or 1e-6)))
        return x0 + math.sin(a) * r, y0 - math.cos(a) * r, r

    def _fire(self, i, pressure=0):
        if self.over or not self.alive[i] or self.ammo[i] <= 0:
            return
        self.ammo[i] -= 1
        a = math.radians(self.angle[i])
        tx, ty, rng = self.burst_point(i)
        x0, y0 = TURRET_X[i], GROUND - 4
        self.shells.append({"x": x0, "y": y0, "tx": tx, "ty": ty,
                            "vx": math.sin(a), "vy": -math.cos(a), "left": rng})

    def update(self, dt, inp):
        self.t += dt
        self.banner = max(0.0, self.banner - dt)
        if "Play" in inp.pressed or (self.over and inp.pressed and self.t > 2):
            self.reset()
            return
        if self.over:
            return
        for name, d in self.knobs.deltas(inp).items():  # each knob aims its own turret
            if name.startswith("Knob "):
                i = int(name.split()[1]) - 1
                if "Shift" in inp.held:                   # Shift + knob = range
                    self.range[i] = max(8.0, min(90.0, self.range[i] + d * 0.12))
                else:                                     # knob = angle
                    self.angle[i] = max(-80.0, min(80.0, self.angle[i] + d * 0.6))
                self.active, self.aim_t = i, 1.5
        self.aim_t = max(0.0, self.aim_t - dt)
        for name in inp.pressed:
            if not name.startswith("Pad "):
                continue
            n = int(name.split()[1])
            p = max(inp.pads[n - 1], 600)
            if 1 <= n <= 8:
                self._fire(n - 1, p)
                self.active = n - 1
            elif n >= 13:
                self._fire(self.active, p)
        # incoming
        self.spawn_t -= dt
        if self.to_spawn > 0 and self.spawn_t <= 0:
            for _ in range(min(self.to_spawn, 1 + self.wave // 3)):
                self._spawn()
                self.to_spawn -= 1
            self.spawn_t = max(0.6, 2.4 - 0.2 * self.wave) * random.uniform(0.6, 1.4)
        for m in self.missiles[:]:
            m["x"] += m["vx"] * dt
            m["y"] += m["vy"] * dt
            if m["split"] and m["y"] > 18:
                m["split"] = False
                for _ in range(2):
                    self._spawn(m["x"], m["y"])
            if m["y"] >= GROUND - 4:
                self.missiles.remove(m)
                self.blasts.append({"x": m["x"], "y": GROUND - 3, "t": 0.0, "r": 8, "enemy": True})
                for i, cx in enumerate(CITY_X):
                    if abs(m["x"] - (cx + 3)) < 6:
                        self.cities[i] = False
                for i, tx in enumerate(TURRET_X):
                    if abs(m["x"] - tx) < 5:
                        self.alive[i] = False
        for s in self.shells[:]:
            step = 110 * dt
            s["x"] += s["vx"] * step
            s["y"] += s["vy"] * step
            s["left"] -= step
            if s["left"] <= 0 or s["y"] < 1:
                self.shells.remove(s)
                self.blasts.append({"x": s["x"], "y": s["y"], "t": 0.0, "r": 11, "enemy": False})
        for b in self.blasts:
            b["t"] += dt
            r = b["r"] * math.sin(min(1.0, b["t"] / 0.9) * math.pi)
            if b["enemy"]:
                continue
            for m in self.missiles[:]:
                if (m["x"] - b["x"]) ** 2 + (m["y"] - b["y"]) ** 2 < r * r:
                    self.missiles.remove(m)
                    self.score += 25 * self.wave
                    self.blasts.append({"x": m["x"], "y": m["y"], "t": 0.0, "r": 7, "enemy": False})  # chain
        self.blasts = [b for b in self.blasts if b["t"] < 0.9]
        if not any(self.cities):
            self.over = True
            self.t = 0.0
            self.best = max(self.best, self.score)
        elif self.to_spawn <= 0 and not self.missiles and not self.blasts and not self.shells:
            self.score += 100 * sum(self.cities) + 5 * sum(self.ammo)
            self.next_wave()

    def draw(self, c):
        c.clear()
        buf, rb = c.buf, c.rb
        c.hline(0, WW - 1, GROUND)
        for i, cx in enumerate(CITY_X):
            c.blit(cx, GROUND - 5, CITY if self.cities[i] else RUBBLE)
        for i, tx in enumerate(TURRET_X):
            if not self.alive[i]:
                c.blit(tx - 3, GROUND - 3, RUBBLE[2:])
                continue
            c.blit(tx - 3, GROUND - 3, DOME)
            a = math.radians(self.angle[i])
            for k in range(2, 8):                              # barrel
                c.pixel(int(tx + math.sin(a) * k), int(GROUND - 3 - math.cos(a) * k))
            for k in range(min(self.ammo[i], 6)):              # ammo dots in the ground line
                c.pixel(tx - 5 + k * 2, GROUND, False)
            bx, by, br = self.burst_point(i)
            bx, by = int(bx), int(by)
            if i == self.active:
                c.hline(bx - 3, bx - 1, by)                  # crosshair at the burst point
                c.hline(bx + 1, bx + 3, by)
                c.vline(bx, by - 3, by - 1)
                c.vline(bx, by + 1, by + 3)
                if self.aim_t > 0:                            # dotted guide while aiming
                    for k in range(10, int(br) - 3, 4):
                        c.pixel(int(tx + math.sin(a) * k), int(GROUND - 4 - math.cos(a) * k))
            else:
                c.pixel(bx - 1, by)                           # small mark for the other turrets
                c.pixel(bx + 1, by)
        for m in self.missiles:                                # trail from launch point
            n = int(max(abs(m["x"] - m["x0"]), abs(m["y"] - m["y0"]))) or 1
            for k in range(0, n, 2):
                set_px(buf, rb, int(m["x0"] + (m["x"] - m["x0"]) * k / n), int(m["y0"] + (m["y"] - m["y0"]) * k / n))
            c.fill(int(m["x"]) - 1, int(m["y"]) - 1, 2, 2)
        for s in self.shells:
            c.fill(int(s["x"]), int(s["y"]), 2, 2)
            c.pixel(int(s["tx"]) - 1, int(s["ty"]))
            c.pixel(int(s["tx"]) + 1, int(s["ty"]))
        for b in self.blasts:                                  # dithered disc + ring
            r = b["r"] * math.sin(min(1.0, b["t"] / 0.9) * math.pi)
            ri = int(r)
            for yy in range(-ri, ri + 1):
                for xx in range(-ri, ri + 1):
                    if xx * xx + yy * yy <= r * r:
                        x, y = int(b["x"]) + xx, int(b["y"]) + yy
                        if 0 <= y < HH and 0.55 > BAYER[y & 3][x & 3]:
                            set_px(buf, rb, x, y)
            circle(c, b["x"], b["y"], r)
        c.text(2, 0, "SKY SHIELD  SCORE %d  WAVE %d" % (self.score, self.wave))
        c.text(WW - scr.text_width("BEST %d" % self.best) - 2, 0, "BEST %d" % self.best)
        if self.banner or self.over:
            msg = "THE END - HIT A PAD" if self.over else "WAVE %d" % self.wave
            w = scr.text_width(msg, 2)
            c.fill((WW - w) // 2 - 6, 16, w + 12, 22, False)
            c.rect((WW - w) // 2 - 6, 16, w + 12, 22)
            c.text((WW - w) // 2, 20, msg, scale=2)


MODES = {"splash": Splash, "demo": Demo, "snake": Snake, "maze": Maze,
         "swarm": StarSwarm, "munch": MunchMaze, "shield": SkyShield}

LABELS = {"status": "STATUS", "test": "TEST PATTERN", "knobs": "KNOB PAGE", "splash": "READYMEDIA",
          "demo": "DEMO", "snake": "SNAKE", "maze": "3D MAZE", "swarm": "STAR SWARM",
          "munch": "MUNCH MAZE", "shield": "SKY SHIELD"}
MENU = ["status", "test", "knobs"] + list(MODES)


def menu_overlay(c, idx):
    """Mode picker drawn on top of whatever is on the 512 x 64 canvas (master encoder menu)."""
    n = len(MENU)
    cur, prev, nxt = LABELS[MENU[idx % n]], LABELS[MENU[(idx - 1) % n]], LABELS[MENU[(idx + 1) % n]]
    x0, w = 96, 320
    c.fill(x0, 8, w, 48, False)
    c.rect(x0, 8, w, 48)
    c.rect(x0 + 2, 10, w - 4, 44)
    c.text(x0 + 8, 13, "SHIFT+TURN: MENU  TURN: CHOOSE  PUSH: START")
    tw = scr.text_width(cur, 2)
    c.text(256 - tw // 2, 26, cur, scale=2)
    c.invert(256 - tw // 2 - 4, 24, tw + 8, 18)
    c.text(x0 + 8, 45, "< " + prev)
    c.text(x0 + w - 8 - scr.text_width(nxt + " >"), 45, nxt + " >")
