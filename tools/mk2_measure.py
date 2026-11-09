"""
MK2 measurements for the controller spec (used by mk2_live.py)
==============================================================

Measures what the C++ controller module needs to know and saves it to
docs/mk2-measurements.json:

  * report timing   - interval and jitter of the pad (0x20) and controls (0x01)
                      reports, and the USB endpoint polling intervals
  * pad hits        - the pressure curve of single hits (soft .. hard): peak,
                      time to peak, pressure after 2/4/8 ms -> velocity formula
  * knob resolution - units per full turn for a knob and steps per turn for the
                      master encoder
  * reconnect       - a log of cable pulls and how long reopening took

It hooks into a Mapper's on_pads/on_controls (runs in the reader thread, so it
only appends numbers there; all maths happens in snapshot()).
"""

import json
import os
import threading
import time
from collections import deque

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "mk2-measurements.json")
VID, PID = 0x17CC, 0x1140
HIT_START = 60       # pressure that starts a trace (above Pad 12's resting ~136 only for other pads)
HIT_END = 40
TRACE_MS = 400


def probe_endpoints():
    """USB endpoint descriptors (address, type, bInterval, max packet) via pyusb, if available."""
    try:
        import usb.core
        backend = None
        try:
            import libusb_package
            backend = libusb_package.get_libusb1_backend()
        except ImportError:
            pass
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
        if dev is None:
            return [{"error": "device not visible to libusb"}]
        speed = {1: "low", 2: "full", 3: "high", 4: "super"}.get(getattr(dev, "speed", None), "unknown")
        out = []
        for cfg in dev:
            for intf in cfg:
                for ep in intf:
                    kind = ("control", "isochronous", "bulk", "interrupt")[ep.bmAttributes & 3]
                    out.append({"interface": intf.bInterfaceNumber, "class": "0x%02x" % intf.bInterfaceClass,
                                "endpoint": "0x%02x" % ep.bEndpointAddress, "type": kind,
                                "bInterval": ep.bInterval, "max_packet": ep.wMaxPacketSize, "usb_speed": speed})
        return out
    except Exception as e:  # never break the live view over a probe
        return [{"error": str(e)}]


def _stats(values):
    v = sorted(values)
    if not v:
        return None
    pick = lambda q: v[min(len(v) - 1, int(q * len(v)))]
    return {"n": len(v), "mean_ms": round(sum(v) / len(v), 3), "p50_ms": round(pick(0.5), 3),
            "p99_ms": round(pick(0.99), 3), "max_ms": round(v[-1], 3)}


