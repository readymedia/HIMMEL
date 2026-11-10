#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Check MK2 screen modes without the hardware.

    python tools/mk2_fun_check.py            run every mode 600 frames with random input
    python tools/mk2_fun_check.py munch      only one mode
    python tools/mk2_fun_check.py --png      also save a 3x PNG of the last frame per mode

Prints init time, average and worst draw+update time per frame (budget: keep the
average well under ~15 ms so USB, not Python, is the limit) and fails loudly on
any exception. PNGs go to tools/_screens/ (git-ignored).
"""

import os
import random
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mk2_fun as fun  # noqa: E402
import mk2_screens as scr  # noqa: E402

BUTTONS = ["Pad %d" % i for i in range(1, 17)] + ["Play", "Transport <", "Transport >"]


def save_png(path, c, scale=3):
    w, h = c.w * scale, c.h * scale
    rows = []
    for y in range(c.h):
        line = bytearray()
        for x in range(c.w):
            on = c.buf[y * c.rb + (x >> 3)] & (0x80 >> (x & 7))
            line += (b"\xcd\xd8\xff" if on else b"\x14\x16\x3c") * scale
        rows += [b"\x00" + bytes(line)] * scale

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def run(name, frames=600, png=False):
    rnd = random.Random(1)
    t0 = time.perf_counter()
    game = fun.MODES[name]()
    init = (time.perf_counter() - t0) * 1000
    c = scr.Canvas(fun.WW, fun.HH)
    total = worst = 0.0
    knob = 0
    for _ in range(frames):
        pressed = {rnd.choice(BUTTONS)} if rnd.random() < 0.15 else set()
        pressed.discard("Play")  # Play restarts most games; keep them running
        held = {rnd.choice(["Pad 10", "Pad 6", "Pad 5", "Pad 7", "Pad 16"])} if rnd.random() < 0.5 else set()
        pads = [0] * 16
        for n in held:
            pads[int(n.split()[1]) - 1] = 2000
        knob = (knob + rnd.randint(-20, 20)) % 1000
        t = time.perf_counter()
        game.update(1 / 30, fun.Input(held, pressed, pads, {"Knob 1": knob}))
        game.draw(c)
        c.split()
        d = (time.perf_counter() - t) * 1000
        total += d
        worst = max(worst, d)
    print("%-8s init %5.0f ms   avg %5.1f ms   worst %5.1f ms" % (name, init, total / frames, worst))
    if png:
        out = os.path.join(HERE, "_screens")
        os.makedirs(out, exist_ok=True)
        save_png(os.path.join(out, name + ".png"), c)


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")] or list(fun.MODES)
    for n in names:
        run(n, png="--png" in sys.argv)


if __name__ == "__main__":
    main()
