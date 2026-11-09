#!/usr/bin/env python3
"""
Maschine hardware test (no video)
=================================

Checks that a Native Instruments Maschine can be used directly over USB,
without the NI software: every button, every knob, all 16 pads and all LEDs.

Fully supported: Maschine MK1 (USB 17cc:0808, white pad lights).
Detected only:   Maschine MK2 (17cc:1140) and other NI devices - the script
                 tells you which one you have, and prints the USB layout.

Protocol source: the Linux kernel driver snd-usb-caiaq (sound/usb/caiaq),
which supports the Maschine MK1:
  - EP 0x01 OUT (bulk): commands  [cmd, payload...]
  - EP 0x81 IN  (bulk): replies   0x01 device info, 0x04 buttons, 0x02 knobs
  - EP 0x84 IN  (bulk): pads, 16-bit little-endian words: pad id << 12 | pressure
  - LEDs: [0x0C, bank, 32 values 0..63], bank 0x00 = LEDs 0-31, bank 0x1E = LEDs 32-63

Setup on Windows (one time):
  1. Install Python 3.10 or newer from python.org.
  2. pip install pyusb libusb-package
  3. Close Maschine / Controller Editor / NI software.
  4. Run Zadig (zadig.akeo.ie): Options > List All Devices, pick
     "Maschine Controller" (if several entries: the one with Interface 0),
     choose WinUSB, press Replace Driver.
     To undo later: Device Manager > the device > Uninstall device, tick
     "delete the driver", unplug and plug in again (or reinstall the NI driver).
  5. python maschine_test.py

Keys while running:  L = LED chase   A = all LEDs on/off   B = backlight on/off
                     R = reset checklist   S = save report   Q = quit
"""

import json
import os
import sys
import threading
import time

VID_NI = 0x17CC
PID_NAMES = {
    0x0808: "Maschine MK1",
    0x1140: "Maschine MK2",
    0x1110: "Maschine Mikro MK1",
    0x1200: "Maschine Mikro MK2",
    0x1300: "Maschine Studio",
    0x1600: "Maschine MK3",
    0x1700: "Maschine Mikro MK3",
}
PID_MK1 = 0x0808

EP_CMD_OUT = 0x01
EP_CMD_IN = 0x81
EP_PADS_IN = 0x84

CMD_GET_DEVICE_INFO = 0x01
CMD_READ_ERP = 0x02
CMD_READ_IO = 0x04
CMD_AUTO_MSG = 0x0B
CMD_DIMM_LEDS = 0x0C

# Button bit order (bit i of the button bitmap), from the kernel's keycode table.
# None = unused bit. Names marked * are best guesses: tell me if the wrong name lights up.
BUTTON_BITS = [
    "Mute", "Solo", "Select", "Duplicate", "Navigate", "Pad Mode", "Pattern", "Scene",
    None,
    "Rec", "Erase", "Shift", "Grid", "Transport >", "Transport <", "Restart",
    "Group E", "Group F", "Group G", "Group H", "Group D", "Group C", "Group B", "Group A",
    "Control", "Browse", "Page <", "Snap", "Auto Write", "Page >", "Sampling", "Step",
    "Top button 8*", "Top button 7*", "Top button 6*", "Top button 5*",
    "Top button 4*", "Top button 3*", "Top button 2*", "Top button 1*",
    "Note Repeat", "Play",
]

