# Getting started: Himmel on your own machine

This guide takes you from a fresh Windows PC to your own Maschine MK2 driving
video. Every step was done for real on the owner's PC; the problems we hit are
in [Troubleshooting](#10-troubleshooting).

## 1. What you need

| | |
|---|---|
| PC | Windows 10 or 11, 64-bit, a Direct3D 11 GPU. Developed on a GTX 1070 laptop; the show PC is an RTX 5090. |
| Controller | **Native Instruments Maschine MK2**, USB id `17cc:1140` (check in Device Manager → Details → Hardware Ids). The owner's unit is the white one. Other Maschine models speak other protocols and will not work as they are. |
| Optional | A USB webcam (for the camera layer and the latency test), extra screens or projectors. |
| Disk | About 6 GB for the build tools, plus your clips (HAP files are large). |

## 2. Install the tools (once)

1. **Git**: <https://git-scm.com> (or `winget install Git.Git`).
2. **Visual Studio 2022 Build Tools** with the C++ workload. It brings MSVC
   and its own CMake, so you need nothing else to build:
   ```
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
   ```
   Windows asks for administrator rights (UAC); say yes, or the installer
   stops with code 1602.
3. **Python 3.10+** for the MK2 tools: <https://www.python.org>. Then
   ```
   pip install hidapi
   ```
   (`pip install pyusb libusb-package` too if you want the libusb fallback.)
4. **FFmpeg** (optional, for making HAP clips): a "full" build from
   <https://www.gyan.dev/ffmpeg/builds/>, with `ffmpeg.exe` on your `PATH`.

## 3. Get the code and build

```
git clone https://github.com/readymedia/HIMMEL.git
cd HIMMEL
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
build\bin\Release\mk2_tests.exe
build\bin\Release\media_tests.exe
```

(If `cmake` is on your `PATH`, plain `cmake` works too.) The first configure
downloads hidapi, nlohmann/json and Snappy. Both test programs must end with
`0 failed`. They need no hardware.

Everything lands in `build\bin\Release\`:

| Program | What it is |
|---|---|
| `himmel.exe` | The supervisor: starts and guards the controller and the renderer. **This is what you run for a show.** |
| `himmel_render.exe` | The deck engine: 2 decks × 4 HAP layers, effects, crossfader / TAKE, outputs; driven by commands (GUI, MK2, `himmel_cmd`). |
| `himmel_render_legacy.exe` | The parked pre-deck renderer (test card, camera, Touch FX, Entrancer, knob pages): `himmel.exe --legacy`. |
| `himmel_cmd.exe` | Sends any engine command from the command line and prints the show (`himmel_cmd clip a 1 0 -- take`). |
| `preview_dump.exe` | Saves the engine's preview pictures (decks, MAIN, outputs) as PNG. |
| `mk2_controller.exe` | The MK2 process: reads the device, drives LEDs and screens. |
| `mk2_monitor.exe` | Test client: prints every MK2 event, lights what you touch. |
| `mk2_pages_demo.exe` | The MK2 screen pages, live on the device. |
| `mk2_pages_check.exe` | Renders every screen page to PNG (no hardware). |
| `hap_probe.exe` | Checks a HAP clip and times its decoding. |

## 4. First contact with your MK2

1. Plug in the MK2. **Close Maschine, Controller Editor and the NI Hardware
   Agent / NIHardwareService** (Task Manager → Services). Only one program can
   own the device.
2. Start the visual live view:
   ```
   python tools/mk2_live.py
   ```
   and open <http://127.0.0.1:8765>. You see the MK2 front panel. Touch
   everything: buttons light up, knobs turn, pads glow by pressure, and each
   control is ticked off as "seen".
3. **If something lights the wrong control**, click the control on the page,
   then press the real one: it is re-learned and `maschine_mk2_map.json` is
   saved (the old one is kept as `.bak.json`). The map in this repo is the
   owner's verified MK2; yours is very likely the same.
4. **Worn pads.** A pad that reads pressure while untouched (the owner's Pad 12
   rests at ~130 of 4095) gets its own dead zone through `pad_noise_floor` in
   the map. The live view shows resting values.
5. Try the LED tests and the screen modes on the page. **Shift + turn the
   master encoder** on the MK2 opens the screen-mode menu (Snake, Star Swarm,
   Sky Shield, ...); push to start.
6. Close the live view before you start the C++ programs: they want the device too.

**No MK2?** Press **Simulate** on the page (or run `python tools/mk2_live.py --sim`).
The drawn panel becomes the controller: hold buttons with the mouse, drag or
scroll knobs, press pads (higher up = harder), keyboard Shift = Shift. The
screen modes and games run as on the device. See [weapons/README.md](../weapons/README.md).

## 5. The C++ controller on its own

```
build\bin\Release\mk2_controller.exe          (finds maschine_mk2_map.json by walking up from the current folder)
build\bin\Release\mk2_monitor.exe             (in a second window)
```

The monitor prints every event (`HIT pad 5 vel 93`, `KNOB 3 +12`, ...), lights
pads by velocity (blue soft → red hard) and draws a status page with eight
knob bars on the MK2 screens. Pull the USB cable: the controller says
`MK2 lost`, then `MK2 connected` again when you plug it back in.

`mk2_pages_demo.exe` (instead of the monitor) shows the real Himmel screen
pages: Perform, Browse, Audio and the Shift + encoder system menu. Its
**Knob align** page draws a line over each knob centre; if a line is not right
above its knob on your unit, adjust `kKnobX` in
`src/controller/include/himmel/mk2/pages.h`.

## 6. Run Himmel

```
build\bin\Release\himmel.exe -- --outputs 3 --size 640x360     three windows, for a desk
build\bin\Release\himmel.exe -- --fullscreen                    one borderless output per monitor
```

Everything after `--` goes to the renderer:

| Renderer option | Meaning |
|---|---|
| `--outputs N` | Number of outputs (1–8). Default: one per monitor in fullscreen, 3 windows otherwise. |
| `--fullscreen` / `--windowed` | Borderless on the displays / normal windows. In full screen each output belongs to its display **by identity** (EDID): unplug a projector and its output waits (the others never notice); plug it in again, on any port, and it comes back to its slot. |
| `--size WxH` | Window size in windowed mode (also the canvas size of each output there, unless `--resolution` is given). |
| `--resolution 720p\|1080p\|1440p\|4k\|custom` | The canvas size of each output (default: 1080p in full screen, `--size` in windows). Clips are scaled on the GPU, never re-encoded. |
| `--refresh 60\|50\|59.94\|30\|25` | Output refresh in full screen (default 60; the display mode is switched for the show and put back when Himmel exits). Clips play at their own frame rate. |
| `--mode span\|mirror` | Span: one canvas across all outputs. Mirror: every output shows the whole canvas. |
| `--spout NAME` | Send MAIN as a Spout sender called NAME (to Resolume, OBS, a recorder …). Off by default. |
| `--blend PX`, `--blend-gamma G` | Projector edge blend: neighbours overlap by PX canvas pixels, ramps with gamma G (2.2). |
| `--clip file.mov` | Add a clip to the playlist (repeatable). Default: every `.mov` under `media/`. |
| `--camera NAME`, `--camera-index N`, `--no-camera` | Pick the webcam (part of its name, or its index), or none. Default: the first camera that is not an IR camera. |

Supervisor options: `--no-controller` (renderer only), `--no-curtains` (no black
windows behind the outputs; by default a crashed or restarting renderer shows
black, not the desktop), `--sim` (no MK2: play
Himmel from the simulated MK2 in the browser, see below). The supervisor logs to
`build\bin\Release\logs\himmel-YYYY-MM-DD.log`, restarts a crashed part within
0.25 s and kills and restarts a part that hangs for 2 s. **Ctrl+C** in its
window stops everything cleanly. So does **QUIT**: on the MK2, `SHIFT` + turn
the wheel opens the SYSTEM menu, then push and hold the wheel for 2 s (a
countdown shows on the right screen, letting go cancels); from the GUI, the
QUIT button asks "Quit Himmel?"; from a script, `himmel_cmd quit --confirm`.
Every part stops cleanly (the Lightpad gets its settings back) and
`himmel.exe` exits with code 0. In a fullscreen output only **Shift+Esc**
closes the renderer (which the supervisor then restarts); **M** toggles
span/mirror.

Outputs at run time (from the GUI later, or `himmel_cmd`): `identify` shows
a big number, the size and the display name on every output for 5 s;
`outblack 2 on` blacks out one output; `place a 1 out 2` puts a layer on
output 2 only (`fill`, `fit`, `free X Y W H` too); `outres 1440p`,
`refresh 50`, `blendw 200`, `outmode mirror`; `himmel_cmd outputs` shows
every output, its display and whether it is lost.

### No MK2? Play Himmel from the browser

1. `python tools/mk2_live.py`, open <http://127.0.0.1:8765> and press **Simulate**.
2. `build\bin\Release\himmel.exe --sim -- --outputs 3 --size 640x360`
3. The pill on the page says **MK2 simulated, playing Himmel**. The drawn panel
   is now Himmel's MK2: its screens show the engine's pages, its LEDs are the
   engine's LEDs, and your clicks, drags and scrolls play the engine (the table
   below applies). Keyboard Shift = Shift.
4. Red pill **"IGNORED: another simulator plays Himmel"**? Another MK2
   simulator (a forgotten page or script) is still running, and only the
   newest one plays. Close it, or press **Take over**.

Check the whole chain without a browser: `python tools/mk2_e2e.py` (starts
its own `himmel.exe --sim`, plays groups, knobs, the wheel and TAKE, and
checks the engine state).

The simulator sends the same USB reports as the hardware over UDP on
127.0.0.1:8766; the controller does the rest exactly as with the real device
([weapons/README.md](../weapons/README.md)).

### Play the slice from the web GUI

The Perform page is a local web app (`gui/`), served by `himmel_ui.exe` on
<http://127.0.0.1:8790>. It shows PREVIEW (green, left) and PROGRAM (red,
right) with their pictures and four layers each, MAIN, the TAKE ring, the
crossfader and the clip list.

1. Start the engine: `build\bin\Release\himmel.exe --sim` (launch config
   `himmel-sim`). The supervisor starts `himmel_ui.exe` itself (`--no-ui` to
   skip it). For the simulated MK2 run `python tools/mk2_live.py --sim` and
   open <http://127.0.0.1:8765>; run only **one** MK2 simulator at a time.
   No engine yet? `build\bin\Release\fake_engine.exe` stands in for it
   (fake clips, moving test pictures; it refuses to start while a real
   engine runs).
2. Open <http://127.0.0.1:8790>.
3. Play:
   - **Load a clip:** click it in the list, then click a layer (Shift keeps
     it armed); or drag it onto a layer; or double-click it to load it into
     the focused layer.
   - **Focus:** click a layer's letter (A–D = PREVIEW, E–H = PROGRAM), like
     pressing a group button on the MK2. The panel at the bottom shows the
     focused layer's effects: pick an effect per slot, then its mix and
     parameters (double-click a slider = default). DECK FX and MASTER FX
     are the tabs next to it. The list has 19 effects and 6 generators
     (`fx/`); a generator on an empty layer draws by itself.
   - From a terminal: `himmel_cmd fx a1 1 kaleido extreme` (layer A1, slot 1,
     with its `extreme` preset), `himmel_cmd press a1 1 0.8` (PRESS),
     `himmel_cmd fx m 1 strobe` (master). `himmel_render --flash-cap 3`
     limits strobe / flash to 3 per second (off by default, D65);
     `--test-signal` lets the reactions move with a fake beat until audio
     exists; `--ticker "TEXT"` sets the text generator.
   - See every effect without the engine: `build\bin\Release\fx_check.exe`
     writes PNGs + contact sheets to `tools/_screens/fx/` and prints the GPU
     time per effect.
   - **TAKE** (space or the ring) travels PREVIEW to PROGRAM; **CUT**
     (Enter) swaps at once; the crossfader moves by hand; **BLACK** is top
     right.
4. Killing or restarting `himmel_ui` (or closing the page) never changes the
   outputs; the page reconnects by itself and says when the engine is
   missing, restarting or of another version.

### The rest of the web GUI (M11)

- **Views** (top left): START (the four-step wizard: clips, weapon, outputs,
  play), PERFORM, MK2 and SETTINGS. The address keeps the view
  (`#perform`, `#mk2`, …).
- **Clock** (top bar): BPM (drag up / down, double-click to type, `+` / `−`,
  Shift = 0.1), the four beat squares, TAP (or the `T` key), nudge `◀ ▶`,
  `1` = resync, and the global GRID (beat snap).
- **CLIP tab** (the drawer under the monitors, for the focused layer): the
  timeline (click = jump; drag `[` `]` = in / out, the thin brackets = the
  inner loop; Alt + drag = scratch), cue pads 1–16 (click = jump,
  Shift + click = set at the playhead, right-click = clear), beat loop
  1/8–16, direction and REVERSE (hold), play mode with its options, time
  base FREE / BPM, trigger style, start, target, snap, fades, glide, inner
  loop, catch-up, autopilot, the transition to a new clip, seed. The layer
  bar has speed, play / pause, restart, RELEASE, STOP and eject.
- **LAYER / DECK / MASTER FX:** click a slot's name to open the **effect
  browser** (families, generators, cost class, help, what it reacts to);
  each slot has its presets (`subtle`, `extreme`, …, `default`) and a
  **PRESS** button (hold it; a pen or touch screen sends real pressure).
  Values show in real units (degrees, ×, %).
- **GENERATORS** (tab above the clip list): click = onto the focused layer;
  on an empty layer the generator is the picture.
- **HIT:** Alt + click a clip fires it on the HIT layer; the HIT tab has
  FIRE (hold), STOP, opacity, blend, fades, trigger and mode.
- **MK2 view:** the MK2 drawn from `weapons/mk2.weapon.json` with its LEDs
  and both screens as the engine drives them, plus pressed buttons, knob
  turns and pad pressure (the bridge reads `Local\HimmelMk2`).
- `himmel_ui --watch --port 8791` (launch config `himmel-ui-watch`) is a
  read-only second view: it sends nothing and takes no MK2 ring, so it can
  watch a show another bridge drives.

### Project, scenes, outputs, library, audio, OSC (M11 part 2)

Each panel shows only when its process runs (its shared block is there and
has the right version); a missing one never stops the rest of the page.

- **Project** (top bar): the name (● = unsaved), SAVE (Ctrl+S; an untitled
  project asks for a name), `⋯` = new (1 or 3 screens), open by path, save
  as, export `.himmelshow`, revert, restore the autosave, relink missing
  clips, open recent. `↶ ↷` = undo / redo (Ctrl+Z / Ctrl+Shift+Z).
- **SCENES tab** (bottom panel): the 16 scenes of the bank (or the song when
  SETLIST is on), `◀ ▶` = bank / song. Click a pad = recall it into the
  focused deck (Alt = a cut), Shift + click = select, double-click = rename.
  REC saves the focused deck on the selected pad (or the next free one),
  +VAR = a new variation, RESET = back to the scene, GO / `◀` = next /
  previous scene. DICE: ROLL, MUTATE, back / forward, and the 8 locks the
  dice keep. The four sliders are the PROGRAM scene's macros (knobs 5–8).
- **OUTPUTS view:** canvas mode, resolution preset (a setup action: one
  hitch), refresh, blend width and gamma, IDENTIFY, one card per output
  (state ON SCREEN / LOST / WINDOW / NEW DISPLAY?, display identity, BLACK,
  "use the new display", SIM: PULL for tests on one screen), and where each
  layer covers the canvas (FILL ALL, FIT, OUT n, FREE).
- **LIBRARY view:** the import worker's status (queued / working / ready /
  failed, the current file, "catalog full"), and every clip of the library
  with its thumbnail, size, fps, length, energy, loop style, palette, mood
  and judder warning ("IN SHOW" = the engine has it). Drop files into the
  library's `originals` folder (the path is shown).
