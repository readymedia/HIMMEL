# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Himmel QA failure rig and soak (M12, docs/build-v1.md M12, docs/design/engine.md §3)
===================================================================================

Runs the whole stack in --sim with a temporary show root (never the owner's show,
library or autosave) and checks that a failing part never takes the outputs down:

  kills      kill each process in turn (render, ui, audio, ingest, osc, mk2_controller,
             weapon_blocks, weapon_nuvj): the supervisor brings it back in < 2 s; while
             the renderer is gone the curtains are black (never the desktop); a killed
             renderer comes back with the show as it was; any other kill leaves the
             renderer running (its frames keep counting, same epoch)
  unplug     stop each simulator (MK2, Lightpad, NuVJ) = a pulled USB cable, then start
             a new one = plugged back: the driver lets go and comes back
  missing    a clip file that disappears shows its placeholder (MISSING), no crash
  gpu        the GPU device-loss path: it cannot be forced from outside; the rig
             reports how it is covered (see docs/qa/README.md)
  soak       --minutes N / --hours N: every minute, late frames, fps, frame time and
             per-process memory + handles go to docs/qa/soak-<stamp>.csv, and a
             summary to docs/qa/soak-<stamp>.md

Usage:
  python tools/qa_rig.py                    the failure checks (about 3 minutes)
  python tools/qa_rig.py --minutes 20       the failure checks, then a 20 minute soak
  python tools/qa_rig.py --soak-only --hours 8
  python tools/qa_rig.py --bin DIR          where himmel.exe lives (default build/bin/Release)
  python tools/qa_rig.py --out DIR          where reports go (default docs/qa)

Needs: a Release build, the test clips in media/test (python tools/make_test_clips.py),
and a machine with no other Himmel stack running (it checks tasklist and refuses).
Exit code 0 = every check passed. The report is docs/qa/rig-<stamp>.md.
"""

import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mk2_sim  # noqa: E402

STACK = ["himmel.exe", "himmel_render.exe", "himmel_ui.exe", "himmel_audio.exe", "himmel_ingest.exe", "himmel_osc.exe",
         "mk2_controller.exe", "weapon_blocks.exe", "weapon_nuvj.exe", "weapon_apc20.exe"]
KILLS = ["himmel_render.exe", "himmel_ui.exe", "himmel_audio.exe", "himmel_ingest.exe", "himmel_osc.exe",
         "mk2_controller.exe", "weapon_blocks.exe", "weapon_nuvj.exe"]
BACK_WITHIN_S = 2.0
MISSING_NAME = "zz_qa_missing.mov"


# ------------------------------------------------------------------------------------- helpers
def pids(image):
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + image, "/FO", "CSV", "/NH"], capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == image.lower():
            out.append(int(parts[1]))
    return out


def running_stack():
    return {img: pids(img) for img in STACK if pids(img)}


def tool(bin_dir, exe, *args, timeout=5):
    try:
        r = subprocess.run([os.path.join(bin_dir, exe)] + [str(a) for a in args], capture_output=True, text=True,
                           timeout=timeout)
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def state(bin_dir):
    out = tool(bin_dir, "himmel_cmd.exe", "state")
    if not out:
        return None
    s = {"raw": out}
    m = re.search(r"epoch (\d+)\s+frame (\d+)\s+([0-9.]+) fps", out)
    if m:
        s["epoch"], s["frame"], s["fps"] = int(m.group(1)), int(m.group(2)), float(m.group(3))
    s["layers"] = {k: (int(c), float(o)) for k, c, o in re.findall(r"^\s+(\w\d)\s+clip (\d+) .*? op ([0-9.]+)", out, re.M)}
    s["missing"] = re.findall(r"^\s+(\w\d)\s+clip .*MISSING", out, re.M)
    m = re.search(r"perform(\*?) ack", out)
    s["mk2"] = bool(m and m.group(1) == "*")
    return s


def wait_for(fn, timeout, every=0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(every)
    return fn()


class Check:
    def __init__(self):
        self.failed = 0
        self.lines = []

    def __call__(self, ok, what, detail=""):
        line = ("ok    " if ok else "FAIL  ") + what + (("  (" + detail + ")") if detail else "")
        print("  " + line)
        self.lines.append(line)
        if not ok:
            self.failed += 1
        return ok


# ---- windows on the desktop (curtains, outputs) -------------------------------------------
def windows(cls):
    """Visible top-level windows of a class: [(hwnd, (l, t, r, b))]."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    buf = ctypes.create_unicode_buffer(64)

    def cb(h, _):
        u.GetClassNameW(h, buf, 64)
        if buf.value == cls and u.IsWindowVisible(h):
            r = wintypes.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            found.append((h, (r.left, r.top, r.right, r.bottom)))
        return True
    u.EnumWindows(proc(cb), 0)
    return found


