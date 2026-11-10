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

## The weapons so far

| No. | Weapon | Kind | Profile | Simulator | Status |
|---|---|---|---|---|---|
| 1 | **MK2** (Maschine MK2) | USB HID: 16 pads, 8 knobs, encoder, 48 buttons, two 1-bit screens | [`mk2.weapon.json`](mk2.weapon.json) | [`tools/mk2_sim.py`](../tools/mk2_sim.py) | Map verified on hardware, C++ driver done |
| 3 | **LUMI Keys** (ROLI LUMI Keys) | USB-MIDI, BLOCKS family: 24 keys with strike velocity, pressure and glide, one RGB LED per key, Mode / Octave − / Octave + | [`lumi.weapon.json`](lumi.weapon.json) | [`tools/lumi_sim.py`](../tools/lumi_sim.py) | Simulated, and checked on the owner's LUMI (firmware 1.3.8, 2026-11-10): keys, velocity, buttons, **Himmel's own key LEDs** |
| 2 | **Lightpad** (ROLI Lightpad Block / Block M) | USB-MIDI: multi-touch surface (x, y, pressure per finger), 15 × 15 RGB LEDs, one Mode button | [`lightpad.weapon.json`](lightpad.weapon.json) | [`tools/lightpad_sim.py`](../tools/lightpad_sim.py) | Simulated, and played on the owner's Lightpad M (2026-11-10): touch, multi-touch, Mode button, orientation, factory note map, **and Himmel's own LED program** (~55 frames/s) |
| 7 | **V-4 Mixer** (Roland / EDIROL V-4, 4-channel SD video mixer) | MIDI (5-pin DIN, via any USB-MIDI interface): T-bar, input A/B 1-4, 4 effects + CONTROL dial per channel, MIX / WIPE / EFX, transformers, output fade, BPM, memory | [`v4.weapon.json`](v4.weapon.json) | [`tools/v4_sim.py`](../tools/v4_sim.py) | Simulated only (the owner has no V-4, 2026-11-10). Protocol from the owner's manual |

## The live view: one tool for every weapon

`python tools/mk2_live.py` draws the weapon from its profile and shows it live.
Pick the weapon in the menu at the top left (or open `/?weapon=<id>`); every
`weapons/<id>.weapon.json` is listed. The MK2 page is the hardware page:
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
- `kind: "surface"` (weapon no. 2): a multi-touch area with an LED grid under it.
  Extra keys: `grid: {cols, rows}`, `max_touches`, `range: {x, y, z}` (raw
  units on the wire). Touches are reported per finger with x, y and pressure.
- `deco` (optional): drawing only, never a control. `body` is the device
  outline (`r` = corner radius), `dna` is a connector dot at `x, y`, and `port`
  is a socket or switch the protocol cannot see, with a small label.
- `protocol` (optional, free text): a transport and message summary for people.
- Video mixers (weapon no. 7) add:
  - `kind: "fader"`: an absolute fader or T-bar, 0..1 from `ends[0]` (top) to
    `ends[1]` (bottom); `range` = raw units on the wire.
  - on a `knob`: `steps` (a selector that clicks between positions, value
    0..steps-1), `endless` (no end stops), `center` (a centre detent).
  - `deco` kinds `display` (a small LED readout, text from the simulator) and
    `monitor` (a video monitor the page draws from the simulator's state;
    `source` = `program` or `preview`).
  - `"live": "mixer"` makes the page show the mixer cards (Himmel driving the
    mixer, the MIDI setup table) instead of the BLOCKS cards.
- `map` is `null` until a map has been learned on real hardware.

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

## Weapon no. 2: the Lightpad (simulated)

The owner chose the Lightpad Block on 2026-11-10. He owns one, so it can be
checked on hardware the way the MK2 was. The other candidates were:
- APC mini mk2: grid + 9 faders;
- DDJ-FLX4: a DJ controller;
- Launchpad Mini MK3: a grid;
- Roland V-1HD: a video mixer.

```
python tools/mk2_live.py --port 8775 --weapon lightpad     then open http://127.0.0.1:8775
python tools/lightpad_sim.py --selftest                    codec round trips, no browser
```

`--weapon lightpad` leaves the MK2 hardware alone, so it can run next to an
MK2 Live on port 8765.

