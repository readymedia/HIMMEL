# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
MK2 end-to-end check (no hardware)
==================================

Plays a simulated MK2 (tools/mk2_sim.py, the same code the live view's
Simulate button uses) into a running Himmel and checks that the engine's state
changes, and that the engine's Perform screens and LEDs come back to the panel.

Chain under test:
  SimMk2 -> UDP 127.0.0.1:8766 -> mk2_controller --sim -> Local\\HimmelMk2 ring 0
  -> himmel_perform (in himmel_render) -> command ring -> engine model
  -> Local\\HimmelEngineState (read with himmel_cmd state)
  and back: Perform LEDs / screens -> Local\\HimmelMk2 outputs -> controller -> UDP -> SimMk2

Run (it starts and stops its own stack unless --attach):
  python tools/mk2_e2e.py                 start himmel.exe --sim --no-ui --no-weapons -- --no-show-files, test, stop
  python tools/mk2_e2e.py --attach        test a Himmel that is already running with --sim
  python tools/mk2_e2e.py --bin DIR       where himmel.exe / himmel_cmd.exe live (default build/bin/Release)

Exactly one MK2 simulator may run: close every MK2 Live page in Simulate mode
first (the newest simulator wins, so this check would take over from it).
Exit code 0 = every check passed.
"""

import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mk2_sim  # noqa: E402

MAP_PATH = os.path.join(ROOT, "maschine_mk2_map.json")


class Runner:
    """Calls SimMk2.read() all the time, like mk2_live.py's Mapper.reader thread."""

    def __init__(self, sim):
        self.sim = sim
        self.run = True
        self.reports = 0
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while self.run:
            if self.sim.read():
                self.reports += 1


def state(bin_dir):
    r = subprocess.run([os.path.join(bin_dir, "himmel_cmd.exe"), "state"], capture_output=True, text=True, timeout=5)
    if r.returncode != 0:
        return None
    out = r.stdout
    s = {"raw": out}
    m = re.search(r"crossfader ([0-9.]+)\s+PROGRAM (\w)", out)
    if m:
        s["xfade"], s["program"] = float(m.group(1)), m.group(2)
    m = re.search(r"focus (\w)(\d)", out)
    s["focus"] = m.group(1) + m.group(2) if m else None
    m = re.search(r"perform(\*?) ack (\d+) rej (\d+)", out)
    if m:
        s["perform_alive"], s["ack"], s["rej"] = m.group(1) == "*", int(m.group(2)), int(m.group(3))
    else:
        s["perform_alive"], s["ack"], s["rej"] = False, 0, 0
    s["take"] = "  TAKE" in out.splitlines()[0] if out else False
    s["ops"] = {k: float(v) for k, v in re.findall(r"^\s+(\w\d)\s+clip .*? op ([0-9.]+)", out, re.M)}
    return s


def wait_for(fn, timeout=3.0, every=0.05):
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

    def __call__(self, ok, what, detail=""):
        print(("  ok    " if ok else "  FAIL  ") + what + (("  (" + detail + ")") if detail else ""))
        if not ok:
            self.failed += 1
        return ok


