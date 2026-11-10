# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Classic two-deck view: the show model behind it
===============================================

Himmel's own take on the simple two-deck VJ workflow that the NuVJ controller
was made for (a nostalgia view). It has the same ideas: two decks of 16 banks x
9 cells, a crossfader, blend modes, keying, play modes, two effect parameters
per cell, a master effect with wet/dry, brightness and contrast. The look,
names and icons are Himmel's own; nothing of the original software's art or
code is used.

There is no render engine here: the page draws simple placeholder previews.
Later the same model drives Himmel's engine (decks, effect chain, mixer).

Played two ways, both changing the same model:
- the page (``tools/vj_classic.html``): click, drag from the browser, sliders;
- the NuVJ controller (real or simulated): ``on_event`` gets the same events
  the NuVJ host gets (pads, BANK, PMTR, speed faders, jog, crossfader, ...).
"""

import os
import random
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIA_DIR = os.path.join(ROOT, "media")
MEDIA_EXT = (".mov", ".mp4", ".avi", ".png", ".jpg", ".jpeg", ".gif")

# Himmel's planned effect set (docs/nuvj-effects.md, "What Himmel takes from this").
# p = the two main parameters (PMTR 1 / 2); speed = parameters that keep running.
EFFECTS = [
    {"name": "Hue Shift", "cat": "Colour", "p": ["Hue", "Saturation"]},
    {"name": "Mono", "cat": "Colour", "p": ["Amount", "Tint"]},
    {"name": "Negative", "cat": "Colour", "p": ["Amount", "-"]},
    {"name": "Posterize", "cat": "Colour", "p": ["Steps", "-"]},
    {"name": "Threshold", "cat": "Colour", "p": ["Level", "-"]},
    {"name": "Soft Blur", "cat": "Blur", "p": ["Amount", "-"]},
    {"name": "Glow", "cat": "Blur", "p": ["Threshold", "Size"]},
    {"name": "Mirror", "cat": "Space", "p": ["Split X", "Split Y"]},
    {"name": "Kaleido", "cat": "Space", "p": ["Wedges", "Spin"], "speed": ["Spin"]},
    {"name": "Tiles", "cat": "Space", "p": ["Count", "Offset"]},
    {"name": "Tunnel", "cat": "Space", "p": ["Speed", "Twist"], "speed": ["Speed"]},
    {"name": "Spin Zoom", "cat": "Space", "p": ["Zoom", "Spin"], "speed": ["Zoom", "Spin"]},
    {"name": "RGB Split", "cat": "Glitch", "p": ["Distance", "Angle"]},
    {"name": "Strobe", "cat": "Glitch", "p": ["Rate", "Hard/soft"]},
    {"name": "Shake", "cat": "Glitch", "p": ["Amount", "Rate"]},
    {"name": "Trails", "cat": "Feedback", "p": ["Length", "-"]},
    {"name": "Echo Spiral", "cat": "Feedback", "p": ["Turn", "Size"]},
    {"name": "Bass Punch", "cat": "Audio", "p": ["Punch", "Release"]},
]
EFFECT_BY_NAME = {e["name"]: e for e in EFFECTS}

# Himmel's own cheap generators (placeholders drawn by the page until the engine has them).
GENERATORS = ["Wave Line", "Spectrum Bars", "Pulse Ring", "Particle Rain", "Moire Drift",
              "Beat Flash", "Colour Wash", "Grid Tunnel", "Orbit Dots", "Sixties Blobs"]
SOURCES = [{"kind": "camera", "name": "Camera %d" % i} for i in (1, 2, 3)] + \
          [{"kind": "text", "name": "Text"}]
BLENDS = ("mix", "add", "subtract", "multiply", "lighten", "darken")
MASKS = ("none", "luma", "chroma")
PLAYS = ("once", "loop", "pingpong")
BANKS, CELLS = 16, 9


def new_cell(item):
    return {"kind": item["kind"], "name": item["name"], "effect": None, "p1": 0.5, "p2": 0.5,
            "blend": "mix", "mask": "none", "key_center": 0.5, "key_width": 0.2, "play": "loop",
            "text": item.get("text", "HIMMEL") if item["kind"] == "text" else ""}


def media_files():
    out = []
    if os.path.isdir(MEDIA_DIR):
        for dp, dn, fn in os.walk(MEDIA_DIR):
            for f in sorted(fn):
                if f.lower().endswith(MEDIA_EXT):
                    out.append(os.path.relpath(os.path.join(dp, f), MEDIA_DIR).replace("\\", "/"))
    return out[:400]


class ClassicShow:
    def __init__(self):
        self.files = media_files()
        self.decks = {}
        for d in "AB":
            self.decks[d] = {"bank": 0, "playing": None, "pos": 0.0, "dir": 1, "speed": 1.0, "nudge": 0.0,
                             "latch": True, "auto": False, "sync": False, "held": None,
                             "banks": [[None] * CELLS for _ in range(BANKS)]}
        self.master = {"fx": None, "on": False, "mix": 1.0, "p1": 0.5, "p2": 0.5, "bright": 0.5, "contrast": 0.0,
                       "xf": 0.5, "black": False, "insta": None, "bpm": 120.0, "fade_speed": 0.0}
        self.rev = 0
        self.t_last = time.time()
        self.t0 = self.t_last
        self.beat_last = -1
        self.scratch = {}
        self._demo()

    def _demo(self):
        """Something to play with at once: generators, files and an effect or two in bank 1."""
        gens = list(GENERATORS)
        files = self.files or []
        a, b = self.decks["A"]["banks"][0], self.decks["B"]["banks"][0]
        for i in range(CELLS):
            g = gens[i % len(gens)]
            if i < 6 or not files:
                a[i] = new_cell({"kind": "generator", "name": g})
                b[i] = new_cell({"kind": "generator", "name": gens[(i + 5) % len(gens)]})
            else:
                a[i] = new_cell({"kind": "file", "name": files[(i - 6) % len(files)]})
                b[i] = new_cell({"kind": "file", "name": files[(i - 3) % len(files)]})
        a[4]["effect"], b[4]["effect"] = "Kaleido", "Mirror"
        b[8] = new_cell({"kind": "text", "name": "Text", "text": "HIMMEL"})

    # ---- helpers ----
    def cell(self, d, n):
        """Cell n = 1..9 (the pad number: 1-3 bottom row, 7-9 top row) of deck d's current bank."""
        dk = self.decks[d]
        return dk["banks"][dk["bank"]][n - 1] if 1 <= n <= CELLS else None

    def playing_cell(self, d):
        dk = self.decks[d]
        return self.cell(d, dk["playing"]) if dk["playing"] else None

    def changed(self):
        self.rev += 1

    def trigger(self, d, n):
        dk = self.decks[d]
        c = self.cell(d, n)
        if c is None:
            return
        if dk["playing"] == n and dk["latch"]:
            dk["playing"] = None              # latch on: hitting the playing cell again stops it
        else:
            dk["playing"], dk["pos"], dk["dir"] = n, 0.0, 1
        self.changed()

    # ---- time ----
    def tick(self):
        now = time.time()
        dt, self.t_last = min(0.2, now - self.t_last), now
        m = self.master
        beat = int((now - self.t0) * m["bpm"] / 60.0)
        for d, dk in self.decks.items():
            c = self.playing_cell(d)
            if c:
                rate = (dk["speed"] + dk["nudge"]) * dt / 8.0      # a placeholder clip is 8 s long
                dk["nudge"] *= 0.85
                dk["pos"] += rate * dk["dir"]
                if dk["pos"] >= 1 or dk["pos"] < 0:
                    if c["play"] == "once":
                        dk["pos"], dk["playing"] = 1.0, None
                        self.changed()
                    elif c["play"] == "pingpong":
                        dk["dir"] = -dk["dir"]
                        dk["pos"] = min(1.0, max(0.0, dk["pos"]))
                    else:
                        dk["pos"] %= 1.0
            if dk["auto"] and beat != self.beat_last and beat % 4 == 0:   # Auto: a new cell every 4 beats
                full = [i + 1 for i, x in enumerate(dk["banks"][dk["bank"]]) if x and i + 1 != dk["playing"]]
                if full:
                    dk["playing"], dk["pos"] = random.choice(full), 0.0
                    self.changed()
        self.beat_last = beat

    # ---- the NuVJ controller ----
    def on_event(self, name, ev, bpm=None):
        if not name:
            return
        m = self.master
        if bpm:
            m["bpm"] = bpm
        k = ev["kind"]
        d = name[0] if name[:2] in ("A ", "B ") else None
        dk = self.decks.get(d)
        if " Pad " in name and k in ("down", "up"):
            n = int(name.split()[-1])
            if k == "down":
                self.trigger(d, n)
                dk["held"] = n
            elif not dk["latch"] and dk["playing"] == n:   # latch off: plays while held
                dk["playing"], dk["held"] = None, None
                self.changed()
        elif name.endswith(" Bank") and k == "delta":
            dk["bank"] = max(0, min(BANKS - 1, dk["bank"] + (1 if ev["delta"] > 0 else -1)))
            self.changed()
        elif name.endswith(("PMTR1", "PMTR2")) and k == "value":
            key = "p1" if name.endswith("1") else "p2"
            if d:
                c = self.playing_cell(d)
                if c:
                    c[key] = ev["value"] / 127
            else:
                m[key] = ev["value"] / 127
            self.changed()
        elif name.endswith(" Speed") and k == "fader":
            dk["speed"] = round((ev["value"] / 16383 - 0.5) * 4, 2)
        elif name.endswith(" Jog") and k == "delta":
            if self.scratch.get(d):
                dk["pos"] = (dk["pos"] + ev["delta"] / 512.0) % 1.0
            else:
                dk["nudge"] = max(-2.0, min(2.0, dk["nudge"] + ev["delta"] * 0.05))
        elif name.endswith(" Scratch") and k in ("down", "up"):
            self.scratch[d] = k == "down"
        elif k == "down" and name[2:] in ("Latch", "Auto", "Sync"):
            key = name[2:].lower()
            dk[key] = not dk[key]
            self.changed()
        elif name == "Crossfader" and k == "fader":
            m["xf"] = ev["value"] / 16383
        elif name in ("Insta A", "Insta B"):
            m["insta"] = name[-1] if k == "down" else None
        elif name == "FX Level" and k == "value":
            m["mix"] = ev["value"] / 127
        elif name == "Master FX Select" and k == "delta":
            i = -1 if m["fx"] is None else [e["name"] for e in EFFECTS].index(m["fx"])
            m["fx"] = EFFECTS[(i + (1 if ev["delta"] > 0 else -1)) % len(EFFECTS)]["name"]
            self.changed()
        elif name == "Master FX" and k == "down":
            m["on"] = not m["on"]
            if m["on"] and not m["fx"]:
                m["fx"] = EFFECTS[0]["name"]
            self.changed()
        elif name == "Bright" and k == "value":
            m["bright"] = ev["value"] / 127
        elif name == "Contrast" and k == "value":
            m["contrast"] = ev["value"] / 127
        elif name == "Insta Black" and k == "down":
            m["black"] = not m["black"]
            self.changed()
        elif name == "Auto Fade Speed" and k == "value":
            m["fade_speed"] = ev["value"] / 127

    # ---- the page ----
    def command(self, c):
        op, d = c.get("op"), c.get("deck")
        dk = self.decks.get(d)
        m = self.master
        n = int(c.get("cell", 0) or 0)
        if op == "trigger" and dk:
            self.trigger(d, n)
        elif op == "stop" and dk:
            dk["playing"] = None
        elif op == "bank" and dk:
            dk["bank"] = max(0, min(BANKS - 1, int(c.get("bank", 0))))
        elif op == "assign" and dk and 1 <= n <= CELLS:
            it = c.get("item") or {}
            if it.get("kind") == "effect":
                if dk["banks"][dk["bank"]][n - 1] and it.get("name") in EFFECT_BY_NAME:
                    dk["banks"][dk["bank"]][n - 1]["effect"] = it["name"]
            elif it.get("kind") in ("file", "generator", "camera", "text") and it.get("name"):
                dk["banks"][dk["bank"]][n - 1] = new_cell({"kind": it["kind"], "name": str(it["name"])[:120]})
        elif op == "move" and dk:          # drag a cell onto another (in either deck)
            src = self.decks.get(c.get("from_deck"))
            sn = int(c.get("from_cell", 0) or 0)
            if src and 1 <= sn <= CELLS and 1 <= n <= CELLS:
                sb, db = src["banks"][src["bank"]], dk["banks"][dk["bank"]]
                if c.get("copy"):
                    db[n - 1] = dict(sb[sn - 1]) if sb[sn - 1] else None
                else:
                    db[n - 1], sb[sn - 1] = sb[sn - 1], db[n - 1]
        elif op == "clear" and dk and 1 <= n <= CELLS:
            dk["banks"][dk["bank"]][n - 1] = None
            if dk["playing"] == n:
                dk["playing"] = None
        elif op == "clear_effect" and dk:
            cl = self.cell(d, n)
            if cl:
                cl["effect"] = None
        elif op == "cell" and dk:
            cl = self.cell(d, n)
            key, v = c.get("key"), c.get("value")
            if cl and key in ("blend", "mask", "play") and v in {"blend": BLENDS, "mask": MASKS, "play": PLAYS}[key]:
                cl[key] = v
            elif cl and key in ("p1", "p2", "key_center", "key_width"):
                cl[key] = max(0.0, min(1.0, float(v)))
            elif cl and key == "text":
                cl["text"] = str(v)[:64]
            elif cl and key == "effect":
                cl["effect"] = v if v in EFFECT_BY_NAME else None
        elif op == "toggle" and dk and c.get("what") in ("latch", "auto", "sync"):
            dk[c["what"]] = not dk[c["what"]]
        elif op == "speed" and dk:
            dk["speed"] = max(-2.0, min(2.0, float(c.get("value", 1))))
        elif op == "pos" and dk:
            dk["pos"] = max(0.0, min(1.0, float(c.get("value", 0))))
        elif op == "master":
            key, v = c.get("key"), c.get("value")
            if key in ("mix", "p1", "p2", "bright", "contrast", "xf"):
                m[key] = max(0.0, min(1.0, float(v)))
            elif key == "bpm":
                m["bpm"] = round(max(40.0, min(240.0, float(v))), 1)
            elif key in ("on", "black"):
                m[key] = bool(v)
            elif key == "fx":
                m["fx"] = v if v in EFFECT_BY_NAME else None
            elif key == "insta":
                m["insta"] = v if v in ("A", "B") else None
        else:
            return
        self.changed()

    def snapshot(self):
        decks = {}
        for d, dk in self.decks.items():
            decks[d] = {k: dk[k] for k in ("bank", "playing", "pos", "speed", "latch", "auto", "sync")}
            decks[d]["cells"] = dk["banks"][dk["bank"]]
            decks[d]["used_banks"] = [i for i, b in enumerate(dk["banks"]) if any(b)]
        return {"rev": self.rev, "decks": decks, "master": dict(self.master), "t": time.time() - self.t0}

    def library(self):
        return {"files": self.files, "effects": EFFECTS, "generators": GENERATORS, "sources": SOURCES,
                "blends": BLENDS, "masks": MASKS, "plays": PLAYS}