- **Audio:** the meter left of the clock (level, bass, mid, high, kick);
  click it for Settings → Audio: the input (mic / line in or sound on this
  PC, device, channels), auto gain, gain, gate, sensitivity, the MIDI clock
  port, the live bands and spectrum, and each effect's default reaction
  with the signal it follows.
- **Settings → Remote / OSC:** the port, the allow list, counters and the
  last 32 messages with their verdict.
- Test runs: `himmel.exe --sim -- --show-root build/test-show-root` (launch
  config `himmel-sim-test`) keeps projects and the library away from your own
  (the children run in `build/bin/Release`, so a relative root lands there).

### Playing it from the MK2

> Since the playable slice (2026-11-15) `himmel_render.exe` is the deck engine
> and the MK2 plays it through the Perform mapping (`docs/design/perform.md`;
> until it lands: wheel = crossfader, PLAY = TAKE, RESTART = CUT). The knob
> pages below belong to the parked renderer: start it with `himmel.exe --legacy`.

| Control | Page 1: TEST CARD | Page 2: CLIP | Page 3: CAMERA | Page 4: TOUCH FX |
|---|---|---|---|---|
| **Page < / >** | switch page | switch page | switch page | switch page |
| Knob 1 | hue | opacity | opacity | effect (turn to step) |
| Knob 2 | saturation | speed −2x..+2x (centre = stop) | zoom | depth |
| Knob 3 | bar speed | zoom | position X | effect X |
| Knob 4 | grid size | position X | position Y | effect Y |
| Knob 5 | grid amount | position Y | mirror on/off | hold on/off |
| Knob 6 | bar width | – | brightness | – |
| Knob 7 | pad fade time | – | contrast | – |
| Knob 8 | master brightness | master brightness | master brightness | master brightness |
| Pads | flash their part of the canvas | 16 jump points in the clip | flash | a 4×4 X/Y surface: press = effect on, pressure = amount |
| Master encoder | next / previous clip | next / previous clip | next / previous clip | next / previous effect |
| Play / Restart | clip pause / to start | same | same | same |
| Navigate | span ↔ mirror | span ↔ mirror | span ↔ mirror | span ↔ mirror |
| Sampling | – | – | start / stop the latency test | – |

