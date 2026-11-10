# Third-party notices and credits

Himmel is MIT-licensed (see [LICENSE](LICENSE)). It stands on other people's
work. This file lists **everything** we use or learned from, with its license,
and says exactly how we use it. If we missed something, please open an issue.

## 1. Code that is compiled into Himmel

These are downloaded by CMake (`FetchContent`) at build time. Nothing is
copied into this repository except their license texts in [`licenses/`](licenses).

| Library | Version | License | Used for | Where |
|---|---|---|---|---|
| [hidapi](https://github.com/libusb/hidapi) (libusb project) | 0.14.0 | Your choice of GPLv3, **BSD-style** or the original HIDAPI license. We use it under the **BSD-style** license. | USB HID access to the Maschine MK2 (`mk2_controller.exe`) | [`licenses/hidapi-LICENSE.txt`](licenses/hidapi-LICENSE.txt), [`licenses/hidapi-LICENSE-bsd.txt`](licenses/hidapi-LICENSE-bsd.txt) |
| [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann | 3.11.3 | MIT | Reading `maschine_mk2_map.json` (later: shows, presets) | [`licenses/nlohmann-json-LICENSE.MIT`](licenses/nlohmann-json-LICENSE.MIT) |
| [Snappy](https://github.com/google/snappy) by Google | 1.2.1 | BSD-3-Clause | HAP's second-stage decompression (`src/media/src/hap.cpp`) | [`licenses/snappy-COPYING.txt`](licenses/snappy-COPYING.txt) |

If you ship Himmel binaries, ship these license texts with them (the BSD and
MIT licenses ask for that).

## 2. Operating-system APIs (not redistributed)

Part of Windows and the Windows SDK. Free to use in Windows programs; nothing
from them is included in this repository.

| API | Used for |
|---|---|
| Direct3D 11, DXGI, D3DCompiler (`d3dcompiler_47.dll`) | Rendering, flip-model swap chains, shader compilation at start-up |
| Media Foundation (`mfplat`, `mf`, `mfreadwrite`, `mfuuid`) | Webcam capture |
| Win32 (file I/O, shared memory, job objects, `winmm` timer resolution) | Processes, IPC, supervisor |
| Windows Runtime C++ Template Library (WRL): `Microsoft::WRL::ComPtr` | COM pointers |

Build tools: Microsoft Visual Studio 2022 Build Tools (MSVC) and CMake (the
copy bundled with Build Tools). Both are used, not redistributed.

## 3. Python tools (`tools/`): installed with pip, not included

| Package | License | Used for |
|---|---|---|
| [hidapi (cython-hidapi)](https://github.com/trezor/cython-hidapi) | One of: BSD-3-Clause, GPLv3, original HIDAPI license | MK2 access from the Python tools |
| [PyUSB](https://github.com/pyusb/pyusb) | BSD-3-Clause | Optional fallback access in `maschine_mk2_test.py` |
| [libusb-package](https://github.com/pyocd/libusb-package) | Apache-2.0 (bundles [libusb](https://libusb.info), LGPL-2.1) | Optional fallback backend for PyUSB |

The rest is the Python standard library (PSF license).

## 4. Specifications, protocol knowledge and algorithms

No code from these was copied. We read them, then wrote our own code and
checked everything on real hardware.

| Source | License | What we learned / used |
|---|---|---|
| [abique/ni-mk2](https://github.com/abique/ni-mk2) by Alexandre Bique | MIT | The Maschine MK2 HID protocol: report ids, LED report layout, the screen strip format (`0xE0`/`0xE1`), knob range. Our main reference for the MK2. Every byte was then re-checked on the owner's device (see `maschine_mk2_map.json` and `docs/mk2-controller-spec.md`). |
| [Vidvox/hap](https://github.com/Vidvox/hap): the HAP video specification (`documentation/HapVideoDRAFT.md`). HAP was written by Tom Butterworth, commissioned by VIDVOX. | BSD-2-Clause | Our HAP decoder follows the spec: section headers, compressor/format nibbles, chunked "decode instructions". The Hap Q scaled-YCoCg → RGB maths in our shaders matches Vidvox's reference shader. "HAP" is VIDVOX's format name. |
| The Linux kernel driver `snd-usb-caiaq` ([sound/usb/caiaq](https://github.com/torvalds/linux/tree/master/sound/usb/caiaq)) by Daniel Mack and contributors | GPL-2.0 (no code used) | Protocol facts for the Maschine **MK1** only (USB endpoints, command bytes, pad word layout, LED banks), used by the old reference script `tools/maschine_test.py`. Only facts were taken; no code was copied, so the GPL does not apply to Himmel. |
| Maschine USB projects such as [wrl/maschine.rs](https://awesome.ecosyste.ms/projects/github.com%2Fwrl%2Fmaschine.rs) | not checked (no code used) | General background on Maschine USB HID devices. |
| QuickTime File Format / ISO base media file format (Apple, ISO/IEC 14496-12) | public specifications | Our minimal MOV reader (`src/media/src/mov.cpp`): atoms `moov/trak/mdia/minf/stbl`, `stsd`, `stts`, `stsc`, `stsz`, `stco`/`co64`. |
| BC1/BC3/BC4 (S3TC/DXT, RGTC) texture compression, as documented by Microsoft for Direct3D | public documentation | HAP frames go to the GPU as BC textures; `hap_probe --png` has a small CPU BC decoder for checking. |
| BT.709 YCbCr → RGB conversion (ITU-R BT.709) | public standard | Camera NV12 → RGB in the shader. |
| Ordered (Bayer 4×4) dithering | classic algorithm, public domain | Dithered images on the 1-bit MK2 screens. |
| The classic 5×7 LCD bitmap font | widely shared, believed public domain | The glyph table in `src/controller/src/output.cpp` and `tools/mk2_screens.py` uses the same well-known 5×7 shapes found in many LCD and Arduino libraries. If you know its original author, please tell us so we can credit them. |
| [WeAreROLI/roli_blocks_basics](https://github.com/WeAreROLI/roli_blocks_basics) by ROLI Ltd. (the BLOCKS SDK as a JUCE module, successor of `juce_blocks_basics`) | ISC, © 2020 ROLI Ltd. | The ROLI BLOCKS protocol for weapon no. 2, the Lightpad: SysEx header, 7-bit bit packing, checksum, message types and bit widths, device commands, ping timing, `sharedDataChange` encoding, the 15 × 15 RGB565 bitmap heap layout, the serial-number request, button 0 = Mode (files `protocol/roli_BitPackingUtilities.h`, `roli_BlocksProtocolDefinitions.h`, `roli_HostPacketBuilder.h`, `roli_HostPacketDecoder.h`, `roli_BlockModels.h`, `topology/internal/roli_BlockSerialReader.cpp`, `littlefoot/roli_LittleFootRemoteHeap.h`, `visualisers/roli_BitmapLEDProgram.cpp`). The LittleFoot bytecode format, opcodes, stack rules, native-function ids and the program upload order come from `littlefoot/roli_LittleFootRunner.h`, `roli_LittleFootCompiler.h` and `topology/internal/roli_BlockImplementation.cpp`. Our Python in `tools/lightpad_sim.py`, `tools/lightpad_hw.py` and `tools/lightpad_littlefoot.py` (a hand assembler for our own renderer, and a small VM) was written from reading them; no code was copied. |
| [hyperb1iss/blocksd](https://github.com/hyperb1iss/blocksd) by Stefanie Jane, and its [documentation](https://hyperb1iss.github.io/blocksd/) | ISC, © 2025-2026 hyperb1iss | A Linux daemon for ROLI Blocks. Its docs confirmed the checksum and the connection lifecycle: serial request, topology, `endAPIMode` + `beginAPIMode`, pings every 400 ms. They also explain why LEDs on real hardware need an uploaded LittleFoot renderer, and that pixels must follow the program upload. Its `opcodes.py` confirmed our opcode numbers, and `config_ids.py` the config item ids (from ROLI's `roli_BlockConfigId.h`). No code used. For the LUMI Keys: `docs/reference/devices.md`, `docs/architecture/littlefoot.md` and `src/blocksd/littlefoot/keys.py` gave the facts that LUMI key lighting needs firmware 1.3.0 or newer, uses a 48-byte RGB565 heap with `fillPixel(colour, key, 0)`, and keeps the musical handler with `setUseDefaultKeyHandler(true, false)`. Our own key renderer in `tools/lightpad_littlefoot.py` was assembled from those facts; no code was copied. |
| ROLI's example LittleFoot script `littlefoot/scripts/Example Scripts/ROLIPianoExample.littlefoot` (in roli_blocks_basics) | ISC, © 2020 ROLI Ltd. | LUMI facts: the callbacks `keyStrike / keyPress / keyLift / keyMove`, buttons 0 = mode, 1 = octave down, 2 = octave up, key 0 = note 48 + 12 × octave, and the config ids it uses (octave = 4). Read, not copied. |
| *V-4 Owner's Manual* (EDIROL / Roland Corporation), "MIDI Implementation" pp. 89-96, panel descriptions pp. 13-16, effects pp. 23-39 and the menu list pp. 75-82. Official download on [Roland's V-4 support page](https://proav.roland.com/es/products/v-4/); we read the copy on [manualsdir.com](https://www.manualsdir.com/manuals/87855/edirol-v-4.html?page=89) | © Roland Corporation, all rights reserved (manual; we use the protocol facts only) | Weapon no. 7, the V-4 video mixer: the panel layout and factory effect names, Bank Select + Program Change for inputs A/B and memory, the factory controllers (video fader CC#11, transition time CC#7), Note Mode note numbers, the assignable CC set, SysEx DT1 format, model ID `00 5B`, the MIDI setup addresses and the Roland checksum (the manual's worked example is in our self-test), MIDI clock for BPM SYNC, and the MIDI Thru rule. The effect, transition and menu catalogue in `docs/v4-effects.md` (names, counts, ranges) comes from the menu list. `tools/v4_sim.py` and `weapons/v4.weapon.json` were written from these facts; no text or code was copied. |
| ["Edirol V4 MIDI controll"](https://forum.vvvv.org/t/edirol-v4-midi-controll/815), a thread on the vvvv forum | forum posts (no code used) | Users confirming that the T-bar and input selectors work over MIDI, and an example of a user CC setup. Background only. |

## 5. External tools (used, never linked)

| Tool | License | Used for |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) (`ffmpeg.exe`, e.g. a [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) "full" build) | LGPL-2.1+ / GPL-2+ depending on the build | Making HAP test clips (`-c:v hap`) and, later, the import pipeline. Himmel runs it as a separate program and does **not** link FFmpeg libraries (decision 2026-11-10). |

## 6. Fonts in the design mockups

`docs/gui-mockups/` and `docs/vj-mockup.html` load
[Inter](https://fonts.google.com/specimen/Inter) and
[JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) from Google
Fonts. Both are under the SIL Open Font License 1.1. They are not included in
this repository.

## 7. Planned dependencies (not used yet: check before adding)

From the agreed stack in `CLAUDE.md`. Licenses as we understand them today;
**verify each one before it is added**, and record the decision here.

| Planned | License (to verify) | Note |
|---|---|---|
| Dear ImGui (operator GUI) | MIT | |
| Spout 2 (video sharing, DX11) | BSD-2-Clause | |
| NDI SDK (network video) | Proprietary NDI SDK license | Free to use, but its terms apply to redistribution. Ask the owner first. |
| Ableton Link (tempo sync) | GPLv2+ (or a commercial license from Ableton) | Plan: keep it in a separate bridge process so the MIT core stays MIT. Ask the owner first. |
| beat-link (Pro DJ Link bridge, Java) | EPL-2.0 | Separate bridge process. |
| glslang, SPIRV-Cross (ISF → HLSL) | BSD-3-Clause / Apache-2.0 (mixed) | |
| SQLite | Public domain | |
| ONNX Runtime + DirectML | MIT (ONNX Runtime), Microsoft license (DirectML redistributable) | Optional ML features. |

## 8. Trademarks

Native Instruments, Maschine and Controller Editor are trademarks of Native
Instruments GmbH. Himmel is **not affiliated with or endorsed by** Native
Instruments; it talks to the hardware you own over standard USB HID. ROLI,
BLOCKS, Lightpad, LUMI and LittleFoot are trademarks of ROLI Ltd. / Luminary; Himmel
is **not affiliated with or endorsed by** them, and its drawings say
"HIMMEL · LIGHTPAD" without the maker's logo. Roland, EDIROL and V-LINK
are trademarks of Roland Corporation; Himmel is **not affiliated with or
endorsed by** Roland, and its V-4 drawing says "HIMMEL · V4" without the
maker's logo. It talks to a V-4 you own over standard MIDI. Windows,
Direct3D and Media Foundation are trademarks of Microsoft. HAP is a format by
VIDVOX. All other names belong to their owners.

## 9. AI assistance

Most of the code and documentation in this repository was written with an AI
assistant. See [docs/ai-assisted-development.md](docs/ai-assisted-development.md).