### How the real block talks

The facts below come from ROLI's `roli_blocks_basics` (ISC license).

- **Connection.** USB-MIDI, class compliant (`2af4:0900`). On Windows the
  WinMM ports are exclusive, so close ROLI Connect / Dashboard first.
- **MIDI mode** (no host): MPE, one channel per finger.
  - Note on/off.
  - Pitch bend = x glide.
  - CC74 = y slide.
  - Channel pressure = z.
- **API mode** frame: SysEx `F0 00 21 10 77 <index> <bits> <checksum> F7`.
  - The bits are an LSB-first stream, packed 7 bits per byte.
  - The index byte is the topology index; block → host messages add `| 0x40`.
  - Checksum: start at the body length, do `c = c + (c * 2 + byte)` for each
    byte (8-bit), then `& 0x7F`.
- **Host → block messages:**
  - `deviceCommand` (type 1) with a 9-bit command: 0 beginAPIMode,
    1 requestTopology, 2 endAPIMode, 3 ping.
  - `sharedDataChange` (type 2): a 16-bit packet index, then 3-bit commands
    (skip / set sequence / set run).
  - The serial dump request is `F0 00 21 10 78 3F F7`.
  - The host must ping every 400 ms. After 5 s with no ping the block goes
    back to MIDI mode.
- **Block → host:** a 32-bit timestamp, then messages:
  - touch start/move/end with velocity (0x13/0x14/0x15): 5-bit time offset,
    5-bit finger, x 12 bit, y 12 bit, z 8 bit, then vx/vy/vz 8 bit signed;
  - button down/up (0x20/0x21); button 0 is Mode;
  - packet ACK (0x02, 10 bits);
  - topology (0x01): serial and battery.
- **LEDs:** 15 × 15 pixels as RGB565 in the block's shared data. A tiny
  LittleFoot program running on the block draws them (ROLI's
  `BitmapLEDProgram`).
  - Himmel uploads its own 93-byte renderer, assembled by
    `tools/lightpad_littlefoot.py` (no LittleFoot compiler needed). The
    pixel heap then sits right after the program.
  - The simulator runs the uploaded bytecode in a small Python VM, so it shows
    what the real block would draw.

### The simulator

`tools/lightpad_sim.py` produces exactly those bytes.

- **The live view is the host.** It is a Python sketch of the future driver:
  - it does the real handshake and the pings;
  - it draws LED pictures (touch glow, rainbow, test pattern) with real
    `sharedDataChange` packets;
  - the simulated block decodes those packets and ACKs them.
- **MIDI monitor:** shows every message, both ways.
- **MIDI mode:** untick **Host plugged in**. After 5 s the block falls back to
  MIDI mode and plays MPE.
- **Engine link:** every message the block sends also goes to UDP
  127.0.0.1:8767. A host there that answers (and pings) takes over the LEDs,
  like the MK2's 8766 link.

### Checked on the owner's block (2026-11-10)

`tools/lightpad_hw.py` talks to the real block over WinMM (ctypes, no new
dependency). In the live view press **Use the real block**; on the console run
`python tools/lightpad_hw.py [seconds]`. The same `LocalHost` as in the
simulator runs on top of it:
1. the handshake and pings;
2. wipe the block's program memory (the factory display stops);
3. upload Himmel's renderer;
4. stream LED frames.

The owner's block is a **Lightpad Block M**:
- serial `LPM…`, firmware 1.1.0;
- WinMM ports "ROLI Lightpad BLOCK" (in 0, out 1).

What we saw:
- **Handshake works as in ROLI's code.** Serial, topology (with battery and
  charging), firmware version (message 0x06), and pings answered with ACKs.
  - The block's own index byte is `0x77`, not `0x40`.
  - Entering API mode sends CC120/CC123 (all sound/notes off) on all 16
    channels.
- **Touches.**
  - Start and end come as 0x13 / 0x15 (with velocity). Moves come as 0x11
    (no velocity).
  - Finger indexes count from 1. Two and three fingers work.
- **Orientation.** x = 0 is left and y = 0 is the top, with the **USB-C port
  pointing away from you**. The profile draws the port at the top.