The MK2 screens always show the current page, with each value right above its knob.

### Touch FX

One effect over the whole picture, played by one finger
([design note](touch-fx-entrancer.md)). X and Y of the finger are the
effect's two parameters, pressure is how much; lift and it fades out, unless
**Hold** is on. Effects: 1 Hue Turn, 2 Duotone, 3 Grid, 4 Kaleido, 5 Bulge,
6 Slide, 7 Waves, 8 Strobe, 9 Trails.

- **MK2:** knob page 4. The pads are a coarse 4×4 surface; the pad under the
  finger lights in the effect's colour.
- **Mouse (no controller needed):** drag on any output window. Keys `1`–`9`
  choose the effect, `H` toggles Hold.
- **Lightpad Block:** planned as a direct source (weapon no. 2). Today it can
  play Touch FX as the pad of the simulated Entrancer.
- **Entrancer (simulated):** `python tools/entrancer_live.py`, then open
  http://127.0.0.1:8770 while the engine runs. A drawn X/Y effects unit with
  HOLD, PAD MOTION, FREEZE, TAP and memories; the mouse or a real Lightpad is
  its pad. See [entrancer-weapon.md](entrancer-weapon.md).

See all nine without starting the engine: `build\bin\Release\touch_fx_check.exe`
writes PNGs to `tools/_screens/touchfx/` and prints the GPU time per effect.

