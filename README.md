# Himmel

**An open-source VJ and installation system for Windows, played from a
Native Instruments Maschine MK2.** One PC drives 1–8 screens or projectors;
the Maschine's pads, knobs and two little screens are the instrument.

> **Open and AI-assisted.** Himmel is MIT-licensed and developed in the open
> by [ReadyMedia.no](https://readymedia.no) **with an AI assistant (Claude by
> Anthropic, via Claude Code)**. The owner sets the direction and tests every
> step on real hardware; the assistant writes most of the code and docs.
> How that works and what it means for you:
> [docs/ai-assisted-development.md](docs/ai-assisted-development.md).
> Everything we use or learned from is credited in
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Goals, in this order: **show-safe** (runs all night; a part that fails never
takes down the output), **low latency**, **easy to use**, **light on the GPU**.

## What works today

| Part | State |
|---|---|
| **MK2 mapping** | Every control and LED verified on the device ([`maschine_mk2_map.json`](maschine_mk2_map.json)); a browser live view to check and re-learn your own unit. |
| **MK2 controller** | C++ process: events over shared memory (~750 Hz pads, velocity, aftertouch), LEDs, both 1-bit screens, automatic reconnect. |
| **No MK2? Simulate it** | The browser live view becomes a virtual MK2 that plays the real engine (`himmel.exe --sim`). See [weapons](weapons/README.md). |
| **MK2 screen pages** | Perform / browse / audio / menu / message pages with values right above the physical knobs. Plus games for the screens (Snake, Star Swarm, Sky Shield, ...). |
| **Engine** | Supervisor that restarts crashed or hung parts in < 1 s; Direct3D 11 renderer, 1–8 outputs, span or mirror, waitable flip-model swap chains. |
| **HAP playback** | Own MOV reader + HAP decoder (Hap, Hap Alpha, Hap Q), frames decoded ahead in a worker and sent to the GPU compressed. |
| **Camera** | Webcam layer (Media Foundation) and an automatic screen-to-camera latency test. |
| **Next** | Black curtains during restarts, clip import pipeline, audio analysis, cues and setlists, Smart Random, operator GUI. |

## Quick start

Needs Windows 10/11 x64, Visual Studio 2022 Build Tools (C++), and a Maschine
MK2 for the fun part. Full guide: **[docs/getting-started.md](docs/getting-started.md)**.

```
git clone https://github.com/readymedia/HIMMEL.git
cd HIMMEL
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
python tools/make_test_clips.py
build\bin\Release\mk2_tests.exe && build\bin\Release\media_tests.exe
build\bin\Release\himmel.exe -- --outputs 3 --size 640x360
```

Close Native Instruments software first (it holds the MK2). Clips: every
`.mov` (HAP) under `media/`; how to make them is in the guide.

## Documentation

| | |
|---|---|
| [Getting started](docs/getting-started.md) | Install, build, map your MK2, run, clips, camera, troubleshooting |
| [Architecture](docs/architecture.md) | Processes, shared memory, threads, frame-loop rules, measurements |
| [MK2 controller spec](docs/mk2-controller-spec.md) | The MK2 USB protocol byte by byte, events, timings, velocity |
| [MK2 screen fun](docs/mk2-screen-fun.md) | How the screen modes work and how to make your own |
| [Weapons](weapons/README.md) | Controllers as "weapons": profile format, simulator (play with no hardware), adding a new one |
| [AI-assisted development](docs/ai-assisted-development.md) | Who did what, and how we keep it honest |
| [Third-party notices](THIRD_PARTY_NOTICES.md) | Every library, spec, tool and font, with its license |
| [Contributing](CONTRIBUTING.md) | How to help |
| [`CLAUDE.md`](CLAUDE.md) | The running hand-off: hardware facts, decisions with dates, next steps |
| [Design mockups](docs/vj-mockup.html) | Interactive mockup of the MK2 + 3-screen wall; GUI sketches in `docs/gui-mockups/` |

## Repository layout

```
src/controller   MK2 controller module, screen pages, test clients
src/engine       supervisor (himmel.exe), renderer, clip player, camera capture
src/media        MOV reader + HAP decoder, hap_probe
tests/           unit tests (no hardware needed)
tools/           Python MK2 tools: live view / mapper, screen modes, measurements
weapons/         controller profiles ("weapons"): layout for the live view + simulator
docs/            guides, spec, measurements, mockups
licenses/        license texts of the libraries we compile in
media/           your clips (not in git)
```

## License

[MIT](LICENSE) © 2026 ReadyMedia.no and Himmel contributors. Third-party code
keeps its own license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Native Instruments and Maschine are trademarks of Native Instruments GmbH;
Himmel is not affiliated with or endorsed by them.

Made by ReadyMedia.no, with AI assistance.