- **Range.** x and y run from about 146 to 3953 (the edges are inset); z runs
  from 0 to 255.
- **Strike velocity.** The API strike velocity (`vz` at touch start) is
  nearly always 1, so it is useless.
  - The factory program **keeps sending MPE in API mode**, and its note-on
    velocity (3–127) follows the hit.
  - A driver should take velocity from that note-on, or from z in the first
    few ms (as with the MK2).
- **Mode button:** `button 0 down` / `button 0 up` in API mode.
- **Factory note layout:** 4 × 4 pads of 3 × 3 LEDs with 1-LED gaps.
  - Note 36 is bottom-left. Notes go +1 to the right and +4 per row up, to 51
    at the top right.
  - The pads glow blue under the finger, and the block keeps them lit all the
    time.
  - The simulator now plays the same notes and also sends MPE in API mode.

**Himmel's own LEDs on the block (checked by the owner, 2026-11-10):**
- **Pictures.** The test pattern shows correctly (red top-left, green
  top-right, blue bottom-left, white bottom-right). Touch glow follows the
  fingers with no visible delay. Mode changes the colour family.
- **Frame rate:** about **55–58 full 15 × 15 frames per second**.
  - About 12 ms from send to ACK.
  - 15–360 bytes per frame (only the changes are sent).
  - No resends.
  - Our host caps it at ~66 pictures a second.
- **ACKs are cumulative.** When several packets go out together, the block
  ACKs only the last one. `HeapSync` treats an ACK as covering everything
  sent before it.
- **Letting go.** `endAPIMode`, sent when Himmel lets go, brings the factory
  4 × 4 display back. A power cycle should too (not tested). The program
  lives in RAM; Himmel never sends `saveProgramAsDefault`.
- **No MPE with our program.** Only the API touch messages arrive.
  - `vz` at touch start is 1 for a slow press and up to 127 for a quick tap.
  - z ramps smoothly from 0, but at the block's own pressure sensitivity (64)
    it reaches 255 at medium pressure.
  - **Pressure sensitivity, config item 13 (fixed 2026-11-10).**
    - **A higher value gives a longer span from a light touch to full
      pressure;** lower values make it shorter. The owner tried 8–127 and
      found **127** (the maximum) gives the most range ("could even be more").
    - Himmel sets 127 when it takes the block and puts the block's own value
      back when it lets go.
  - **Config values are stored on the block.** They survive a new session,
    so restoring is required.
    - The host loop is stopped first, so it cannot redo a handshake while
      the port closes; then Himmel waits for the block to confirm.
    - If Himmel was killed and left 127, the next session treats 127 as its
      own leftover and restores ROLI's default, 64.
    - Config messages: set = `0x10`, command 0, item 8 bit, value 32 bit.
      Request = command 1, 32 zero bits, item 8 bit. The answer is `0x18`
      command 4: item, value, min, max.
  - The block's other config values: velocity sensitivity 50, z tracking 1,
    brightness 100. They are the same with our program as with the factory
    one.
- **Touch moves** arrive every ~28 ms (~36 a second) while LED frames stream.

### Games on the Lightpad (easter eggs, 2026-11-10)

`tools/lightpad_games.py` has three small games. They are drawn by the host,
so they run on the simulated block and on the real one alike. The owner played
all three on his block.

**Rally (paddle duel)**
- A finger in the lower half moves the bottom paddle, and one in the upper
  half moves the top paddle, so two people can play.
- If nobody touches the top half, Himmel plays the top paddle.
- First to 5 wins.

**Brick Burst**
- Slide the paddle with a finger, and press harder for a wider paddle.
- Tap or press Mode to launch the ball, then clear the bricks.

**Star Swarm**
- The ship follows your finger and fires while you touch; press harder to
  fire faster.
- The swarm sways and dives. It shares its name with the MK2 screen game.

**On the block**
- **Hold Mode ~1 s** to walk touch glow → Rally → Brick Burst → Star Swarm.
  The game's letter is shown when it starts.
- A short press launches, fires or restarts.
- After game over, a tap restarts the game; otherwise it restarts by itself
  after 4 s.

**In the live view** there is a button for each game.

To check the games without hardware, run `python tools/lightpad_games.py --png`.
It writes frame strips to `tools/_screens/`.

