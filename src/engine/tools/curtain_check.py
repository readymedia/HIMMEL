#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""curtain_check.py - M6 check: kill the renderer, the outputs show black (the
supervisor's curtains), never the desktop, and the show comes back in < 2 s.

Run it against a running supervisor with something on air, e.g.

    build\\bin\\Release\\himmel.exe --no-controller --no-weapons --no-ui --no-audio -- --outputs 3 --size 640x360 --no-show-files
    build\\bin\\Release\\himmel_cmd.exe clip a 1 0
    python src\\engine\\tools\\curtain_check.py [--runs 5] [--exe-dir build\\bin\\Release]

For each run it reads the output windows from `himmel_cmd outputs`, samples 5
screen pixels in each output (GDI), kills himmel_render.exe, and then samples
every ~15 ms: the time until every output is black (the curtain; the desktop
or anything else = FAIL), and the time until every output shows the show again.
Windows only, stdlib only.
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

user32 = ctypes.windll.user32 if os.name == "nt" else None
gdi32 = ctypes.windll.gdi32 if os.name == "nt" else None


def outputs(exe_dir):
    out = subprocess.run([os.path.join(exe_dir, "himmel_cmd.exe"), "outputs"], capture_output=True, text=True).stdout
    rects = []
    for m in re.finditer(r"OUT (\d+)\s+(\S+).*?rect (-?\d+),(-?\d+) (-?\d+),(-?\d+)", out):
        if m.group(2) in ("OK", "window"):
            rects.append(tuple(int(m.group(i)) for i in range(3, 7)))
    return rects, out


def samples(rect):
    l, t, r, b = rect
    w, h = r - l, b - t
    # The client area (below a window's title bar), away from the status overlay at the top left.
    return [(l + int(w * fx), t + int(h * fy)) for fx, fy in ((0.5, 0.55), (0.3, 0.7), (0.7, 0.7), (0.3, 0.45), (0.8, 0.5))]


class Grabber:
    """One BitBlt of the union of the outputs per round into a 32-bit DIB (GetPixel per point
    is ~20 ms with DWM; this keeps a round at a few ms)."""

    def __init__(self, rects):
        self.l = min(r[0] for r in rects)
        self.t = min(r[1] for r in rects)
        self.w = max(r[2] for r in rects) - self.l
        self.h = max(r[3] for r in rects) - self.t
        self.screen = user32.GetDC(0)
        self.mem = gdi32.CreateCompatibleDC(self.screen)

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32), ("biHeight", ctypes.c_int32),
                        ("biPlanes", ctypes.c_uint16), ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                        ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                        ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32), ("biClrImportant", ctypes.c_uint32)]

        bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), self.w, -self.h, 1, 32, 0, 0, 0, 0, 0, 0)
        self.bits = ctypes.c_void_p()
        gdi32.CreateDIBSection.restype = ctypes.c_void_p
        self.bmp = gdi32.CreateDIBSection(self.mem, ctypes.byref(bi), 0, ctypes.byref(self.bits), None, 0)
        gdi32.SelectObject(self.mem, ctypes.c_void_p(self.bmp))
        self.buf = (ctypes.c_uint8 * (self.w * self.h * 4)).from_address(self.bits.value)

    def grab(self):
        gdi32.BitBlt(self.mem, 0, 0, self.w, self.h, self.screen, self.l, self.t, 0x00CC0020)  # SRCCOPY

    def value(self, x, y):
        i = ((y - self.t) * self.w + (x - self.l)) * 4
        return self.buf[i] + self.buf[i + 1] + self.buf[i + 2]

    def close(self):
        gdi32.DeleteObject(ctypes.c_void_p(self.bmp))
        gdi32.DeleteDC(self.mem)
        user32.ReleaseDC(0, self.screen)


def pixels(grabber, pts):
    return [grabber.value(x, y) for x, y in pts]


def is_black(v):
    return all(0 <= x <= 12 for x in v)


def lit(v):
    return sum(1 for x in v if x > 30) >= 2


def one_run(exe_dir, timeout=5.0):
    rects, text = outputs(exe_dir)
    if not rects:
        return None, "no shown outputs:\n" + text
    pts = [samples(r) for r in rects]
    dc = Grabber(rects)
    try:
        dc.grab()
        before = [pixels(dc, p) for p in pts]
        if not all(lit(v) for v in before):
            return None, f"put something on air first (samples before: {before})"
        subprocess.run(["taskkill", "/IM", "himmel_render.exe", "/F"], capture_output=True)
        t0 = time.perf_counter()
        black_at = [None] * len(rects)
        back_at = [None] * len(rects)
        not_black = []  # frames where an output was neither the show nor black (the desktop)
        while time.perf_counter() - t0 < timeout:
            t = time.perf_counter() - t0
            dc.grab()
            for i, p in enumerate(pts):
                v = pixels(dc, p)
                if black_at[i] is None:
                    if is_black(v):
                        black_at[i] = t
                    elif t > 0.15:  # the old picture may stay a few frames; after that it must be black
                        not_black.append((i + 1, round(t, 3), v))
                elif back_at[i] is None and lit(v):
                    back_at[i] = t
            if all(b is not None for b in back_at):
                break
            time.sleep(0.02)  # gentle: a screen grab makes DWM work; faster sampling can disturb the renderer
        return {"black_ms": [None if b is None else round(b * 1000) for b in black_at],
                "back_ms": [None if b is None else round(b * 1000) for b in back_at],
                "not_black": not_black[:5]}, ""
    finally:
        dc.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--exe-dir", default=os.path.join("build", "bin", "Release"))
    ap.add_argument("--gap", type=float, default=4.0, help="seconds between runs (the renderer warms up)")
    a = ap.parse_args()
    if os.name != "nt":
        print("Windows only")
        return 2
    user32.SetProcessDPIAware()
    ok = True
    for k in range(a.runs):
        r, err = one_run(a.exe_dir)
        if r is None:
            print("run", k + 1, "cannot run:", err)
            return 2
        black_ok = all(b is not None for b in r["black_ms"]) and not r["not_black"]
        back = [b for b in r["back_ms"] if b is not None]
        back_ok = len(back) == len(r["back_ms"]) and max(back) < 2000
        ok &= black_ok and back_ok
        print(f"run {k + 1}: black after {r['black_ms']} ms, show back after {r['back_ms']} ms  "
              f"{'PASS' if black_ok and back_ok else 'FAIL'}" + (f"  not black: {r['not_black']}" if r["not_black"] else ""))
        time.sleep(a.gap)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
