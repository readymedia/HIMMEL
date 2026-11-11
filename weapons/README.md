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
| 5 | **NuVJ** (Numark NuVJ, 2008) | USB-MIDI: 2 × 9 pads, 2 jog wheels, 2 speed faders, crossfader, 12 knobs, 2 push BANK encoders, 31 LEDs, front switches, blue segment LCD (feedback by SysEx) | [`nuvj.weapon.json`](nuvj.weapon.json) | [`tools/nuvj_sim.py`](../tools/nuvj_sim.py) | **Checked on the owner's unit (2026-11-11)**: map of all 53 controls verified, 31 LEDs, LCD text and the whole segment map (camera mapper); protocol in [`docs/nuvj-protocol.md`](../docs/nuvj-protocol.md) |

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
- Weapon no. 5 (NuVJ) adds `fader` (`orient` `v`/`h`, `len`, 14-bit `range`), `wheel`
  (a jog wheel: endless, relative), `switch` (`positions`), screens of `kind: "lcd"`
  (`text_cols` × `text_rows`) and `deco` kind `panel` (an inset area). A profile may
  name its own page (`"page": "nuvj_live.html"` in `tools/`); the live view serves
  it at `/?weapon=<id>`.
- `deco` (optional): drawing only, never a control. `body` is the device
  outline (`r` = corner radius), `dna` is a connector dot at `x, y`, and `port`
  is a socket or switch the protocol cannot see, with a small label.
- `protocol` (optional, free text): a transport and message summary for people.
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

## Weapon no. 5: the NuVJ

The owner's **Numark NuVJ** (2008, made for ArKaos' NuVJ software) is a
two-deck VJ controller. It is a class-compliant USB-MIDI device, powered by USB
or 6 V DC. The drawing (`nuvj.weapon.json`, badge "HIMMEL · VJ") follows the
owner's photos:
- white deck plates;
- black rubber pads and buttons whose two light strips glow;
- knurled jog wheels;
- the blue segment LCD, with unlit segments showing faintly.

```
python tools/mk2_live.py --port 8777 --weapon nuvj        then open http://127.0.0.1:8777
python tools/nuvj_sim.py --selftest                       every control and every feedback message round-trips
python tools/nuvj_sim.py --ports                          the MIDI ports Windows sees
```

The NuVJ has its own page (`tools/nuvj_live.html`), served by the same live
view. Its profile names it with `"page"`.

### The protocol

The full protocol is in [`docs/nuvj-protocol.md`](../docs/nuvj-protocol.md). The effects, generators and mixer of the original NuVJ software (and what Himmel can take from them) are in [`docs/nuvj-effects.md`](../docs/nuvj-effects.md).

- **Controls → host:** notes, CCs and pitch bend.
  - Endless controls (BANK, MASTER FX, jog wheels) send relative CCs.
  - The BANK knobs can be pushed (notes 2 and 3).
- **Host → controls:** SysEx `F0 00 01 3F 7F 20 <cmd> 00 <len> <data> F7`.

  | cmd | What it sets |
  |---|---|
  | `60` | init |
  | `01` | 8 characters of LCD text |
  | `02` | 64 bytes of LCD segment memory |
  | `03` | 8 bytes, one bit for each of the 31 LEDs |
  | `7A` | LCD contrast |

- We read this from the owner's NuVJ 1.0 software: we unpacked the installer
  and analysed the program statically, without running it. Credited in
  `THIRD_PARTY_NOTICES.md`.
- Without these messages the LEDs stay dark and the LCD just says "NuVJ".
- Himmel never sends `7B`, the command that stores the contrast in the
  controller.

### How it is built

- **Simulator** (`SimNuVJ`): the drawing is the controller.
  - Buttons and pads: hold the mouse.
  - Knobs: drag or scroll.
  - BANK and MASTER FX: one step at a time. Right-click (or Alt+click) a BANK
    knob to push it.
  - Jog wheels: drag around the wheel.
  - Faders: drag the cap.
  - Switches: click to step.
  - It sends the real MIDI bytes. It also decodes the host's feedback SysEx,
    so its LEDs and LCD show exactly what was sent.
- **Real device** (`HwNuVJ`): WinMM through the ctypes bindings in
  `lightpad_hw.py`, so no new dependency.
  - It finds a port named "NuVJ" or "Numark", or uses the one you pick.
  - It reconnects after a cable pull and sends init again.
  - It sends only feedback SysEx with the NuVJ header (never `7B`), short
    messages, and the MIDI identity request.
