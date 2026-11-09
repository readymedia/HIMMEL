#!/usr/bin/env python3
"""
Maschine MK2 hardware test and mapper (no video)
================================================

Checks that a Maschine MK2 (USB 17cc:1140) can be used directly over USB,
without the NI software, and LEARNS the map of every control:
  1. Hands off for 2 seconds (finds values that move on their own).
  2. Turn each knob (8 knobs + master encoder).
  3. Press each button (45).
  4. Hit each pad (16).
  5. LED tests: pads in colors, groups in colors, all buttons, one-by-one chase.
The result is saved as maschine_mk2_map.json, ready to use in Himmel.

Known protocol (MK2 is a vendor HID device):
  input  report 0x01 = buttons and knobs (25 bytes), 0x20 = pads (65 bytes)
  output report 0x80 = 16 pads x RGB, 0x81 = 8 groups x 2 LEDs x RGB + 8 transport,
                0x82 = 31 single-color buttons. LED values 0..127.
Sources: github.com/abique/ni-mk2 (LED layout), Encdr and maschine-rs notes.

Setup on Windows:
  1. Python 3.10+ from python.org.
  2. pip install hidapi pyusb libusb-package
  3. Close Maschine, Controller Editor and NI Hardware Agent if it runs.
  4. Run: python maschine_mk2_test.py
     If it says the Maschine is not reachable, the NI driver holds it. Then run Zadig
     (zadig.akeo.ie): Options > List All Devices, pick the Maschine MK2 entry whose
     interface the script names (the HID one), choose WinUSB, Replace Driver, and run again.
     Undo later: Device Manager > Uninstall device (tick delete driver), replug.

Keys: SPACE = skip step   BACKSPACE = previous step   1..4 = LED tests   0 = LEDs off
      S = save map   Q = quit

Modes:
  python tools/maschine_mk2_test.py           learn the map (saved to <repo>/maschine_mk2_map.json)
  python tools/maschine_mk2_test.py --verify  load the saved map and show every control by name;
                                              pressing a button lights the LED the tables think
                                              belongs to it (wrong LED = wrong table entry)
  python tools/maschine_mk2_test.py --list    list Native Instruments USB/HID devices and exit
An existing map is copied to maschine_mk2_map.bak.json before it is overwritten.
"""

import json
import os
import shutil
import sys
import threading
import time

MAP_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "maschine_mk2_map.json")

VID = 0x17CC
PID_MK2 = 0x1140
PID_NAMES = {0x0808: "Maschine MK1", 0x1140: "Maschine MK2", 0x1110: "Maschine Mikro MK1",
             0x1200: "Maschine Mikro MK2", 0x1300: "Maschine Studio", 0x1600: "Maschine MK3"}

KNOB_STEPS = ["Knob 1", "Knob 2", "Knob 3", "Knob 4", "Knob 5", "Knob 6", "Knob 7", "Knob 8", "Master encoder (turn)"]
BUTTON_STEPS = [
    "Control", "Step", "Browse", "Sampling", "Page <", "Page >", "All", "Auto Write",
    "Top 1", "Top 2", "Top 3", "Top 4", "Top 5", "Top 6", "Top 7", "Top 8",
    "Volume", "Swing", "Tempo", "Note Repeat", "Master encoder (push)",
    "Group A", "Group B", "Group C", "Group D", "Group E", "Group F", "Group G", "Group H",
    "Restart", "Transport <", "Transport >", "Grid", "Play", "Rec", "Erase", "Shift",
    "Scene", "Pattern", "Pad Mode", "Navigate", "Duplicate", "Select", "Solo", "Mute",
]
PAD_STEPS = ["Pad %d" % i for i in range(1, 17)]
STEPS = [("idle", "Hands off for 2 seconds")] + [("knob", n) for n in KNOB_STEPS] + \
        [("button", n) for n in BUTTON_STEPS] + [("pad", n) for n in PAD_STEPS] + [("done", "Done")]