**What we learned while playing:**
- **The packet counter wraps at 1023 → 0.** Sending 1024 makes the real
  block ignore every LED packet after ~20 s of play.
- **Buttons with our renderer.** While our renderer runs, the firmware stops
  sending button messages. The renderer therefore has
  `handleButtonDown/Up(index)`, which call `sendMessageToHost(1, index,
  down)`.
  - That arrives as a program event (message 0x28: three 32-bit ints).
  - The decoder turns it back into a button event.
  - The renderer is now 117 bytes.

## Weapon no. 3: the LUMI Keys

The owner chose it on 2026-11-10. It is a BLOCKS device like the Lightpad,
so `tools/lumi_sim.py` reuses the codec, `HeapSync` and the host lifecycle and
only adds what differs.

```
python tools/mk2_live.py --port 8775 --weapon lightpad   then pick "LUMI Keys" (or ?weapon=lumi)
python tools/lumi_sim.py                                 self-test
```

### Windows: the MIDI driver may need to be chosen by hand

On the owner's PC (Windows 11, Windows MIDI Services) the LUMI's MIDI
interface first failed to start. Device Manager showed "USB-lydenhet" (USB
Audio Device) with a yellow ⚠, error code 10, `STATUS_DEVICE_DATA_ERROR`,
driver `usbaudio` (`wdma_usb.inf`). With no MIDI port, Himmel cannot reach
the device at all. The Maschine MK2's MIDI part ("Maschine Controller MK2
MIDI IAD") showed the same error. The MK2 is not affected, because Himmel
uses its HID interface.

The fix, done by the owner:
1. Open Device Manager → *Sound, video and game controllers*.
2. Right-click the ⚠ device → *Update driver*.
3. Choose *Browse my computer* → *Let me pick from a list*.
4. Untick *Show compatible hardware*, then pick **Microsoft → USBMidi2-ACX**
   (the USB MIDI 2.0 class driver of Windows MIDI Services).
5. Replug the LUMI.

After that the port is called **"USBMidi2-ACX"**, not "LUMI", so the LUMI's
`make_hw()` looks for both names. Do this while the LUMI is on: without a host
talking to it, it switches itself off after a few seconds. Once Himmel pings
it, it stays awake. To undo: *Uninstall device* and replug.

### What we saw on the owner's LUMI (firmware 1.3.8)

**Identity and settings**
- Serial `LKB…`. Topology and version work as on the Lightpad. Battery 31/31,
  charging.
- The block index byte is `0x46`.
- Config values: octave (item 4) = 1, range −4..5. Velocity sensitivity 110,
  pressure sensitivity 127, lift sensitivity 127, brightness 100.
- Himmel changes only the octave, and puts it back when it lets go.

**LEDs**
- Himmel's key renderer runs on the device. It is the same LittleFoot program
  as the Lightpad's, sized for 24 keys, so 121 bytes.
  - It calls `setUseDefaultKeyHandler(true, false)` (keep the musical key
    handling, take the lighting) and `fillPixel(colour, key, 0)`.
- The owner confirmed the test pattern and that played keys light up.
- Once, the LEDs did not update after the test pattern (only 3 frames were
  confirmed). After a restart they updated at frame rate (>550 frames while
  playing). Cause unknown; watch for it.

**Keys**
- The block's own handler keeps sending MPE while our renderer runs: note
  on/off with velocity, which the owner saw follow soft and hard strikes,
  plus pressure and pitch bend.
- Key 0 = note 48 + 12 × octave.
- **It also sends BLOCKS touch messages per key** (0x13 / 0x11 / 0x15, finger
  index, x/y/z). They are not used yet, and could later give key position
  and pressure without MIDI.

**Buttons**
- 0 = Mode, 1 = Octave −, 2 = Octave +.
- **Every press arrives twice**: once as the firmware's own button message
  and once as our renderer's program event.
- `LumiHost` ignores the program copies once it has seen a firmware button
  message. Without that, one Octave + press moved two octaves.

### The simulator
- **Keys:** click = strike (lower on the key = harder), hold = pressure (it
  grows; the scroll wheel changes it), drag sideways = glide.
