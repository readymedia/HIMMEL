# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""GUI wave 7 end-to-end: the first-time user's whole flow, through himmel_ui's HTTP API.

The owner's test (2026-11-17) stopped at "I found no way to get the clips onto layers". This script
walks the same flow the web page does, with the same requests the page sends:

  start a stack on a temp show root -> list the folder (GET /fs/list) -> import it (POST /library/cmd
  Import) -> watch the progress in the "library" event (n of m) -> map library rows to engine catalog
  ids (by ready order + name, like gui/flows.js) -> LayerClip onto PREVIEW layers -> Take -> FxSet ->
  clip editor (in / out, cue, beat loop) -> SceneSave -> Project SaveAs -> kill himmel_ui mid-show and
  check that nothing on air changed -> SystemQuit.

Never touches the owner's files: everything lives under a temp folder (--show-root), which is deleted
at the end (--keep keeps it). Only one Himmel stack per machine: the script refuses to start while
tasklist shows any himmel process.

  python tools/gui_flow_e2e.py              # start himmel.exe --sim -- --show-root <temp>, test, quit
  python tools/gui_flow_e2e.py --keep       # keep the temp folder (logs, project, library)
  python tools/gui_flow_e2e.py --port 8790  # the bridge port (himmel.exe starts himmel_ui on 8790)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin", "Release")


class Check:
    def __init__(self):
        self.failed = 0
        self.n = 0

    def __call__(self, ok, what):
        self.n += 1
        print(("  ok    " if ok else "  FAIL  ") + what)
        if not ok:
            self.failed += 1
        return ok


class Bridge:
    def __init__(self, port):
        self.base = "http://127.0.0.1:%d" % port

    def get(self, path, timeout=5):
        with urllib.request.urlopen(self.base + path, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))

    def post(self, path, body, timeout=5):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode("utf-8"),
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))

    def cmd(self, c):
        return self.post("/cmd", c)

    def state(self):
        try:
            return self.get("/state", timeout=2)
        except Exception:
            return None

    def event(self, name, timeout=5.0):
        """The next SSE event `name` from /events (a fresh connection; the bridge sends every panel at once)."""
        end = time.time() + timeout
        try:
            with urllib.request.urlopen(self.base + "/events", timeout=timeout) as r:
                ev = None
                while time.time() < end:
                    line = r.readline().decode("utf-8", "replace").rstrip("\r\n")
                    if line.startswith("event: "):
                        ev = line[7:]
                    elif line.startswith("data: ") and ev == name:
                        return json.loads(line[6:])
        except Exception:
            return None
        return None


def wait_for(fn, timeout, step=0.25):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


def himmel_processes():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and (parts[0].lower().startswith("himmel") or parts[0].lower() in
                                ("mk2_controller.exe", "fake_engine.exe")):
            rows.append((parts[0], int(parts[1])))
    return rows


def child_pid(name, parent):
    """PID of process `name` whose parent is `parent` (so we never touch another stack)."""
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='%s'\" | Where-Object { $_.ParentProcessId -eq %d } | "
          "Select-Object -ExpandProperty ProcessId" % (name, parent))
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None


# ---- library row -> engine catalog id (the same rule as gui/flows.js, GUESS documented there)
def trunc(s, n):
    b = s.encode("utf-8")
    if len(b) <= n:
        return s
    while n > 0 and (b[n] & 0xC0) == 0x80:
        n -= 1
    return b[:n].decode("utf-8", "ignore")