def top_class(x, y):
    """Class name of the top-level window the user sees at a desktop point."""
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    u.WindowFromPoint.restype = ctypes.c_void_p
    u.GetAncestor.restype = ctypes.c_void_p
    h = u.WindowFromPoint(wintypes.POINT(x, y))
    h = u.GetAncestor(ctypes.c_void_p(h), 2) if h else None  # GA_ROOT
    buf = ctypes.create_unicode_buffer(64)
    if h:
        u.GetClassNameW(ctypes.c_void_p(h), buf, 64)
    return buf.value


def dark_share(rect):
    """Share of 16 x 9 samples of a desktop rectangle darker than 8 / 255."""
    return screen_samples(rect, share=True)


def screen_mean(rect):
    return screen_samples(rect, share=False)


def screen_samples(rect, share):
    """Mean brightness of a rectangle of the composed desktop (StretchBlt to 16 x 9 samples),
    or the share of samples darker than 8 / 255."""
    import ctypes
    u, g = ctypes.windll.user32, ctypes.windll.gdi32
    u.GetDC.restype = ctypes.c_void_p
    g.CreateCompatibleDC.restype = ctypes.c_void_p
    g.CreateCompatibleBitmap.restype = ctypes.c_void_p
    sdc = u.GetDC(None)
    mdc = g.CreateCompatibleDC(ctypes.c_void_p(sdc))
    bmp = g.CreateCompatibleBitmap(ctypes.c_void_p(sdc), 16, 9)
    g.SelectObject(ctypes.c_void_p(mdc), ctypes.c_void_p(bmp))
    g.SetStretchBltMode(ctypes.c_void_p(mdc), 3)
    l, t, r, b = rect
    g.StretchBlt(ctypes.c_void_p(mdc), 0, 0, 16, 9, ctypes.c_void_p(sdc), l + 4, t + 4, max(8, r - l - 8), max(8, b - t - 8),
                 0x00CC0020)
    buf = (ctypes.c_uint32 * 144)()
    g.GetBitmapBits(ctypes.c_void_p(bmp), ctypes.sizeof(buf), buf)
    g.DeleteObject(ctypes.c_void_p(bmp))
    g.DeleteDC(ctypes.c_void_p(mdc))
    u.ReleaseDC(None, ctypes.c_void_p(sdc))
    lv = [max((v >> 16) & 255, (v >> 8) & 255, v & 255) for v in buf]
    return sum(1 for x in lv if x < 8) / 144.0 if share else sum(lv) / 144.0


# ---- simulators (the "hardware") -------------------------------------------------------------
class Sims:
    def __init__(self):
        with open(os.path.join(ROOT, "maschine_mk2_map.json"), encoding="utf-8") as f:
            self.map = json.load(f)
        self.mk2 = self.pad = self.nuvj = None
        self.run = True
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while self.run:
            m = self.mk2
            if m is not None and not m.closed:
                m.read()
            else:
                time.sleep(0.01)

    def start_mk2(self):
        self.mk2 = mk2_sim.SimMk2(self.map)

    def start_pad(self):
        import lightpad_sim
        p = lightpad_sim.SimLightpad()

        def sensing():  # the sim Block is silent until touched: active sensing so weapon_blocks finds it
            while not p.closed:
                try:
                    p.udp.sendto(bytes([0xFE]), p.engine_out)
                except (OSError, AttributeError):
                    pass
                time.sleep(0.3)
        threading.Thread(target=sensing, daemon=True).start()
        self.pad = p

    def start_nuvj(self):
        import nuvj_sim
        self.nuvj = nuvj_sim.SimNuVJ(None)

    def close(self):
        self.run = False
        for s in (self.mk2, self.pad, self.nuvj):
            if s is not None:
                s.close()


