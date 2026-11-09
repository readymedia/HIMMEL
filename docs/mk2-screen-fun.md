# MK2 screens: graphics, animation and games

How the fun modes in `tools/mk2_fun.py` work, and how to build more of them.
The protocol and format facts are in `CLAUDE.md` (checked on the device
2026-11-09). This note is about *what you can draw* and *how*.

## 1. One wide image across both screens

The two screens sit side by side, so treat them as **one 512 × 64 canvas**:

```python
wide = mk2_screens.Canvas(512, 64)    # draw anything here
left, right = wide.split()            # 2048 bytes each (256 x 64, 1 bit)
sender.send(t, "left",  mk2_screens.pack_buf(left))
sender.send(t, "right", mk2_screens.pack_buf(right))
```

Anything that moves from x = 255 to x = 256 pans from the left screen to the
right one. There is a black gap of a few cm between the glass, so things look
best when they move steadily (scrollers, stars) rather than sit on the seam.

## 2. Frame budget

| Step | Cost |
|---|---|
| One 8-row strip over hidapi | ~0.8–0.9 ms |
| Full frame, both screens (16 strips) | ~13–15 ms |
| Python update + draw per frame (`mk2_fun_check.py`) | splash 2, swarm 1, munch 3, snake 3, maze 9, demo 13 ms |
| Only the strips that changed | much less (the sender skips equal strips) |

So **~30 fps is safe** even when every pixel changes (demo, maze). 60 fps
is not realistic for full-screen motion. Python drawing cost per 512 × 64 frame
was measured at about 3 ms (snake), 9 ms (maze) and 14 ms (demo). The C++
version will be far cheaper, so USB is the real limit.

Rules that keep it show-safe:
- Draw in its own thread, never in the render loop, and never while holding
  the lock that the input thread needs (`mk2_live.py` copies the input under the
  lock, then draws without it).
- Only send changed strips, and cap the rate (~30 Hz).
- A screen error must never stop button/pad reading.

## 3. Grey levels on a 1-bit screen: ordered dithering

The screens are on/off only. Brightness `v` (0..1) is faked with a 4 × 4 Bayer
matrix: a pixel is lit when `v > BAYER[y & 3][x & 3]`. This gives 16 even
grey steps that do not flicker or crawl when the image is still, unlike random
dithering. Used for the maze walls (distance shading) and the floor gradient.

Tips:
- Keep edges solid (draw the top/bottom of a wall as full lines) so shapes
  stay readable through the pattern.
- Error-diffusion (Floyd–Steinberg) looks better for photos and clip
  thumbnails, but it shimmers on moving images. Use it for still images only.

## 4. Text

`Canvas.text(x, y, s, scale=n)` draws a 5 × 7 font, scaled by whole pixels.
Scale 1 gives about 85 characters across both screens, scale 5 gives ~17 large
letters. The demo scroller moves each letter on a sine wave and clears a box
behind it so stars do not show through.

## 5. Input for games

`mk2_fun.Input` gives each mode:
- `pressed`: names that went down since the last frame (buttons and pads).
  Use this for turn-based moves (snake) so a tap is never lost between frames.
- `held`: names held now (buttons, and pads above the hit threshold).
- `pads`: pressure 0..4095 per pad. Good for analog control: in the maze,
  pressing harder walks faster.
- `knobs`: absolute knob values (demo: Knob 1 = speed).

The d-pad uses pads 10 / 5 / 6 / 7 (up / left / down / right). Modes can light
their control pads with `pad_leds()`; the live view turns off pressure
feedback while a mode owns the pad LEDs.

## 6. The three modes

- **Demo**: parallax starfield (3 speeds), big wavy "HIMMEL" scroller panning
  across both screens, pad hits spawn rings (harder hit = bigger ring).
- **Snake**: 4-px grid, 128 × 14 cells. Speed rises as the snake grows. Top
  bar shows score and best.
- **3D maze**: an original raycaster (DDA grid stepping), 256 rays of 2 px each
  over a 100° field of view, wall height ∝ 1/distance with the fish-eye
  correction, distance and side shading by dithering, a pre-built dithered
  floor, a minimap in the top-left corner, and a checkerboard exit door.