class Measure:
    def __init__(self):
        self.lock = threading.Lock()
        self.pad_iv = deque(maxlen=3000)
        self.ctl_iv = deque(maxlen=600)
        self.last_pad = self.last_ctl = None
        self.armed = 0
        self.cur = None
        self.hits = []
        self.slot_of = {}
        self.knob = None
        self.knob_results = {}
        self.conn = []
        self.lost_at = None
        self.endpoints = None
        self.saved_at = ""

    # ---------- hooks (reader thread) ----------
    def attach(self, m, slot_of_id):
        """Wrap the Mapper callbacks. slot_of_id: hardware pad id -> pad number (1..16)."""
        self.slot_of = dict(slot_of_id)
        on_pads, on_controls = m.on_pads, m.on_controls

        def pads(p):
            on_pads(p)
            self._on_pads(time.perf_counter(), m.pads)

        def controls(p):
            on_controls(p)
            t = time.perf_counter()
            with self.lock:
                if self.last_ctl is not None:
                    self.ctl_iv.append((t - self.last_ctl) * 1000)
                self.last_ctl = t

        m.on_pads, m.on_controls = pads, controls
        if self.endpoints is None:
            threading.Thread(target=self._probe, daemon=True).start()

    def _probe(self):
        self.endpoints = probe_endpoints()

    def _on_pads(self, t, pressures):
        with self.lock:
            if self.last_pad is not None:
                self.pad_iv.append((t - self.last_pad) * 1000)
            self.last_pad = t
            if not self.armed:
                return
            if self.cur is None:
                for pid in range(16):
                    if pressures[pid] > HIT_START and self.slot_of.get(pid) != 12:  # Pad 12 rests ~136
                        self.cur = {"pid": pid, "t0": t, "s": [(0.0, pressures[pid])]}
                        break
                return
            c = self.cur
            ms = (t - c["t0"]) * 1000
            p = pressures[c["pid"]]
            c["s"].append((round(ms, 2), p))
            peak = max(v for _, v in c["s"])
            if (p < HIT_END and peak > HIT_START * 2) or ms > TRACE_MS:
                self.hits.append(self._hit_stats(c))
                self.cur = None
                self.armed -= 1

    def _hit_stats(self, c):
        s = c["s"]
        peak_ms, peak = max(s, key=lambda x: x[1])

        def at(ms):
            best = min(s, key=lambda x: abs(x[0] - ms))
            return best[1]
        return {"pad": self.slot_of.get(c["pid"], c["pid"]), "peak": peak, "t_peak_ms": peak_ms,
                "p_2ms": at(2), "p_4ms": at(4), "p_8ms": at(8), "duration_ms": s[-1][0],
                "p_max_4ms": max(v for t, v in s if t <= 4.0),  # velocity input, from every sample
                "samples": [list(x) for x in s if x[0] <= 150][::2]}  # every 2nd sample, for plots

    # ---------- device thread ----------
    def knob_tick(self, values):
        with self.lock:
            k = self.knob
            if not k:
                return
            v = values.get(k["name"])
            if v is None:
                return
            if k["last"] is not None and v != k["last"]:
                span = 16 if k["name"].startswith("Master") else 1000
                d = v - k["last"]
                d = d - span if d > span // 2 else d + span if d < -span // 2 else d
                k["sum"] += d
                k["changes"] += 1
            k["last"] = v

    def event(self, what):
        with self.lock:
            now = time.time()
            stamp = time.strftime("%H:%M:%S")
            if what == "disconnected":
                self.lost_at = now
                self.conn.append({"time": stamp, "event": "disconnected"})
            elif what == "connected":
                e = {"time": stamp, "event": "connected"}
                if self.lost_at:
                    e["after_s"] = round(now - self.lost_at, 2)
                    self.lost_at = None
                self.conn.append(e)
            self.conn = self.conn[-20:]

    # ---------- commands / state ----------
    def command(self, d):
        act = d.get("measure")
        with self.lock:
            if act == "arm_hits":
                self.armed = int(d.get("n", 10))
                self.cur = None
            elif act == "clear_hits":
                self.hits, self.armed, self.cur = [], 0, None
            elif act == "knob_start":
                self.knob = {"name": d.get("knob", "Knob 1"), "sum": 0, "changes": 0, "last": None}
            elif act == "knob_stop" and self.knob:
                k = self.knob
                self.knob_results[k["name"]] = {"units_per_turn": abs(k["sum"]), "changes": k["changes"]}
                self.knob = None
            elif act == "reset_timing":
                self.pad_iv.clear()
                self.ctl_iv.clear()
        if act == "save":
            self.save()

    def snapshot(self):
        with self.lock:
            return {
                "pad_interval": _stats(self.pad_iv),
                "controls_interval": _stats(self.ctl_iv),
                "endpoints": self.endpoints,
                "armed": self.armed,
                "hits": self.hits[-16:],
                "knob": dict(self.knob, last=None) if self.knob else None,
                "knob_results": dict(self.knob_results),
                "conn": list(self.conn),
                "saved_at": self.saved_at,
            }

    def save(self):
        s = self.snapshot()
        s["hits"] = list(self.hits)
        s["date"] = time.strftime("%Y-%m-%d %H:%M:%S")
        s["note"] = ("Measured with tools/mk2_live.py (Measure card). Intervals are host-side arrival times "
                     "of HID input reports (hidapi, Windows). Pad pressure 0..4095.")
        s.pop("armed", None)
        s.pop("knob", None)
        tmp = OUT + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(s, f, indent=1)
        os.replace(tmp, OUT)
        self.saved_at = time.strftime("%H:%M:%S")