## 7. Clips

Himmel plays **HAP** (`.mov`): Hap, Hap Alpha and Hap Q, plain or chunked.
Not yet: Hap Q Alpha, Hap R.

The test clips (`media/test/`, not in git) are made by one script, in about
10 seconds; it checks each clip with `hap_probe.exe` if that is built:
```
python tools/make_test_clips.py
```
It makes Hap (4 chunks), Hap Q, Hap Alpha (alpha ramp) and an uncompressed
1276x716 25 fps clip from ffmpeg's test pattern. Make a single clip by hand:
```
ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30:duration=10 -vf format=rgba -c:v hap -chunks 4 media/test/testcard.mov
```
Convert your own footage (width and height must be divisible by 4):
```
ffmpeg -i input.mp4 -an -vf "scale=trunc(iw/4)*4:trunc(ih/4)*4,format=rgba" -c:v hap -format hap_q -chunks 4 media/my_clip.mov
```
Use `-format hap` for the smallest files, `hap_q` for better quality,
`hap_alpha` for transparency. Check a clip without the engine:
```
build\bin\Release\hap_probe.exe media/my_clip.mov --png first_frame.png
```

## 8. Camera and the latency test

Plug in a USB webcam and start Himmel (add `--camera "c920"` or similar to
pick one). On knob page 3 the camera is a layer over the clip.

