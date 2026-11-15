# Weapon no. 4: the Entrancer (simulated, Lightpad as its pad)

Status (2026-11-10): **simulated and playing the engine**, checked in the
browser and on the engine's output window. The owner has a real unit; its
MIDI is not wired yet.

The idea and the research are in [touch-fx-entrancer.md](touch-fx-entrancer.md).
In short: the Korg KAOSS PAD Entrancer (2003) is an X/Y touch-pad effects unit
for audio and video. Himmel plays its gesture with Himmel's own Touch FX
effects. Korg, KAOSS PAD and Entrancer are trademarks of KORG Inc.; Himmel is
not affiliated with KORG. The drawing says "HIMMEL · KPE1" and carries no
maker logo, as with the MK2.

## Try it

```
build\bin\Release\himmel_render.exe --outputs 1 --size 960x540
python tools/entrancer_live.py                        then open http://127.0.0.1:8770
```

(`himmel.exe` works too; the render process listens either way.)

- The page draws the unit from [`weapons/entrancer.weapon.json`](../weapons/entrancer.weapon.json):
  layout, colours and control shapes after product photos and the owner's own
  unit (blue-violet front, side rails, green display, round TAP/BPM, round
  PAD MOTION lever, chrome-framed pad that glows in the effect's colour).
  No maker logo or wordmark; the badge says "HIMMEL · KPE1". TAP/BPM blinks on
  the beat, the recalled memory key lights red.
- **Mouse:** drag on the pad (Shift = light touch), drag or scroll the knobs,
  click the levers and buttons.
- **Real Lightpad as the pad:** press *Use the real Lightpad as the pad* (or
  start with `--lightpad`). Close ROLI software and the Lightpad live view first:
  the MIDI ports are exclusive. The block's first finger is the unit's finger,
  pressure is sent too, the Mode button is HOLD, and the 15 × 15 LEDs show the
  pad's light (colour from the program and the finger, a cross where HOLD or
  PAD MOTION keeps the effect, a blue flash while frozen, dark on AUDIO).
- The pill at the top says **playing Himmel: KALEIDO ON** while the engine
  answers.

## What works like the real unit

