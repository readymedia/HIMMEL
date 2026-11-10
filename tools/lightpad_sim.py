# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Simulated Lightpad ("virtual weapon no. 2")
===========================================

A pretend ROLI Lightpad Block at the MIDI level: a 15 x 15 RGB LED grid under a
multi-touch surface (x, y, pressure per finger) and one Mode button on the side.

The real block speaks two languages over USB-MIDI:

  - **MIDI mode** (stand-alone, no host talking to it): MPE notes, one MIDI
    channel per finger (note on/off, pitch bend = x glide, CC74 = y slide,
    channel pressure = z). The simulator plays an *approximation* of the
    factory 5 x 5 note grid; check the real numbers on hardware.
  - **API mode** (the BLOCKS protocol): SysEx `F0 00 21 10 77 <index> <7-bit
    packed bits> <checksum> F7`. The host sends `beginAPIMode` and must ping
    every ~400 ms, or the block falls back to MIDI mode after 5 s. In API mode
    the block sends raw touches (start/move/end with x 12 bit, y 12 bit,
    z 8 bit and velocities) and button down/up, and the host writes the LEDs
    into the block's shared data memory ("sharedDataChange" packets, ACKed by
    the block).

The packet format follows ROLI's own reference implementation
(roli_blocks_basics, ISC license): roli_BitPackingUtilities.h,
roli_BlocksProtocolDefinitions.h, roli_HostPacketBuilder.h,
roli_HostPacketDecoder.h, roli_BitmapLEDProgram.cpp. No code was copied; this
is our own Python, written from reading those files.

LEDs in the simulator: the block shows the first 450 bytes of its shared data
as 15 x 15 RGB565 pixels, as ROLI's BitmapLEDProgram does with its heap. (On a
real block a small LittleFoot renderer program must be uploaded first and its
pixel heap sits after the program; that is the hardware step, not done here.)

Two hosts can talk to the simulated block:

  - LocalHost (below): the live view itself, a Python stand-in for Himmel's
    future Lightpad driver. It does the real handshake (serial, topology,
    begin API mode, pings) and draws LED pictures with real sharedDataChange
    packets, so both directions of the codec are exercised all the time.
  - An engine over UDP: every message the block sends goes as one datagram to
    127.0.0.1:8767; whatever is sent back to the sender port is treated as
    MIDI from the host. While such a host pings, the local host stays quiet
    and the panel shows the engine's LEDs (same idea as the MK2's 8766 link).