# LED index for each button name (kernel control table). Values 0..63.
LED_FOR_BUTTON = {
    "Mute": 16, "Solo": 17, "Select": 18, "Duplicate": 19, "Navigate": 20, "Pad Mode": 21,
    "Pattern": 22, "Scene": 23, "Shift": 24, "Erase": 25, "Grid": 26, "Transport >": 27,
    "Rec": 28, "Play": 29, "Transport <": 32, "Restart": 33,
    "Group H": 34, "Group G": 35, "Group D": 36, "Group C": 37, "Group F": 38, "Group E": 39,
    "Group B": 40, "Group A": 41, "Auto Write": 42, "Snap": 43, "Page >": 44, "Page <": 45,
    "Sampling": 46, "Browse": 47, "Step": 48, "Control": 49,
    "Top button 8*": 50, "Top button 7*": 51, "Top button 6*": 52, "Top button 5*": 53,
    "Top button 4*": 54, "Top button 3*": 55, "Top button 2*": 56, "Top button 1*": 57,
    "Note Repeat": 58,
}
LED_PAD = [3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12]  # LED for pad 1..16
LED_BACKLIGHT = 59
LED_MAX = 63

# Knobs: (name, byte a, byte b) inside the knob message (after the command byte).
KNOBS = [
    ("Knob 1", 21, 20), ("Knob 2", 15, 14), ("Knob 3", 9, 8), ("Knob 4", 3, 2),
    ("Knob 5", 19, 18), ("Knob 6", 13, 12), ("Knob 7", 7, 6), ("Knob 8", 1, 0),
    ("Volume", 17, 16), ("Tempo", 11, 10), ("Swing", 5, 4),
]

PAD_HIT = 300  # pressure (0..4095) that counts as a hit


