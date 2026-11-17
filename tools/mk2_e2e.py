# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
MK2 end-to-end check (no hardware)
==================================

Plays a simulated MK2 (tools/mk2_sim.py, the same code the live view's
Simulate button uses) into a running Himmel and checks that the engine's state
changes, and that the engine's Perform screens and LEDs come back to the panel.
Then it plays **a whole song from the MK2 alone** (M4) and **measures input ->
glass** (the time from the MK2 report to the output window's pixels changing on
the composed desktop).

Chain under test:
  SimMk2 -> UDP 127.0.0.1:8766 -> mk2_controller --sim -> Local\\HimmelMk2 ring 0
  -> himmel_perform (in himmel_render) -> command ring -> engine model / show runtime
  -> Local\\HimmelState (himmel_cmd state) and Local\\HimmelShow (show_cmd status)
  and back: Perform LEDs / screens -> Local\\HimmelMk2 outputs -> controller -> UDP -> SimMk2

Run (it starts and stops its own stack unless --attach):
  python tools/mk2_e2e.py                 start himmel.exe --sim --no-ui --weapons lightpad -- --no-show-files, test, stop
  python tools/mk2_e2e.py --attach        test a Himmel that is already running with --sim
  python tools/mk2_e2e.py --bin DIR       where himmel.exe / himmel_cmd.exe / show_cmd.exe live (default build/bin/Release)
  python tools/mk2_e2e.py --basic         only the basic checks (no song, no latency)
  python tools/mk2_e2e.py --no-latency    skip the input -> glass measurement
  python tools/mk2_e2e.py --runs N        latency samples (default 20)
  python tools/mk2_e2e.py --no-lightpad   no Lightpad (M9) stage, and no weapon drivers