- **Buttons:** Mode, Octave −, Octave +.
- **LEDs:** the simulated block runs the uploaded bytecode in MiniVM.
- **Picture modes:** key colours (played keys glow by pressure; Mode
  changes the colour family), rainbow, test pattern (key 1 red, 12 green,
  13 blue, 24 white), off.
- **Engine link:** UDP 127.0.0.1:8769 (8768 is the Entrancer engine).

## Weapon no. 7: the V-4 video mixer

The owner asked for it on 2026-11-10 (link: Roland's V-4 page) and chose
number 7. It is the original **V-4** (sold as EDIROL V-4, later Roland V-4):
four SD inputs (composite / S-Video, NTSC or PAL), a T-bar, two frame
synchronisers. Discontinued. Not the HD V-4EX. **The owner has no V-4**, so it
is simulated only; the hardware steps below wait for a device.

```
python tools/mk2_live.py --port 8781 --weapon v4     then open http://127.0.0.1:8781 (launch config v4-live)
python tools/v4_sim.py                              self-test
```

### What it could do for Himmel

- **Himmel drives an external mixer.** The V-4 has no computer link other than
  MIDI, but over MIDI Himmel can pick inputs A/B, move the T-bar, run an auto
  take with its own timing, fade the output to black or white, switch effects,
  and lock BPM SYNC to Himmel's tempo with MIDI clock. Useful when cameras or a
  second PC go through a V-4 and Himmel's output is one of its inputs.
- **The V-4 as a controller (a weapon).** Its T-bar, input selectors, effect
  buttons and CONTROL dials can play Himmel. Out of the box only the inputs,
  memory and the T-bar send MIDI. Himmel's MIDI setup (one SysEx message per
  function) makes every function send its own CC.

### How it talks (owner's manual, pp. 89-96)

- **Connection:** MIDI IN / OUT, 5-pin DIN. No USB, no RS-232. Channel 1 both
  ways at the factory. With MIDI Thru on, the V-4's own messages are **not**
  sent.
- **Inputs:** Bank Select MSB `00` (A) or `01` (B), LSB `00`, then Program
  Change 0-3 = input 1-4. Sent when you turn an INPUT SELECT, received the same
  way.
- **Memory 1-8:** Bank `50H` / `00H` + Program Change 0-7.
- **Factory controllers:**
  - Video fader (T-bar): **CC#11**, 0 = A ... 127 = B.
  - Transition time: CC#7, 0-127 = 0.0-4.0 s.
  - Everything else is unassigned: effects A/B 1-4 (0 = off, 1-127 = on +
    parameter), transition select (0-2), transformer A/B (< 64 off), output fade
    (0 black, 64 centre, 127 white), BPM sync (< 64 off).
- **MIDI setup over SysEx:** `F0 41 <dev> 00 5B 12 <addr ×3> <data> <sum> F7`
  (DT1, model ID `00 5B`, device ID `10H` in the manual's example).
  - Addresses: `01 10 00` ... `01 10 0C` = transition time, video fader,
    transition select, transformer A, transformer B, output fade, BPM sync;
    `02 10 00` ... `02 10 0E` = A effect 1-4, B effect 1-4.
  - The value is one byte sent as two nibbles (high, low): a CC number (1-5,
    7-31, 64-95), `D0` = channel pressure, `E0` = pitch bend, `EF` = off.
  - Checksum: 128 − (sum of address + data bytes mod 128).
  - At least 20 ms between DT1 messages.
- **Note Mode** (off at the factory): note `24H 26H 28H 29H` = A 1-4,
  `2BH 2DH 2FH 30H` = B 1-4.
- **MIDI clock:** Timing Clock `F8` drives BPM SYNC; Start / Continue mark the
  beat.
- **Not documented, so the simulator guesses** (check on a real V-4):
  - Does a CONTROL dial send the effect's CC while turning?
  - What exactly does a second press on a lit effect button do?
  - The two-nibble ("#") encoding of the setup values. The manual's own example
    for it is garbled in the copy we read.

### Himmel's MIDI setup

| Function | CC | Function | CC |
|---|---|---|---|
| Transition time | 7 (factory) | A effect 1-4 | 16-19 |
| Video fader | 11 (factory) | B effect 1-4 | 80-83 |
| Transition select | 20 | Output fade | 23 |
| Transformer A / B | 21 / 22 | BPM sync | 24 |

### The simulator (checked in the browser, 2026-11-10)

- **Panel:** buttons are held with the mouse. Knobs: drag or scroll; the INPUT
  and MEMORY selectors click between positions. T-bar: drag it (top = A).
- **Effects:** a press turns an effect on and makes it blink; the CONTROL dial
  then sets its parameter. A press on the blinking one turns it off.
- **The two monitors** show a made-up picture of what the mixer outputs:
  four test sources, the transition (mix / wipe / slide), the effects, keys,
  P in P, transformers, BPM sync cuts and output fade. It is a sketch, not the
  V-4's real effects.
- **Host card (Himmel driving the mixer):** input buttons, a T-bar slider,
  Auto take, output fade, MIDI clock, "Send Himmel's MIDI setup" (15 DT1
  messages), Note Mode and MIDI Thru tests.
- **MIDI monitor:** every message both ways, with the bytes. MIDI clock is
  hidden unless ticked.
- **Engine link:** every MIDI message the V-4 sends also goes to UDP
  127.0.0.1:8773. A datagram sent back is MIDI into the V-4.

### When a V-4 turns up

1. Connect it with a USB-MIDI interface (V-4 MIDI OUT → interface IN, and back).
   If the interface fails to start on Windows, see the USBMidi2-ACX steps above.
2. Read-only probe first: open the interface's WinMM port (as
   `tools/lightpad_hw.py` does, ctypes, no new dependency). Then turn every
   control and compare with the simulator's monitor.