# LED layout (output reports). Names marked * are guesses; the chase test (key 4) shows the truth.
LED_82 = ["Control", "Step", "Browse", "Sampling", "Page <", "Page >", "All", "Auto Write",
          "Top 1", "Top 2", "Top 3", "Top 4", "Top 5", "Top 6", "Top 7", "Top 8",
          "Scene", "Pattern", "Pad Mode", "Navigate", "Duplicate", "Select", "Solo", "Mute",
          "Volume", "Swing", "Tempo", "Master <*", "Master >*", "Enter*", "Note Repeat"]
LED_81_TRANSPORT = ["Restart*", "Transport <*", "Transport >*", "Grid*", "Play*", "Rec*", "Erase*", "Shift*"]
GROUPS = ["Group %s" % c for c in "ABCDEFGH"]
LED_MAX = 127
PAD_HIT = 400
PALETTE = [(127, 0, 0), (127, 40, 0), (127, 100, 0), (60, 127, 0), (0, 127, 20), (0, 127, 90),
           (0, 90, 127), (0, 30, 127), (40, 0, 127), (100, 0, 127), (127, 0, 90), (127, 0, 30),
           (127, 127, 127), (60, 60, 60), (127, 60, 60), (60, 60, 127)]


class Transport:
    """Opens the MK2 with hidapi if possible, else with libusb (WinUSB via Zadig)."""

    def __init__(self):
        self.kind = None
        self.h = None
        self.dev = None
        self.ep_in = None
        self.ep_out = None
        self.intf = None
        self.notes = []
        self.wlock = threading.Lock()  # LED and screen writers run in different threads

    def open(self):
        try:
            import hid
            for d in hid.enumerate(VID, PID_MK2):
                try:
                    h = hid.device()
                    h.open_path(d["path"])
                    h.set_nonblocking(False)
                    self.h, self.kind = h, "hidapi"
                    self.notes.append("Opened with hidapi (interface %s)" % d.get("interface_number"))
                    return True
                except Exception as e:
                    self.notes.append("hidapi open failed: %s" % e)
        except ImportError:
            self.notes.append("hidapi not installed (pip install hidapi)")
        try:
            import usb.core
            import usb.util
            backend = None
            try:
                import libusb_package
                backend = libusb_package.get_libusb1_backend()
            except ImportError:
                pass
            dev = usb.core.find(idVendor=VID, idProduct=PID_MK2, backend=backend)
            if dev is None:
                self.notes.append("libusb: MK2 not visible (NI driver active? use Zadig)")
                return False
            try:
                dev.set_configuration()
            except usb.core.USBError:
                pass
            cfg = dev.get_active_configuration()
            for intf in cfg:
                if intf.bInterfaceClass != 3:
                    continue
                ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
                ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
                try:
                    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                        dev.detach_kernel_driver(intf.bInterfaceNumber)
                except Exception:
                    pass
                usb.util.claim_interface(dev, intf.bInterfaceNumber)
                self.dev, self.ep_in, self.ep_out, self.intf = dev, ep_in, ep_out, intf.bInterfaceNumber
                self.kind = "libusb"
                self.notes.append("Opened with libusb, HID interface %d" % self.intf)
                return True
            self.notes.append("libusb: no HID interface found. Interfaces: " + ", ".join(
                "%d (class 0x%02x)" % (i.bInterfaceNumber, i.bInterfaceClass) for i in cfg))
        except ImportError:
            self.notes.append("pyusb not installed (pip install pyusb libusb-package)")
        except Exception as e:
            self.notes.append("libusb open failed: %s" % e)
        return False

    def read(self):
        """Returns one whole report (report id first) or None."""
        if self.kind == "hidapi":
            data = self.h.read(128, 100)
            return bytes(data) if data else None
        import usb.core
        try:
            data = bytes(self.ep_in.read(self.ep_in.wMaxPacketSize, timeout=100))
        except usb.core.USBTimeoutError:
            return None
        if data and data[0] == 0x20 and len(data) < 65:
            try:  # 65-byte pad report arrives as 64 + 1 bytes
                data += bytes(self.ep_in.read(self.ep_in.wMaxPacketSize, timeout=20))
            except usb.core.USBTimeoutError:
                pass
        return data

    def write(self, report):
        with self.wlock:
            self._write(bytes(report))

    def _write(self, report):
        if self.kind == "hidapi":
            self.h.write(report)
        elif self.ep_out is not None:
            self.ep_out.write(report, timeout=200)
        else:  # HID SET_REPORT over the control endpoint
            self.dev.ctrl_transfer(0x21, 0x09, 0x0200 | report[0], self.intf, report, timeout=200)

    def close(self):
        try:
            if self.kind == "hidapi":
                self.h.close()
            elif self.kind == "libusb":
                import usb.util
                usb.util.release_interface(self.dev, self.intf)
        except Exception:
            pass