The song needs clips: media/*.mov (python tools/make_test_clips.py makes them).
--no-show-files: the run never opens or writes the owner's project or autosave.
Exactly one MK2 simulator may run: close every MK2 Live page in Simulate mode
first (the newest simulator wins, so this check would take over from it), and
only one Himmel stack per machine (check tasklist).
Exit code 0 = every check passed.
"""

import ctypes
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mk2_sim  # noqa: E402

MAP_PATH = os.path.join(ROOT, "maschine_mk2_map.json")


class TimedSim(mk2_sim.SimMk2):
    """SimMk2 that remembers when a changed controls report (0x01) left for the engine."""

    def __init__(self, mapping):
        super().__init__(mapping)
        self.last_controls = None
        self.sent_change_t = 0.0

    def _forward(self, report):
        if report and report[0] == 0x01 and report != self.last_controls:
            self.last_controls = report
            self.sent_change_t = time.perf_counter()
        super()._forward(report)


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


def run_tool(bin_dir, exe, *args):
    try:
        r = subprocess.run([os.path.join(bin_dir, exe)] + list(args), capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def state(bin_dir):
    out = run_tool(bin_dir, "himmel_cmd.exe", "state")
    if out is None:
        return None
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
    first = out.splitlines()[0] if out else ""
    s["take"] = "  TAKE" in first
    s["black"] = "  BLACK" in first
    s["ops"] = {k: float(v) for k, v in re.findall(r"^\s+(\w\d)\s+clip .*? op ([0-9.]+)", out, re.M)}
    s["clips"] = {k: int(v) for k, v in re.findall(r"^\s+(\w\d)\s+clip (\d+) ", out, re.M)}
    s["speed"] = {k: float(v) for k, v in re.findall(r"^\s+(\w\d)\s+clip .*? spd ([-+0-9.]+)", out, re.M)}
    s["layer_fx"] = {k: n for k, n in re.findall(r"^\s+(\w\d) fx1 (\S+)", out, re.M)}
    s["master_fx"] = {int(k): n for k, n in re.findall(r"^\s+master fx(\d) (\S+)", out, re.M)}
    m = re.search(r"HIT\s+clip (\d+)", out)
    s["hit"] = int(m.group(1)) if m else None
    return s


def show(bin_dir):
    out = run_tool(bin_dir, "show_cmd.exe", "status")
    if out is None:
        return None
    s = {"raw": out}
    s["pads"] = {int(p): int(v) for p, v in re.findall(r"^\s+pad\s+(\d+)\s.*?\+(\d+) var", out, re.M)}
    s["deck"] = {}
    for d, sc, pad, var in re.findall(r"^deck (\w)\s+scene (-?\d+) pad (-?\d+)(\w?)", out, re.M):
        s["deck"][d] = (int(sc), int(pad), var)
    m = re.search(r"undo (\d+)\s+redo (\d+)", out)
    s["undo"], s["redo"] = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    s["macros"] = [float(v) for v in re.findall(r"\s\S+ ([0-9.]+)", out.split("macros", 1)[1].splitlines()[0])] \
        if "macros" in out else []
    m = re.search(r'^last\s+"([^"]*)"', out, re.M)
    s["message"] = m.group(1) if m else ""
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


class Player:
    """The hands on the simulated MK2."""

    def __init__(self, sim):
        self.sim = sim

    def tap(self, name, hold=0.06):
        self.sim.press(name, True)
        time.sleep(hold)
        self.sim.press(name, False)
        time.sleep(0.06)

    def down(self, name):
        self.sim.press(name, True)
        time.sleep(0.05)

    def up(self, name):
        self.sim.press(name, False)
        time.sleep(0.05)

    def pad(self, n, pressure=3000, hold=0.08):
        self.sim.pad("Pad %d" % n, pressure)
        time.sleep(hold)
        self.sim.pad("Pad %d" % n, 0)
        time.sleep(0.06)

    def knob(self, k, delta):
        self.sim.turn("Knob %d" % k, delta)
        time.sleep(0.06)

    def wheel(self, detents):
        step = 1 if detents > 0 else -1
        for _ in range(abs(detents)):
            self.sim.turn("Master encoder (turn)", step)
            time.sleep(0.03)

    def page_to_play(self):
        self.down("Shift")
        self.tap("Page <")
        self.up("Shift")


# ------------------------------------------------------------------------------------- basic
def basic(bin_dir, sim, check):
    hand = Player(sim)
    # 1. Every group sets the focus: A-D = PREVIEW layer 1-4, E-H = PROGRAM layer 1-4.
    for g, (side, layer) in zip("ABCDEFGH", [(0, 1), (0, 2), (0, 3), (0, 4), (1, 1), (1, 2), (1, 3), (1, 4)]):
        hand.tap("Group " + g)
        time.sleep(0.4)  # longer than a double-tap
        prog = state(bin_dir)["program"]
        prev = "B" if prog == "A" else "A"
        want = (prev if side == 0 else prog) + str(layer)
        got = wait_for(lambda: (lambda s: s if s["focus"] == want else None)(state(bin_dir)), timeout=2)
        check(bool(got), "Group %s -> focus %s" % (g, want), "" if got else "focus is %s" % state(bin_dir)["focus"])
    # Quick presses of different groups (a double-tap needs the same group twice).
    ok = True
    for _ in range(3):
        for g, idx in (("C", 2), ("B", 1), ("G", 6), ("A", 0)):
            hand.tap("Group " + g, hold=0.03)
            prog = state(bin_dir)["program"]
            prev = "B" if prog == "A" else "A"
            want = (prev if idx < 4 else prog) + str((idx & 3) + 1)
            if not wait_for(lambda: (lambda s: s if s["focus"] == want else None)(state(bin_dir)), timeout=2):
                ok = False
    check(ok, "12 quick group presses all reach the engine")
    # Double-tap Group A = PROGRAM layer 1 (perform.md §1).
    time.sleep(0.4)
    hand.tap("Group A", hold=0.03)
    hand.tap("Group A", hold=0.03)
    prog = state(bin_dir)["program"]
    got = wait_for(lambda: (lambda s: s if s["focus"] == prog + "1" else None)(state(bin_dir)), timeout=2)
    check(bool(got), "double-tap Group A -> PROGRAM layer 1 (%s1)" % prog)
    time.sleep(0.4)

    # 2. Pad 1 loads a clip into PREVIEW layer 1; LAYER page knob 1 = opacity.
    hand.tap("Group A")
    hand.pad(1)
    st = state(bin_dir)
    prev = "B" if st["program"] == "A" else "A"
    got = wait_for(lambda: (lambda s: s if (prev + "1") in s["ops"] else None)(state(bin_dir)), timeout=2)
    if check(bool(got), "Pad 1 loads a clip into %s1" % prev, "" if got else "no clips in media/? run tools/make_test_clips.py"):
        op0 = got["ops"][prev + "1"]
        hand.tap("Page >")
        hand.tap("Page >")  # LAYER
        hand.knob(1, -300)  # -0.3 of a turn
        got2 = wait_for(lambda: (lambda s: s if abs(s["ops"].get(prev + "1", op0) - op0) > 0.1 else None)(state(bin_dir)),
                        timeout=2)
        check(bool(got2), "LAYER page knob 1 turns the opacity of %s1" % prev,
              "%.2f -> %.2f" % (op0, got2["ops"][prev + "1"]) if got2 else "still %.2f" % op0)
        hand.knob(1, 300)
        hand.page_to_play()

    # 3. Wheel = TAKE T-bar: +6 detents moves the crossfader by 0.2 towards PREVIEW.
    st = state(bin_dir)
    x0, p0 = st["xfade"], st["program"]
    hand.wheel(6)
    want = x0 + (0.2 if p0 == "A" else -0.2)
    got = wait_for(lambda: (lambda s: s if abs(s["xfade"] - want) < 0.02 else None)(state(bin_dir)), timeout=2)
    check(bool(got), "wheel +6 detents -> crossfader %.3f" % want, "" if got else "is %.3f" % state(bin_dir)["xfade"])
    hand.wheel(-6)
    wait_for(lambda: (lambda s: s if abs(s["xfade"] - x0) < 0.02 else None)(state(bin_dir)), timeout=2)

    # 4. PLAY = TAKE: PROGRAM flips.
    p0 = state(bin_dir)["program"]
    hand.tap("Play")
    got = wait_for(lambda: (lambda s: s if s["program"] != p0 and not s["take"] else None)(state(bin_dir)), timeout=5)
    check(bool(got), "PLAY runs a TAKE: PROGRAM %s -> %s" % (p0, "B" if p0 == "A" else "A"))

    # 5. Screens and LEDs come back to the simulated panel.
    time.sleep(0.3)
    lit = sum(1 for b in sim.rx_screens["left"] if b) + sum(1 for b in sim.rx_screens["right"] if b)
    check(lit > 50, "engine screens reach the simulator", "%d non-empty bytes" % lit)
    leds = sum(1 for v in sim.rx_leds[0x81] if v)
    check(leds > 0, "engine group LEDs reach the simulator", "%d lit" % leds)


# ------------------------------------------------------------------------------------- the song
def song(bin_dir, sim, check):
    """A whole song from the MK2 alone: build 3 scenes, then play them live (perform.md §2, §6)."""
    hand = Player(sim)
    st = state(bin_dir)
    sh = show(bin_dir)
    if not check(sh is not None, "show block readable (show_cmd status)"):
        return
    clip_dir = os.path.join(ROOT, "media", "test")
    n_clips = len([f for f in os.listdir(clip_dir) if f.endswith(".mov")]) if os.path.isdir(clip_dir) else 4
    print("  song: %d scenes on the page before, PROGRAM %s" % (len(sh["pads"]), st["program"]))
    progd = lambda: state(bin_dir)["program"]  # noqa: E731
    prevd = lambda: "B" if progd() == "A" else "A"  # noqa: E731
    pads_before = set(sh["pads"])

    def scene_on(deck, pad, var=None, timeout=3):
        def f():
            s = show(bin_dir)
            if not s or deck not in s["deck"]:
                return None
            sc, p, v = s["deck"][deck]
            return s if p == pad and sc >= 0 and (var is None or v == var) else None
        return wait_for(f, timeout=timeout)

    def build_scene(layers, fx=False):
        """layers: [(group, pad)] on PREVIEW, then REC. Returns the pad it was saved to."""
        for g, p in layers:
            hand.tap("Group " + g)
            time.sleep(0.4)
            hand.pad(p)
        if fx:  # FX page knob 1: an effect on the focused layer, then back to PLAY
            for _ in range(3):
                hand.tap("Page >")
            hand.knob(1, 120)
            hand.page_to_play()
        time.sleep(0.2)
        before = set(show(bin_dir)["pads"])
        hand.tap("Rec")
        got = wait_for(lambda: (lambda s: s if set(s["pads"]) - before else None)(show(bin_dir)), timeout=3)
        return min(set(got["pads"]) - before) if got else None

    # 1. Prepare three scenes on PREVIEW with the pads, and REC each one.
    c = lambda i: 1 + (i % max(1, min(16, n_clips or 4)))  # noqa: E731  pad of the i-th clip
    s1 = build_scene([("A", c(0)), ("B", c(1))], fx=True)
    p = prevd()
    st = state(bin_dir)
    check(st["clips"].get(p + "1") == c(0) - 1 and st["clips"].get(p + "2") == c(1) - 1,
          "pads 1 + 2 built PREVIEW %s1 / %s2" % (p, p), str(st["clips"]))
    check(p + "2" in st["layer_fx"], "FX page knob 1 put an effect on %s2" % p, st["layer_fx"].get(p + "2", "none"))
    check(s1 is not None, "REC saved scene 1 to a pad", "pad %s" % s1)
    s2 = build_scene([("A", c(2))])
    check(s2 is not None and s2 != s1, "REC saved scene 2", "pad %s" % s2)
    s3 = build_scene([("B", c(3)), ("C", c(0))])
    check(s3 is not None and s3 not in (s1, s2), "REC saved scene 3", "pad %s" % s3)
    if None in (s1, s2, s3):
        return
    check(set(show(bin_dir)["pads"]) - pads_before == {s1, s2, s3}, "three new scenes on the page")

    # 2. LOOKS: tap scene 1 in PREP focus -> into PREVIEW.
    hand.tap("Group A")
    time.sleep(0.4)
    hand.tap("Scene")
    hand.pad(s1)
    check(bool(scene_on(prevd(), s1)), "LOOKS pad %d (PREP) recalls scene 1 into PREVIEW %s" % (s1, prevd()))

    # 3. The wheel takes it live: 30 detents = one full T-bar move.
    p0 = progd()
    hand.wheel(30)
    got = wait_for(lambda: (lambda s: s if s["program"] != p0 and not s["take"] else None)(state(bin_dir)), timeout=4)
    check(bool(got), "wheel 30 detents takes scene 1 live (PROGRAM %s -> %s)" % (p0, "B" if p0 == "A" else "A"))
    check(bool(scene_on(progd(), s1)), "scene 1 is on PROGRAM %s" % progd())

    # 4. ENTER = GO: the next scene live.
    hand.tap("Enter")
    check(bool(scene_on(progd(), s2)), "ENTER: scene 2 live on PROGRAM %s" % progd())

    # 5. Ride the macros (knobs 5-8 = PROGRAM): knob 5 INTENSITY.
    m0 = show(bin_dir)["macros"][:1] or [0.0]
    hand.knob(5, 250)
    got = wait_for(lambda: (lambda s: s if s["macros"] and abs(s["macros"][0] - m0[0]) > 0.1 else None)(show(bin_dir)))
    check(bool(got), "knob 5 rides macro 1 (INTENSITY)", "%.2f -> %s" % (m0[0], got["macros"][0] if got else "same"))

    # 6. Live holds: BLACK while Top 7 is held, FX PADS on PROGRAM while a pad is held.
    hand.down("Top 7")
    check(bool(wait_for(lambda: (lambda s: s if s["black"] else None)(state(bin_dir)), timeout=2)), "Top 7 held = BLACK")
    hand.up("Top 7")
    check(bool(wait_for(lambda: (lambda s: s if not s["black"] else None)(state(bin_dir)), timeout=2)), "Top 7 let go = picture back")
    hand.tap("Pad Mode")
    sim.pad("Pad 1", 3000)
    got = wait_for(lambda: (lambda s: s if 3 in s["master_fx"] else None)(state(bin_dir)), timeout=2)
    check(bool(got), "FX PADS pad 1 held: a master effect is on", got["master_fx"].get(3) if got else "")
    sim.pad("Pad 1", 0)
    check(bool(wait_for(lambda: (lambda s: s if 3 not in s["master_fx"] else None)(state(bin_dir)), timeout=2)),
          "FX PADS pad 1 let go: the effect is off")

    # 7. ENTER again: scene 3. Then PLAY focus: speed up layer 1 live, REC = a new variation.
    hand.tap("Enter")
    check(bool(scene_on(progd(), s3)), "ENTER: scene 3 live")
    hand.tap("Group E")
    time.sleep(0.4)
    pg = progd()
    sp0 = state(bin_dir)["speed"].get(pg + "1", 1.0)
    hand.knob(4, 250)
    got = wait_for(lambda: (lambda s: s if abs(s["speed"].get(pg + "1", sp0) - sp0) > 0.2 else None)(state(bin_dir)))
    check(bool(got), "PLAY focus: knob 4 speeds up %s1 live" % pg)
    u0 = show(bin_dir)["undo"]
    hand.tap("Rec")
    got = scene_on(pg, s3, var="b")
    check(bool(got), "REC in PLAY focus = new variation %db" % s3, show(bin_dir)["message"])

    # 8. ERASE = undo the live speed change.
    hand.tap("Erase")
    got = wait_for(lambda: (lambda s: s if s["undo"] < max(u0, 1) or s["redo"] > 0 else None)(show(bin_dir)))
    check(bool(got), "ERASE = undo", "undo %d -> %s" % (u0, got["undo"] if got else "?"))

    # 9. SOLO + pad = HIT on top of PROGRAM; CLIPS mode first.
    hand.tap("Pattern")
    hand.down("Solo")
    sim.pad("Pad %d" % c(1), 3000)
    got = wait_for(lambda: (lambda s: s if s["hit"] is not None else None)(state(bin_dir)), timeout=2)
    check(bool(got), "SOLO + pad = HIT clip on top of PROGRAM", "clip %s" % (got["hit"] if got else None))
    sim.pad("Pad %d" % c(1), 0)
    hand.up("Solo")

    # 10. GO wraps the bank back to the first scene (end of the song).
    hand.tap("Enter")
    got = wait_for(lambda: (lambda s: s if s["deck"].get(progd(), (-1, -1, ""))[1] != s3 else None)(show(bin_dir)))
    check(bool(got), "ENTER after the last scene goes on (bank wraps)",
          "PROGRAM scene pad %s" % (got["deck"][progd()][1] if got else "?"))
    st = state(bin_dir)
    check(st["rej"] == 0, "no Perform command refused by the engine", "rej %d" % st["rej"])


# ------------------------------------------------------------------------------------- the Lightpad (M9)
def lightpad(bin_dir, sim, check):
    """The Block next to the MK2 (hardware-mapping.md §5): MIX fades the decks with soft takeover
    against the wheel, TAKE row, LED feedback, TOUCH FX. Needs weapon_blocks.exe lightpad --sim."""
    import lightpad_sim
    hand = Player(sim)
    pad = lightpad_sim.SimLightpad()
    try:
        if not check(wait_for(pad.engine_active, timeout=10), "weapon_blocks talks to the simulated Lightpad"):
            return
        # The engine's picture reaches the block: TOUCH mode has a red top row.
        def top_red():
            px = pad.leds()
            return all(int(p[0:2], 16) > 60 and int(p[2:4], 16) < 30 for p in px[:15])
        check(bool(wait_for(top_red, timeout=6)), "the engine draws TOUCH on the block (red border)")

        def mode_double_tap():
            for _ in range(2):
                pad.press(True)
                time.sleep(0.04)
                pad.press(False)
                time.sleep(0.06)
            time.sleep(0.5)

        def xf():
            return state(bin_dir)["xfade"]

        def finger(phase, x, y=0.5, z=0.5):
            pad.touch(1, x, y, z, phase)
            time.sleep(0.15)

        # TOUCH: a finger puts a touch effect on PROGRAM (master FX slot 2); lifting takes it off.
        finger("start", 0.05, 0.2)
        got = wait_for(lambda: (lambda s: s if 2 in s["master_fx"] else None)(state(bin_dir)), timeout=2)
        check(bool(got), "TOUCH: a finger puts an effect on PROGRAM", got["master_fx"].get(2) if got else "")
        finger("end", 0.05, 0.2)
        check(bool(wait_for(lambda: (lambda s: s if 2 not in s["master_fx"] else None)(state(bin_dir)), timeout=2)),
              "TOUCH: lifting takes it off")

        # MIX (double-tap Mode).
        mode_double_tap()

        def white_column():
            px = pad.leds()
            want = max(0, min(14, round(xf() * 14)))
            p = px[7 * 15 + want]
            return all(int(p[k:k + 2], 16) > 0xC0 for k in (0, 2, 4))
        check(bool(wait_for(white_column, timeout=3)), "MIX: the block shows the crossfader as a white line")

        # Put the crossfader in the middle with the MK2 wheel (A on air: + = towards B).
        p0 = state(bin_dir)["program"]
        sign = 1 if p0 == "A" else -1
        hand.wheel(15 * sign)
        x0 = xf()
        check(abs(x0 - 0.5) < 0.04, "the wheel set the crossfader to the middle", "%.3f" % x0)

        # Soft takeover: a finger far from the fader does nothing until it crosses it.
        finger("start", 0.1)
        finger("move", 0.3)
        check(abs(xf() - x0) < 0.01, "MIX: a finger away from the fader does not jump it", "%.3f" % xf())
        finger("move", 0.7)
        got = wait_for(lambda: abs(xf() - 0.7) < 0.02, timeout=2)
        check(bool(got), "MIX: once the finger crosses the fader it drives it (0.70)", "%.3f" % xf())
        finger("move", 0.8)
        check(bool(wait_for(lambda: abs(xf() - 0.8) < 0.02, timeout=2)), "MIX: the fader follows the finger (0.80)")

        # The wheel moves the fader under the resting finger: the finger lets go.
        hand.wheel(-6 * sign)
        x1 = xf()
        check(abs(x1 - 0.6) < 0.03, "the MK2 wheel moved the fader under the finger", "%.3f" % x1)
        finger("move", 0.9)
        time.sleep(0.2)
        check(abs(xf() - x1) < 0.01, "MIX: after the wheel moved, the finger does not jump the fader back",
              "%.3f" % xf())
        finger("move", x1 + 0.01)
        finger("move", x1 + 0.1)
        check(bool(wait_for(lambda: abs(xf() - (x1 + 0.1)) < 0.02, timeout=2)),
              "MIX: the finger catches the fader again where the wheel left it")
        # A swipe all the way fades the decks: PROGRAM flips.
        prog = state(bin_dir)["program"]
        for v in (0.9, 0.95, 1.0) if sign > 0 else (0.5, 0.2, 0.0):
            finger("move", v)
        got = wait_for(lambda: (lambda s: s if s["program"] != prog else None)(state(bin_dir)), timeout=3)
        check(bool(got), "MIX: a swipe to the end fades PREVIEW in (PROGRAM %s -> %s)" % (prog, "B" if prog == "A" else "A"))
        finger("end", 1.0 if sign > 0 else 0.0)

        # The top row = TAKE.
        prog = state(bin_dir)["program"]
        finger("start", 0.5, 0.02)
        finger("end", 0.5, 0.02)
        got = wait_for(lambda: (lambda s: s if s["program"] != prog and not s["take"] else None)(state(bin_dir)), timeout=5)
        check(bool(got), "MIX: the top row runs a TAKE")
        mode_double_tap()  # SCRUB
        mode_double_tap()  # TOUCH again
        st = state(bin_dir)
        check(st["rej"] == 0, "no Perform command refused (MK2 + Lightpad)", "rej %d" % st["rej"])
    finally:
        pad.release_all()
        pad.close()


# ------------------------------------------------------------------------------------- input -> glass
class Glass:
    """Reads the output window from the composed desktop (GDI StretchBlt of the whole client
    area down to 32 x 18 samples), so a moving test clip never looks like black."""

    W, H = 32, 18

    def __init__(self):
        self.ok = False
        if os.name != "nt":
            return
        u, g = ctypes.windll.user32, ctypes.windll.gdi32
        self.u, self.g = u, g
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except (AttributeError, OSError):
            u.SetProcessDPIAware()
        u.FindWindowW.restype = ctypes.c_void_p
        self.hwnd = u.FindWindowW("HimmelOutput", None)
        if not self.hwnd:
            return

        class RECT(ctypes.Structure):
            _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]

        class POINT(ctypes.Structure):
            _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

        rc = RECT()
        u.GetClientRect(ctypes.c_void_p(self.hwnd), ctypes.byref(rc))
        pt = POINT(0, 0)
        u.ClientToScreen(ctypes.c_void_p(self.hwnd), ctypes.byref(pt))
        self.x, self.y, self.w, self.h = pt.x + 4, pt.y + 4, max(8, rc.r - 8), max(8, rc.b - 8)
        # Keep the window above others without taking the focus (SWP_NOMOVE | NOSIZE | NOACTIVATE).
        u.SetWindowPos(ctypes.c_void_p(self.hwnd), ctypes.c_void_p(-1), 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0010)
        time.sleep(0.3)
        u.GetDC.restype = ctypes.c_void_p
        g.CreateCompatibleDC.restype = ctypes.c_void_p
        g.CreateCompatibleBitmap.restype = ctypes.c_void_p
        self.sdc = u.GetDC(None)
        self.mdc = g.CreateCompatibleDC(ctypes.c_void_p(self.sdc))
        self.bmp = g.CreateCompatibleBitmap(ctypes.c_void_p(self.sdc), self.W, self.H)
        g.SelectObject(ctypes.c_void_p(self.mdc), ctypes.c_void_p(self.bmp))
        g.SetStretchBltMode(ctypes.c_void_p(self.mdc), 3)  # COLORONCOLOR: plain samples, fast
        self.buf = (ctypes.c_uint32 * (self.W * self.H))()
        self.ok = True

    def sample(self):
        """(mean, brightest) of the samples, 0..255."""
        g = self.g
        g.StretchBlt(ctypes.c_void_p(self.mdc), 0, 0, self.W, self.H, ctypes.c_void_p(self.sdc), self.x, self.y,
                     self.w, self.h, 0x00CC0020)
        g.GetBitmapBits(ctypes.c_void_p(self.bmp), ctypes.sizeof(self.buf), self.buf)
        tot = top = 0
        for v in self.buf:
            lv = max((v >> 16) & 255, (v >> 8) & 255, v & 255)
            tot += lv
            top = max(top, lv)
        return tot / len(self.buf), top

    def close(self):
        if self.ok:
            self.u.SetWindowPos(ctypes.c_void_p(self.hwnd), ctypes.c_void_p(-2), 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0010)
            self.g.DeleteObject(ctypes.c_void_p(self.bmp))
            self.g.DeleteDC(ctypes.c_void_p(self.mdc))
            self.u.ReleaseDC(None, ctypes.c_void_p(self.sdc))


def latency(bin_dir, sim, check, runs):
    """Top 7 (BLACK while held) flips the output between picture and black; the time from the
    report leaving the simulator to the first sampled desktop frame that shows the change.
    Judged on the mean brightness (the status overlay stays lit on a black output)."""
    glass = Glass()
    if not check(glass.ok, "output window found for the glass measurement"):
        return None
    try:
        hand = Player(sim)
        hand.tap("Pattern")  # CLIPS
        if glass.sample()[0] < 20:  # nothing bright on PROGRAM: put clip 1 live
            hand.tap("Group E")
            time.sleep(0.4)
            hand.pad(1)
            time.sleep(0.8)
        t = time.perf_counter()
        for _ in range(20):
            glass.sample()
        poll_ms = (time.perf_counter() - t) / 20 * 1000.0
        base = min(glass.sample()[0] for _ in range(10))
        if not check(base >= 20, "the output shows a picture to black out", "mean brightness %.0f" % base):
            return None
        sim.press("Top 7", True)
        time.sleep(0.3)
        dark = max(glass.sample()[0] for _ in range(10))
        sim.press("Top 7", False)
        time.sleep(0.3)
        lo, hi = dark + (base - dark) * 0.25, dark + (base - dark) * 0.6
        print("  picture mean %.0f, black mean %.1f: black below %.1f, picture above %.1f" % (base, dark, lo, hi))
        samples = {"black": [], "picture": []}
        for i in range(runs):
            down = i % 2 == 0
            sent0 = sim.sent_change_t
            sim.press("Top 7", down)
            end = time.perf_counter() + 1.0
            seen = None
            while time.perf_counter() < end:
                mean = glass.sample()[0]
                if sim.sent_change_t != sent0 and (mean <= lo if down else mean >= hi):
                    seen = time.perf_counter()
                    break
            if seen is not None:
                samples["black" if down else "picture"].append((seen - sim.sent_change_t) * 1000.0)
            else:
                print("  (run %d: no %s seen within 1 s, mean %.1f)" % (i + 1, "black" if down else "picture", mean))
            time.sleep(0.25 + (i % 5) * 0.013)  # not in step with the frame clock
        sim.press("Top 7", False)
        allv = sorted(samples["black"] + samples["picture"])
        if not check(len(allv) >= runs * 0.8, "input -> glass measured", "%d/%d samples" % (len(allv), runs)):
            return None
        med = statistics.median(allv)
        res = {"runs": len(allv), "median_ms": round(med, 1), "min_ms": round(allv[0], 1), "max_ms": round(allv[-1], 1),
               "p90_ms": round(allv[max(0, int(len(allv) * 0.9) - 1)], 1), "poll_ms": round(poll_ms, 2),
               "to_black_ms": [round(x, 1) for x in samples["black"]],
               "to_picture_ms": [round(x, 1) for x in samples["picture"]]}
        print("  input -> glass: median %.1f ms  min %.1f  p90 %.1f  max %.1f  (%d runs, desktop poll %.2f ms)" % (
            med, res["min_ms"], res["p90_ms"], res["max_ms"], res["runs"], poll_ms))
        return res
    finally:
        glass.close()


def main():
    args = sys.argv[1:]
    attach = "--attach" in args
    bin_dir = os.path.join(ROOT, "build", "bin", "Release")
    if "--bin" in args:
        bin_dir = os.path.abspath(args[args.index("--bin") + 1])
    runs = int(args[args.index("--runs") + 1]) if "--runs" in args else 20
    with open(MAP_PATH, encoding="utf-8") as f:
        mapping = json.load(f)

    stack = None
    if not attach:
        exe = os.path.join(bin_dir, "himmel.exe")
        # --no-show-files: a test run never opens or autosaves the owner's project (SHOW M3, D109)
        weapons = ["--no-weapons"] if "--no-lightpad" in args else ["--weapons", "lightpad"]
        print("starting", exe, "--sim --no-ui", " ".join(weapons), "-- --no-show-files")
        stack = subprocess.Popen([exe, "--sim", "--no-ui"] + weapons + ["--", "--no-show-files"], cwd=ROOT,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    check = Check()
    sim = TimedSim(mapping)
    runner = Runner(sim)
    result = None
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
        print("basic")
        basic(bin_dir, sim, check)
        if "--basic" not in args:
            print("song (from the MK2 alone)")
            song(bin_dir, sim, check)
            if "--no-lightpad" not in args:
                print("Lightpad (M9)")
                lightpad(bin_dir, sim, check)
            if "--no-latency" not in args:
                print("input -> glass")
                result = latency(bin_dir, sim, check, runs)
        st = state(bin_dir)
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
    if result:
        out = os.path.join(ROOT, "build", "mk2_e2e_latency.json")
        with open(out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
        print("  latency samples written to", out)
    print("PASS" if check.failed == 0 else "FAILED: %d check(s)" % check.failed)
    return 0 if check.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
