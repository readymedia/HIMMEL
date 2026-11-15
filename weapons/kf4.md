# Weapon no. 6: the KF4 (a 4-input SD video mixer)

The owner owns a Korg KF4 "krossfour" 4-channel video mixer & switcher and
wants it (1) simulated fully as a weapon and (2) checked as a possible control
surface for Himmel. Started 2026-11-10.

```
python tools/mk2_live.py --port 8779 --weapon kf4      then open http://127.0.0.1:8779
python tools/kf4_sim.py --selftest                       mixer logic + engine link, no browser
```

`.claude/launch.json` has the config `kf4-live`. In the picker of any live view
the KF4 entry opens its own page, `tools/kf4_live.html` (profile field
`page`, the same hook as the NuVJ), because it needs picture outputs that the other weapons do not.

## The unit

| | |
|---|---|
| Model | Korg KF4 krossfour, "4 channel video mixer & switcher" |
| Year | Announced at NAMM, January 2005 (US MSRP $900, street ~$699), as a companion to the KPE1 Kaoss Pad Entrancer. Discontinued. |
| Video | NTSC / PAL (rear switch, read **only at power-on**), ITU-601, 13.5 MHz 4:2:2 8-bit, two frame synchronizers (A and B) = a time base corrector on each channel |
| Inputs | IN 1: S-Video **or** composite (S-Video wins if both are plugged). IN 2, IN 3: composite (rear). IN 4: composite (front) |
| Outputs | OUTPUT: S-Video + composite, the same mixed picture. MONITOR OUT (front, composite): any input, or OUT (the mix) |
| Power | DC 7 V adapter, power switch ON / STANDBY |
| Size | 185 × 249 × 75 mm, 1.5 kg |
| **Data ports** | **None.** No MIDI, no USB, no RS-232, no GPI/tally, no remote or sync input. The manual's rear and front panel lists have only video jacks, NTSC/PAL, DC IN and power. |

### Controls (all simulated)

