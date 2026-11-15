# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""Entrancer simulator page: the drawn panel (weapons/entrancer.weapon.json) in
the browser, played with the mouse or with the real Lightpad Block as its pad.
The simulated unit (tools/entrancer_sim.py) sends its MIDI to the engine.

Run:   python tools/entrancer_live.py [--port 8770] [--engine-port 8768] [--lightpad]
Then:  open http://127.0.0.1:8770
And:   build\\bin\\Release\\himmel_render.exe --outputs 1 --size 960x540
       (or himmel.exe): the engine listens for the unit on UDP 127.0.0.1:8768.

--lightpad takes the real Lightpad Block at start (also a button on the page).
Close ROLI software and the Lightpad live view first: the MIDI ports are exclusive.
Stdlib only.
"""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import entrancer_sim  # noqa: E402

PROFILE = os.path.join(os.path.dirname(HERE), "weapons", "entrancer.weapon.json")
PAGE = os.path.join(HERE, "entrancer_live.html")


def _arg(name, default):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


class Live:
    def __init__(self, engine_port):
        self.unit = entrancer_sim.Entrancer(engine_port)
        self.block = None
        self.host = None
        self.lp_error = ""
        self.lock = threading.Lock()
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while not self.unit.closed:
            try:
                self.unit.step(time.time())
            except Exception as ex:   # a bug in the unit must never stop the loop
                self.unit.show("Err", 2)
                print("entrancer step:", ex)
            time.sleep(0.004)

    def use_lightpad(self, on):
        with self.lock:
            if on and not self.block:
                try:
                    self.block, self.host = entrancer_sim.make_lightpad_host(self.unit)
                    self.lp_error = ""
                except Exception as ex:
                    self.block = self.host = None
                    self.lp_error = "Lightpad: %s" % ex
            elif not on and self.block:
                self.host.let_go()     # puts the block's own settings back
                self.block.close()
                self.block = self.host = None
                with self.unit.lock:
                    self.unit.lightpad = None

    def lightpad_state(self):
        b, h = self.block, self.host
        if not b:
            return {"on": False, "error": self.lp_error}
        return {"on": True, "connected": getattr(b, "connected", False), "state": h.state,
                "serial": h.serial, "battery": h.battery, "notes": list(getattr(b, "notes", []))[-2:],
                "leds": ["%02x%02x%02x" % p for p in (h.preview or [])]}

    def snapshot(self):
        now = time.time()
        s = self.unit.snapshot(now)
        s["lightpad"] = self.lightpad_state()
        if not s["lightpad"]["on"]:     # show what the block would show, also without one
            s["lightpad"]["leds"] = ["%02x%02x%02x" % p for p in entrancer_sim.lightpad_picture(self.unit, now)]
        return s

    def close(self):
        self.use_lightpad(False)
        self.unit.close()


LIVE = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send_body(self, body, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            with open(PAGE, "rb") as f:
                self.send_body(f.read(), "text/html; charset=utf-8")
        elif path == "/weapon":
            with open(PROFILE, "rb") as f:
                self.send_body(f.read(), "application/json")
        elif path == "/state":
            self.send_body(json.dumps(LIVE.snapshot()).encode(), "application/json")
        else:
            self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            d = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            self.send_error(400)
            return
        if "lightpad" in d:
            threading.Thread(target=LIVE.use_lightpad, args=(bool(d["lightpad"]),), daemon=True).start()
        else:
            LIVE.unit.command(d)
        self.send_body(b"{}", "application/json")


def main():
    global LIVE
    port = int(_arg("--port", 8770))
    LIVE = Live(int(_arg("--engine-port", entrancer_sim.ENGINE_PORT)))
    if "--lightpad" in sys.argv:
        LIVE.use_lightpad(True)
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("Entrancer simulator: http://127.0.0.1:%d  (sends to the engine on UDP 127.0.0.1:%d)"
          % (port, LIVE.unit.addr[1]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        LIVE.close()


if __name__ == "__main__":
    main()