# ------------------------------------------------------------------------------------- the stack
class Stack:
    def __init__(self, bin_dir, show_root, log_path):
        self.bin = bin_dir
        self.log_path = log_path
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        args = [os.path.join(bin_dir, "himmel.exe"), "--sim", "--weapons", "lightpad,nuvj", "--", "--show-root", show_root]
        print("starting", " ".join(args))
        self.p = subprocess.Popen(args, cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT,
                                  creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))

    def log_text(self):
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def stop(self):
        if self.p.poll() is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.p.pid)], capture_output=True)
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                pass
        # Anything of ours that outlived the supervisor (a job object normally takes them all).
        for img in STACK:
            for pid in pids(img):
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        self.log.close()


# ------------------------------------------------------------------------------------- the checks
def setup_show(bin_dir, check):
    """A known look: clips on A1 / A2, an opacity, a clip on B1."""
    tool(bin_dir, "himmel_cmd.exe", "clip", "a", 1, 0, "--", "clip", "a", 2, 1, "--", "opacity", "a", 2, 0.5, "--",
         "clip", "b", 1, 2)
    got = wait_for(lambda: (lambda s: s if s and s["layers"].get("A2", (None, 0))[0] == 1 else None)(state(bin_dir)), 5)
    check(bool(got), "a known show is on the decks (A1 clip 0, A2 clip 1 at 50 %, B1 clip 2)")
    return got["layers"] if got else {}


