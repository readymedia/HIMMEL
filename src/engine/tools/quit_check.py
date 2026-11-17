#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""quit_check.py - QUIT check: a confirmed SystemQuit (74) stops every child and
the supervisor exits 0 (command-protocol.md §4d).

    python src\\engine\\tools\\quit_check.py [--exe-dir build\\bin\\Release] [--hw]

It refuses to run while any himmel process is running (one stack per machine).
Then it:
1. starts `himmel.exe --sim` (simulated weapons and MK2, so no hardware is
   touched; `--hw` drops `--sim`) in its own console window, with the renderer
   in a small window and `--no-show-files`;
2. waits until every child is up (the supervisor's child processes);
3. checks that an unconfirmed quit is refused (`himmel_cmd quit` exits 1, and
   `raw 74` with f = 0 is refused) and that Himmel still runs;
4. sends `himmel_cmd quit --confirm`;
5. checks that himmel.exe exits with code 0 within 15 s, that every child
   process it had is gone, and that its log has "quit requested".
Windows only, stdlib only.
"""
import argparse
import csv
import datetime
import io
import os
import subprocess
import sys
import time


def himmel_processes():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    names = []
    for row in csv.reader(io.StringIO(out)):
        if row and (row[0].lower().startswith("himmel") or row[0].lower().startswith("weapon_")
                    or row[0].lower() == "mk2_controller.exe"):
            names.append((row[0], int(row[1])))
    return names


def children(pid):
    cmd = ("Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d' | "
           "ForEach-Object { '{0},{1}' -f $_.ProcessId, $_.Name }") % pid
    out = subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True).stdout
    kids = []
    for line in out.splitlines():
        if "," in line:
            p, n = line.strip().split(",", 1)
            if n.lower() != "conhost.exe":
                kids.append((int(p), n))
    return kids


def alive(pid):
    out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return str(pid) in out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe-dir", default=os.path.join("build", "bin", "Release"))
    ap.add_argument("--hw", action="store_true", help="real devices (no --sim)")
    args = ap.parse_args()
    exe_dir = os.path.abspath(args.exe_dir)
    cmd_exe = os.path.join(exe_dir, "himmel_cmd.exe")

    running = himmel_processes()
    if running:
        print("FAIL: a Himmel stack is running already (one per machine): %s" % running)
        return 2

    argv = [os.path.join(exe_dir, "himmel.exe")]
    if not args.hw:
        argv.append("--sim")
    argv += ["--", "--outputs", "1", "--size", "480x270", "--no-show-files"]
    print("start:", " ".join(argv))
    # Its own console: the supervisor's quit raises Ctrl+C in its console, never in ours.
    sup = subprocess.Popen(argv, cwd=exe_dir, creationflags=subprocess.CREATE_NEW_CONSOLE)
    ok = True
    try:
        kids = []
        t0 = time.time()
        while time.time() - t0 < 20:
            time.sleep(0.5)
            kids = children(sup.pid)
            names = {n.lower() for _, n in kids}
            if "himmel_render.exe" in names and subprocess.run([cmd_exe, "hello"], capture_output=True).returncode == 0:
                break
        time.sleep(3)  # let every driver and the renderer settle
        kids = children(sup.pid)
        print("children: %s" % ", ".join("%s (%d)" % (n, p) for p, n in kids))
        if not any(n.lower() == "himmel_render.exe" for _, n in kids):
            print("FAIL: the renderer did not start")
            return 1

        r = subprocess.run([cmd_exe, "quit"], capture_output=True, text=True)
        print("quit without --confirm: exit %d (%s)" % (r.returncode, r.stderr.strip()))
        ok &= r.returncode == 1
        subprocess.run([cmd_exe, "raw", "74", "255", "255", "255", "255", "0", "0"], capture_output=True)
        time.sleep(1.0)
        ok &= sup.poll() is None
        print("unconfirmed quit refused, Himmel still runs: %s" % (sup.poll() is None))

        t_quit = time.time()
        r = subprocess.run([cmd_exe, "quit", "--confirm"], capture_output=True, text=True)
        print("quit --confirm: exit %d" % r.returncode)
        try:
            code = sup.wait(timeout=15)
        except subprocess.TimeoutExpired:
            print("FAIL: himmel.exe still runs 15 s after quit")
            return 1
        took = time.time() - t_quit
        print("himmel.exe exited with %d after %.2f s" % (code, took))
        ok &= code == 0
        time.sleep(0.5)
        left = [(p, n) for p, n in kids if alive(p)]
        print("children still alive: %s" % (left or "none"))
        ok &= not left
        log = os.path.join(exe_dir, "logs", "himmel-%s.log" % datetime.date.today().isoformat())
        if os.path.exists(log):
            tail = open(log, encoding="utf-8", errors="replace").read().splitlines()[-40:]
            seen = any("quit requested" in line for line in tail)
            print("log has 'quit requested': %s" % seen)
            for line in tail:
                if "quit" in line or "stopped" in line or "bye" in line:
                    print("  " + line)
            ok &= seen
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        if sup.poll() is None:
            print("cleaning up: stopping the supervisor this check started (pid %d)" % sup.pid)
            subprocess.run(["taskkill", "/PID", str(sup.pid), "/T", "/F"], capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
