#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
M7 measurement: import a mixed set of files while a show plays, and count the
renderer's late frames (docs/measurements/2026-11-17-ingest-during-show.md).

    python tools/ingest_measure.py [--set DIR] [--baseline 30] [--timeout 1200] [--keep]

1. Refuses to run when another Himmel stack runs (one stack per machine).
2. Starts build/bin/Release/himmel.exe --no-controller --no-weapons --no-ui
   --no-audio -- --show-root TMP --size 960x540 (3 windowed outputs; the test
   root keeps the owner's projects and library untouched), with the ingest
   worker importing into TMP/Library.
3. Plays 3 HAP layers (clips 0-2 of the media/ folder) through himmel_cmd.
4. Baseline: late frames over --baseline seconds with nothing to import.
5. Copies the set (default: build/ingest_test, made by make_ingest_test_set.py)
   into TMP/Library/originals and waits until every item is ready or failed.
   Halfway it loads a freshly imported clip on deck B layer 2 (the catalog
   refresh, live).
6. Prints late frames, throttle pauses, timings; stops the stack.

Reads Local\\HimmelEngine and Local\\HimmelLibrary directly (mmap).
"""

import argparse
import mmap
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin", "Release")


def read_engine():
    try:
        m = mmap.mmap(-1, 64, tagname="Local\\HimmelEngine", access=mmap.ACCESS_READ)
    except OSError:
        return None
    b = m[:64]
    m.close()
    magic, _ver, _pid, _stop, hb, frames, outputs, fps, avg, mx, late = struct.unpack_from("<IIIIQQIIIII", b)
    if magic != 0x4C4D4948:
        return None
    return {"frames": frames, "late": late, "fps": fps / 100.0, "outputs": outputs}


def read_library():
    try:
        m = mmap.mmap(-1, 72, tagname="Local\\HimmelLibrary", access=mmap.ACCESS_READ)
    except OSError:
        return None
    b = m[:72]
    m.close()
    magic = struct.unpack_from("<I", b, 0)[0]
    if magic != 0x424C4D48:
        return None
    (pid, stop, hb, ready_seq, mode, paused, queued, working, ready, failed, pauses, _js) = struct.unpack_from("<IIQQIIIIIIII", b, 16)
    return {"mode": mode, "paused": paused, "queued": queued, "working": working, "ready": ready, "failed": failed,
            "pauses": pauses, "ready_seq": ready_seq}


def cmd(*args):
    r = subprocess.run([os.path.join(BIN, "himmel_cmd.exe")] + [str(a) for a in args], capture_output=True, text=True)
    return r.returncode, r.stdout


def clip_count():
    rc, out = cmd("catalog")
    first = out.splitlines()[0].split() if rc == 0 and out else ["0"]
    return int(first[0]) if first and first[0].isdigit() else 0


def others_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return [n for n in ("himmel.exe", "himmel_render.exe", "himmel_ingest.exe") if n in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default=os.path.join(ROOT, "build", "ingest_test"))
    ap.add_argument("--baseline", type=float, default=30)
    ap.add_argument("--timeout", type=float, default=1200)
    ap.add_argument("--keep", action="store_true", help="keep the temp library")
    a = ap.parse_args()
    busy = others_running()
    if busy:
        sys.exit("another Himmel stack is running (%s) - one stack per machine" % ", ".join(busy))
    if not os.path.isdir(a.set) or not os.listdir(a.set):
        sys.exit(f"no test set in {a.set} (python tools/make_ingest_test_set.py --out {a.set})")
    items = len(os.listdir(a.set))
    tmp = tempfile.mkdtemp(prefix="himmel_ingest_measure_")
    proc = subprocess.Popen([os.path.join(BIN, "himmel.exe"), "--no-controller", "--no-weapons", "--no-ui", "--no-audio",
                             "--", "--show-root", tmp, "--size", "960x540"],
                            cwd=BIN, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result = 1
    try:
        t0 = time.time()
        while time.time() - t0 < 30:
            e = read_engine()
            if e and e["frames"] > 60 and cmd("state")[0] == 0 and read_library():
                break
            time.sleep(0.5)
        else:
            raise SystemExit("the stack did not come up")
        clips_before = clip_count()
        cmd("clip", "a", 1, 0, "--", "clip", "a", 2, 1, "--", "opacity", "a", 2, 0.6, "--", "clip", "b", 1, 2,
            "--", "xfade", 0.5)
        time.sleep(3)
        e0 = read_engine()
        time.sleep(a.baseline)
        e1 = read_engine()
        base_late = e1["late"] - e0["late"]
        base_frames = e1["frames"] - e0["frames"]
        print(f"baseline: {base_frames} frames in {a.baseline:.0f} s, {base_late} late, {e1['fps']:.1f} fps, "
              f"{e1['outputs']} outputs, {clips_before} clips in the catalog")

        dest = os.path.join(tmp, "Library", "originals")
        t_copy = time.time()
        for name in sorted(os.listdir(a.set)):
            s = os.path.join(a.set, name)
            (shutil.copytree if os.path.isdir(s) else shutil.copy2)(s, os.path.join(dest, name))
        print(f"copied {items} items into the watch folder in {time.time() - t_copy:.1f} s")
        e_start = read_engine()
        t_start = time.time()
        loaded_new = False
        max_paused = 0
        last_print = 0
        while time.time() - t_start < a.timeout:
            lib = read_library() or {}
            done = lib.get("ready", 0) + lib.get("failed", 0)
            if not loaded_new and lib.get("ready", 0) >= items // 2:
                n = clip_count()
                if n > clips_before:
                    rc2, _ = cmd("clip", "b", 2, n - 1)
                    time.sleep(2)
                    rc3, st = cmd("state")
                    b2 = [line.strip() for line in st.splitlines() if line.strip().startswith("B2")]
                    print(f"  loaded new library clip {n - 1} on B2 (exit {rc2}); catalog now {n} clips; {b2[:1]}")
                    loaded_new = True
            if time.time() - last_print > 15:
                e = read_engine()
                print(f"  {time.time() - t_start:5.0f} s: {lib.get('ready', 0)} ready, {lib.get('failed', 0)} failed, "
                      f"{lib.get('queued', 0)} queued, mode {lib.get('mode')}, paused {lib.get('paused')}, "
                      f"pauses {lib.get('pauses')}, late +{e['late'] - e_start['late']}")
                last_print = time.time()
            if done >= items:
                break
            time.sleep(0.5)
        e_end = read_engine()
        lib = read_library() or {}
        dur = time.time() - t_start
        late = e_end["late"] - e_start["late"]
        frames = e_end["frames"] - e_start["frames"]
        clips_after = clip_count()
        print(f"import: {lib.get('ready', 0)} ready, {lib.get('failed', 0)} failed of {items} in {dur:.0f} s; "
              f"{frames} frames, {late} late; throttle pauses {lib.get('pauses', 0)}; catalog {clips_before} -> {clips_after}")
        result = 0 if late == 0 and lib.get("ready", 0) + lib.get("failed", 0) >= items else 1
        print("RESULT:", "PASS (no late frame)" if result == 0 else "FAIL")
    finally:
        try:
            os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)
    return result


if __name__ == "__main__":
    sys.exit(main())