3. Then send: inputs, T-bar, and Himmel's MIDI setup.
4. Check the guesses above, and only then write a map.

## Plan: how a generic weapon feeds Himmel (not wired yet)

Today `mk2_controller.exe` owns `Local\HimmelMk2`:
- event rings of 16-byte `Event`s (`src/controller/include/himmel/mk2/types.h`);
- seqlock LED/screen outputs;
- a status block.

Proposal for weapon no. 2 and later:

1. **One driver process per weapon kind.** It follows the same rules as the
   MK2 controller: its own process, a heartbeat, reconnect, and `--sim` over
   UDP. For the Lightpad that is `weapon_blocks.exe`:
   - WinMM, which is part of Windows, so no new dependency;
   - the sim link on port 8767.
2. **One shared block per weapon slot**, `Local\HimmelWeapon<N>`.
   - Same layout as `HimmelMk2`, so the engine reads every weapon the same way.
   - Plus a header naming the weapon id and its profile.
   - The MK2 stays as it is.
3. **Events:** keep the 16-byte `Event` and add types. Nothing existing changes.

   | Device input | Event | Fields |
   |---|---|---|
   | Mode button (and any MIDI note/CC button) | `ButtonDown` / `ButtonUp` | `id` = control index from the profile |
   | Touch start | new `TouchStart` | `id` = finger 0..31, `value` = x 0..4095, `raw` = y 0..4095, `reserved` = z (low byte) + strike velocity (high byte) |
   | Touch move | new `TouchMove` | same fields |
   | Touch end | new `TouchEnd` | `id` = finger |
   | The same touch, seen as a pad | `PadHit` / `PadPressure` / `PadRelease` | The driver can also split the surface into zones (e.g. 4 × 4 = 16 "pads", velocity from the strike, pressure from z), so the engine's pad pages work unchanged. |
   | MIDI CC (knob or fader on generic MIDI weapons) | `KnobDelta` | an absolute CC turned into a delta, like the MK2 knobs |
   | Port appears / goes / API mode lost | `Connected` / `Disconnected` | |

4. **Output.**
   - The engine writes a whole LED frame (Lightpad: 225 RGB) into the seqlock
     output.
   - The driver diffs it into `sharedDataChange` packets.
   - It keeps at most ~200 bytes waiting for ACKs, as ROLI's own host does, and
     resends lost packets.
   - Pings run on their own 400 ms timer, never behind LED traffic.
5. **Hardware steps first** (with the owner):
   - a small Python probe over WinMM (ctypes, no new dependency). It opens the
     block, enters API mode, logs raw touches/buttons in the live view and
     checks the x/y orientation;
   - then the LittleFoot renderer upload for the LEDs;
   - then a verified map.