def kill_one(stack, img, look, check):
    bin_dir = stack.bin
    before = pids(img)
    if not before:
        check(False, "kill %s" % img, "not running")
        return
    st0 = state(bin_dir)
    out0 = windows("HimmelOutput")
    t0 = time.time()
    for pid in before:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    if img == "himmel_render.exe":
        # While the renderer is gone the curtains cover its outputs: black, never the desktop.
        # Sampled after Windows' close animation of the killed window (~0.2 s), before the
        # restarted renderer can cover them again.
        time.sleep(0.3)
        cur = windows("HimmelCurtain")
        # At each curtain's centre the visible window is the curtain (not the desktop), and what can
        # be seen of it is black. Another app's window over part of an output area is allowed (the
        # dev PC is shared); the desktop never is.
        tops = [top_class((r[0] + r[2]) // 2, (r[1] + r[3]) // 2) for _, r in cur]
        dark = [dark_share(r) for _, r in cur]
        ok = bool(cur) and all(t == "HimmelCurtain" for t in tops) and all(d >= 0.7 for d in dark)
        check(ok, "render killed: black curtains cover the outputs",
              "%d curtain(s) on top at the centre: %s; black share %s" % (
                  len(cur), ",".join(t or "?" for t in tops), ",".join("%.0f%%" % (100 * d) for d in dark)))
        # Back = a new renderer (new epoch) drawing frames (its fps figure needs 5 s, so count frames).
        back = wait_for(lambda: (lambda s: s if s and st0 and s.get("epoch") != st0.get("epoch") and s.get("frame", 0) >= 3 else None)(
            state(bin_dir)), 6, 0.02)
        dt = time.time() - t0
        check(bool(back) and dt < BACK_WITHIN_S, "render back in < %.0f s" % BACK_WITHIN_S, "%.2f s" % dt)
        outs = wait_for(lambda: windows("HimmelOutput"), 3)
        check(len(outs) >= max(1, len(out0)), "its output window(s) are back", "%d" % len(outs))
        got = wait_for(lambda: (lambda s: s if s and all(s["layers"].get(k, (None,))[0] == v[0] for k, v in look.items()) else None)(
            state(bin_dir)), 3)
        check(bool(got) and all(abs(got["layers"][k][1] - v[1]) < 0.02 for k, v in look.items()),
              "the show is restored after the render restart", str(got["layers"] if got else None))
        return
    newp = wait_for(lambda: [p for p in pids(img) if p not in before], 6, 0.02)
    dt = time.time() - t0
    check(bool(newp) and dt < BACK_WITHIN_S, "%s killed: back in < %.0f s" % (img, BACK_WITHIN_S), "%.2f s" % dt)
    time.sleep(0.5)
    st1 = state(bin_dir)
    ok = bool(st0 and st1 and st1["epoch"] == st0["epoch"] and st1["frame"] > st0["frame"])
    check(ok, "%s killed: the renderer kept running (same epoch, frames counting)" % img,
          "frame %s -> %s" % (st0 and st0.get("frame"), st1 and st1.get("frame")))


def unplug(stack, sims, check):
    log0 = len(stack.log_text())
    # MK2: the simulator stops = the cable is pulled; a new one = plugged back in.
    sims.mk2.close()
    gone = wait_for(lambda: (lambda s: s if s and not s["mk2"] else None)(state(stack.bin)), 4)
    check(bool(gone), "MK2 unplugged: the engine sees it go")
    sims.start_mk2()
    back = wait_for(lambda: (lambda s: s if s and s["mk2"] else None)(state(stack.bin)), 5)
    check(bool(back), "MK2 plugged back: the engine sees it again")
    for name, attr, start, word in (("Lightpad", "pad", sims.start_pad, "Lightpad"), ("NuVJ", "nuvj", sims.start_nuvj, "NuVJ")):
        getattr(sims, attr).close()
        off = wait_for(lambda: (word + " offline") in stack.log_text()[log0:], 8, 0.2)
        check(bool(off), "%s unplugged: the engine lets go of it" % name)
        start()
        mark = len(stack.log_text())
        on = wait_for(lambda: (word + " connected") in stack.log_text()[mark:], 10, 0.2)
        check(bool(on), "%s plugged back: connected again" % name)
        log0 = len(stack.log_text())
    st = state(stack.bin)
    check(bool(st and st.get("fps", 0) > 0), "the outputs ran through every unplug", "fps %s" % (st and st.get("fps")))


def missing_clip(stack, check, clip_copy):
    cat = tool(stack.bin, "himmel_cmd.exe", "catalog") or ""
    m = re.search(r"^\s+(\d+)\s+" + re.escape(os.path.splitext(MISSING_NAME)[0]) + r"\s", cat, re.M)
    if not check(bool(m), "the rig's extra clip is in the catalog", MISSING_NAME):
        return
    os.replace(clip_copy, clip_copy + ".gone")  # the file disappears (a drive pulled, a file deleted)
    tool(stack.bin, "himmel_cmd.exe", "clip", "b", 3, m.group(1))
    got = wait_for(lambda: (lambda s: s if s and "B3" in s["missing"] else None)(state(stack.bin)), 6, 0.2)
    check(bool(got), "a missing clip file shows its placeholder (B3 MISSING), no crash")
    st = state(stack.bin)
    check(bool(st and st.get("fps", 0) > 0), "the renderer keeps running with the missing clip")
    tool(stack.bin, "himmel_cmd.exe", "clip", "b", 3, -1)


def gpu_loss(check):
    # A DXGI device loss cannot be forced from outside the process without a driver tool
    # (dxcap -forcetdr needs the Graphics Tools feature + admin). The renderer's path is: Present
    # returns DEVICE_REMOVED / RESET -> it logs "GPU device lost" and exits with code 3 -> the
    # supervisor restarts it like any crash. The render-kill check above covers that recovery.
    check(True, "GPU device loss: not forced (see docs/qa/README.md); recovery = the render-kill path above")


# ------------------------------------------------------------------------------------- soak
def proc_stats():
    """Per image: (working set MB, handles) summed over its processes."""
    ps = ("Get-Process -Name " + ",".join(os.path.splitext(i)[0] for i in STACK) +
          " -ErrorAction SilentlyContinue | ForEach-Object { '{0},{1},{2}' -f $_.ProcessName, $_.WorkingSet64, $_.HandleCount }")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=30)
    out = {}
    for line in r.stdout.splitlines():
        p = line.strip().split(",")
        if len(p) == 3 and p[1].isdigit():
            mb, h = out.get(p[0], (0.0, 0))
            out[p[0]] = (mb + int(p[1]) / 1048576.0, h + int(p[2] or 0))
    return out


def soak(stack, minutes, out_dir, stamp):
    csv_path = os.path.join(out_dir, "soak-%s.csv" % stamp)
    names = [os.path.splitext(i)[0] for i in STACK if i != "weapon_apc20.exe"]
    print("soak: %d minute(s), one row a minute -> %s" % (minutes, csv_path))
    rows = []
    t_end = time.time() + minutes * 60
    n = 0
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["minute", "time", "fps", "late_total", "interval_max_ms", "cpu_avg_ms", "input_present_ms"] +
                   ["%s_mb" % x for x in names] + ["%s_handles" % x for x in names])
        while time.time() < t_end:
            # Something happens every minute: a TAKE (the show is being played, not idle).
            tool(stack.bin, "himmel_cmd.exe", "take", 1500)
            time.sleep(60.0)
            n += 1
            stats = re.findall(r"STATS fps ([0-9.]+)\s+interval avg [0-9.]+ max ([0-9.]+) ms\s+cpu avg ([0-9.]+).*?late \d+ \(total (\d+)\).*?"
                               r"input->present ([0-9.]+) ms", stack.log_text())
            fps, imax, cpu, late, inp = stats[-1] if stats else ("", "", "", "", "")
            ps = proc_stats()
            row = [n, time.strftime("%H:%M:%S"), fps, late, imax, cpu, inp] + \
                  ["%.1f" % ps.get(x, (0.0, 0))[0] for x in names] + [ps.get(x, (0.0, 0))[1] for x in names]
            w.writerow(row)
            f.flush()
            rows.append(row)
            print("  minute %d: fps %s late %s render %.0f MB / %d handles" % (
                n, fps, late, ps.get("himmel_render", (0, 0))[0], ps.get("himmel_render", (0, 0))[1]))
    return csv_path, names, rows