def catalog_map(rows, cat):
    names = [c["name"].lower() for c in cat]
    ready = sorted([r for r in rows if r["state"] == "ready"], key=lambda r: r.get("order", 0))
    count = {}
    for r in ready:
        count[r["name"].lower()] = count.get(r["name"].lower(), 0) + 1
    out, cursor = {}, 0
    for r in ready:
        base = trunc(r["name"], 35)
        cands = [trunc(r["name"], 47).lower()] + [trunc(base + (" (lib)" if k == 1 else " (lib %d)" % k), 47).lower() for k in range(1, 6)]
        hit = next((i for i in range(cursor, len(names)) if names[i] in cands), -1)
        if hit < 0:
            continue
        if names[hit] == cands[0] and count[r["name"].lower()] < 2:
            hit = next((i for i in range(hit + 1, len(names)) if names[i] != cands[0] and names[i] in cands), hit)
        out[r["id"]] = hit
        cursor = hit + 1
    return out


def make_sources(src):
    os.makedirs(src, exist_ok=True)
    test = os.path.join(ROOT, "media", "test")
    if not os.path.isdir(test) or not any(f.endswith(".mov") for f in os.listdir(test)):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_test_clips.py")], check=False)
    n = 0
    for f in sorted(os.listdir(test)) if os.path.isdir(test) else []:
        if f.endswith(".mov"):
            shutil.copy(os.path.join(test, f), os.path.join(src, f))
            n += 1
    ff = shutil.which("ffmpeg")
    if ff:   # a few H.264 clips, so the import really converts (and the progress shows)
        for k, lav in enumerate(["testsrc2=s=640x360:r=30", "mandelbrot=s=640x360:r=25", "cellauto=s=640x360:r=30"]):
            subprocess.run([ff, "-v", "error", "-y", "-f", "lavfi", "-i", lav, "-t", "5", "-pix_fmt", "yuv420p",
                            "-c:v", "libx264", os.path.join(src, "flow_%d.mp4" % (k + 1))], check=False)
            n += 1
    return n