- **Map learning**: click a control in the drawing, then move the real one.
  - Endless knobs and jog wheels: turn them clockwise.
  - Switches: walk them through every position.
  - "Seen" is only ticked by real input. **Mark verified** is enabled when all
    53 controls are seen.
- **LEDs and LCD** (`NuvjHost`): the live view stands in for Himmel.
  - LED modes:
    - **Himmel**: latch / auto / sync / master FX / insta black as toggles, the
      playing pad per deck, BPM TAP on the beat;
    - Follow;
    - All on;
    - Chase;
    - Off.
  - The LCD is one model: `{text, big, small, bar, icons}`.
    - The text goes out as `cmd 01`.
    - The big number goes where the NuVJ software puts its 2-digit value.
    - Elements learned by the segment hunt are lit through `cmd 02`.
- **Segment hunt**:
  1. It lights one segment bit at a time.
  2. It writes `SEGbb.b` on the text row.
  3. The owner clicks the part of the drawn LCD that lit up: a digit segment,
     a bar block, an icon or a label.
  4. It is saved in `nuvj_map.json` under `lcd_segments`.
- **Contrast**: the slider sends `cmd 7A` (not stored).

### The classic two-deck view (nostalgia)

`http://127.0.0.1:8777/?weapon=nuvj&view=classic` is **Himmel's own take on the
simple two-deck VJ workflow** the NuVJ was made for. The original software was
easy and fun, and this view keeps the workflow without its art, logos or
names. It runs on its own, or next to the controller view.

**What is on the page**
- A **browser** with Media (the `media/` folder), Effects (Himmel's planned
  set), Generators and Inputs.
- **Two decks.** Each has:
  - 16 banks of 9 cells, laid out like the pads (7 8 9 on top);
  - a preview;
  - a position bar and a speed slider;
  - LATCH / AUTO / SYNC;
  - PMTR 1 / 2 knobs named after the playing cell's effect.
- The **master**:
  - a preview;
  - brightness and contrast;
  - a master effect with mix and two parameters;
  - BLACK;
  - the crossfader with A / B cut buttons;
  - BPM and TAP;
  - full-screen output.
- **Cell properties**:
  - how deck B blends onto A: mix, add, subtract, multiply, lighter, darker;
  - brightness or colour key with centre and width;
  - once / loop / ping-pong;
  - text;
  - the effect and its two parameters.

**How you play it**
- Drag an item onto a cell (or click the item, then the cell).
- Click a cell to trigger it. Shift-click or right-click a cell to edit it.
- Drag cells around (Ctrl = copy), or drop one on the bin to clear it.

**How it connects**
- The page and the NuVJ (real or simulated) change the **same model**,
  `tools/vj_classic.py`, so either one can play it:
  - the pads trigger cells;
  - BANK steps the banks;
  - PMTR turns the playing cell's parameters;
  - the faders, jog, crossfader and master section all work.
- The NuVJ's LEDs and LCD follow the model too.
- **No render engine yet.** The previews are simple 2D stand-ins drawn by
  the page. Later Himmel's engine renders the decks from the same model.

### NuVJ events in Himmel's event model

The NuVJ uses the generic weapon plan below: one driver process (WinMM), a
shared block, and the same 16-byte `Event`. `id` = the control's index in the
profile.

| NuVJ input | Himmel event | Notes |
|---|---|---|
| Button / pad note on, note off | `ButtonDown` / `ButtonUp`, or `PadHit` / `PadRelease` for pads | The pads send no pressure: velocity is a fixed 127, so `PadHit` carries full intensity. |
| Absolute knob CC (PMTR, FX LEVEL, CONTRAST, BRIGHT, AUTO FADE SPEED) | `KnobDelta` (`raw` = the 0..127 value) | The engine can pick up (soft takeover) after a page change. |
| BANK, MASTER FX, jog wheel (relative CC) | `KnobDelta` (±n ticks) | The direction comes from the learned `cw` flag. |
| BANK push | `ButtonDown` / `ButtonUp` | |
| SPEED faders, crossfader (pitch bend) | new `Fader` event: `value` 0..16383 | 14-bit, smooth enough for a crossfade. |
| FADER MODE, FADER START | new `Switch` event: `value` = position | The engine learns the position on the first move. |
| LED out | The engine writes an LED level per control. The driver packs them into the 8 LED bytes (`cmd 03`) and sends them on change, plus every 2 s. | |
| LCD out | The engine writes the LCD model. The driver sends text (`cmd 01`) and segments (`cmd 02`). | |