class Mapper:
    def __init__(self, t):
        self.t = t
        self.running = True
        self.lock = threading.Lock()
        self.state = None            # latest report 0x01 payload (without id)
        self.prev = None
        self.change_count = {}       # byte index -> changes seen (for noise and knobs)
        self.changed_at = {}         # byte index -> time of last change (verify view)
        self.noise_bytes = set()
        self.pads = [0] * 16
        self.pad_max = [0] * 16
        self.raw_other = {}
        self.reports = {"0x01": 0, "0x20": 0, "other": 0}
        self.step = 0
        self.step_started = time.time()
        self.step_changes = {}
        self.result = {"buttons": {}, "knobs": {}, "pads": {}, "skipped": []}
        self.message = ""
        self.leds82 = [0] * 31
        self.leds81 = [0] * 56
        self.leds80 = [0] * 48
        self.dirty = True
        self.errors = []
        self.pad_feedback = True     # pads light by pressure; off while an LED test runs
        self.chase_stop = threading.Event()
        self.chase_thread = None

    # ---------- input ----------
    def reader(self):
        while self.running:
            try:
                r = self.t.read()
            except Exception as e:
                self.errors.append("read: %s" % e)
                time.sleep(0.2)
                continue
            if not r:
                continue
            rid, payload = r[0], r[1:]
            if rid == 0x01:
                self.reports["0x01"] += 1
                self.on_controls(payload)
            elif rid == 0x20:
                self.reports["0x20"] += 1
                self.on_pads(payload)
            else:
                self.reports["other"] += 1
                self.raw_other[rid] = payload[:16].hex()

    def on_controls(self, p):
        with self.lock:
            prev = self.state
            self.state = p
            if prev is None:
                return
            kind, name = STEPS[self.step]
            now = time.time()
            for i in range(min(len(p), len(prev))):
                if p[i] != prev[i]:
                    self.changed_at[i] = now
                    if kind != "done":
                        self.change_count[i] = self.change_count.get(i, 0) + 1
                        self.step_changes.setdefault(i, []).append((prev[i], p[i]))
            if kind == "button":
                masks = self.knob_masks()
                for i in range(min(len(p), len(prev))):
                    if i in self.noise_bytes:
                        continue
                    # ignore bits owned by knobs / encoder nibbles so a bumped knob is not learned as a button
                    newbits = p[i] & ~prev[i] & ~masks.get(i, 0) & 0xFF
                    for bit in range(8):
                        if newbits & (1 << bit) and (i, bit) not in self.used_bits():
                            self.result["buttons"][name] = {"byte": i, "bit": bit}
                            self.light_button(name)
                            self.next_step()
                            return
            elif kind == "knob":
                best = None
                for i, ch in self.step_changes.items():
                    if i in self.noise_bytes or i in self.knob_bytes():
                        continue
                    if len(ch) >= 6 and (best is None or len(ch) > len(self.step_changes[best])):
                        best = i
                if best is not None:
                    ch = self.step_changes[best]
                    lo = any((a ^ b) & 0x0F for a, b in ch)
                    hi = any((a ^ b) & 0xF0 for a, b in ch)
                    nib = "both" if lo and hi else ("low" if lo else "high")
                    partner = best + 1 if (best + 1) in self.step_changes and best % 2 == 0 else None
                    self.result["knobs"][name] = {"byte": best, "nibble": nib, "next_byte_moves": partner is not None}
                    self.next_step()

    def on_pads(self, p):
        best_id, best_p = None, 0
        for i in range(0, len(p) - 1, 2):
            w = p[i] | (p[i + 1] << 8)
            pid, pr = w >> 12, w & 0x0FFF
            if pid < 16:
                self.pads[pid] = pr
                self.pad_max[pid] = max(self.pad_max[pid], pr)
                if pr > best_p:
                    best_id, best_p = pid, pr
        for pid in range(16):
            if not self.pad_feedback:  # an LED test or the chase owns the pad LEDs
                break
            v = self.pads[pid]
            slot = self.result["pads"].get("_id_to_slot", {}).get(str(pid))
            if slot is not None:
                r, g, b = PALETTE[slot]
                k = min(1.0, v / 2048)
                # pad LEDs are assumed to follow the hardware pad id order (id 0 = top-left), like the input
                self.set80(pid, int(r * k), int(g * k), int(b * k))
        kind, name = STEPS[self.step]
        if kind == "pad" and best_id is not None and best_p > PAD_HIT:
            used = {v["id"] for k, v in self.result["pads"].items() if not k.startswith("_")}
            if best_id not in used:
                slot = int(name.split()[1]) - 1
                self.result["pads"][name] = {"id": best_id}
                self.result["pads"].setdefault("_id_to_slot", {})[str(best_id)] = slot
                self.next_step()

    def used_bits(self):
        return {(v["byte"], v["bit"]) for v in self.result["buttons"].values()}

    def knob_bytes(self):
        return {v["byte"] for v in self.result["knobs"].values() if v["nibble"] == "both"}

    def knob_masks(self):
        """byte index -> bit mask owned by a learned knob or encoder."""
        masks = {}
        for v in self.result["knobs"].values():
            m = {"both": 0xFF, "low": 0x0F, "high": 0xF0}[v["nibble"]]
            masks[v["byte"]] = masks.get(v["byte"], 0) | m
        return masks

    def learned_anything(self):
        return bool(self.result["buttons"] or self.result["knobs"] or
                    any(not k.startswith("_") for k in self.result["pads"]))

    # ---------- steps ----------
    def next_step(self):
        self.step = min(self.step + 1, len(STEPS) - 1)
        self.step_started = time.time()
        self.step_changes = {}

    def prev_step(self):
        with self.lock:
            self.step = max(self.step - 1, 0)
            kind, name = STEPS[self.step]
            self.result["buttons"].pop(name, None)
            self.result["knobs"].pop(name, None)
            if name in self.result["skipped"]:
                self.result["skipped"].remove(name)
            if name in self.result["pads"]:
                pid = self.result["pads"].pop(name)["id"]
                self.result["pads"].get("_id_to_slot", {}).pop(str(pid), None)
            self.step_started = time.time()
            self.step_changes = {}

    def skip(self):
        with self.lock:
            kind, name = STEPS[self.step]
            if kind != "done":
                self.result["skipped"].append(name)
                self.next_step()

    def tick(self):
        kind, _ = STEPS[self.step]
        if kind == "idle" and time.time() - self.step_started > 2.0:
            with self.lock:
                self.noise_bytes = {i for i, c in self.change_count.items() if c >= 3}
                self.change_count = {}
                self.next_step()

    # ---------- LEDs ----------
    def set80(self, pad, r, g, b):
        i = pad * 3
        if self.leds80[i:i + 3] != [r, g, b]:
            self.leds80[i:i + 3] = [r, g, b]
            self.dirty = True

    def light_button(self, name):
        if name in LED_82:
            self.leds82[LED_82.index(name)] = LED_MAX
        elif name in GROUPS:
            g = GROUPS.index(name)
            r, gg, b = PALETTE[g * 2]
            self.leds81[g * 6:g * 6 + 6] = [r, gg, b, r, gg, b]
        else:
            for i, n in enumerate(LED_81_TRANSPORT):
                if n.rstrip("*") == name:
                    self.leds81[48 + i] = LED_MAX
        self.dirty = True

    def all_off(self):
        self.leds80, self.leds81, self.leds82 = [0] * 48, [0] * 56, [0] * 31
        self.dirty = True

    def led_writer(self):
        while self.running:
            if self.dirty:
                self.dirty = False
                try:
                    self.t.write([0x80] + self.leds80)
                    self.t.write([0x81] + self.leds81)
                    self.t.write([0x82] + self.leds82)
                except Exception as e:
                    self.errors.append("LED write: %s" % e)
            time.sleep(1 / 30)

    def leds_off(self):
        """Stop the chase, all LEDs off and hand the pad LEDs back to pressure feedback."""
        self.chase_stop.set()
        self.all_off()
        self.pad_feedback = True

    def chase_running(self):
        return self.chase_thread is not None and self.chase_thread.is_alive()

    def toggle_chase(self):
        """One chase at a time: starts it, or stops the one that runs."""
        if self.chase_running():
            self.leds_off()
            self.message = "LED chase stopped"
            return
        self.chase_stop = threading.Event()
        self.chase_thread = threading.Thread(target=self.chase, args=(self.chase_stop,), daemon=True)
        self.chase_thread.start()

    def test_pads(self):
        self.chase_stop.set()
        self.pad_feedback = False
        for p in range(16):
            self.set80(p, *PALETTE[p])

    def test_groups(self):
        self.chase_stop.set()
        for g in range(8):
            r, gg, b = PALETTE[g * 2]
            self.leds81[g * 6:g * 6 + 6] = [r, gg, b, r, gg, b]
        self.dirty = True

    def test_buttons(self):
        self.chase_stop.set()
        self.leds82 = [LED_MAX] * 31
        self.leds81[48:56] = [LED_MAX] * 8
        self.dirty = True

    def chase(self, stop):
        steps = [("pad", i) for i in range(16)] + [("group", i) for i in range(8)] + \
                [("transport", i) for i in range(8)] + [("button", i) for i in range(31)]
        self.pad_feedback = False
        for kind, i in steps:
            if not self.running or stop.is_set():
                return
            self.all_off()
            if kind == "pad":
                self.set80(i, 127, 127, 127); label = "Pad LED slot %d" % (i + 1)
            elif kind == "group":
                self.leds81[i * 6:i * 6 + 6] = [127] * 6; label = GROUPS[i] + " (assumed)"
            elif kind == "transport":
                self.leds81[48 + i] = LED_MAX; label = LED_81_TRANSPORT[i]
            else:
                self.leds82[i] = LED_MAX; label = LED_82[i]
            self.dirty = True
            self.message = "LED chase: %s - check that this one is lit" % label
            if stop.wait(0.3):
                return
        self.leds_off()
        self.message = "LED chase done"