- **✦ ReadyMedia (splash)**: "Made by ReadyMedia.no" in a chrome italic look,
  animated on a 14-s loop (also played once when `mk2_live.py` starts):
  slides in on the left screen → glides over to the right screen → dissolves
  into a big logo across both → shine and sparkles → dissolves out.
  How: each logo size is pre-rendered once (`_Logo`) as a list of lit points
  `(x, y, rank)`. The text is drawn scaled, sheared for italics
  (x += (bottom − y) / 3), given a 1-px outline (dilate − letters), a dithered
  gradient fill, a chrome cut line and a swash underline. Moving = add an x
  offset to every point. Dissolve = draw a point only if its random `rank` <
  alpha, so two logos can cross-fade on a 1-bit screen. Shine = XOR a diagonal
  band over the outline points. Sparkles = short 4-point stars on outline
  points (pads add more). A timeline function (`_state`) returns which logo, at
  which offset and alpha, for the current time.
- **Star Swarm**: an original horizontal fixed shooter in the classic
  formation-and-dive genre. A 6 × 5 formation sways on the right; aliens peel
  off and swoop left along a sine path towards where you were, dropping bombs.
  The ship flies anywhere on the left screen: the d-pad (pads 10 / 5 / 6 / 7,
  pressure = speed) moves up / back / down / forward, Knob 1 (up/down) and
  Knob 2 (back/forward) work as paddles. The right-hand pad column (4, 8, 12,
  16) fires (held = auto-fire).
  New wave when the formation is cleared; dives get faster each wave.
- **Munch Maze**: an original maze-chase game. The maze is 64 × 8 tiles of 8 px,
  designed as a 32-wide half and mirrored, with a wrap-around tunnel on row 4.
  The player and three hunters move tile to tile and decide at tile centres.
  Hunter 1 chases the player, hunter 2 aims 4 tiles ahead, hunter 3 is 40 %
  random. Big dots scare the hunters for 6 s (they flee and can be eaten).
  Static walls are pre-drawn once; each frame copies that image and adds dots
  and sprites.

- **Sky Shield**: an original missile-defence game made for the MK2 layout.
  Eight turrets stand exactly under the eight physical knobs
  (`TURRET_X = 10, 87, 163, 240, 266, 343, 419, 496`), so **each knob aims the
  turret right below it** (±80°). Pads 1–8 fire turret 1–8, the top-row pads
  13–16 fire the turret you aimed last. **Shift + knob sets the range** of
  that turret; a crosshair on the active turret (and a small mark for the
  others) shows exactly where the shell will burst. The burst point is clamped
  to stay on screen (never above y = 10, never past the sides). Pad pressure
  does not change the range (it was too hard to control). Shells burst into a blast that destroys
  incoming missiles, and each kill bursts again (chain reactions). Six cities
  sit between the turrets. Missiles start above their target so the paths stay
  steep on the wide, low screen; from wave 3 some split in two. Ammo is shown as
  gaps in the ground line under each turret.

All games are our own designs in well-known genres. Do not copy a specific
commercial game (its name, characters, art, maze or levels): make new ones.

## 7. Add a new mode (recipe)

1. **Write a class** in `tools/mk2_fun.py`:

   ```python
   class MyMode:
       """One line on what it is and its controls."""

       def __init__(self):          # build static images here (walls, logos)
           self.t = 0.0

       def pad_leds(self):          # {pad number: (r, g, b)} or None
           return DPAD_LEDS         # None = pads glow by pressure

       def update(self, dt, inp):   # game logic only; dt in seconds (≤ 0.1)
           self.t += dt
           if "Play" in inp.pressed:
               ...                  # convention: Play restarts

       def draw(self, c):           # c is a 512 x 64 Canvas; draw everything
           c.clear()
           c.text(2, 0, "MY MODE")
   ```

2. **Register it** at the bottom of `mk2_fun.py`: add `"mine": MyMode` to
   `MODES` and `"mine": "MY MODE"` to `LABELS`. The live view and the master
   encoder menu pick it up automatically.
3. **Add a button** in `tools/mk2_live.html` inside `#scrFun`:
   `<button data-mode="mine">My mode</button>`.
