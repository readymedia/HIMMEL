# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Lightpad hardware link (weapon no. 2, the real block)
=====================================================

Opens the real ROLI Lightpad Block over WinMM (part of Windows; ctypes, no
new dependency), puts it in API mode and shows what it sends. It has the
same interface as lightpad_sim.SimLightpad, so the live view, the MIDI monitor
and the LocalHost handshake run unchanged on top of it.

The LocalHost on top of it (lightpad_sim.py) does the handshake, wipes the
block's program memory, uploads Himmel's own LED renderer (lightpad_littlefoot.py,
117 bytes) and then streams LED frames. Checked on the owner's Lightpad M
(2026-11-10): the test pattern shows correctly, about 55-58 full frames per
second, ~12 ms per frame from send to ACK. The program lives in RAM only:
endAPIMode (sent when you stop) brings the factory display back (checked); a
power cycle should too (not tested). Himmel never sends saveProgramAsDefault.

Before you run it: plug the block in with USB (not Bluetooth) and close
ROLI Connect / Dashboard (WinMM ports are exclusive).

Live:    python tools/mk2_live.py --port 8775 --weapon lightpad   then press "Use the real block"
Console: python tools/lightpad_hw.py            prints every message for 20 s
"""

import ctypes
import sys
import threading
import time
from ctypes import wintypes

import lightpad_sim as L

PORT_NAME = "Lightpad"
NO_ANSWER = 3.0          # s without any SysEx from the block in API mode: do the handshake again
BUFFERS, BUF_SIZE = 16, 1024

MIM_DATA, MIM_LONGDATA, MIM_LONGERROR = 0x3C3, 0x3C4, 0x3C6
CALLBACK_FUNCTION = 0x30000
MHDR_DONE = 0x1


class MIDIHDR(ctypes.Structure):
    pass


MIDIHDR._fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wintypes.DWORD),
                    ("dwBytesRecorded", wintypes.DWORD), ("dwUser", ctypes.c_size_t),
                    ("dwFlags", wintypes.DWORD), ("lpNext", ctypes.POINTER(MIDIHDR)),
                    ("reserved", ctypes.c_size_t), ("dwOffset", wintypes.DWORD),
                    ("dwReserved", ctypes.c_size_t * 8)]


class _INCAPS(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32), ("dwSupport", wintypes.DWORD)]


class _OUTCAPS(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32), ("wTechnology", wintypes.WORD), ("wVoices", wintypes.WORD),
                ("wNotes", wintypes.WORD), ("wChannelMask", wintypes.WORD), ("dwSupport", wintypes.DWORD)]


MIDIINPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)

_mm = None


def winmm():
    global _mm
    if _mm is None:
        _mm = ctypes.WinDLL("winmm")
        _mm.midiInOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
        _mm.midiOutOpen.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.UINT, ctypes.c_size_t, ctypes.c_size_t, wintypes.DWORD]
        for f in ("midiInPrepareHeader", "midiInUnprepareHeader", "midiInAddBuffer",
                  "midiOutPrepareHeader", "midiOutUnprepareHeader", "midiOutLongMsg"):
            getattr(_mm, f).argtypes = [wintypes.HANDLE, ctypes.POINTER(MIDIHDR), wintypes.UINT]
        for f in ("midiInStart", "midiInStop", "midiInReset", "midiInClose", "midiOutReset", "midiOutClose"):
            getattr(_mm, f).argtypes = [wintypes.HANDLE]
        _mm.midiInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.c_void_p, wintypes.UINT]
        _mm.midiOutGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.c_void_p, wintypes.UINT]
    return _mm


def list_ports():
    mm = winmm()
    ins, outs = [], []
    for i in range(mm.midiInGetNumDevs()):
        c = _INCAPS()
        if mm.midiInGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            ins.append((i, c.szPname))
    for i in range(mm.midiOutGetNumDevs()):
        c = _OUTCAPS()
        if mm.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            outs.append((i, c.szPname))
    return ins, outs


class HwLightpad:
    """The real block. Same attributes/methods as SimLightpad that the live view uses."""

    kind = "hw"
    leds_supported = True

    def __init__(self, port_name=PORT_NAME, device_name="Lightpad"):
        self.port_name, self.device_name = port_name, device_name   # e.g. "LUMI" for the LUMI Keys
        self.lock = threading.RLock()
        self.api = False
        self.fingers = {}          # finger index -> dict(i, x, y, z) from the block's own touch messages
        self.button = False
        self.log = []
        self.counts = {"to_host": 0, "to_device": 0, "bad": 0}
        self.listeners = []
        self.notes = ["Real Lightpad: looking for the MIDI ports…"]
        self.closed = False
        self.connected = False
        self.port_names = ("", "")
        self.last_rx = 0.0
        self.stats = {"touch_msgs": 0, "rate": 0.0, "x": [None, None], "y": [None, None], "z": [None, None],
                      "vz_start": [], "mpe_notes": 0, "serial": None, "battery": None, "acks": 0}
        self._rate_n, self._rate_t = 0, time.time()
        self.hin = wintypes.HANDLE()
        self.hout = wintypes.HANDLE()
        self.headers = []
        self.bufs = []
        self.partial = bytearray()
        self.out_lock = threading.Lock()
        self.cb = MIDIINPROC(self._callback)   # keep a reference: WinMM calls it from its own thread
        self.readd = []
        self.wake = threading.Event()
        threading.Thread(target=self._loop, daemon=True).start()

    # ---- ports ----
    def _open(self):
        mm = winmm()
        ins, outs = list_ports()
        names = (self.port_name,) if isinstance(self.port_name, str) else tuple(self.port_name)
        hit = lambda n: any(p.lower() in n.lower() for p in names)
        i = next((k for k, n in ins if hit(n)), None)
        o = next((k for k, n in outs if hit(n)), None)
        if i is None or o is None:
            self.notes = ["No %s MIDI ports found. Plug it in with USB (not Bluetooth). " % self.device_name +
                          "Ports seen: in %s, out %s" % ([n for _, n in ins], [n for _, n in outs])]
            return False
        r = mm.midiInOpen(ctypes.byref(self.hin), i, ctypes.cast(self.cb, ctypes.c_void_p), 0, CALLBACK_FUNCTION)
        if r:
            self.notes = ["Could not open MIDI in %d (error %d). Close ROLI Connect / Dashboard: "
                          "Windows lets only one program use the port." % (i, r)]
            return False
        r = mm.midiOutOpen(ctypes.byref(self.hout), o, 0, 0, 0)
        if r:
            mm.midiInClose(self.hin)
            self.notes = ["Could not open MIDI out %d (error %d). Close ROLI Connect / Dashboard." % (o, r)]
            return False
        self.headers, self.bufs = [], []
        for _ in range(BUFFERS):
            b = ctypes.create_string_buffer(BUF_SIZE)
            h = MIDIHDR()
            h.lpData = ctypes.cast(b, ctypes.c_void_p)
            h.dwBufferLength = BUF_SIZE
            mm.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            self.headers.append(h)
            self.bufs.append(b)
        mm.midiInStart(self.hin)
        self.port_names = (dict(ins)[i], dict(outs)[o])
        self.connected = True
        self.notes = ["Real %s on MIDI in %%d / out %%d (%%s). Himmel drives the LEDs (own renderer, RAM only)." % self.device_name % (
            i, o, self.port_names[0])]
        self._event("ports opened")
        return True

    def _close_ports(self):
        if not self.connected:
            return
        mm = winmm()
        self.connected = False
        try:
            mm.midiInStop(self.hin)
            mm.midiInReset(self.hin)
            for h in self.headers:
                mm.midiInUnprepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            mm.midiInClose(self.hin)
            mm.midiOutReset(self.hout)
            mm.midiOutClose(self.hout)
        except OSError:
            pass
        with self.lock:
            self.api = False
            self.fingers.clear()

    def close(self):
        if self.connected and self.api:
            self.receive(L.host_command(L.CMD_END_API))  # leave the block in its normal MIDI mode
            time.sleep(0.05)
        self.closed = True
        self.wake.set()
        self._close_ports()

    def engine_active(self):
        return False

    # ---- WinMM callback (WinMM's thread: copy and return, no WinMM calls here) ----
    def _callback(self, handle, msg, inst, p1, p2):
        try:
            if msg == MIM_DATA:
                st = p1 & 0xFF
                n = 1 if st >= 0xF8 else 2 if (st & 0xF0) in (0xC0, 0xD0) else 3
                self._incoming(bytes([(p1 >> (8 * k)) & 0xFF for k in range(n)]))
            elif msg in (MIM_LONGDATA, MIM_LONGERROR):
                h = MIDIHDR.from_address(p1)
                data = ctypes.string_at(h.lpData, h.dwBytesRecorded)
                if data:
                    self.partial += data
                    while 0xF7 in self.partial:
                        k = self.partial.index(0xF7) + 1
                        m = bytes(self.partial[:k])
                        del self.partial[:k]
                        s = m.find(b"\xF0")
                        if s >= 0:
                            self._incoming(m[s:])
                if self.connected:
                    self.readd.append(p1)
                    self.wake.set()
        except Exception:
            pass

    def _incoming(self, msg):
        now = time.time()
        with self.lock:
            self.counts["to_host"] += 1
            self._log("block → host", msg, False)
            if msg[:1] == b"\xF0":
                self.last_rx = now
        for e in L.decode_device(msg):
            with self.lock:
                st = self.stats
                if e["type"] == "touch":
                    st["touch_msgs"] += 1
                    self._rate_n += 1
                    for k in ("x", "y", "z"):
                        lo, hi = st[k]
                        st[k] = [e[k] if lo is None else min(lo, e[k]), e[k] if hi is None else max(hi, e[k])]
                    if e["phase"] == "start" and "vz" in e:
                        st["vz_start"] = (st["vz_start"] + [e["vz"]])[-8:]
                    if e["phase"] == "start":  # where each touch began, for the corner check
                        st["starts"] = (st.get("starts", []) + [[e["i"], e["x"], e["y"]]])[-12:]
                    key = "0x%02X %s" % (e.get("code", 0), e["phase"])
                    st.setdefault("types", {})[key] = st.get("types", {}).get(key, 0) + 1
                    if e["phase"] == "end":
                        self.fingers.pop(e["i"], None)
                    else:
                        self.fingers[e["i"]] = {"i": e["i"], "x": e["x"] / 4095, "y": e["y"] / 4095, "z": e["z"] / 255}
                elif e["type"] == "button":
                    st["buttons"] = st.get("buttons", 0) + 1
                    self.button = e["down"]
                elif e["type"] == "ack":
                    st["acks"] += 1
                elif e["type"] == "serial":
                    st["serial"] = e["serial"]
                elif e["type"] == "topology" and e["devices"]:
                    st["battery"] = "%d / 31%s" % (e["devices"][0]["battery"],
                                                   " charging" if e["devices"][0]["charging"] else "")
        if msg[:1] != b"\xF0" and (msg[0] & 0xF0) == 0x90 and len(msg) > 2 and msg[2]:
            with self.lock:
                self.stats["mpe_notes"] += 1
                starts = self.stats.get("starts") or []
                if starts:  # the factory note layout: which note each touch start played
                    _, x, y = starts[-1]
                    self.stats.setdefault("note_at", []).append([msg[1], msg[2], x, y])
                    del self.stats["note_at"][:-40]
        for f in list(self.listeners):
            try:
                f(msg)
            except Exception:
                pass

    def _log(self, direction, msg, to_device):
        text = L.describe(msg, to_device)
        noisy = text == "ping" or text.startswith(("sharedDataChange", "ACK "))
        self.log.append((time.strftime("%H:%M:%S"), direction,
                         bytes(msg[:40]).hex(" ") + (" …" if len(msg) > 40 else ""), text, noisy))
        del self.log[:-300]

    def _event(self, text):
        with self.lock:
            self.log.append((time.strftime("%H:%M:%S"), "probe", "", text, False))

    # ---- to the block ----
    def receive(self, msg, source="local"):
        """Same name as SimLightpad.receive: MIDI from the host, here sent to the real block."""
        msg = bytes(msg)
        if not self.connected:
            return
        body = L.unframe(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log("host → block", msg, True)
        mm = winmm()
        with self.out_lock:
            b = ctypes.create_string_buffer(msg, len(msg))
            h = MIDIHDR()
            h.lpData = ctypes.cast(b, ctypes.c_void_p)
            h.dwBufferLength = len(msg)
            h.dwBytesRecorded = len(msg)
            mm.midiOutPrepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))
            r = mm.midiOutLongMsg(self.hout, ctypes.byref(h), ctypes.sizeof(h))
            t_end = time.time() + 0.2
            while r == 0 and not (h.dwFlags & MHDR_DONE) and time.time() < t_end:
                time.sleep(0.001)
            mm.midiOutUnprepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))
        if r:
            self._event("send failed (error %d): block unplugged?" % r)
            self.connected = False
            return
        if body is not None:
            r2 = L.BitReader(body[1])
            if r2.get(7) == L.HOST_COMMAND:
                cmd = r2.get(9)
                if cmd == L.CMD_BEGIN_API:
                    with self.lock:
                        self.api, self.last_rx = True, time.time()
                elif cmd == L.CMD_END_API:
                    with self.lock:
                        self.api = False

    # ---- housekeeping thread: re-add input buffers, watch for silence, reopen ----
    def _loop(self):
        mm = winmm()
        while not self.closed:
            if not self.connected:
                self._close_ports()
                if not self._open():
                    for _ in range(10):
                        if self.closed:
                            return
                        time.sleep(0.1)
                    continue
            self.wake.wait(0.1)
            self.wake.clear()
            while self.readd and self.connected:
                p = self.readd.pop(0)
                h = MIDIHDR.from_address(p)
                h.dwBytesRecorded = 0
                mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            now = time.time()
            with self.lock:
                if now - self._rate_t >= 1.0:
                    self.stats["rate"] = round(self._rate_n / (now - self._rate_t), 1)
                    self._rate_n, self._rate_t = 0, now
                if self.api and now - self.last_rx > NO_ANSWER:
                    self.api = False
                    self.log.append((time.strftime("%H:%M:%S"), "probe", "",
                                     "no SysEx from the block for %.0f s in API mode: handshake again" % NO_ANSWER, False))

    # ---- the simulator's input methods: the real block has real fingers ----
    def touch(self, *a):
        pass

    def press(self, down):
        pass

    def release_all(self):
        pass

    def leds(self):
        """We do not know what the real LEDs show (no renderer of ours yet): dark grid."""
        return ["000000"] * (L.GRID * L.GRID)


def console(seconds=20.0):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ins, outs = list_ports()
    print("MIDI in:", ins)
    print("MIDI out:", outs)
    hw = HwLightpad()
    host = L.LocalHost(hw)
    t_end = time.time() + seconds
    shown = 0
    try:
        while time.time() < t_end:
            time.sleep(0.1)
            with hw.lock:
                new = hw.log[shown:]
                shown = len(hw.log)
            for t, d, hx, text, noisy in new:
                if not noisy:
                    print(t, d.ljust(13), text.ljust(48), hx)
    finally:
        hw.close()
        print("notes:", hw.notes)
        print("host:", host.state, "| stats:", hw.stats)


if __name__ == "__main__":
    console(float(sys.argv[1]) if len(sys.argv) > 1 else 20.0)