def render(m):
    kind, name = STEPS[m.step]
    L = []
    L.append("MASCHINE MK2 TEST AND MAPPER  -  %s" % ", ".join(m.t.notes[-1:]))
    L.append("Reports received: controls %d, pads %d, other %d" % (m.reports["0x01"], m.reports["0x20"], m.reports["other"]))
    L.append("Learned: knobs %d/%d   buttons %d/%d   pads %d/16   skipped %d" % (
        len(m.result["knobs"]), len(KNOB_STEPS), len(m.result["buttons"]), len(BUTTON_STEPS),
        len([k for k in m.result["pads"] if not k.startswith("_")]), len(m.result["skipped"])))
    L.append("")
    verb = {"idle": "", "knob": "TURN", "button": "PRESS", "pad": "HIT", "done": ""}[kind]
    L.append(">>> Step %d/%d:  %s %s" % (m.step + 1, len(STEPS), verb, name))
    if kind == "done":
        L.append("    All steps done. Press S to save the map, 4 for the LED chase, Q to quit.")
    L.append("")
    if m.state is not None:
        L.append("Controls report (bytes, changing ones marked *):")
        cells = []
        for i, b in enumerate(m.state):
            mark = "*" if m.step_changes.get(i) else " "
            cells.append("%02d:%02x%s" % (i, b, mark))
        for r in range(0, len(cells), 12):
            L.append("  " + " ".join(cells[r:r + 12]))
    else:
        L.append("No controls report yet - press a button.")
    L.append("")
    L.append("Pads (pressure 0..4095, by raw pad id):")
    for r in range(4):
        L.append("  " + "   ".join("id%-2d %4d" % (r * 4 + c, m.pads[r * 4 + c]) for c in range(4)))
    L.append("")
    L.append("Keys: SPACE skip  BACKSPACE back  1 pads  2 groups  3 buttons  4 chase  0 off  S save  Q quit")
    if m.message:
        L.append(m.message)
    if m.errors:
        L.append("Errors: " + " | ".join(m.errors[-2:]))
    draw(L)