4. **Check it without hardware**: `python tools/mk2_fun_check.py mine --png`.
   It runs 600 frames with random input, prints the frame time and writes
   `tools/_screens/mine.png` so you can look at a frame. Keep the average well
   under ~15 ms.
5. **Try it on the MK2**: restart MK2 Live (`mk2-live` in the browser pane, or
   `python tools/mk2_live.py`) and pick the mode.

Toolbox (all in `mk2_screens.Canvas` unless noted):

| Need | Use |
|---|---|
| pixel / line / box | `pixel`, `hline`, `vline`, `rect`, `fill`, `invert` |
| text, also big | `text(x, y, s, on=True, scale=n)`, `text_width(s, scale)` (upper + lower case) |
| sprites | `blit(x, y, ["..#..", ".###."])` – strings, `#` = pixel |
| grey levels | brightness `v` lit if `v > BAYER[y & 3][x & 3]` |
| fast pixels in loops | write `c.buf` directly: `buf[y*c.rb + (x>>3)] |= 0x80 >> (x&7)` |
| static background | build a Canvas once in `__init__`, keep `bytes(c.buf)`, and `c.buf[:] = that` each frame |

### Knobs and the master encoder

- **The 8 knobs belong to the mode.** They sit right under the screens, 4 per
  screen, at about x = 10, 87, 163, 240 (left) and 266, 343, 419, 496 (right)
  on the 512-wide canvas. Put whatever a knob controls *above that knob*
  (Sky Shield's turrets, a parameter label, a meter).
- Knob values are absolute and wrap, so use deltas:

  ```python
  self.knobs = KnobTracker()                  # in __init__
  for name, d in self.knobs.deltas(inp).items():   # in update()
      if name == "Knob 3":
          self.speed += d * 0.01
  ```

- **The master encoder belongs to the system**: **Shift + turn** opens the
  mode menu on the MK2 screens (on top of any mode), turning chooses, pushing
  starts the highlighted mode. The menu closes by itself after 4 s. Without
  Shift the encoder is free; in Himmel it is the browse/select wheel. Modes
  should not use the master encoder. The order of the menu is `MENU` in `mk2_fun.py`, the
  names are in `LABELS`; a new mode needs an entry in both.

Conventions that keep the modes consistent:
- **Controls**: d-pad = pads 10 / 5 / 6 / 7 (up / left / down / right), fire =
  the right pad column (4, 8, 12, 16), Shift + knob = a second function for
  that knob, Play = restart, Knob 1 = speed or paddle.
  Use `inp.pressed` for taps, `inp.held` / `inp.pads` for holding and pressure.
- **Knobs** are absolute 0..1000 and wrap: use the delta and fix the wrap
  (`d -= 1000 if d > 500`, `d += 1000 if d < -500`).
- **HUD**: a 7-px bar on the top row (inverted text) or small text at the top.
- **Messages** ("GAME OVER", "READY!"): scale-2 text in a cleared, outlined box
  in the middle.
- **Pad 12 is worn** (rests at ~136): never use pad pressure below ~300 as
  "pressed".
- **Never** block, sleep or do file I/O in `update`/`draw`; they run in the
  screen thread at ~30 fps.

## 8. What about running Doom?

Technically possible: the open-source Doom ports (for example the
"doomgeneric" style ports) only need a framebuffer and key input. You would
scale the 320 × 200 frame to 512 × 64 (or crop to a 4:1 slice), convert to 1
bit with ordered dithering, and map pads to keys. The USB budget allows
~30 fps.

Recommended **not** to put it in Himmel: the engine is GPL (would affect how
Himmel can be licensed), it needs the game data files, and it adds nothing to a
VJ tool. An original raycaster like the maze shows the same thing with our own
code.

## 9. Ideas that are useful in Himmel itself

- **Beat-synced screen visuals**: the same scroller/starfield driven by BPM and
  audio bands, as an idle screensaver or "show mode" on the controller.
- **Clip thumbnails**: the current clip, dithered, with the name over it.
- **VU / spectrum** across both screens (bars are cheap and change few strips).
- **Smart Random feedback**: a short animation when a new preset is picked.
- **Wide parameter page**: 8 knob labels placed over the real knob positions
  (see CLAUDE.md: knob centres ≈ x 10, 87, 163, 240 per screen).