def main():
    args = sys.argv[1:]
    attach = "--attach" in args
    bin_dir = os.path.join(ROOT, "build", "bin", "Release")
    if "--bin" in args:
        bin_dir = os.path.abspath(args[args.index("--bin") + 1])
    with open(MAP_PATH, encoding="utf-8") as f:
        mapping = json.load(f)

    stack = None
    if not attach:
        exe = os.path.join(bin_dir, "himmel.exe")
        # --no-show-files: a test run never opens or autosaves the owner's project (SHOW M3, D109)
        print("starting", exe, "--sim --no-ui --no-weapons -- --no-show-files")
        stack = subprocess.Popen([exe, "--sim", "--no-ui", "--no-weapons", "--", "--no-show-files"], cwd=ROOT,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    check = Check()
    sim = mk2_sim.SimMk2(mapping)
    runner = Runner(sim)
    try:
        st = wait_for(lambda: state(bin_dir), timeout=15)
        if not check(st is not None, "engine state block readable (himmel_cmd state)"):
            return 2
        check(wait_for(lambda: sim.engine_active(), timeout=8), "controller answers the simulator (hello / LEDs)")
        check(not sim.busy(), "this simulator is the one playing Himmel (not ignored)")
        st = wait_for(lambda: (lambda s: s if s and s["perform_alive"] else None)(state(bin_dir)), timeout=5)
        check(bool(st), "engine sees the MK2 (producer 'perform' alive)")
        st = state(bin_dir)
        print("  start: focus %s  xfade %.3f  PROGRAM %s  ack %d rej %d" % (st["focus"], st["xfade"], st["program"],
                                                                         st["ack"], st["rej"]))

        def tap(name, hold=0.06):
            sim.press(name, True)
            time.sleep(hold)
            sim.press(name, False)
            time.sleep(0.06)

        # 1. Every group sets the focus: A-D = PREVIEW layer 1-4, E-H = PROGRAM layer 1-4.
        for g, (side, layer) in zip("ABCDEFGH", [(0, 1), (0, 2), (0, 3), (0, 4), (1, 1), (1, 2), (1, 3), (1, 4)]):
            tap("Group " + g)
            prog = state(bin_dir)["program"]
            prev = "B" if prog == "A" else "A"
            want = (prev if side == 0 else prog) + str(layer)
            got = wait_for(lambda: (lambda s: s if s["focus"] == want else None)(state(bin_dir)), timeout=2)
            check(bool(got), "Group %s -> focus %s" % (g, want), "" if got else "focus is %s" % state(bin_dir)["focus"])
        # Repeat quickly: 3 rounds of C, B, G, A with short gaps.
        ok = True
        for _ in range(3):
            for g, idx in (("C", 2), ("B", 1), ("G", 6), ("A", 0)):
                tap("Group " + g, hold=0.03)
                prog = state(bin_dir)["program"]
                prev = "B" if prog == "A" else "A"
                want = (prev if idx < 4 else prog) + str((idx & 3) + 1)
                if not wait_for(lambda: (lambda s: s if s["focus"] == want else None)(state(bin_dir)), timeout=2):
                    ok = False
        check(ok, "12 quick group presses all reach the engine")

        # 2. Knob 1 = opacity of the focused layer (needs a clip there): load clip 0 with pad 1.
        tap("Group A")
        sim.pad("Pad 1", 3000)
        time.sleep(0.08)
        sim.pad("Pad 1", 0)
        st = state(bin_dir)
        prev = "B" if st["program"] == "A" else "A"
        got = wait_for(lambda: (lambda s: s if (prev + "1") in s["ops"] else None)(state(bin_dir)), timeout=2)
        if check(bool(got), "Pad 1 loads a clip into %s1" % prev, "" if got else "no clips in media/? run tools/make_test_clips.py"):
            op0 = got["ops"][prev + "1"]
            sim.turn("Knob 1", -300)  # -0.3 of a turn
            time.sleep(0.1)
            got2 = wait_for(lambda: (lambda s: s if abs(s["ops"].get(prev + "1", op0) - op0) > 0.1 else None)(state(bin_dir)),
                            timeout=2)
            check(bool(got2), "Knob 1 turns the opacity of %s1" % prev,
                  "%.2f -> %.2f" % (op0, got2["ops"][prev + "1"]) if got2 else "still %.2f" % op0)

        # 3. Wheel = TAKE T-bar: +6 detents moves the crossfader by 0.2 towards PREVIEW.
        st = state(bin_dir)
        x0, p0 = st["xfade"], st["program"]
        for _ in range(6):
            sim.turn("Master encoder (turn)", 1)
            time.sleep(0.03)
        want = x0 + (0.2 if p0 == "A" else -0.2)
        got = wait_for(lambda: (lambda s: s if abs(s["xfade"] - want) < 0.02 else None)(state(bin_dir)), timeout=2)
        check(bool(got), "wheel +6 detents -> crossfader %.3f" % want, "" if got else "is %.3f" % state(bin_dir)["xfade"])
        # Back to the rest position.
        for _ in range(6):
            sim.turn("Master encoder (turn)", -1)
            time.sleep(0.03)
        wait_for(lambda: (lambda s: s if abs(s["xfade"] - x0) < 0.02 else None)(state(bin_dir)), timeout=2)

        # 4. PLAY = TAKE: PROGRAM flips.
        p0 = state(bin_dir)["program"]
        tap("Play")
        got = wait_for(lambda: (lambda s: s if s["program"] != p0 and not s["take"] else None)(state(bin_dir)), timeout=5)
        check(bool(got), "PLAY runs a TAKE: PROGRAM %s -> %s" % (p0, "B" if p0 == "A" else "A"))

        # 5. Screens and LEDs come back to the simulated panel.
        time.sleep(0.3)
        lit = sum(1 for b in sim.rx_screens["left"] if b) + sum(1 for b in sim.rx_screens["right"] if b)
        check(lit > 50, "engine screens reach the simulator", "%d non-empty bytes" % lit)
        leds = sum(1 for v in sim.rx_leds[0x81] if v)
        check(leds > 0, "engine group LEDs reach the simulator", "%d lit" % leds)

        st = state(bin_dir)
        check(st["rej"] == 0, "no Perform command refused", "rej %d" % st["rej"])
        print("  end:   focus %s  xfade %.3f  PROGRAM %s  ack %d rej %d   (%d reports sent)" % (
            st["focus"], st["xfade"], st["program"], st["ack"], st["rej"], runner.reports))
    finally:
        runner.run = False
        sim.close()
        if stack is not None:
            subprocess.run([os.path.join(bin_dir, "himmel_cmd.exe"), "quit"], capture_output=True)
            try:
                stack.wait(5)
            except subprocess.TimeoutExpired:
                pass
            if stack.poll() is None:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(stack.pid)], capture_output=True)
    print("PASS" if check.failed == 0 else "FAILED: %d check(s)" % check.failed)
    return 0 if check.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