**Latency test:** point the webcam at one output (fullscreen works best),
press **Sampling**. The canvas flips black/white 16 times; the MK2 screen then
shows `LOOP 95 MS (80-112)`: median (min–max) time from the frame being handed
to the GPU until the camera delivers the frame that shows it. That is close to
what an audience sees as camera-to-screen delay. A camera built into the
laptop's own screen can't see it; use a mirror or an external webcam.

## 9. Make it yours

- **Your own screen modes / games** on the MK2 screens: recipe in
  [mk2-screen-fun.md](mk2-screen-fun.md).
- **Use the MK2 from your own program:** run `mk2_controller.exe` and open the
  shared memory block `Local\HimmelMk2`. You get button / knob / encoder / pad
  events through a lock-free ring and set LEDs and the two screens through
  seqlocks. `src/controller/app/mk2_monitor_main.cpp` is a complete
  ~200-line example; the byte-level details are in
  [mk2-controller-spec.md](mk2-controller-spec.md).
- **Draw MK2 screen pages** in your app with `himmel/mk2/pages.h` (pure
  drawing, no Windows calls); see `mk2_pages_demo_main.cpp`.
- **Play HAP in your own engine:** `src/media` (MOV reader + HAP decoder) has
  no Windows or GPU dependencies.

## 10. Troubleshooting

| Problem | Fix |
|---|---|
| `MK2 not found` / `Waiting for the MK2` | Close Maschine, Controller Editor, NI Hardware Agent/NIHardwareService, the Python live view and any other Himmel. Re-plug the USB cable. |
| Build Tools install fails with **1602** | The UAC prompt was not accepted (it may hide behind other windows). Run it again and say yes. |
| An installer fails with **1603** / "Install server not responding" | Windows Installer was busy (often Windows Update). Wait and try again. |
| `LNK1104: cannot open file ...exe` when building | The program is still running. Stop Himmel (Ctrl+C) and build again. |
| A clip does not load | Look in the log: `hap_probe` tells you why. Hap Q Alpha is not supported yet; sizes must be multiples of 4. |
| Camera shows `CAMERA OFF` | Another app (Teams, the Camera app, a browser) holds it. Himmel retries every 2 s by itself. |
| Output windows vanish for a moment | The renderer crashed or hung and was restarted by the supervisor; the log says why. (Planned: black "curtain" windows so the desktop never shows.) |
| Compiler error about `small` | `windows.h` defines `small` as a macro; don't use it as a name in code that sees Windows headers. |