| Control | In the simulator |
|---|---|
| Touch pad | X / Y, touch on/off. On most programs Y is *how much* (the unit's Y = blend). The display shows the position as 1.1 … 9.9 while touched. |
| PROGRAM / BPM knob + display | program 00–99 (program change); after TAP/BPM it sets the tempo for 5 s |
| TAP/BPM | tap tempo (40–300 BPM). Auto BPM needs audio: not in Himmel. |
| FX BALANCE | overall amount (dry … wet) |
| HOLD | keeps the effect at the last position after you let go |
| PAD MOTION (lever up) | touch to record up to 6 s, let go and it loops; FUNCTION + lever = erase |
| MUTE/FREEZE (lever down, held) | freezes the picture on all outputs |
| PROGRAM MEMORY 1–8 | press = recall (program, FX balance, tempo, HOLD + position); FUNCTION + key held 2 s = store. Saved in `tools/_entrancer/memories.json` (gitignored). |
| CONTROL SELECT | AUDIO = the pad leaves the video alone; COMBI / VIDEO = video effect |
| LINE IN VOLUME, PEAK, VIDEO 2 | drawn only (Himmel has no audio in or second analog input) |
| SAMPLE 1/2, REC/STOP | show `n/a`: sampling is not in Himmel yet |

The 100 programs are played with the closest of Himmel's 9 Touch FX effects
(table in `src/engine/src/entrancer.cpp`; for example 14–26 tiles → Grid,
36 Moving Mirror → Kaleido, 29 Stroboscope → Strobe). More programs get their
own look as Himmel gains effects.

## How it talks to the engine

```
mouse / Lightpad --> tools/entrancer_sim.py (the unit's logic)
                       |  MIDI bytes over UDP 127.0.0.1:8768
                       v
himmel_render.exe: EntrancerLink (receive thread, lock-free ring)
                   -> entrancer::Mapper (MIDI -> Touch FX) -> ps_touchfx
                   <- one status line every 250 ms ("HIMMEL program=36 effect=KALEIDO on=1 ...")
```

- The bytes are what the real unit sends on MIDI out: **CC 12 = X, CC 13 = Y,
  CC 92 = touch on/off, program change 0–99**. So the real unit needs only a
  MIDI input in front of the same `Mapper` (WinMM, planned).
- Panel controls the real unit keeps to itself travel as **Himmel-only
  controllers** outside the unit's assignable ranges (CC 1–31, 64–95):
  102 FX balance, 104 freeze, 105 lever on AUDIO, 106/107 tempo (MSB/LSB),
  108 pressure. A real unit never sends them.
- The simulator sends MIDI active sensing (`FE`) every 250 ms. No bytes for
  1 s = the unit is lost: the engine lifts the finger and unfreezes; the
  output keeps running (checked: 60 fps, "Entrancer lost" in the log).
- Engine options: `--entrancer-port N`, `--no-entrancer`.
- Tests: `entrancer_tests.exe` (MIDI parsing, running status, the table, the
  extensions), `python tools/entrancer_sim.py --selftest` (HOLD, PAD MOTION,
  tap tempo, freeze, the lever, the LED picture).

## The real unit over MIDI (built 2026-11-10, not yet tried on the unit)

The unit has 5-pin MIDI IN / OUT, so it needs a **USB-MIDI interface**. Its
MIDI OUT goes to the interface's IN.

```
build\bin\Release\himmel_render.exe --midi-list
build\bin\Release\himmel_render.exe --entrancer-midi "part of the port name" [--entrancer-channel N]
```

- `--midi-list` prints the MIDI input ports and exits.
- `--entrancer-midi NAME` opens the first input port whose name contains NAME
  (case-insensitive). If none is there yet, the engine waits and opens it when
  it appears; a pulled cable closes it ("Entrancer MIDI lost") and it reopens
  by itself. The output never stops.
- `--entrancer-channel N` listens to one channel (the unit's MIDI Edit
  setting: FUNCTION + SAMPLE 2, then the PROGRAM/BPM knob). Default: all.
- The real unit has its own parser (`emap_midi`), so it and the simulator can
  run at the same time without mixing bytes. Both play Touch FX.
- Code: `src/engine/src/midi_in.cpp` (WinMM, part of Windows: no new
  dependency; a watcher thread opens / reopens, the WinMM callback fills a
  lock-free ring, the render thread reads it). Tests in `entrancer_tests.exe`
  (the ring and the channel filter, no port needed).
- Ports are exclusive on Windows: close other MIDI software that holds the
  interface's port.

What the manual says the unit sends: CC 12 = X, CC 13 = Y (also CC 1 and CC 2),
CC 93 = X+Y, CC 92 = touch on/off, program change 0–99; PAD MOTION playback
is sent as normal pad messages. Himmel uses 12, 13, 92 and program change.

**Check on the unit** when it is up from the cellar:
1. `--midi-list`, then `--entrancer-midi` with the interface's name. The log
   says "MIDI port … open".
2. Touch the pad: the effect follows; let go: it fades out.
3. HOLD on, touch, let go: does the unit keep the touch on (CC 92) or send off?
   If it sends off, Himmel needs its own hold for this weapon.
4. PAD MOTION: is the playback sent?
5. Program knob: does the program change arrive (it can be switched off in
   MIDI Edit)?
6. FX BALANCE, MUTE/FREEZE and the AUDIO/VIDEO lever probably send nothing;
   play them from the MK2 or the simulator meanwhile.

## Later

- **MIDI out to the unit:** it receives CC 12/13/92, program change and MIDI
  clock, so Himmel could play the unit's own video effects and keep its BPM
  effects in time.
- **Its video out** (composite / S-video) can come in through a USB capture
  dongle as a Himmel camera; measure that latency with the Sampling test first.
