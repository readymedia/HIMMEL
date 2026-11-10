# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Simulated MK2 ("virtual weapon")
================================

A pretend Maschine MK2 at the USB report level. It has the same interface as
maschine_mk2_test.Transport (open / read / write / close), so the Mapper, the
live view, the screen modes and the games run unchanged on top of it.

Input comes from the browser (click, drag, scroll on the drawn panel). The
simulator turns that into the same reports the hardware sends, using the
verified map in maschine_mk2_map.json:

  - report 0x01 (controls): buttons as bits, knobs as 16-bit LE words,
    master encoder as the low nibble of byte 7. Sent when something changes,
    at most every 10 ms (the real knob report rate).
  - report 0x20 (pads): 16 words (4-bit hardware id + 12-bit pressure),
    streamed every 4 ms like the real pad stream (1.33 ms on hardware; 4 ms
    is plenty for Python and still a steady heartbeat).

LED and screen reports written to it are accepted and dropped; the live view
already shows what was sent (screens) and the Mapper keeps the LED state.
"""

import threading
import time

PAD_PERIOD = 0.004
CONTROLS_MIN_GAP = 0.010
CONTROLS_PAYLOAD = 24          # report 0x01 is 25 bytes with the id
PAD_REPORT = 65                # report 0x20 with the id
KNOB_RANGE = 1000              # about one turn of a knob (wraps)
ENCODER_RANGE = 16             # master encoder nibble (wraps)


class SimMk2:
    kind = "sim"

    def __init__(self, mapping):
        """mapping: the dict from maschine_mk2_map.json (buttons, knobs, pads)."""
        self.map = mapping
        self.notes = ["Simulated MK2 - no hardware. Click, drag and scroll the panel."]
        self.lock = threading.Lock()
        self.held = set()
        self.values = {}                      # knob name -> raw value
        for name, v in mapping.get("knobs", {}).items():
            self.values[name] = 0 if v.get("nibble") == "low" else KNOB_RANGE // 2
        self.pressure = [0] * 16              # by hardware pad id
        self.dirty = True
        self.next_pad = 0.0
        self.next_controls = 0.0
        self.closed = False
        self.wake = threading.Event()

    # ---- Transport interface ----
    def open(self):
        return True

    def close(self):
        self.closed = True
        self.wake.set()

    def write(self, report):
        pass  # LEDs (0x80-0x82) and screens (0xE0/0xE1): nothing to drive

    def read(self):
        """Returns the next report (id first), pacing like the device. None on timeout."""
        if self.closed:
            time.sleep(0.05)
            return None
        now = time.perf_counter()
        with self.lock:
            if self.dirty and now >= self.next_controls:
                self.dirty = False
                self.next_controls = now + CONTROLS_MIN_GAP
                return bytes([0x01]) + self._controls()
        if now >= self.next_pad:
            self.next_pad = max(self.next_pad + PAD_PERIOD, now - PAD_PERIOD)
            return self._pads()
        self.wake.wait(min(self.next_pad - now, 0.1))
        self.wake.clear()
        return None

    # ---- input from the browser ----
    def press(self, name, down):
        if name not in self.map.get("buttons", {}):
            return False
        with self.lock:
            (self.held.add if down else self.held.discard)(name)
            self.dirty = True
        self.wake.set()
        return True

    def turn(self, name, delta):
        v = self.map.get("knobs", {}).get(name)
        if v is None:
            return False
        rng = ENCODER_RANGE if v.get("nibble") == "low" else KNOB_RANGE
        with self.lock:
            self.values[name] = (self.values[name] + int(delta)) % rng
            self.dirty = True
        self.wake.set()
        return True

    def pad(self, name, pressure):
        v = self.map.get("pads", {}).get(name)
        if not isinstance(v, dict) or "id" not in v:
            return False
        self.pressure[v["id"]] = max(0, min(4095, int(pressure)))
        return True

    def release_all(self):
        with self.lock:
            self.held.clear()
            self.pressure = [0] * 16
            self.dirty = True

    # ---- report building ----
    def _controls(self):
        p = bytearray(CONTROLS_PAYLOAD)
        for name in self.held:
            b = self.map["buttons"][name]
            if b["byte"] < len(p):
                p[b["byte"]] |= 1 << b["bit"]
        for name, v in self.map.get("knobs", {}).items():
            i, val = v["byte"], self.values.get(name, 0)
            if i >= len(p):
                continue
            if v.get("nibble") == "low":
                p[i] = (p[i] & 0xF0) | (val & 0x0F)
            elif v.get("nibble") == "high":
                p[i] = (p[i] & 0x0F) | ((val & 0x0F) << 4)
            else:
                p[i] = val & 0xFF
                if i >= 8 and i + 1 < len(p):  # 16-bit knob words start at byte 8
                    p[i + 1] = (val >> 8) & 0xFF
        return bytes(p)

    def _pads(self):
        r = bytearray(PAD_REPORT)
        r[0] = 0x20
        for pid in range(16):
            w = (pid << 12) | self.pressure[pid]
            r[1 + 2 * pid] = w & 0xFF
            r[2 + 2 * pid] = w >> 8
        return bytes(r)