| Control | Behaviour (owner's manual / EasyStart) |
|---|---|
| INPUT A 1–4, INPUT B 1–4 | Choose the input for each channel; the key glows red. The same input may be on both channels. **Press the lit key again: it blinks** and the input is ignored: in BK / WH the channel becomes a black / white background, in C-K / L-K a blinking A gives a black background and a blinking B drops channel B (only A is output). An input with no signal shows **blue**. |
| HOLD (A, B) | Toggle: freezes the channel's picture (red). You can change the input while frozen; the still stays until HOLD is released. In C-K, HOLD B while a B key blinks makes channel B black. Tapping HOLD in time with music = stutter. |
| Crossfader | DJ-style. Mixes A and B through the selected curve. In C-K / L-K it only fades the B overlay (A stays the background). |
| FADER CURVE (front, 4 positions) | 1 Normal (linear, 50:50 at the centre) · 2 No mix (through black at the centre) · 3 Center mix (blends only over the middle of the travel) · 4 Switcher (a cut, no fades). |
| MODE (front, 4 positions) | BK black background · WH white background · C-K chroma key (blue in B is replaced by A) · L-K luma key (black / white / grey in B is replaced by A). |
| FINE (front knob) | Only in C-K / L-K: the key colour (C-K) or the keyed brightness from white through grey to black (L-K). |
| MONITOR SELECT 1–4, OUT | What MONITOR OUT shows (red when selected). OUT makes the monitor a third copy of the mix. |
| Power LED | On while powered (blinks on a fault: "turn the power off at once"). |

### Guesses to check on the owner's unit

The manual does not say these; the simulator marks them `GUESS` in the code.

1. **Panel positions** in `weapons/kf4.weapon.json` are drawn from the manual's
   text, not a photo. Fix them from the owner's photos (top, front, rear).
2. The state after power-on (we assume A = 1, B = 2, monitor = OUT, no HOLD).
3. A **third** press on a blinking key: back to solid? (we assume yes).
4. Curve 3: which part of the travel blends (we use the middle half; the
   EasyStart guide says "from the centre").
5. Curve 4: where the cut is (we use the centre).
6. The FINE range in C-K (we use hue 180–300°, blue in the middle) and the
   blink rate (~2 Hz).
7. Front panel item 6 in the manual has no description in the copies we
   found. Look at the unit.

## Goal 2: can the real KF4 be a control surface?

**Plainly: no, not directly.** The KF4 has no data port of any kind, so it
cannot send what you do on it, and nothing can control it. Its keys, fader
and knob only steer its own video chip. Himmel cannot read or write it over a
cable. Options, for the owner to choose:

| Option | What it takes | What you get | Notes |
|---|---|---|---|
| **A. KF4 as a video source into Himmel** | A USB capture device with S-Video / composite in (UVC, e.g. a cheap "USB video grabber", or a better SD capture card), on the KF4's OUTPUT. | The KF4's mix (cameras, VCR, DVD, game consoles, anything analog) arrives in Himmel as a camera. `CameraCapture` (Media Foundation) already does this; nothing new to write. | Easiest and fully reversible. Adds the capture latency (typically 1–3 frames) on top of the KF4's own frame synchronizer delay. SD 4:3. Measure with Himmel's latency test. |
| **B. KF4 after Himmel (an analog mixer at the end)** | An HDMI → composite / S-Video converter on one Himmel output, into a KF4 input; the KF4's OUTPUT to a projector or CRT. | The KF4 mixes Himmel with live analog sources, played by hand, with its real crossfader feel and the retro SD look. | Himmel's output is downscaled to SD; the KF4's limits (blue key, SD) apply. Good for a "retro" set, not for the 3-screen wall. |
| **C. Read the KF4 optically** (experimental) | Option A, plus Himmel feeding known test pictures into the KF4's inputs (e.g. 2–4 HDMI → composite converters, each showing a coded colour / pattern). | Himmel analyses the captured mix and works out the crossfader position (from the mix ratio), which inputs are on A / B, HOLD (a frame counter stops), mode and curve. The KF4 then *is* a control surface, through its video. | Clever but limited: needs spare outputs and converters, ~2–4 frames of delay, and it only works while the KF4 mixes Himmel's own test signals. Worth a small test only if A works well. |
| **D. Open it up and add a microcontroller** | Soldering: tap the fader pot, FINE pot, switches and key matrix with e.g. a Raspberry Pi Pico running USB-MIDI firmware. | A real MIDI control surface with the KF4's fader and keys. | Hard to undo and risks a collector's unit. Not recommended unless the owner wants exactly this. Needs the service manual or a careful look inside. |

My recommendation: start with **A** (it is a capture device and a cable, and
Himmel already supports cameras). Use the **simulator** for everything that
needs the KF4 as a controller. Try **B** for a retro set if the owner likes it.

## Goal 1: the simulator and how it plays Himmel

`tools/kf4_sim.py` is the mixer: the state machine, LEDs, curves, keys and
power-on latching, plus `curve_gains()` (also mirrored in the page). The page
`tools/kf4_live.html` draws the panel from the profile and renders four
simulated inputs (camera, colour bars, a dancer on a blue screen, luma shapes),
MAIN OUT and MONITOR OUT at 192 × 144 with the real mixing rules: curves,
BK / WH backgrounds, blue for no signal, HOLD stills, chroma and luma key,
and a rolling, colourless picture when the NTSC/PAL setting does not match the
sources. Cables can be pulled and plugged by clicking the jacks.

### Events: the simulator speaks Himmel's event model directly

There are no device bytes to imitate, so every control change goes out as a
**Himmel event** (JSON datagram) to UDP **127.0.0.1:8771**, with a full state
heartbeat every 250 ms. Nothing reads them yet; a future engine bridge (or
`weapon_kf4` driver process, as in the plan in `README.md`) would turn them
into the 16-byte `Event`s:

| KF4 control | Datagram | Himmel `Event` (proposal) |
|---|---|---|
| Any key (INPUT A/B 1–4, HOLD A/B, MONITOR 1–4/OUT) | `{"t":"button","id":"A 3","down":true}` | `ButtonDown` / `ButtonUp`, `id` = control index in the profile |
| Crossfader | `{"t":"fader","id":"Crossfader","v":0..1023}` | new `FaderMove`: `id` = control index, `value` = absolute 0..1023 (a fader is absolute; it is not turned into deltas like the MK2 knobs) |
| FINE | `{"t":"knob","id":"Fine","v":0..1023}` | `KnobDelta` (the delta) plus the absolute in `raw`, like the MK2 knobs |
| FADER CURVE, MODE, NTSC/PAL, POWER | `{"t":"switch","id":"Mode","pos":2}` | new `SwitchPos`: `id`, `value` = position |
| Power cycle | `{"t":"power","on":true,"std":"PAL"}` | `Connected` / `Disconnected` |
| Cable pulled / plugged (simulator only) | `{"t":"cable","id":"in2","plugged":false}` | none (it is the simulated world, not a control) |
| Heartbeat | `{"t":"state","state":{...}}` every 250 ms | the driver's heartbeat; a missing heartbeat for 1 s = `Disconnected` |

### How Himmel acts as the KF4 (proposal for the engine)

The KF4 maps naturally onto Himmel's deck model:

- **IN 1–4 = four Himmel sources.** Each slot holds a clip, a camera or a
  generator (a show setting). Unassigned = a blue "no signal" card.
- **Channel A / B = two Himmel decks.** INPUT A/B keys load that source into the
  deck. A blinking key = the deck shows the background (black / white by
  MODE), exactly as on the unit.
- **HOLD = freeze the deck** (keep the last frame on the GPU; the source keeps
  running under it, so release is instant). Tapping HOLD on the beat = stutter
  (the MK2's Note Repeat can do the same, synced).
- **Crossfader + FADER CURVE = the A/B mix**, a tiny shader with the four
  transfer functions from `curve_gains()`. Cheap on the GPU.
- **MODE + FINE = the composite mode:** BK / WH background colour, or a chroma
  key (FINE = key hue) / luma key (FINE = threshold) of B over A. Our own HLSL
  core effects; the same keyer later serves the cameras.
- **MONITOR SELECT = the preview output**: the operator GUI's preview pane or a
  spare output shows an input or the program, like MONITOR OUT.
- **NTSC/PAL** has no meaning in Himmel; the simulator keeps it only to be
  faithful to the unit.

This "KF4 mixer mode" could also be a Himmel page any weapon can play (the MK2:
group buttons A/B, pads 1–4 + 5–8 as the input keys, a knob as the fader).

## Sources

- KORG KF4 krossfour **Owner's Manual** (English/French/German): panel
  descriptions, rear panel, specifications. Read on
  [ManualsLib](https://www.manualslib.com/manual/829982/Korg-Kf4.html)
  (pages 5–8, 11).
- KORG **krossfour EasyStart** guide (5 pages): features, curves, fade to black
  / white, chroma and luma key, combinations, a suggested setup with the
  Entrancer. Copy on the
  [Internet Archive](https://archive.org/download/manualsonline-id-f4def0b0-55f0-42bd-a340-82af8ad3074f/f4def0b0-55f0-42bd-a340-82af8ad3074f.pdf)
  and [ManualsDir](https://www.manualsdir.com/manuals/157159/korg-krossfour-kf4.html).
- Sonicstate, "NAMM: VJ video value from Korg", 2005-01-19:
  [announcement, price](https://www.sonicstate.com/news/2005/01/19/namm-vj-video-value-from-korg/).
- zZounds product page (discontinued):
  [KORKF4](https://www.zzounds.com/item--KORKF4).

All Himmel code here is our own; nothing was copied from these documents.