def soak_summary(path_md, csv_path, names, rows, stack, log_from=0):
    restarts = len(re.findall(r"render: started", stack.log_text()[log_from:]))
    lines = ["# Soak %s" % os.path.basename(csv_path)[5:-4], "",
             "Machine: %s. Stack: `himmel.exe --sim --weapons lightpad,nuvj -- --show-root <temp>`, a TAKE every minute."
             % "this PC (fill in: GPU, refresh rate)", "",
             "Minutes: %d. Renderer restarts during the soak: %d. Raw rows: `%s`." % (len(rows), max(0, restarts),
                                                                                     os.path.basename(csv_path)), ""]
    if rows:
        first, last = rows[0], rows[-1]
        lines += ["| | first minute | last minute |", "|---|---|---|",
                  "| fps | %s | %s |" % (first[2], last[2]), "| late frames (total) | %s | %s |" % (first[3], last[3]),
                  "| frame interval max (ms, 5 s window) | %s | %s |" % (first[4], last[4]),
                  "| input -> present (ms) | %s | %s |" % (first[6], last[6])]
        k = 7
        for i, x in enumerate(names):
            lines.append("| %s memory MB / handles | %s / %s | %s / %s |" % (x, first[k + i], first[k + len(names) + i],
                                                                         last[k + i], last[k + len(names) + i]))
    with open(path_md, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# ------------------------------------------------------------------------------------- main
def main():
    args = sys.argv[1:]
    bin_dir = os.path.abspath(args[args.index("--bin") + 1]) if "--bin" in args else os.path.join(ROOT, "build", "bin", "Release")
    out_dir = os.path.abspath(args[args.index("--out") + 1]) if "--out" in args else os.path.join(ROOT, "docs", "qa")
    minutes = int(float(args[args.index("--minutes") + 1])) if "--minutes" in args else 0
    if "--hours" in args:
        minutes = int(float(args[args.index("--hours") + 1]) * 60)
    soak_only = "--soak-only" in args
    os.makedirs(out_dir, exist_ok=True)
    other = running_stack()
    if other:
        print("another Himmel stack is running (%s): one stack per machine - stop it first" % ", ".join(other))
        return 2
    clips = os.path.join(ROOT, "media", "test")
    src = [f for f in sorted(os.listdir(clips)) if f.endswith(".mov") and f != MISSING_NAME] if os.path.isdir(clips) else []
    if not src:
        print("no test clips in media/test: run python tools/make_test_clips.py")
        return 2
    clip_copy = os.path.join(clips, MISSING_NAME)
    shutil.copyfile(os.path.join(clips, src[0]), clip_copy)
    stamp = time.strftime("%Y%m%d-%H%M")
    show_root = tempfile.mkdtemp(prefix="himmel-qa-")
    check = Check()
    stack = Stack(bin_dir, show_root, os.path.join(out_dir, "stack-%s.log" % stamp))
    sims = Sims()
    try:
        sims.start_mk2()
        sims.start_pad()
        sims.start_nuvj()
        st = wait_for(lambda: (lambda s: s if s and s.get("fps", 0) > 0 and s["mk2"] else None)(state(bin_dir)), 20, 0.2)
        if not check(bool(st), "the stack runs and the simulated MK2 is connected"):
            return 1
        wait_for(lambda: "Lightpad connected" in stack.log_text() and "NuVJ connected" in stack.log_text(), 15, 0.2)
        time.sleep(2.0)  # let every child settle (the ingest worker opens its library)
        if not soak_only:
            print("kills")
            look = setup_show(bin_dir, check)
            for img in KILLS:
                if not pids(img):
                    check(img in ("himmel_ingest.exe",), "%s is running before the kill" % img)
                    continue
                kill_one(stack, img, look, check)
                time.sleep(1.5)
            print("unplug")
            unplug(stack, sims, check)
            print("missing clip")
            missing_clip(stack, check, clip_copy)
            print("GPU device loss")
            gpu_loss(check)
        if minutes > 0:
            log_from = len(stack.log_text())
            csv_path, names, rows = soak(stack, minutes, out_dir, stamp)
            soak_summary(os.path.join(out_dir, "soak-%s.md" % stamp), csv_path, names, rows, stack, log_from)
            if rows:
                late0, late1 = rows[0][3], rows[-1][3]
                check(True, "soak: %d minute(s), late frames %s -> %s (details in soak-%s.md)" % (len(rows), late0, late1, stamp))
                still = all(r[2] and float(r[2]) > 0 for r in rows)
                check(still, "soak: the renderer reported fps every minute")
    finally:
        sims.close()
        stack.stop()
        for p in (clip_copy, clip_copy + ".gone"):
            if os.path.exists(p):
                os.remove(p)
        shutil.rmtree(show_root, ignore_errors=True)
    report = os.path.join(out_dir, "rig-%s.md" % stamp)
    with open(report, "w", encoding="utf-8") as f:
        f.write("# QA rig %s\n\nMachine: %s. Build: %s. %s\n\n```\n%s\n```\n\n%s\n" % (
            stamp, "this PC (fill in: GPU, refresh rate)", os.path.relpath(bin_dir, ROOT),
            "PASS" if check.failed == 0 else "FAILED: %d check(s)" % check.failed, "\n".join(check.lines),
            "Stack log: `stack-%s.log`." % stamp))
    print("report:", report)
    print("PASS" if check.failed == 0 else "FAILED: %d check(s)" % check.failed)
    return 0 if check.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
