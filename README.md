# Himmel

A local Windows VJ and interactive-installation system, played from a
**Native Instruments Maschine MK2**. One PC drives 1–6 screens or projectors.

Goals, in order: show-safe (runs all night, a failing part never takes down the
output), minimal latency, easy to use, light on the GPU.

## Status

| Part | State |
|---|---|
| MK2 map (`maschine_mk2_map.json`) | Verified on the device: 48 buttons, 8 knobs, encoder, 16 pads, all LEDs |
| MK2 controller (`src/controller`) | C++ process: events over shared memory, LEDs, both 1-bit screens, auto reconnect |
| MK2 screen pages | Perform / browse / audio / menu / message / knob-align pages |
| Engine skeleton (`src/engine`) | Supervisor + D3D11 renderer, 1–8 outputs (span / mirror), MK2 control |
| HAP playback (`src/media`) | Own MOV reader + HAP decoder (Snappy), frames decoded ahead in a worker |
| Camera | Media Foundation capture layer + automatic screen-to-camera latency test |
| Next | Black "curtain" windows on restart, import pipeline, audio analysis, cues, Smart Random, operator GUI |

`CLAUDE.md` is the detailed hand-off (hardware facts, decisions, next steps).

## Layout

```
src/controller   MK2 controller module, screen pages, test clients
src/engine       supervisor (himmel.exe), renderer (himmel_render.exe), clip player, camera
src/media        MOV reader + HAP decoder, hap_probe tool
tests/           unit tests (no hardware needed)
tools/           Python MK2 tools: live view / mapper, screen fun modes, measurements
docs/            controller spec, measurements, screen docs, mockups
media/           clips (not in git)
```

## Build (Windows, Visual Studio 2022 Build Tools)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\bin\Release\mk2_tests.exe
build\bin\Release\media_tests.exe
```

hidapi, nlohmann/json and Snappy are fetched by CMake.

## Run

```
build\bin\Release\himmel.exe -- --outputs 3 --size 640x360     (windows, for development)
build\bin\Release\himmel.exe -- --fullscreen                    (one output per monitor)
```

Close Native Instruments software and the Python MK2 Live view first (they
hold the MK2). Clips: every `.mov` under `media/`, or `--clip file.mov`.
Test clips: `ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30:duration=10 -vf format=rgba -c:v hap out.mov`.

Made by ReadyMedia.no
