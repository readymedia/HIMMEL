# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""Writes weapons/v4.weapon.json: the V-4 panel drawing, laid out from the owner's photos.

Run:  python tools/v4_profile.py weapons/v4.weapon.json
Keeps the hardware/protocol fields already in the file and replaces the drawing.
"""
import json, math, sys
NAVY, INK, WHITE = "#353d55", "#2a3150", "#eef1f8"
deco, controls = [], []


def T(label, x, y, size=9, color=INK, weight=800, align="center", **kw):
    d = {"kind": "text", "label": label, "x": x, "y": y, "size": size, "color": color, "weight": weight, "align": align}
    d.update(kw)
    deco.append(d)


def R(x, y, w, h, fill=None, stroke=None, r=0):
    d = {"kind": "rect", "x": x, "y": y, "w": w, "h": h, "r": r}
    if fill:
        d["fill"] = fill
    if stroke:
        d["stroke"] = stroke
    deco.append(d)


def jack(x, y):
    deco.append({"kind": "jack", "x": x, "y": y, "r": 13})


def rb(name, x, y, r, led):
    controls.append({"name": name, "kind": "button", "shape": "round", "x": x - r, "y": y - r, "w": 2 * r, "h": 2 * r,
                     "label": "", "led_color": led})


def knob(name, x, y, r, **kw):
    d = {"name": name, "kind": "knob", "style": "black", "x": x, "y": y, "r": r, "range": 127, "dial": 11}
    d.update(kw)
    controls.append(d)


deco.append({"kind": "body", "x": 10, "y": 10, "w": 730, "h": 880, "r": 10, "fill": "linear-gradient(160deg, #e4e6e9, #c9ccd1)"})
# rear-panel labels along the top edge
T("1—INPUT—2", 95, 28); T("S-VIDEO", 95, 39, 6)
T("OUTPUT", 185, 28); T("S-VIDEO", 185, 39, 6)
T("IN — MIDI — OUT/THRU", 345, 28)
R(316, 34, 58, 11, fill=NAVY, r=2); T("V-LINK", 345, 40, 7, WHITE, 900, style="italic")
T("DC IN", 605, 28); T("▲", 655, 20, 7); T("POWER ON", 655, 30)
# jacks and preview select
T("INPUT", 115, 94, 10)
for i, x in enumerate((55, 95, 135, 175)):
    jack(x, 118)
    T(str(i + 1), x, 144, 10)
T("PREVIEW OUT SELECT", 372, 94, 10)
R(270, 116, 205, 3, fill=INK)
for lab, x in (("1", 270), ("2", 320), ("3", 370), ("4", 420), ("OUTPUT", 475)):
    T(lab, x, 144, 10 if len(lab) == 1 else 8)
jack(565, 118); T("PREVIEW OUT", 565, 141, 8); T("(ON SCREEN MENU)", 565, 151, 6)
jack(640, 118); jack(680, 118); T("OUTPUT", 660, 141, 8)
# the dark band: menu buttons, BPM display, tap, output fade
R(25, 166, 700, 104, fill=NAVY, r=8)
R(85, 221, 200, 3, fill="#1d2234")
for lab, x in (("MENU", 85), ("▲", 140), ("▼", 195), ("ENTER", 250)):
    T(lab, x, 192, 10, WHITE)
T("BPM", 369, 190, 10, WHITE)
deco.append({"kind": "display", "id": "bpm", "x": 287, "y": 198, "w": 165, "h": 56, "color": "#ff2a1a", "size": 44})
T("TAP", 531, 190, 10, WHITE); T("(BPM SYNC)", 531, 261, 7, WHITE)
T("BLACK", 632, 186, 8, WHITE); T("WHITE", 700, 186, 8, WHITE); T("OUTPUT FADE", 665, 261, 8, WHITE)
# centre column (BPM / CONTROL, MEMORY) and the transition panel
R(298, 262, 154, 222, fill=NAVY)
R(270, 478, 205, 126, fill=NAVY, r=6)
T("BPM /", 375, 271, 8, WHITE); T("CONTROL", 375, 280, 8, WHITE)
R(310, 361, 130, 2, fill="#cfd4e0")
T("MEMORY", 375, 370, 8, WHITE)
for i in range(8):
    a = math.radians(-135 + i * 270 / 7)
    T(str(i + 1), round(375 + 30 * math.sin(a)), round(414 - 30 * math.cos(a)), 8, WHITE)
T("TRANSITION", 372, 470, 13, WHITE, 900)
T("1 MIX", 297, 500, 9, WHITE); T("2 WIPE", 350, 500, 9, WHITE); T("3 EFX", 400, 500, 9, WHITE)
T("BPM", 450, 491, 8, WHITE); T("SYNC", 450, 500, 8, WHITE)
deco.append({"kind": "ladder", "source": "beat", "x": 440, "y": 551, "w": 22, "h": 30, "n": 5, "z": 3})
# input select (two rows of 4 key-cap buttons)
for side, x0, tab in (("A", 45, 75), ("B", 451, 481)):
    R(tab, 340, 190, 18, fill=NAVY, r=3); T("INPUT SELECT", tab + 95, 350, 10, WHITE)
    R(x0, 356, 244, 16, fill=NAVY)
    for i in range(4):
        x = x0 + i * 62
        T(str(i + 1), x + 29, 365, 10, WHITE)
        controls.append({"name": "%s input %d" % (side, i + 1), "kind": "button", "style": "cap", "x": x, "y": 372,
                         "w": 58, "h": 58, "label": "", "led_color": "#ff5a1f"})
# effect boxes
fxA = ["STROBE", "NEGATIVE", "COLORIZE", "MULTI"]
fxB = ["MIRROR", "CHROMA\n-KEY", "LUMINANCE\n-KEY", "P in P"]
for side, bx in (("A", 40), ("B", 488)):
    R(bx, 452, 222, 318, stroke="#5b627a", r=6)
    R(bx + 45, 444, 132, 17, fill=NAVY, r=3); T("EFFECTS", bx + 111, 453, 10, WHITE, 900)
for i, y in enumerate((482, 532, 582, 632)):
    T(str(i + 1), 92, y, 14, INK, 900, "left"); T(fxA[i], 106, y + 1, 9, INK, 800, "left")
    T(str(i + 1), 658, y, 14, INK, 900, "right"); T(fxB[i], 643, y + 1, 8, INK, 800, "right")
T("CONTROL", 205, 470, 9); T("CONTROL", 545, 470, 9)
T("A", 62, 712, 34, INK, 500); T("⏮", 157, 668, 15); T("TRANSFORMER", 157, 754, 8)
T("B", 688, 712, 34, INK, 500); T("⏭", 592, 668, 15); T("TRANSFORMER", 592, 754, 8)
T("▲", 375, 879, 11)
# Himmel's own words where the maker's logo and model name are printed
T("V4", 652, 826, 54, INK, 900, style="italic", font="Arial Black, Arial, sans-serif")
T("4-CHANNEL VIDEO MIXER", 652, 860, 10, INK, 800)
deco.append({"kind": "monitor", "id": "program", "source": "program", "x": 765, "y": 44, "w": 222, "h": 166, "label": "OUTPUT (simulated picture)"})
deco.append({"kind": "monitor", "id": "preview", "source": "preview", "x": 765, "y": 270, "w": 222, "h": 166, "label": "PREVIEW OUT (on-screen menu)"})

# controls
for name, x in (("Preview 1", 270), ("Preview 2", 320), ("Preview 3", 370), ("Preview 4", 420), ("Preview Out", 475)):
    rb(name, x, 118, 11, "#ff5a1f")
rb("Menu", 85, 222, 13, "#ff3b30"); rb("Up", 140, 222, 13, "#57e05a"); rb("Down", 195, 222, 13, "#57e05a"); rb("Enter", 250, 222, 13, "#ff8a1f")
controls.append({"name": "Tap", "kind": "button", "style": "dark", "x": 497, "y": 199, "w": 68, "h": 50, "label": "", "led_color": "#ff3b30"})
knob("Output fade", 665, 222, 19, center=64, dial=0, show_value=False)
knob("BPM control", 375, 312, 19, endless=True, show_value=False)
knob("Memory", 375, 414, 18, steps=8, range=7, dial=0, show_value=False)
for i, y in enumerate((482, 532, 582, 632)):
    rb("A effect %d" % (i + 1), 72, y, 13, "#7cfc4a")
    rb("B effect %d" % (i + 1), 680, y, 13, "#7cfc4a")
knob("A control", 205, 532, 22)
knob("B control", 545, 532, 22)
controls.append({"name": "A transformer", "kind": "button", "style": "dark", "x": 130, "y": 683, "w": 55, "h": 55, "label": "", "led_color": "#ff3b30"})
controls.append({"name": "B transformer", "kind": "button", "style": "dark", "x": 565, "y": 683, "w": 55, "h": 55, "label": "", "led_color": "#ff3b30"})
rb("Mix", 297, 533, 13, "#7cfc4a"); rb("Wipe", 350, 533, 13, "#7cfc4a"); rb("EFX", 400, 533, 13, "#ff3b30"); rb("BPM sync", 450, 533, 14, "#ff3b30")
controls.append({"name": "Video fader", "kind": "fader", "plate": True, "x": 223, "y": 543, "w": 304, "h": 304, "range": 127,
                 "ends": ["A", "B"], "orientation": "vertical", "orientations": ["vertical", "horizontal"],
                 "note": "The T-bar mounts up/down or, turned 90 degrees, left/right. Vertical: A near you (bottom), B away. Horizontal: A left, B right. Same MIDI either way."})

old = json.load(open(sys.argv[1], encoding="utf-8"))
w = {k: old[k] for k in ("format", "id", "name", "badge", "number", "hardware", "driver", "map", "live", "protocol")}
w["hardware"]["note"] = ("Drawn from the owner's photos of the V-4 (EDIROL and Roland editions) and the owner's manual (panel items 12-30). "
                         "The maker's logo and model print are replaced by Himmel's own words. The owner has no V-4 (2026-11-10), so this weapon is simulated only.")
w["brand"] = {"x": 40, "y": 836, "size": 22, "color": INK}
w["panel"] = {"w": 1000, "h": 900}
w["deco"], w["sections"], w["controls"] = deco, [], controls
one = lambda v: json.dumps(v, ensure_ascii=False)
parts = []
for k, v in w.items():
    if isinstance(v, list) and v:
        parts.append('  %s: [\n%s\n  ]' % (one(k), ",\n".join("    " + one(x) for x in v)))
    elif isinstance(v, dict) and k == "hardware":
        parts.append('  %s: {\n%s\n  }' % (one(k), ",\n".join("    %s: %s" % (one(a), one(b)) for a, b in v.items())))
    else:
        parts.append("  %s: %s" % (one(k), one(v)))
text = "{\n" + ",\n".join(parts) + "\n}\n"
json.loads(text)
open(sys.argv[1], "w", encoding="utf-8", newline="\n").write(text)
print(len(deco), "deco,", len(controls), "controls")
