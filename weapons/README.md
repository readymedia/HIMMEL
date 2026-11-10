# Weapons

In Himmel a **weapon** is a controller you play the show with. The Maschine MK2
is weapon no. 1. The idea: the rest of Himmel never talks to a device directly.
It only sees events (button, knob delta, pad velocity / pressure) and sends LED
and screen commands. So any controller can become a weapon, and a weapon you do
not own can be **simulated** on screen.

A weapon has up to four parts:

| Part | What it is | MK2 |
|---|---|---|
| **Profile** | `weapons/<id>.weapon.json`: every control with its kind, place on the drawing, label and colours. | [`mk2.weapon.json`](mk2.weapon.json) |
| **Map** | Which byte / bit / id each control has on the wire. Learned and checked on real hardware. | [`maschine_mk2_map.json`](../maschine_mk2_map.json) (verified 2026-11-09) |
| **Driver** | Talks to the device (USB HID, MIDI, ...) and turns reports into events. | `src/controller/` (C++), `tools/maschine_mk2_test.py` (Python) |
| **Simulator** | A pretend device that sends the same reports as the real one, driven by mouse and keyboard on the drawn panel. | [`tools/mk2_sim.py`](../tools/mk2_sim.py) |

## The live view: one tool for every weapon

`python tools/mk2_live.py` draws the weapon from its profile and shows it live:
controls light up when touched, knobs turn, pads glow by pressure, LEDs and
screens mirror what was sent to the device. Click a control and press the real
one to (re)learn it.

**No hardware?** Press **Simulate** (or start with `--sim`). The drawn panel is
now the controller:

- **Buttons:** hold the mouse button down.
- **Knobs and the encoder:** drag up/down, or use the scroll wheel.
- **Pads:** press. Higher up on the pad = harder hit; move while holding for
  aftertouch.
- **Shift:** the keyboard Shift key holds the Shift button, so Shift + knob and
  Shift + encoder (the mode menu) work.

Simulated input never ticks controls as "seen", never changes the map and is
never measured. The screen modes and games run on the simulator exactly as on
the device.

## Profile format (`himmel-weapon/1`)

```json
{
  "format": "himmel-weapon/1",
  "id": "mk2",
  "name": "MK2",
  "badge": "◎ HIMMEL · MK2",
  "hardware": { "maker": "...", "model": "...", "usb": "vid:pid", "note": "..." },
  "driver": "hid-mk2",
  "map": "maschine_mk2_map.json",
  "panel": { "w": 720, "h": 620 },
  "screens": [ { "id": "left", "x": 150, "y": 72, "w": 240, "h": 66, "px_w": 256, "px_h": 64 } ],
  "sections": [ { "label": "PADS", "x": 280, "y": 222, "w": 400 } ],
  "controls": [
    { "name": "Play", "kind": "button", "x": 42, "y": 522, "w": 40, "h": 20, "label": "PLAY", "on_color": "#3ddc84" },
    { "name": "Knob 1", "kind": "knob", "x": 166, "y": 160, "r": 18, "range": 1000 },
    { "name": "Master encoder (turn)", "kind": "encoder", "x": 116, "y": 238, "r": 30, "range": 16 },
    { "name": "Pad 1", "kind": "pad", "n": 1, "x": 355, "y": 476, "w": 63, "h": 63, "color": "#e8413c", "range": 4095 }
  ]
}
```

- Units are pixels on the drawing (`panel.w` × `panel.h`); the page scales it.
- `kind`: `button`, `knob` (absolute, wraps at `range`), `encoder` (endless,
  wraps at `range` steps), `pad` (pressure 0..`range`).
- `name` is the key shared with the map and the driver. Keep names stable.
- `badge` is what is printed on the drawing. Use Himmel's own words, not the
  maker's logo or brand (see [Trademarks](../THIRD_PARTY_NOTICES.md#8-trademarks)).
- Labels may contain `\n` for two lines.

## Adding a weapon (the same process as for the MK2)

1. **Profile:** write `weapons/<id>.weapon.json` from a photo of the front panel.
2. **Driver + map:** find the protocol (MIDI implementation chart, open-source
   projects, or watch the reports), then learn and check every control on the
   real device, with the live view. Commit the map only when every control is
   seen.
3. **Simulator:** a pretend device that produces the same reports.
4. **Credit** every source in `THIRD_PARTY_NOTICES.md`.

## Playing Himmel with the simulator

The simulator can also be the MK2 for the real engine:

```
python tools/mk2_live.py                    then press Simulate on the page
build\bin\Release\himmel.exe --sim -- --outputs 3 --size 640x360
```

`--sim` makes `mk2_controller.exe` read the simulated MK2 over UDP on
127.0.0.1:8766 instead of USB. The simulator sends the same raw reports as the
hardware; the controller sends its LED and screen reports back, so the drawn
panel shows the engine's screen pages and LEDs (the pill says "playing
Himmel"). Everything between the reports and the engine is the code that runs
with the real MK2. Stop simulating and the controller reports the MK2 lost
after 1 s; the engine keeps running, as with a pulled cable.

Today the live view and the simulator are MK2-specific in Python, and only the
drawing is generic. The next step is a generic MIDI weapon.