### What each control could do in Himmel

| Control | In Himmel |
|---|---|
| Crossfader | Deck A ↔ deck B mix (the deck / layer mixer). |
| INSTA A / INSTA B | Hold = cut to that deck, with an auto fade at the AUTO FADE SPEED. |
| SPEED faders | Clip speed per deck, −2..+2×, centre = 1×. |
| Jog wheel | Nudge the speed; with SCRATCH held, scrub the clip (HAP frames are random-access). |
| Pads 1–9 | The 9 clips of the deck's current bank. LED = the playing clip. |
| BANK (turn / push) | Step through the deck's banks / load the bank. The LCD shows the bank number. |
| PMTR 1 / 2 (deck) | The two main parameters of the deck's effect chain. |
| LATCH / AUTO / SYNC | Latch = clips keep playing after the pad is released. Auto = Smart Random inside the bank. Sync = the deck's clip follows the beat. |
| Master FX section | MASTER FX knob = choose the master effect (name on the LCD), button = on/off, FX LEVEL = wet/dry, PMTR 1/2 = its parameters. |
| CONTRAST / BRIGHT | Output contrast and brightness. |
| INSTA BLACK | The show's blackout (toggle; LED and BLACK icon on while black). |
| BPM TAP | Tap tempo for Himmel's beat clock (LED on the beat); hold 3 s = back to the audio-detected BPM. |
| FADER START + FADER MODE | Crossfader start: moving away from one side fires the other deck's selected cue (A/B/C = cue, scene or Smart Random). |

### Checked on the owner's unit (2026-11-11)

- **Device.** USB `15e4:0020`; WinMM ports "NuVJ Controller" (in 0, out 1).
- **Map verified.** All 53 controls are in `nuvj_map.json`.
  - The 12 top knobs are endless (1 / 127 per tick); Himmel keeps a 0..127
    value.
  - Before init the faders send CC 10 / 11 / 12. After Himmel's init they
    send pitch bend on channels 1–3, and the device reports where its faders
    and switches are.
  - The BANK knobs can be pushed (notes 2 and 3).
- **LEDs.** All 31 work with the `03` bits.
- **LCD.**
  - Text (`01`) works.
  - The segment memory (`02`) is fully mapped: wedges, words, boxes, the
    188 and PRESET digits, and 11 16-segment characters (15 bits each).
  - Checked with test text.
- **How the LCD was mapped: the camera mapper** in the live view's LCD
  hunt.
  1. A webcam (here DroidCam) looks at the glass, and the four corners are
     set by click and drag.
  2. Each bit is lit on its own while the camera watches.
  3. The structure found that way was confirmed with a few photos of single
     bits.
  4. Bits learned by hand are protected from the camera.

  The browser pane in the Claude app cannot use the camera, so the mapper
  runs in Chrome.

### Jog Pong (easter egg, played on the owner's NuVJ 2026-11-11)

`tools/nuvj_games.py` is a three-lane pong on the NuVJ's own LCD. It shows
that Himmel drives every part of the controller.

- **The field** is the 11 16-segment characters. The ball is a bar in the
  top, middle or bottom lane. The paddles are brackets in the end characters.
- **The jogs.** The left jog moves A and the right jog moves B (clockwise =
  down). Spinning as you hit puts an angle on the ball.
- **Serve.** Push your BANK knob or press SCRATCH; PRESS ENTER blinks for the
  server.
- **Score.** A shows in "188" and B under PRESET. First to 5 wins.
- **The rest of the controller.**
  - The dot-matrix row shows the messages.
  - The wedges show the ball speed and the rally.
  - The pads flash on hits.
  - The A / B box and the INSTA LED show who serves.
- **Playing alone.** A side whose jog is idle for 5 s is played by Himmel.
- **Start / stop.** Hold both SCRATCH buttons, or press **★ Jog Pong** in the
  live view.
- **Without hardware.** `python tools/nuvj_games.py` runs a self-test.

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
