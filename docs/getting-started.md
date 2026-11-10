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
| `himmel_render.exe` | The renderer (outputs, test card, clips, camera). |
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
| `--fullscreen` / `--windowed` | Borderless on the monitors (left to right) / normal windows. |
| `--size WxH` | Window size in windowed mode. |
| `--mode span\|mirror` | Span: one canvas across all outputs. Mirror: every output shows the whole canvas. |
| `--clip file.mov` | Add a clip to the playlist (repeatable). Default: every `.mov` under `media/`. |
| `--camera NAME`, `--camera-index N`, `--no-camera` | Pick the webcam (part of its name, or its index), or none. Default: the first camera that is not an IR camera. |

Supervisor option: `--no-controller` (renderer only). The supervisor logs to
`build\bin\Release\logs\himmel-YYYY-MM-DD.log`, restarts a crashed part within
0.25 s and kills and restarts a part that hangs for 2 s. **Ctrl+C** in its
window stops everything cleanly. In a fullscreen output only **Shift+Esc**
closes the renderer (which the supervisor then restarts); **M** toggles
span/mirror.

### Playing it from the MK2

| Control | Page 1: TEST CARD | Page 2: CLIP | Page 3: CAMERA |
|---|---|---|---|
| **Page < / >** | switch page | switch page | switch page |
| Knob 1 | hue | opacity | opacity |
| Knob 2 | saturation | speed −2x..+2x (centre = stop) | zoom |
| Knob 3 | bar speed | zoom | position X |
| Knob 4 | grid size | position X | position Y |
| Knob 5 | grid amount | position Y | mirror on/off |
| Knob 6 | bar width | – | brightness |
| Knob 7 | pad fade time | – | contrast |
| Knob 8 | master brightness | master brightness | master brightness |
| Pads | flash their part of the canvas | 16 jump points in the clip | flash |
| Master encoder | next / previous clip | next / previous clip | next / previous clip |
| Play / Restart | clip pause / to start | same | same |
| Navigate | span ↔ mirror | span ↔ mirror | span ↔ mirror |
| Sampling | – | – | start / stop the latency test |

The MK2 screens always show the current page, with each value right above its knob.

## 7. Clips

Himmel plays **HAP** (`.mov`): Hap, Hap Alpha and Hap Q, plain or chunked.
Not yet: Hap Q Alpha, Hap R.

Make a test clip:
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