def draw(lines):
    # cursor home + clear each line's tail: no full-screen clear, so no flicker on Windows
    sys.stdout.write("\x1b[H" + "".join(l + "\x1b[K\n" for l in lines) + "\x1b[J")
    sys.stdout.flush()


def save(m):
    out = dict(m.result)
    out["device"] = "Maschine MK2 (17cc:1140)"
    out["noise_bytes"] = sorted(m.noise_bytes)
    out["pad_max"] = m.pad_max
    out["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    path = MAP_PATH
    if os.path.exists(path) and not getattr(m, "backed_up", False):
        shutil.copyfile(path, path.replace(".json", ".bak.json"))
        m.backed_up = True
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    os.replace(tmp, path)
    return path


def key():
    if os.name == "nt":
        import msvcrt
        if msvcrt.kbhit():
            c = msvcrt.getwch()
            return {"\x08": "BACK", " ": "SPACE"}.get(c, c.lower())
        return None
    import select
    if select.select([sys.stdin], [], [], 0)[0]:
        c = sys.stdin.read(1)
        return {"\x7f": "BACK", "\x08": "BACK", " ": "SPACE"}.get(c, c.lower())
    return None


def list_ni_devices():
    found = []
    try:
        import hid
        for d in hid.enumerate(VID, 0):
            found.append("hidapi: %04x:%04x %s (interface %s)" % (d["vendor_id"], d["product_id"], d.get("product_string"), d.get("interface_number")))
    except ImportError:
        pass
    try:
        import usb.core
        backend = None
        try:
            import libusb_package
            backend = libusb_package.get_libusb1_backend()
        except ImportError:
            pass
        for d in usb.core.find(find_all=True, idVendor=VID, backend=backend):
            found.append("libusb: %04x:%04x %s" % (d.idVendor, d.idProduct, PID_NAMES.get(d.idProduct, "")))
            try:
                for cfg in d:
                    for intf in cfg:
                        eps = ", ".join("0x%02x" % e.bEndpointAddress for e in intf)
                        found.append("   interface %d alt %d class 0x%02x endpoints %s" % (intf.bInterfaceNumber, intf.bAlternateSetting, intf.bInterfaceClass, eps))
            except Exception:
                pass
    except ImportError:
        pass
    return found


INV, DIM, RST = "\x1b[7m", "\x1b[2m", "\x1b[0m"
CELL = 11


def cell(text, on=False, known=True):
    s = ("%s?" % text if not known else text)[:CELL - 2].ljust(CELL - 2)
    s = "[" + s + "]"
    return (INV + s + RST) if on else (s if known else DIM + s + RST)


def panel(m, st, held, by_slot):
    """ASCII picture of the MK2 front panel. Held controls are drawn inverted."""
    btn = lambda n, label=None: cell(label or n, n in held, n in m.result["buttons"])

    def knob(name, label):
        v = m.result["knobs"].get(name)
        if v is None:
            return cell(label, known=False)
        if st is None or v["byte"] >= len(st):
            return cell(label + " -")
        b = v["byte"]
        if v["nibble"] == "both":
            val = st[b] | ((st[b + 1] if b + 1 < len(st) else 0) << 8)
        else:
            val = st[b] & 0x0F if v["nibble"] == "low" else st[b] >> 4
        moved = time.time() - m.changed_at.get(b, 0) < 0.4
        return cell("%s %d" % (label, val), moved)

    def pad(n):
        known = ("Pad %d" % n) in m.result["pads"]
        p = by_slot[n - 1]
        return cell("P%d %d" % (n, p), p > PAD_HIT, known)

    gap, sep = " " * CELL, " | "
    rows = [
        [btn("Control"), btn("Step"), btn("Browse"), btn("Sampling")] + [sep] + [btn("Top %d" % i, "T%d" % i) for i in range(1, 9)],
        [btn("Page <"), btn("Page >"), btn("All"), btn("Auto Write", "AutoWrite")] + [sep] + ["      (left screen)      ", "                    (right screen)"],
        [btn("Volume"), btn("Swing"), btn("Tempo"), knob("Master encoder (turn)", "Enc")] + [sep] + [knob("Knob %d" % i, "K%d" % i) for i in range(1, 9)],
        [btn("Note Repeat", "NoteRep"), btn("Master <", "Mst <"), btn("Master >", "Mst >"), btn("Master encoder (push)", "Enc push")] + [sep, btn("Enter")],
        [],
        [btn("Group A", "A"), btn("Group B", "B"), btn("Group C", "C"), btn("Group D", "D"), sep, btn("Scene"), sep] + [pad(i) for i in (13, 14, 15, 16)],
        [gap, gap, gap, gap, sep, btn("Pattern"), sep],
        [btn("Group E", "E"), btn("Group F", "F"), btn("Group G", "G"), btn("Group H", "H"), sep, btn("Pad Mode"), sep] + [pad(i) for i in (9, 10, 11, 12)],
        [gap, gap, gap, gap, sep, btn("Navigate"), sep],
        [btn("Restart"), btn("Transport <", "Trans <"), btn("Transport >", "Trans >"), btn("Grid"), sep, btn("Duplicate"), sep] + [pad(i) for i in (5, 6, 7, 8)],
        [gap, gap, gap, gap, sep, btn("Select"), sep],
        [btn("Play"), btn("Rec"), btn("Erase"), btn("Shift"), sep, btn("Solo"), sep] + [pad(i) for i in (1, 2, 3, 4)],
        [gap, gap, gap, gap, sep, btn("Mute"), sep],
    ]
    return ["  " + "".join(r) for r in rows]


def unmapped_bits(m, st):
    if st is None:
        return ""
    used = m.used_bits()
    masks = m.knob_masks()
    out = []
    for i in range(min(len(st), 8)):  # button/encoder area only; knobs are 16-bit words after it
        for bit in range(8):
            if st[i] >> bit & 1 and (i, bit) not in used and not (masks.get(i, 0) >> bit & 1):
                out.append("byte %d bit %d" % (i, bit))
    return ", ".join(out)


def verify(t):
    """Live view of the saved map: names of held buttons, knob values, pad pressure per slot."""
    with open(MAP_PATH, encoding="utf-8") as f:
        saved = json.load(f)
    m = Mapper(t)
    m.result = {"buttons": saved.get("buttons", {}), "knobs": saved.get("knobs", {}),
                "pads": saved.get("pads", {}), "skipped": saved.get("skipped", [])}
    m.step = len(STEPS) - 1  # "done": the reader only tracks state, learns nothing
    threading.Thread(target=m.reader, daemon=True).start()
    threading.Thread(target=m.led_writer, daemon=True).start()
    held_before = set()
    last_seen = []
    try:
        while True:
            st = m.state
            held = set()
            if st is not None:
                for name, v in m.result["buttons"].items():
                    if v["byte"] < len(st) and st[v["byte"]] >> v["bit"] & 1:
                        held.add(name)
            if held != held_before:
                m.leds81[48:56] = [0] * 8
                m.leds82 = [0] * 31
                for g in range(8):
                    m.leds81[g * 6:g * 6 + 6] = [0] * 6
                for name in held:
                    m.light_button(name)
                m.dirty = True
                for name in held - held_before:
                    last_seen = ([name] + last_seen)[:8]
                held_before = held
            slot_of = {int(k): s for k, s in m.result["pads"].get("_id_to_slot", {}).items()}
            by_slot = [0] * 16
            for pid, s in slot_of.items():
                by_slot[s] = m.pads[pid]
            L = ["MASCHINE MK2 MAP VERIFY  -  %s" % MAP_PATH,
                 "Reports: controls %d, pads %d" % (m.reports["0x01"], m.reports["0x20"])]
            if saved.get("verified") is False:
                L.append("Map is PREFILLED and not yet verified on this device.")
            L.append("")
            L += panel(m, st, held, by_slot)
            L.append("")
            L.append("Highlighted = what the map thinks you touch. Press each control and check that the")
            L.append("right box lights up (and the right LED on the device). ? = not in the map.")
            L.append("Last pressed: " + (", ".join(last_seen) or "-"))
            L.append("Unmapped bits set: " + (unmapped_bits(m, st) or "-"))
            L.append("")
            L.append("Keys: 1 pads  2 groups  3 buttons  4 chase  0 off  Q quit")
            if m.message:
                L.append(m.message)
            if m.errors:
                L.append("Errors: " + " | ".join(m.errors[-2:]))
            draw(L)
            k = key()
            if k == "q":
                break
            elif k in ("1", "2", "3"):
                {"1": m.test_pads, "2": m.test_groups, "3": m.test_buttons}[k]()
            elif k == "4":
                m.toggle_chase()
            elif k == "0":
                m.leds_off()
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        m.running = False
        time.sleep(0.15)


def main():
    if os.name == "nt":
        os.system("")
    if "--list" in sys.argv:
        for line in list_ni_devices() or ["none"]:
            print(line)
        return
    if "--verify" in sys.argv and not os.path.exists(MAP_PATH):
        print("No map yet at %s - run without --verify first." % MAP_PATH)
        sys.exit(1)
    t = Transport()
    if not t.open():
        print("Could not open the Maschine MK2.\n")
        for n in t.notes:
            print("  " + n)
        print("\nNI devices seen:")
        for line in list_ni_devices() or ["  none"]:
            print("  " + line)
        print("\nIf the MK2 is listed by libusb but not opened: run Zadig, pick the MK2 HID interface")
        print("(class 0x03 above), choose WinUSB, Replace Driver, and run again.")
        sys.exit(1)
    sys.stdout.write("\x1b[2J")
    if "--verify" in sys.argv:
        try:
            verify(t)
        finally:
            leds_off_and_close(t)
        return
    m = Mapper(t)
    threading.Thread(target=m.reader, daemon=True).start()
    threading.Thread(target=m.led_writer, daemon=True).start()
    try:
        while True:
            m.tick()
            render(m)
            k = key()
            if k == "q":
                break
            elif k == "SPACE":
                m.skip()
            elif k == "BACK":
                m.prev_step()
            elif k == "1":
                m.test_pads()
            elif k == "2":
                m.test_groups()
            elif k == "3":
                m.test_buttons()
            elif k == "4":
                m.toggle_chase()
            elif k == "0":
                m.leds_off()
            elif k == "s":
                m.message = "Map saved: " + save(m)
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        m.running = False
        time.sleep(0.15)
        leds_off_and_close(t)
        if m.learned_anything():
            print("\nMap saved to %s" % save(m))
        else:
            print("\nNothing learned - existing map left untouched.")


def leds_off_and_close(t):
    try:
        t.write([0x80] + [0] * 48); t.write([0x81] + [0] * 56); t.write([0x82] + [0] * 31)
    except Exception:
        pass
    t.close()


if __name__ == "__main__":
    main()