def main():
    args = sys.argv[1:]
    port = int(args[args.index("--port") + 1]) if "--port" in args else 8790
    keep = "--keep" in args
    busy = himmel_processes()
    if busy:
        print("Another Himmel stack runs (%s): one stack per machine. Stop it first." % ", ".join(n for n, _ in busy))
        return 2
    tmp = tempfile.mkdtemp(prefix="himmel_gui_flow_")
    src = os.path.join(tmp, "My Clips")
    n_src = make_sources(src)
    print("temp show root", tmp, "·", n_src, "source files")
    exe = os.path.join(BIN, "himmel.exe")
    stack = subprocess.Popen([exe, "--sim", "--no-weapons", "--", "--show-root", tmp, "--size", "960x540"], cwd=ROOT,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    ck = Check()
    br = Bridge(port)
    try:
        print("start")
        st = wait_for(lambda: (lambda s: s if s and s.get("bridge") and s["bridge"].get("engine") == "ok" and s.get("live") else None)(br.state()), 30)
        if not ck(bool(st), "himmel_ui answers and the engine is ok"):
            return 1
        lib = wait_for(lambda: (lambda e: e if e and e.get("present") and e.get("alive") else None)(br.event("library")), 20)
        ck(bool(lib), "the import worker is alive (library block)")
        ck(lib and lib.get("root", "").lower().startswith(tmp.lower()), "the library is under the temp show root")
        n_cat0 = len(st["catalog"]["clips"])

        print("1. import a folder")
        ls = br.get("/fs/list?path=" + urllib.parse.quote(src))
        ck(ls.get("ok") and ls.get("importable") == n_src, "GET /fs/list sees %d importable files" % n_src)
        top = br.get("/fs/list?path=")
        ck(top.get("ok") and len(top.get("drives", [])) > 0, "GET /fs/list without a path lists the drives")
        disk = br.get("/library/disk")
        ck(disk.get("ok") and disk.get("free", 0) > 0, "GET /library/disk: %.1f GB free" % (disk.get("free", 0) / 1e9))
        r = br.post("/library/cmd", {"type": "Import", "path": src})
        ck(r.get("ok"), "Import accepted by the bridge (seq %s)" % r.get("seq"))
        reply = wait_for(lambda: next((x for x in ((br.event("library") or {}).get("cmd") or {}).get("replies", []) if x["seq"] == r.get("seq")), None), 15)
        ck(reply and reply["status"] == "Accepted" and reply["count"] == n_src, "the worker queued %s of %d" % (reply and reply["count"], n_src))

        print("2. watch the progress")
        seen_jobs, t0, last = set(), time.time(), None
        while time.time() - t0 < 180:
            e = br.event("library", 3) or {}
            if e.get("job", {}).get("name"):
                seen_jobs.add(e["job"]["name"])
            last = e
            if e and e.get("queued", 1) == 0 and e.get("working", 1) == 0 and e.get("ready_total", 0) + e.get("failed_total", 0) >= n_src:
                break
            time.sleep(0.3)
        ck(last and last.get("ready_total") == n_src and last.get("failed_total") == 0,
           "all %d imported in %.0f s (ready %s, failed %s)" % (n_src, time.time() - t0, last and last.get("ready_total"), last and last.get("failed_total")))
        ck(len(seen_jobs) >= 2, "the current file was shown during the import (%d names seen)" % len(seen_jobs))

        print("3. library clips -> layers")
        st = wait_for(lambda: (lambda s: s if s and len(s["catalog"]["clips"]) >= n_cat0 + n_src else None)(br.state()), 15) or br.state()
        rows = br.get("/library/clips")["clips"]
        ck(all("order" in x for x in rows), "library rows carry their ready order")
        cmap = catalog_map(rows, st["catalog"]["clips"])
        ck(len(cmap) == n_src, "every ready row maps to an engine catalog id (%d of %d)" % (len(cmap), n_src))
        ids = list(cmap.values())
        names_ok = all(st["catalog"]["clips"][cmap[x["id"]]]["name"].replace(" (lib)", "").lower().startswith(x["name"].lower()[:20]) for x in rows if x["id"] in cmap)
        ck(names_ok, "the mapped catalog names match the rows")
        live = st["live"]
        pd = 1 - live["program_deck"]
        br.cmd([{"type": "LayerClip", "deck": pd, "layer": 0, "value": ids[0]}, {"type": "LayerClip", "deck": pd, "layer": 1, "value": ids[1]},
                {"type": "Focus", "deck": pd, "layer": 0}])
        st = wait_for(lambda: (lambda s: s if s["live"]["deck"][pd]["layer"][0]["clip"] == ids[0] and s["live"]["deck"][pd]["layer"][1]["clip"] == ids[1] else None)(br.state()), 5)
        ck(bool(st), "two library clips are on PREVIEW A and B")

        print("4. TAKE + an effect")
        br.cmd({"type": "Take", "value": 500})
        st = wait_for(lambda: (lambda s: s if s["live"]["program_deck"] == pd and not s["live"].get("take_active") else None)(br.state()), 6)
        ck(bool(st), "TAKE: the clips are on PROGRAM")
        eff = next((e for e in st["catalog"]["effects"] if e["name"].lower() == "kaleido"), None) or next(e for e in st["catalog"]["effects"] if e.get("kind") != "generator")
        br.cmd({"type": "FxSet", "deck": pd, "layer": 0, "slot": 0, "value": eff["id"]})
        st = wait_for(lambda: (lambda s: s if s["live"]["deck"][pd]["layer"][0]["fx"][0]["effect"] == eff["id"] else None)(br.state()), 4)
        ck(bool(st), "%s is on PROGRAM E slot 1" % eff["name"])

        print("5. the clip editor")
        t = {"deck": pd, "layer": 0}
        br.cmd([{"type": "LayerSet", **t, "param": "In", "value": 0.1}, {"type": "LayerSet", **t, "param": "Out", "value": 0.8},
                {"type": "LayerCue", **t, "cue": 2, "value": "Set"}, {"type": "LayerBeatLoop", **t, "value": 1},
                {"type": "LayerSet", **t, "param": "Mode", "value": "PingPong"}, {"type": "LayerSpeed", **t, "value": 0.5}])
        tr = wait_for(lambda: (lambda e: e if e and abs(e["layer"][pd][0]["in"] - 0.1) < 1e-3 and e["layer"][pd][0]["beat_loop"] else None)(br.event("transport")), 5)
        ck(bool(tr) and abs(tr["layer"][pd][0]["out"] - 0.8) < 1e-3 and tr["layer"][pd][0]["cue"][2] >= 0 and tr["layer"][pd][0]["mode"] == "PingPong",
           "in 0.1 / out 0.8, cue 3 set, beat loop on, PING-PONG")
        st = br.state()
        ck(abs(st["live"]["deck"][pd]["layer"][0]["speed"] - 0.5) < 1e-3, "speed 0.5x")
        br.cmd({"type": "LayerBeatLoop", **t, "value": 0})

        print("6. save a scene and the project")
        sh0 = br.event("show") or {}
        br.cmd({"type": "SceneSave", "deck": pd, "value": -1})
        sh = wait_for(lambda: (lambda e: e if e and e.get("scene_count", 0) > sh0.get("scene_count", 0) else None)(br.event("show")), 6)
        ck(bool(sh), "SceneSave: scene count %s -> %s" % (sh0.get("scene_count"), sh and sh["scene_count"]))
        br.cmd({"type": "Project", "param": "SaveAs", "text": "FlowTest"})
        sh = wait_for(lambda: (lambda e: e if e and e.get("files", {}).get("path", "").lower().endswith("flowtest.himmel") and not e.get("dirty") else None)(br.event("show")), 10)
        ck(bool(sh) and sh["files"]["path"].lower().startswith(tmp.lower()), "Project SaveAs FlowTest under the temp root, clean")

        print("7. kill himmel_ui mid-show")
        render = child_pid("himmel_render.exe", stack.pid)
        ui = child_pid("himmel_ui.exe", stack.pid)
        ck(bool(render and ui), "found this stack's renderer (%s) and bridge (%s)" % (render, ui))
        before = br.state()["live"]
        subprocess.run(["taskkill", "/F", "/PID", str(ui)], capture_output=True)
        # The supervisor restarts it within ~0.25 s, so "gone" may not be observable; the new PID proves it.
        back = wait_for(lambda: (lambda s: s if s and s.get("live") and s["bridge"]["engine"] == "ok" else None)(br.state()), 15)
        ck(bool(back), "the supervisor restarted himmel_ui and it answers")
        ui2 = child_pid("himmel_ui.exe", stack.pid)
        ck(ui2 and ui2 != ui, "a new himmel_ui (%s)" % ui2)
        ck(child_pid("himmel_render.exe", stack.pid) == render, "the renderer PID is the same (the show never stopped)")
        after = back["live"]

        def onair(L):
            return [L["program_deck"], L["crossfader"], L["black"],
                    [[(l["clip"], l["opacity"], l["blend"], l["mute"], l["speed"], [(f["effect"], f.get("mix"), f.get("param")) for f in l["fx"]])
                      for l in d["layer"]] for d in L["deck"]], L["master_fx"]]
        ck(onair(before) == onair(after), "what is on air is identical before and after (clips, mix, effects)")
        rows2 = br.get("/library/clips")
        ck(rows2.get("ok") and len(rows2["clips"]) == n_src, "the new bridge lists the library")

        print("8. quit")
        br.cmd({"type": "SystemQuit", "value": 1})
        try:
            stack.wait(15)
        except subprocess.TimeoutExpired:
            pass
        ck(stack.poll() is not None, "SystemQuit stopped the stack (exit %s)" % stack.poll())
    finally:
        if stack.poll() is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(stack.pid)], capture_output=True)
        time.sleep(0.5)
        if keep:
            print("kept", tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print("PASS (%d checks)" % ck.n if ck.failed == 0 else "FAILED: %d of %d check(s)" % (ck.failed, ck.n))
    return 0 if ck.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