Self-test (no browser, no hardware):  python tools/lightpad_sim.py --selftest
"""

import colorsys
import math
import socket
import sys
import threading
import time

HEADER = bytes([0xF0, 0x00, 0x21, 0x10, 0x77])
SERIAL_REQUEST = bytes([0xF0, 0x00, 0x21, 0x10, 0x78, 0x3F, 0xF7])
SERIAL_HEADER = bytes([0xF0, 0x00, 0x21, 0x10, 0x78])
SIM_SERIAL = "LPBHIMMELSIM0001"          # 16 chars; "LPB" = Lightpad Block
SIM_MAC = "48:B6:20:00:00:01"            # the serial reply carries a MAC that starts 48:B6:20:

ENGINE_ADDR = ("127.0.0.1", 8767)        # MK2 uses 8766; the Lightpad link is the next port
HOST_TIMEOUT = 1.0                        # an engine pings every 400 ms
API_PING_TIMEOUT = 5.0                    # apiModeHostPingTimeoutMs
PING_INTERVAL = 0.4                       # masterPingIntervalMs
GRID = 15
PIXEL_BYTES = GRID * GRID * 2             # 15 x 15 RGB565
DATA_BLOCK = 7200                         # padBlockProgramAndHeapSize
MAX_PACKET_BITS = 7 * 140                 # keep each SysEx well under 200 bytes

# Device -> host message types (roli_BlocksProtocolDefinitions.h, MessageFromDevice)
DEV_TOPOLOGY, DEV_ACK = 0x01, 0x02
TOUCH_START, TOUCH_MOVE, TOUCH_END = 0x10, 0x11, 0x12
TOUCH_START_V, TOUCH_MOVE_V, TOUCH_END_V = 0x13, 0x14, 0x15
BUTTON_DOWN, BUTTON_UP = 0x20, 0x21
# Host -> device (MessageFromHost) and device commands (DeviceCommands)
HOST_COMMAND, HOST_DATA, HOST_PROGRAM_EVENT = 0x01, 0x02, 0x03
HOST_CONFIG, HOST_FACTORY_RESET, HOST_BLOCK_RESET = 0x10, 0x11, 0x12
CMD_BEGIN_API, CMD_TOPOLOGY, CMD_END_API, CMD_PING = 0x00, 0x01, 0x02, 0x03
CMD_NAMES = {0: "beginAPIMode", 1: "requestTopology", 2: "endAPIMode", 3: "ping",
             4: "debugMode", 5: "saveProgramAsDefault"}
MODE_BUTTON = 0                           # button index 0 = "mode" (the side button)

# MIDI mode (approximation of the factory note grid; verify on hardware)
MPE_BASE_NOTE = 48
MPE_ROW_STEP = 5
MPE_PADS = 5


# ---------------------------------------------------------------- codec
def checksum(body):
    """calculatePacketChecksum: starts at the size, c = c + (c * 2 + byte), 8-bit, then & 0x7F."""
    c = len(body) & 0xFF
    for b in body:
        c = (c + c * 2 + b) & 0xFF
    return c & 0x7F


class BitWriter:
    """LSB-first bit stream packed into 7-bit bytes (Packed7BitArrayBuilder)."""

    def __init__(self):
        self.v = 0
        self.n = 0

    def put(self, value, bits):
        self.v |= (int(value) & ((1 << bits) - 1)) << self.n
        self.n += bits
        return self

    def to_bytes(self):
        return bytes((self.v >> (7 * i)) & 0x7F for i in range((self.n + 6) // 7))


class BitReader:
    """Reads the same stream back (Packed7BitArrayReader)."""

    def __init__(self, data):
        self.v = 0
        for i, b in enumerate(data):
            self.v |= (b & 0x7F) << (7 * i)
        self.total = 7 * len(data)
        self.pos = 0

    def left(self):
        return self.total - self.pos

    def get(self, bits):
        v = (self.v >> self.pos) & ((1 << bits) - 1)
        self.pos += bits
        return v


def frame(index_byte, w):
    body = w.to_bytes()
    return HEADER + bytes([index_byte & 0x7F]) + body + bytes([checksum(body), 0xF7])


def unframe(msg):
    """(index byte, body) of a BLOCKS SysEx, or None if it is not one or the checksum is wrong."""
    if len(msg) < 9 or msg[:5] != HEADER or msg[-1] != 0xF7:
        return None
    body, ck = msg[6:-2], msg[-2]
    if checksum(body) != ck:
        return None
    return msg[5], body


def signed8(v):
    return v - 256 if v & 0x80 else v


# ---- host side (what Himmel's driver will do) ----
def host_command(cmd, index=0):
    return frame(index, BitWriter().put(HOST_COMMAND, 7).put(cmd, 9))


def host_data_changes(packet_index, old, new):
    """sharedDataChange packets that turn `old` into `new` (same length). Returns
    [(packet_index, sysex)], one per packet; each packet starts at byte 0."""
    n = len(new)
    out = []
    pos = 0
    while pos < n and old[pos] == new[pos]:
        pos += 1
    if pos >= n:
        return out
    while pos < n:
        w = BitWriter().put(HOST_DATA, 7).put(packet_index & 0xFFFF, 16)
        _skip(w, pos)
        last = 0
        while pos < n and w.n < MAX_PACKET_BITS:
            if old[pos] == new[pos]:
                k = pos
                while k < n and old[k] == new[k]:
                    k += 1
                if k >= n:
                    pos = n
                    break
                _skip(w, k - pos)
                pos = k
                continue
            run = 1
            while pos + run < n and new[pos + run] == new[pos] and run < 255:
                run += 1
            if run >= 3:
                v = new[pos]
                if run > 15:
                    w.put(7, 3).put(run, 8).put(v, 8)        # setManyBytesWithValue
                elif v == last:
                    w.put(6, 3).put(run, 4)                  # setFewBytesWithLastValue
                else:
                    w.put(5, 3).put(run, 4).put(v, 8)        # setFewBytesWithValue
                last = v
                pos += run
                continue
            # a mixed sequence up to the next unchanged byte or the next run of 3
            seq = []
            while pos < n and old[pos] != new[pos] and len(seq) < 32:
                if pos + 2 < n and new[pos] == new[pos + 1] == new[pos + 2] and seq:
                    break
                seq.append(new[pos])
                pos += 1
            w.put(4, 3)                                      # setSequenceOfBytes
            for i, b in enumerate(seq):
                w.put(b, 8).put(1 if i < len(seq) - 1 else 0, 1)
            last = seq[-1]
        done = pos >= n
        w.put(1 if done else 0, 3)                           # endOfChanges / endOfPacket
        out.append((packet_index & 0x3FF, frame(0, w)))
        packet_index += 1
    return out


def _skip(w, count):
    while count > 0:
        k = min(count, 255)
        if k > 15:
            w.put(3, 3).put(k, 8)                            # skipBytesMany
        else:
            w.put(2, 3).put(k, 4)                            # skipBytesFew
        count -= k


def decode_device(msg):
    """Device -> host SysEx into a list of event dicts (HostPacketDecoder)."""
    if msg[:5] == SERIAL_HEADER and msg[:5] != HEADER:
        s = bytes(msg).decode("ascii", "replace")
        i = s.find("48:B6:20:")
        return [{"type": "serial", "serial": s[i + 17:i + 33] if i >= 0 else "?"}]
    u = unframe(msg)
    if u is None:
        return []
    index, body = u
    r = BitReader(body)
    if r.left() < 32:
        return []
    ts = r.get(32)
    ev = []
    while r.left() >= 7:
        t = r.get(7)
        if t == 0:
            break
        if t in (TOUCH_START, TOUCH_MOVE, TOUCH_END, TOUCH_START_V, TOUCH_MOVE_V, TOUCH_END_V):
            off, idx, x, y, z = r.get(5), r.get(5), r.get(12), r.get(12), r.get(8)
            e = {"type": "touch", "phase": "start move end".split()[(t - 0x10) % 3], "t": ts + off,
                 "i": idx, "x": x, "y": y, "z": z}
            if t >= TOUCH_START_V:
                e.update(vx=signed8(r.get(8)), vy=signed8(r.get(8)), vz=signed8(r.get(8)))
            ev.append(e)
        elif t in (BUTTON_DOWN, BUTTON_UP):
            off, bid = r.get(5), r.get(12)
            ev.append({"type": "button", "down": t == BUTTON_DOWN, "id": bid, "t": ts + off})
        elif t == DEV_ACK:
            ev.append({"type": "ack", "packet": r.get(10)})
        elif t == DEV_TOPOLOGY:
            ver, nd, nc = r.get(8), r.get(7), r.get(8)
            devs = []
            for _ in range(nd):
                serial = "".join(chr(r.get(7)) for _ in range(16))
                devs.append({"serial": serial, "index": r.get(7), "battery": r.get(5), "charging": r.get(1)})
            for _ in range(nc):
                r.get(7 + 5 + 7 + 5)
            ev.append({"type": "topology", "version": ver, "devices": devs})
        else:
            ev.append({"type": "unknown", "msg": t})
            break
    return ev


def rgb565(r, g, b):
    return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)


def pixels_to_bytes(px):
    """225 (r, g, b) -> 450 bytes, the BitmapLEDProgram heap layout (bit = (x + y * 15) * 16)."""
    out = bytearray(PIXEL_BYTES)
    for i, (r, g, b) in enumerate(px):
        v = rgb565(r, g, b)
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    return out


def describe(msg, to_device):
    """One-line human description of a MIDI message, for the monitor."""
    if not msg:
        return ""
    s = msg[0]
    if s == 0xF0:
        if bytes(msg) == SERIAL_REQUEST:
            return "serial dump request"
        if msg[:5] == SERIAL_HEADER and msg[:5] != HEADER and to_device is False:
            return "serial reply " + decode_device(msg)[0]["serial"]
        u = unframe(msg)
        if u is None:
            return "SysEx (not BLOCKS / bad checksum)"
        if to_device:
            r = BitReader(u[1])
            parts = []
            while r.left() >= 7:
                t = r.get(7)
                if t == 0:
                    break
                if t == HOST_COMMAND:
                    parts.append(CMD_NAMES.get(r.get(9), "command?"))
                elif t == HOST_DATA:
                    parts.append("sharedDataChange #%d (%d bytes)" % (r.get(16) & 0x3FF, len(msg)))
                    break
                else:
                    parts.append("host msg 0x%02X" % t)
                    break
            return ", ".join(parts)
        out = []
        for e in decode_device(msg):
            if e["type"] == "touch":
                out.append("touch %s #%d x=%d y=%d z=%d%s" % (e["phase"], e["i"], e["x"], e["y"], e["z"],
                                                             " vz=%d" % e["vz"] if "vz" in e else ""))
            elif e["type"] == "button":
                out.append("button %d %s" % (e["id"], "down" if e["down"] else "up"))
            elif e["type"] == "ack":
                out.append("ACK %d" % e["packet"])
            elif e["type"] == "topology":
                out.append("topology: " + ", ".join("%s battery %d%s" % (d["serial"], d["battery"],
                                                                         " charging" if d["charging"] else "")
                                                    for d in e["devices"]))
            else:
                out.append("msg 0x%02X" % e.get("msg", 0))
        return ", ".join(out)
    ch = (s & 0x0F) + 1
    kind = s & 0xF0
    if kind == 0x90 and len(msg) >= 3:
        return "ch%d note on %d vel %d" % (ch, msg[1], msg[2])
    if kind == 0x80 and len(msg) >= 3:
        return "ch%d note off %d vel %d" % (ch, msg[1], msg[2])
    if kind == 0xB0 and len(msg) >= 3:
        return "ch%d CC%d = %d" % (ch, msg[1], msg[2])
    if kind == 0xD0 and len(msg) >= 2:
        return "ch%d pressure %d" % (ch, msg[1])
    if kind == 0xE0 and len(msg) >= 3:
        return "ch%d pitch bend %+d" % (ch, (msg[1] | msg[2] << 7) - 8192)
    return "MIDI " + bytes(msg).hex(" ")


# ---------------------------------------------------------------- the simulated block
class SimLightpad:
    def __init__(self):
        self.lock = threading.RLock()
        self.t0 = time.time()
        self.api = False
        self.last_ping = 0.0
        self.data = bytearray(DATA_BLOCK)
        self.fingers = {}          # pointer id -> dict(i, x, y, z, ...)
        self.button = False
        self.battery = 22          # 0..31
        self.charging = True
        self.log = []              # (time, dir, hex, text)
        self.counts = {"to_host": 0, "to_device": 0, "bad": 0}
        self.listeners = []        # callables(msg) for messages to the host (the local host)
        self.engine_time = 0.0
        self.engine_addr = None
        self.closed = False
        self.notes = ["Simulated Lightpad - no hardware. Touch the surface with the mouse."]
        self.udp = None
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.bind(("127.0.0.1", 0))
            u.settimeout(0.2)
            if hasattr(socket, "SIO_UDP_CONNRESET"):
                u.ioctl(socket.SIO_UDP_CONNRESET, False)
            self.udp = u
            threading.Thread(target=self._rx_loop, daemon=True).start()
        except OSError as e:
            self.notes.append("No link to the engine: %s" % e)
        threading.Thread(target=self._tick_loop, daemon=True).start()

    def close(self):
        self.closed = True
        if self.udp is not None:
            try:
                self.udp.close()
            except OSError:
                pass

    def now_ms(self):
        return int((time.time() - self.t0) * 1000) & 0xFFFFFFFF

    def engine_active(self):
        return time.time() - self.engine_time < HOST_TIMEOUT

    # ---- output ----
    def _log(self, direction, msg, to_device):
        text = describe(msg, to_device)
        noisy = text == "ping" or text.startswith(("sharedDataChange", "ACK "))
        self.log.append((time.strftime("%H:%M:%S"), direction,
                         bytes(msg[:40]).hex(" ") + (" …" if len(msg) > 40 else ""), text, noisy))
        del self.log[:-300]

    def send(self, msg):
        """A message from the block to the host: USB-MIDI out."""
        msg = bytes(msg)
        with self.lock:
            self.counts["to_host"] += 1
            self._log("block → host", msg, False)
        for f in list(self.listeners):
            try:
                f(msg)
            except Exception:
                pass
        if self.udp is not None:
            try:
                self.udp.sendto(msg, ENGINE_ADDR)
            except OSError:
                pass

    def _packet(self, *items):
        w = BitWriter().put(self.now_ms(), 32)
        for it in items:
            for value, bits in it:
                w.put(value, bits)
        return frame(0x40, w)  # 0x40 = device -> host, topology index 0 (the USB master)

    # ---- input from a host ----
    def _rx_loop(self):
        while not self.closed:
            try:
                r, addr = self.udp.recvfrom(4096)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue  # Windows: queued "port unreachable" for datagrams nobody read; drain, no sleep
            except OSError:
                if self.closed:
                    return
                time.sleep(0.05)
                continue
            if r:
                self.engine_time = time.time()
                self.engine_addr = addr
                self.receive(r, "engine")

    def receive(self, msg, source="local"):
        """MIDI from the host (USB-MIDI in)."""
        msg = bytes(msg)
        with self.lock:
            self.counts["to_device"] += 1
            self._log(("engine" if source == "engine" else "host") + " → block", msg, True)
        if msg == SERIAL_REQUEST:
            self.send(SERIAL_HEADER + (SIM_MAC + SIM_SERIAL).encode("ascii") + b"\xF7")
            return
        u = unframe(msg)
        if u is None:
            with self.lock:
                self.counts["bad"] += 1
            return
        r = BitReader(u[1])
        while r.left() >= 7:
            t = r.get(7)
            if t == 0:
                break
            if t == HOST_COMMAND:
                self._command(r.get(9))
            elif t == HOST_DATA:
                if not self._data_change(r):
                    break
            elif t == HOST_PROGRAM_EVENT:
                r.get(96)
            elif t in (HOST_FACTORY_RESET, HOST_BLOCK_RESET):
                with self.lock:
                    self.data = bytearray(DATA_BLOCK)
                    self.api = False
            else:
                break  # config / firmware / name: not simulated

    def _command(self, cmd):
        now = time.time()
        if cmd == CMD_BEGIN_API:
            with self.lock:
                self.api, self.last_ping = True, now
        elif cmd == CMD_END_API:
            with self.lock:
                self.api = False
            self._lift_all()
        elif cmd == CMD_PING:
            with self.lock:
                self.last_ping = now
            self.send(self._packet([(DEV_ACK, 7), (0, 10)]))
        elif cmd == CMD_TOPOLOGY:
            items = [(DEV_TOPOLOGY, 7), (1, 8), (1, 7), (0, 8)]
            items += [(ord(c), 7) for c in SIM_SERIAL]
            items += [(0, 7), (self.battery, 5), (1 if self.charging else 0, 1)]
            self.send(self._packet(items))

    def _data_change(self, r):
        """Applies one sharedDataChange message; ACKs it. False if the stream is broken."""
        if r.left() < 16:
            return False
        pkt = r.get(16)
        pos, last = 0, 0
        with self.lock:
            d = self.data
            while r.left() >= 3:
                c = r.get(3)
                if c in (0, 1):
                    break
                if c == 2:
                    pos += r.get(4)
                elif c == 3:
                    pos += r.get(8)
                elif c == 4:
                    while True:
                        v, more = r.get(8), r.get(1)
                        if pos < len(d):
                            d[pos] = v
                        pos += 1
                        last = v
                        if not more or r.left() < 9:
                            break
                elif c == 5:
                    k, v = r.get(4), r.get(8)
                    d[pos:pos + k] = bytes([v]) * len(d[pos:pos + k])
                    pos, last = pos + k, v
                elif c == 6:
                    k = r.get(4)
                    d[pos:pos + k] = bytes([last]) * len(d[pos:pos + k])
                    pos += k
                elif c == 7:
                    k, v = r.get(8), r.get(8)
                    d[pos:pos + k] = bytes([v]) * len(d[pos:pos + k])
                    pos, last = pos + k, v
            self.last_ping = time.time()  # any host traffic keeps API mode alive
        self.send(self._packet([(DEV_ACK, 7), (pkt & 0x3FF, 10)]))
        return True

    def _tick_loop(self):
        while not self.closed:
            time.sleep(0.1)
            with self.lock:
                if self.api and time.time() - self.last_ping > API_PING_TIMEOUT:
                    self.api = False
                    self.log.append((time.strftime("%H:%M:%S"), "block", "",
                                     "no ping for 5 s: API mode ended, back to MIDI (MPE) mode", False))

    # ---- input from the browser ----
    def touch(self, pid, x, y, z, phase):
        """x, y 0..1 from the top-left corner; z 0..1; phase start/move/end."""
        x = min(1.0, max(0.0, float(x)))
        y = min(1.0, max(0.0, float(y)))
        z = min(1.0, max(0.0, float(z)))
        with self.lock:
            f = self.fingers.get(pid)
            if phase == "start" or f is None:
                if f is not None:
                    self._finger_out(f, "end")
                used = {g["i"] for g in self.fingers.values()}
                i = next(k for k in range(32) if k not in used)
                f = {"i": i, "x": x, "y": y, "z": z, "z0": z, "sx": x, "sy": y, "px": x, "py": y, "pz": 0.0,
                     "note": None, "ch": 1 + (i % 15), "api": self.api}
                self.fingers[pid] = f
                self._finger_out(f, "start")
                return
            f["px"], f["py"], f["pz"] = f["x"], f["y"], f["z"]
            f["x"], f["y"], f["z"] = x, y, (z if phase != "end" else 0.0)
            if phase == "end":
                self._finger_out(f, "end")
                del self.fingers[pid]
            elif f["api"] != self.api:
                # the block changed mode under a resting finger: end it in the old language,
                # start it again in the new one (no MPE note without a note on, no API move without a start)
                self._finger_out(f, "end")
                f.update(api=self.api, sx=x, sy=y, pz=0.0)
                self._finger_out(f, "start")
            else:
                self._finger_out(f, "move")

    def _finger_out(self, f, phase):
        if phase == "start":
            f["api"] = self.api
        if f["api"]:
            t = {"start": TOUCH_START_V, "move": TOUCH_MOVE_V, "end": TOUCH_END_V}[phase]
            vx = int(max(-127, min(127, (f["x"] - f["px"]) * 1270))) & 0xFF
            vy = int(max(-127, min(127, (f["y"] - f["py"]) * 1270))) & 0xFF
            vz = int(max(-127, min(127, (f["z"] if phase == "start" else f["z"] - f["pz"]) * 127))) & 0xFF
            self.send(self._packet([(t, 7), (0, 5), (f["i"], 5), (round(f["x"] * 4095), 12),
                                    (round(f["y"] * 4095), 12), (round(f["z"] * 255), 8),
                                    (vx, 8), (vy, 8), (vz, 8)]))
        else:
            self._mpe(f, phase)

    def _mpe(self, f, phase):
        """Approximate factory note mode: 5 x 5 pads, notes up by 1 to the right and 5 per row up."""
        ch = f["ch"]
        if phase == "start":
            col = min(MPE_PADS - 1, int(f["x"] * MPE_PADS))
            row = min(MPE_PADS - 1, int((1 - f["y"]) * MPE_PADS))
            f["note"] = MPE_BASE_NOTE + col + MPE_ROW_STEP * row
            f["pad"] = (col, row)
            self.send(bytes([0xE0 | ch, 0, 64]))
            self.send(bytes([0x90 | ch, f["note"], max(1, round(f["z"] * 127))]))
            return
        if phase == "end":
            self.send(bytes([0x80 | ch, f["note"], 64]))
            return
        bend = int(8192 + (f["x"] - f["sx"]) * MPE_PADS * 8192 / 48)
        bend = max(0, min(16383, bend))
        slide = round(min(1.0, max(0.0, (1 - f["y"]) * MPE_PADS - f["pad"][1])) * 127)
        self.send(bytes([0xE0 | ch, bend & 0x7F, bend >> 7]))
        self.send(bytes([0xB0 | ch, 74, slide]))
        self.send(bytes([0xD0 | ch, round(f["z"] * 127)]))

    def press(self, down):
        with self.lock:
            if down == self.button:
                return
            self.button = down
            api = self.api
        if api:
            self.send(self._packet([(BUTTON_DOWN if down else BUTTON_UP, 7), (0, 5), (MODE_BUTTON, 12)]))

    def _lift_all(self):
        with self.lock:
            for pid in list(self.fingers):
                f = self.fingers.pop(pid)
                f["z"] = 0.0
                self._finger_out(f, "end")

    def release_all(self):
        self._lift_all()
        self.press(False)

    # ---- what the LEDs show ----
    def leds(self):
        """225 '#rrggbb' strings, row-major from the top-left."""
        with self.lock:
            if self.api:
                d = self.data
                out = []
                for i in range(GRID * GRID):
                    v = d[2 * i] | d[2 * i + 1] << 8
                    out.append("%02x%02x%02x" % ((v & 31) << 3, ((v >> 5) & 63) << 2, ((v >> 11) & 31) << 3))
                return out
            return self._note_mode_picture()

    def _note_mode_picture(self):
        """What the block shows on its own in MIDI mode: the 5 x 5 pads (approximation)."""
        lit = {f.get("pad") for f in self.fingers.values()}
        out = []
        for y in range(GRID):
            for x in range(GRID):
                col, row = x // 3, (GRID - 1 - y) // 3
                note = MPE_BASE_NOTE + col + MPE_ROW_STEP * row
                edge = x % 3 == 1 and y % 3 == 1
                if (col, row) in lit:
                    out.append("ffffff" if edge else "8fd6ff")
                elif note % 12 == 0:
                    out.append("ff7a3d" if edge else "5a2410")
                else:
                    out.append("3d8bff" if edge else "0f2244")
        return out


# ---------------------------------------------------------------- the stand-in host
class LocalHost:
    """The live view acting as the BLOCKS host (a Python sketch of Himmel's driver)."""

    MODES = ("touch", "rainbow", "test", "off")

    def __init__(self, sim):
        self.sim = sim
        self.mode = "touch"
        self.enabled = True        # False = "unplug the host": no pings, the block falls back to MIDI mode
        self.state = "idle"
        self.serial = None
        self.battery = None
        self.sent = bytearray(PIXEL_BYTES)  # what we believe the block holds
        self.packet = 0
        self.need_full = True
        self.next_ping = 0.0
        self.fingers = {}          # touch index -> (x, y, z) 0..1, from decoded block messages
        self.glow = [[0.0, 0.0, 0.0] for _ in range(GRID * GRID)]
        self.palette = 0
        self.acks = 0
        self.lock = threading.Lock()
        sim.listeners.append(self.on_block)
        threading.Thread(target=self.loop, daemon=True).start()

    def quiet(self):
        return not self.enabled or self.sim.engine_active()

    def on_block(self, msg):
        for e in decode_device(msg):
            with self.lock:
                if e["type"] == "serial":
                    self.serial = e["serial"]
                elif e["type"] == "topology" and e["devices"]:
                    self.battery = e["devices"][0]["battery"]
                elif e["type"] == "ack":
                    self.acks += 1
                elif e["type"] == "touch":
                    if e["phase"] == "end":
                        self.fingers.pop(e["i"], None)
                    else:
                        self.fingers[e["i"]] = (e["x"] / 4095, e["y"] / 4095, e["z"] / 255)
                elif e["type"] == "button" and e["down"]:
                    self.palette = (self.palette + 1) % 6

    def loop(self):
        while not self.sim.closed:
            time.sleep(0.04)
            try:
                self.step()
            except Exception as ex:  # a drawing bug must never stop the host loop
                self.state = "error: %s" % ex

    def step(self):
        sim = self.sim
        if self.quiet():
            self.state = "engine is the host" if sim.engine_active() else "host unplugged (MIDI mode)"
            self.need_full = True
            if not self.enabled and sim.api:
                pass  # no pings: the block times out by itself after 5 s, like the real one
            return
        now = time.time()
        if not sim.api:
            self.state = "handshake"
            sim.receive(SERIAL_REQUEST)
            sim.receive(host_command(CMD_TOPOLOGY))
            sim.receive(host_command(CMD_END_API))
            sim.receive(host_command(CMD_BEGIN_API))
            self.need_full = True
            self.next_ping = now + PING_INTERVAL
            return
        self.state = "API mode, pinging"
        if now >= self.next_ping:
            self.next_ping = now + PING_INTERVAL
            sim.receive(host_command(CMD_PING))
        target = pixels_to_bytes(self.picture(now))
        old = bytes(b ^ 0xFF for b in target) if self.need_full else self.sent
        packets = host_data_changes(self.packet + 1, old, target)
        for idx, p in packets:
            sim.receive(p)
            self.packet = idx
        self.sent = target
        self.need_full = False

    def picture(self, now):
        with self.lock:
            fingers = dict(self.fingers)
            pal = self.palette
        px = []
        if self.mode == "off":
            return [(0, 0, 0)] * (GRID * GRID)
        if self.mode == "test":
            # border, diagonal, and coloured corners so orientation can be checked on the device
            for y in range(GRID):
                for x in range(GRID):
                    c = (0, 0, 0)
                    if x in (0, GRID - 1) or y in (0, GRID - 1):
                        c = (60, 60, 60)
                    if x == y:
                        c = (255, 200, 0)
                    if (x, y) == (0, 0):
                        c = (255, 0, 0)
                    elif (x, y) == (GRID - 1, 0):
                        c = (0, 255, 0)
                    elif (x, y) == (0, GRID - 1):
                        c = (0, 0, 255)
                    elif (x, y) == (GRID - 1, GRID - 1):
                        c = (255, 255, 255)
                    px.append(c)
            return px
        if self.mode == "rainbow":
            for y in range(GRID):
                for x in range(GRID):
                    h = (x + y) / 28.0 - now * 0.15
                    r, g, b = colorsys.hsv_to_rgb(h % 1.0, 1.0, 0.8)
                    px.append((int(r * 255), int(g * 255), int(b * 255)))
            return px
        # touch: glowing trails under the fingers; the Mode button changes the colour family
        base = pal / 6.0
        for i, cell in enumerate(self.glow):
            for k in range(3):
                cell[k] *= 0.82
        for idx, (fx, fy, fz) in fingers.items():
            h = (base + idx * 0.13) % 1.0
            cr, cg, cb = colorsys.hsv_to_rgb(h, 0.85, 1.0)
            rad = 1.0 + 3.5 * fz
            cx, cy = fx * (GRID - 1), fy * (GRID - 1)
            for y in range(max(0, int(cy - rad - 1)), min(GRID, int(cy + rad + 2))):
                for x in range(max(0, int(cx - rad - 1)), min(GRID, int(cx + rad + 2))):
                    d = math.hypot(x - cx, y - cy)
                    k = max(0.0, 1 - d / rad) * (0.35 + 0.65 * fz)
                    cell = self.glow[y * GRID + x]
                    cell[0] = max(cell[0], cr * k)
                    cell[1] = max(cell[1], cg * k)
                    cell[2] = max(cell[2], cb * k)
        hr, hg, hb = colorsys.hsv_to_rgb(base, 0.9, 0.10)
        for cell in self.glow:
            px.append((int(255 * max(cell[0], hr)), int(255 * max(cell[1], hg)), int(255 * max(cell[2], hb))))
        return px


# ---------------------------------------------------------------- live-view wrapper
class LightpadLive:
    """What tools/mk2_live.py talks to for ?weapon=lightpad: snapshot() and command()."""

    def __init__(self, profile):
        self.profile = profile
        self.sim = SimLightpad()
        self.host = LocalHost(self.sim)
        self.show_noise = False

    def snapshot(self):
        s, h = self.sim, self.host
        with s.lock:
            fingers = [{"i": f["i"], "x": round(f["x"], 4), "y": round(f["y"], 4), "z": round(f["z"], 3)}
                       for f in s.fingers.values()]
            log = [e[:4] for e in s.log if self.show_noise or not e[4]][-18:]
            api, button, counts = s.api, s.button, dict(s.counts)
        return {
            "weapon": "lightpad",
            "simulated": True,
            "engine": s.engine_active(),
            "api": api,
            "mode_name": "API mode (BLOCKS SysEx)" if api else "MIDI mode (MPE)",
            "host": {"state": h.state, "mode": h.mode, "enabled": h.enabled, "serial": h.serial,
                     "battery": h.battery, "acks": h.acks},
            "leds": s.leds(),
            "fingers": fingers,
            "held": ["Mode"] if button else [],
            "counts": counts,
            "log": log,
            "notes": s.notes,
        }

    def command(self, d):
        s = self.sim
        if "sim_input" in d:
            i = d["sim_input"]
            if "touch" in i:
                t = i["touch"]
                s.touch(t.get("id", 0), t.get("x", 0), t.get("y", 0), t.get("z", 0), t.get("phase", "move"))
            elif i.get("name") == "Mode" and "down" in i:
                s.press(bool(i["down"]))
            elif i.get("release_all"):
                s.release_all()
        elif d.get("led_mode") in LocalHost.MODES:
            self.host.mode = d["led_mode"]
        elif "show_noise" in d:
            self.show_noise = bool(d["show_noise"])
        elif "host_enabled" in d:
            self.host.enabled = bool(d["host_enabled"])

    def close(self):
        self.sim.close()


# ---------------------------------------------------------------- self-test
def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond

    # bit packing round trip with odd widths
    w = BitWriter()
    vals = [(5, 3), (0xABC, 12), (1, 1), (0xDEADBEEF, 32), (77, 7), (0x3FF, 10)]
    for v, b in vals:
        w.put(v, b)
    r = BitReader(w.to_bytes())
    check("bit packing round trip", all(r.get(b) == v & ((1 << b) - 1) for v, b in vals))
    check("all bytes 7-bit", all(b < 0x80 for b in w.to_bytes()))
    # a ping as ROLI builds it: F0 00 21 10 77 00 <01 | cmd 3 in 9 bits> <ck> F7
    p = host_command(CMD_PING)
    check("ping frame " + p.hex(" "), p[:6] == HEADER + b"\x00" and unframe(p) is not None)
    # the block answers touches and the decoder reads them back
    s = SimLightpad()
    got = []
    s.listeners.append(lambda m: got.extend(decode_device(m)))
    s.receive(host_command(CMD_BEGIN_API))
    s.touch(1, 0.25, 0.75, 0.5, "start")
    t = [e for e in got if e["type"] == "touch"]
    check("touch start decoded", t and t[0]["phase"] == "start" and t[0]["x"] == round(0.25 * 4095)
          and t[0]["y"] == round(0.75 * 4095) and t[0]["z"] == round(0.5 * 255))
    s.press(True)
    check("mode button decoded", any(e["type"] == "button" and e["down"] and e["id"] == 0 for e in got))
    # LED data round trip through sharedDataChange packets
    px = [((x * 17) % 256, (y * 17) % 256, ((x + y) * 9) % 256) for y in range(GRID) for x in range(GRID)]
    target = pixels_to_bytes(px)
    packets = host_data_changes(1, bytes(PIXEL_BYTES), target)
    for _, pk in packets:
        s.receive(pk)
    check("LED frame in %d packets lands in the block" % len(packets), bytes(s.data[:PIXEL_BYTES]) == bytes(target))
    check("packets are ACKed", sum(1 for e in got if e["type"] == "ack") >= len(packets))
    small = bytearray(target)
    small[100] ^= 0xFF
    small[101] ^= 0x0F
    packets = host_data_changes(9, target, small)
    for _, pk in packets:
        s.receive(pk)
    check("small diff is one short packet (%d bytes)" % len(packets[0][1]),
          len(packets) == 1 and len(packets[0][1]) < 20 and bytes(s.data[:PIXEL_BYTES]) == bytes(small))
    fill = pixels_to_bytes([(255, 0, 0)] * (GRID * GRID))
    for _, pk in host_data_changes(20, small, fill):
        s.receive(pk)
    check("solid fill (run-length)", bytes(s.data[:PIXEL_BYTES]) == bytes(fill) and s.leds()[0] == "f80000")
    s.close()
    print("selftest", "passed" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(0 if selftest() else 1)
    print(__doc__)