def decode_erp(a, b):
    """Endless knob with two 90-degree tapers -> position 0..999 (port of the kernel's decode_erp)."""
    high_peak, low_peak = 268, -7
    rng = high_peak - low_peak
    deg90, deg180 = rng // 2, rng
    deg270, deg360 = deg90 + deg180, deg180 * 2
    mid = (high_peak + low_peak) // 2
    weight_b = abs(mid - a) - (rng // 2 - 100) // 2
    weight_b = max(0, min(100, weight_b))
    weight_a = 100 - weight_b
    if a < mid:
        pos_b = b - low_peak + deg270
        if pos_b >= deg360:
            pos_b -= deg360
    else:
        pos_b = high_peak - b + deg90
    if b > mid:
        pos_a = a - low_peak
    else:
        pos_a = high_peak - a + deg180
    ret = pos_a * weight_a + pos_b * weight_b
    ret = int(ret * 10 / deg360)  # C integer division truncates toward zero
    if ret < 0:
        ret += 1000
    if ret >= 1000:
        ret -= 1000
    return ret


def parse_buttons(payload):
    """Bitmap -> set of pressed button names."""
    pressed = set()
    for i, name in enumerate(BUTTON_BITS):
        if name is None or i // 8 >= len(payload):
            continue
        if payload[i // 8] & (1 << (i % 8)):
            pressed.add(name)
    return pressed


def parse_pads(data):
    """Pad stream -> {pad_index: pressure}. Each word carries its own pad id."""
    out = {}
    for i in range(0, len(data) - 1, 2):
        word = data[i] | (data[i + 1] << 8)
        out[word >> 12] = word & 0x0FFF
    return out


class Maschine:
    def __init__(self, dev):
        import usb.core
        import usb.util
        self.usb = usb
        self.dev = dev
        self.lock = threading.Lock()
        self.running = True
        self.info = None
        self.pressed = set()
        self.seen_buttons = set()
        self.knob_val = {k[0]: None for k in KNOBS}
        self.knob_start = {}
        self.seen_knobs = set()
        self.pad_val = [0] * 16
        self.pad_max = [0] * 16
        self.seen_pads = set()
        self.leds = [0] * 64
        self.leds_dirty = True
        self.errors = []
        self.msg_count = {"buttons": 0, "knobs": 0, "pads": 0}

        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except Exception:
            pass  # not supported on Windows; WinUSB has no kernel driver to detach
        try:
            dev.set_configuration()
        except usb.core.USBError as e:
            self.errors.append("set_configuration: %s (often harmless)" % e)
        usb.util.claim_interface(dev, 0)
        dev.set_interface_altsetting(interface=0, alternate_setting=1)

    def send(self, data):
        with self.lock:
            self.dev.write(EP_CMD_OUT, bytes(data), timeout=200)

    def start(self):
        threading.Thread(target=self._read_cmd_loop, daemon=True).start()
        threading.Thread(target=self._read_pads_loop, daemon=True).start()
        threading.Thread(target=self._led_loop, daemon=True).start()
        self.send([CMD_GET_DEVICE_INFO])
        time.sleep(0.2)
        self.send([CMD_AUTO_MSG, 1, 10, 5])  # report buttons, analog, knobs automatically

    def _read_cmd_loop(self):
        while self.running:
            try:
                buf = bytes(self.dev.read(EP_CMD_IN, 64, timeout=100))
            except self.usb.core.USBTimeoutError:
                continue
            except self.usb.core.USBError as e:
                self.errors.append("EP1 read: %s" % e)
                time.sleep(0.2)
                continue
            if not buf:
                continue
            cmd, payload = buf[0], buf[1:]
            if cmd == CMD_GET_DEVICE_INFO:
                fw = payload[0] | (payload[1] << 8) if len(payload) >= 2 else 0
                self.info = "firmware %d, %d knobs, %d digital in, %d digital out" % (
                    fw, payload[3] if len(payload) > 3 else -1,
                    payload[5] if len(payload) > 5 else -1, payload[6] if len(payload) > 6 else -1)
            elif cmd == CMD_READ_IO:
                self.msg_count["buttons"] += 1
                pressed = parse_buttons(payload)
                self.seen_buttons |= pressed
                for name in pressed ^ self.pressed:
                    led = LED_FOR_BUTTON.get(name)
                    if led is not None:
                        self.set_led(led, LED_MAX if name in pressed else 0)
                self.pressed = pressed
            elif cmd == CMD_READ_ERP and len(payload) >= 22:
                self.msg_count["knobs"] += 1
                for name, a, b in KNOBS:
                    v = decode_erp(payload[a], payload[b])
                    if name not in self.knob_start:
                        self.knob_start[name] = v
                    else:
                        d = abs(v - self.knob_start[name])
                        if min(d, 1000 - d) > 30:
                            self.seen_knobs.add(name)
                    self.knob_val[name] = v

    def _read_pads_loop(self):
        while self.running:
            try:
                buf = bytes(self.dev.read(EP_PADS_IN, 512, timeout=100))
            except self.usb.core.USBTimeoutError:
                continue
            except self.usb.core.USBError as e:
                self.errors.append("EP4 read: %s" % e)
                time.sleep(0.2)
                continue
            if len(buf) < 32:
                continue
            self.msg_count["pads"] += 1
            for pad, p in parse_pads(buf).items():
                if pad > 15:
                    continue
                self.pad_val[pad] = p
                self.pad_max[pad] = max(self.pad_max[pad], p)
                if p > PAD_HIT:
                    self.seen_pads.add(pad)
                self.set_led(LED_PAD[pad], min(LED_MAX, p * LED_MAX // 2048))

    def set_led(self, index, value):
        if self.leds[index] != value:
            self.leds[index] = value
            self.leds_dirty = True

    def _led_loop(self):
        while self.running:
            if self.leds_dirty:
                self.leds_dirty = False
                try:
                    self.send([CMD_DIMM_LEDS, 0x00] + self.leds[0:32])
                    self.send([CMD_DIMM_LEDS, 0x1E] + self.leds[32:64])
                except self.usb.core.USBError as e:
                    self.errors.append("LED write: %s" % e)
            time.sleep(1 / 30)

    def chase(self):
        order = list(range(0, 30)) + list(range(32, 60))
        saved = self.leds[:]
        for i in order:
            self.leds = [0] * 64
            self.leds[LED_BACKLIGHT] = saved[LED_BACKLIGHT]
            self.leds[i] = LED_MAX
            self.leds_dirty = True
            time.sleep(0.06)
        self.leds = saved
        self.leds_dirty = True

    def close(self):
        self.running = False
        time.sleep(0.15)
        try:
            self.leds = [0] * 64
            self.send([CMD_DIMM_LEDS, 0x00] + self.leds[0:32])
            self.send([CMD_DIMM_LEDS, 0x1E] + self.leds[32:64])
        except Exception:
            pass
        try:
            self.usb.util.release_interface(self.dev, 0)
        except Exception:
            pass


def bar(value, maximum, width=20):
    n = int(width * min(value, maximum) / maximum) if maximum else 0
    return "#" * n + "." * (width - n)


def render(m, name):
    real_buttons = [b for b in BUTTON_BITS if b]
    lines = []
    lines.append("MASCHINE TEST  -  %s  -  %s" % (name, m.info or "waiting for device info..."))
    lines.append("Messages: buttons %d, knobs %d, pads %d" % (m.msg_count["buttons"], m.msg_count["knobs"], m.msg_count["pads"]))
    lines.append("Checklist: buttons %d/%d   knobs %d/%d   pads %d/16" % (
        len(m.seen_buttons), len(real_buttons), len(m.seen_knobs), len(KNOBS), len(m.seen_pads)))
    lines.append("")
    lines.append("BUTTONS  ([x] = seen, >> = held now)")
    row = []
    for b in real_buttons:
        mark = ">>" if b in m.pressed else ("[x]" if b in m.seen_buttons else "[ ]")
        row.append("%-3s %-15s" % (mark, b))
        if len(row) == 4:
            lines.append("  " + " ".join(row))
            row = []
    if row:
        lines.append("  " + " ".join(row))
    lines.append("")
    lines.append("KNOBS  (0..999, turn each one a bit)")
    for name, _, _ in KNOBS:
        v = m.knob_val[name]
        mark = "[x]" if name in m.seen_knobs else "[ ]"
        lines.append("  %s %-8s %s %s" % (mark, name, "---" if v is None else "%3d" % v, bar(v or 0, 999, 25)))
    lines.append("")
    lines.append("PADS  (pressure now / max, 0..4095; pad LED follows pressure)")
    for r in range(4):
        cells = []
        for c in range(4):
            i = r * 4 + c
            mark = "x" if i in m.seen_pads else " "
            cells.append("[%s] P%-2d %4d/%4d" % (mark, i + 1, m.pad_val[i], m.pad_max[i]))
        lines.append("  " + "   ".join(cells))
    lines.append("")
    lines.append("Keys: L = LED chase   A = all LEDs   B = backlight   R = reset   S = save report   Q = quit")
    if m.errors:
        lines.append("")
        lines.append("Last errors:")
        for e in m.errors[-3:]:
            lines.append("  " + e)
    sys.stdout.write("\x1b[H\x1b[2J" + "\n".join(lines) + "\n")
    sys.stdout.flush()


def save_report(m, name):
    real_buttons = [b for b in BUTTON_BITS if b]
    report = {
        "device": name,
        "info": m.info,
        "buttons_seen": sorted(m.seen_buttons),
        "buttons_missing": [b for b in real_buttons if b not in m.seen_buttons],
        "knobs_seen": sorted(m.seen_knobs),
        "knobs_missing": [k[0] for k in KNOBS if k[0] not in m.seen_knobs],
        "pads_seen": sorted(p + 1 for p in m.seen_pads),
        "pad_max_pressure": m.pad_max,
        "errors": m.errors[-20:],
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    path = os.path.abspath("maschine_test_report.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    return path


def describe(dev):
    lines = ["USB %04x:%04x" % (dev.idVendor, dev.idProduct)]
    try:
        for cfg in dev:
            for intf in cfg:
                lines.append("  interface %d alt %d class 0x%02x" % (intf.bInterfaceNumber, intf.bAlternateSetting, intf.bInterfaceClass))
                for ep in intf:
                    kind = {0: "control", 1: "iso", 2: "bulk", 3: "interrupt"}[ep.bmAttributes & 3]
                    lines.append("    endpoint 0x%02x %s, max %d bytes" % (ep.bEndpointAddress, kind, ep.wMaxPacketSize))
    except Exception as e:
        lines.append("  (could not read descriptors: %s)" % e)
    return "\n".join(lines)


def find_devices():
    try:
        import usb.core
    except ImportError:
        print("pyusb is missing. Run:  pip install pyusb libusb-package")
        sys.exit(1)
    backend = None
    try:
        import libusb_package
        backend = libusb_package.get_libusb1_backend()
    except ImportError:
        pass
    devs = list(usb.core.find(find_all=True, idVendor=VID_NI, backend=backend))
    return devs


def key_pressed():
    if os.name == "nt":
        import msvcrt
        if msvcrt.kbhit():
            return msvcrt.getwch().lower()
        return None
    import select
    if select.select([sys.stdin], [], [], 0)[0]:
        return sys.stdin.read(1).lower()
    return None


def self_test():
    """Offline check of the decoders (no hardware needed): python maschine_test.py --selftest"""
    assert parse_buttons(bytes([0x01, 0, 0, 0, 0, 0])) == {"Mute"}
    assert parse_buttons(bytes([0, 0, 0, 0, 0, 0x02])) == {"Play"}
    assert parse_buttons(bytes([0, 0, 0x80, 0, 0, 0])) == {"Group A"}
    pads = parse_pads(bytes([0x34, 0x12, 0xFF, 0xFF]))
    assert pads == {1: 0x234, 15: 0xFFF}, pads
    vals = [decode_erp(a, b) for a in range(0, 256, 15) for b in range(0, 256, 15)]
    assert all(0 <= v <= 999 for v in vals)
    print("Self-test OK")


def main():
    if "--selftest" in sys.argv:
        self_test()
        return
    if os.name == "nt":
        os.system("")  # turn on ANSI colors/cursor codes in the Windows console
    devs = find_devices()
    if not devs:
        print("No Native Instruments device found over libusb.")
        print("- Is the Maschine plugged in and powered (MK1 needs its own power supply or a strong USB port)?")
        print("- On Windows: has Zadig switched it to WinUSB? With the NI driver, libusb cannot see it.")
        sys.exit(1)
    for d in devs:
        print("Found %s" % PID_NAMES.get(d.idProduct, "unknown NI device"))
        print(describe(d))
    mk1 = [d for d in devs if d.idProduct == PID_MK1]
    if not mk1:
        print("\nThis test drives the Maschine MK1. Your device is different: send me the lines above")
        print("and I will make the matching version.")
        sys.exit(0)
    name = PID_NAMES[PID_MK1]
    try:
        m = Maschine(mk1[0])
    except Exception as e:
        print("\nCould not open the Maschine: %s" % e)
        print("On Windows this usually means the NI driver is still active: run Zadig (see the top of this file).")
        print("Also close Maschine, Controller Editor and other NI software.")
        sys.exit(1)
    m.start()
    time.sleep(0.3)
    threading.Thread(target=m.chase, daemon=True).start()
    all_on = False
    note = ""
    try:
        while True:
            render(m, name)
            if note:
                print(note)
            k = key_pressed()
            if k == "q":
                break
            if k == "l":
                threading.Thread(target=m.chase, daemon=True).start()
            elif k == "a":
                all_on = not all_on
                for i in range(64):
                    m.set_led(i, LED_MAX if all_on and i not in (30, 31) else 0)
            elif k == "b":
                m.set_led(LED_BACKLIGHT, 0 if m.leds[LED_BACKLIGHT] else LED_MAX)
            elif k == "r":
                m.seen_buttons.clear(); m.seen_knobs.clear(); m.seen_pads.clear()
                m.knob_start.clear(); m.pad_max = [0] * 16
            elif k == "s":
                note = "Report saved: " + save_report(m, name)
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        path = save_report(m, name)
        m.close()
        print("\nReport saved to %s" % path)


if __name__ == "__main__":
    main()
