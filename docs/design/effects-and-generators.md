# Effects and generators reference

Status: the working list, 2026-11-11. It merges the "Effects and generators"
tab from the voice call, the 42-effect catalogue from design spec v1, the
Touch FX set that is already built, and the NuVJ / V-4 / Entrancer catalogues
(`docs/nuvj-effects.md`, `docs/v4-effects.md`, `docs/touch-fx-entrancer.md`
on their branches).

- **Effects** change a picture that already exists: a clip, a camera, a USB
  capture input, a layer, a deck, the master or an output.
- **Generators** make a picture by themselves, live.
- **Every effect works on every source,** USB inputs included.

**Naming rule:** common words (strobe, blur, mirror, kaleido) are fine. Any
name that belongs to one product gets a Himmel name of its own. The "Seen in"
column is credit only. Every effect is written fresh for Himmel.

## 1. What every effect declares (metadata)

The shuffle, the macros, the GUI and the help system all read this. It lives
in one JSON file per effect, next to the shader.

| Field | Example | Used by |
|---|---|---|
| `id`, `name`, `family` | `kaleido`, "Kaleido", `space` | Browser, shuffle family weights |
| `params[]` | `{id, name, min, max, default, good:[lo,hi], speed:true/false, unit}` | Knobs 1–4, shuffle ranges |
| `main` | the two main parameters (knobs 1–2; PMTR 1 / 2 on the NuVJ; X / Y on the Lightpad) | Every weapon |
| `reaction` | `{signal:"kick", param:"zoom", amount:0.4, attack:5, release:180, mode:"add"}` | The default music reaction. **Every effect must have one** (D88), and every parameter can get its own (`music.md` §4) |
| `cost` | class `S` / `M` / `L` + the ms measured at Prepare | Budget, browser dimming |
| `passes`, `state` | 1, `none` / `feedback` / `history` | Graph, off-air shrink |
| `compat` | `{avoid:["dark-source"], not_with:["kaleido"]}` | Shuffle rules |
| `presets[]` | named or unnamed parameter sets, incl. `subtle` and `extreme` | `SHIFT + pad`, good ranges, FX-pad velocity |
| `press` | `{param:"rate", lo:0.25, hi:0.03125, curve:"exp", release_beats:0.5}` | Pad velocity / pressure, Block Z, LUMI pressure (`clips-and-playback.md` §8.2) |
| `scopes` | which params may be LOOK / GLOBAL / MOVE (default: all) | Parameter scope badges (`clips-and-playback.md` §4.2) |
| `tier`, `shelf` | 1 / 2 / 3, `builtin` / `addon` | Roadmap, safety |
| `help` | two lines | The MK2 screen and the Help menu |

The knob layout on every FX page: **1–4 parameters (1–2 = the main ones),
5 MIX, 6 AUDIO, 7 SYNC, 8 VARIATION.**

**Control levels.** Every effect can sit on a clip, a layer, a deck, the
master or an output, and every parameter value can be fixed in its slot,
different per look / scene, shared globally, or moved by a modulator. Presets
and the dice work on whichever level is selected, and everything is tried on
PREVIEW first. The full model is in
[`clips-and-playback.md`](clips-and-playback.md) §4–5.

## 2. Effects

Tier 1 = the first set (phase 4), tier 2 = soon after, tier 3 = later. The
cost classes are S < 0.15 ms, M < 0.6 ms and L above that, at 1080p on a
GTX 1070. A ✓ marks the effects already built as Touch FX.

| Effect | Family | Knob 1 / Knob 2 | Default reaction | Cost | Tier | Seen in |
|---|---|---|---|---|---|---|
| Hue / saturation ✓(Hue Turn) | Colour | Hue turn / saturation | mid → hue | S | 1 | All |
| Brightness / contrast | Colour | Brightness / contrast | — | S | 1 | All |
| Colorize / duotone ✓ | Colour | Dark colour / light colour | — | S | 1 | V-4, Entrancer |
| Invert / negative | Colour | Amount / channel | snare → flash | S | 1 | V-4, NuVJ |
| Posterize | Colour | Levels / smoothing | energy → levels | S | 1 | V-4, NuVJ |
| Threshold | Colour | Level / softness | — | S | 1 | NuVJ |
| Palette map (to the mood palette) | Colour | Strength / shift | beat → shift | S | 1 | — |
| Colour pass (keep one hue) | Colour | Kept hue / width | — | S | 2 | V-4 |
| Film look (LUTs) | Colour | Look / strength | — | S | 2 | — |
| Levels / gamma | Colour | Black–white points / gamma | — | S | 1 | All |
| Tint / colour balance | Colour | Shadows–highlights tint / amount | — | S | 1 | Modul8, Resolume |
| Gradient map | Colour | Gradient / mix | beat → shift | S | 2 | — |
| Mirror | Space | Axis / position | — | S | 1 | V-4, NuVJ |
| Kaleido ✓ | Space | Segments / turn | mid → turn | S | 1 | NuVJ, Entrancer |
| Tile / multi ✓(Grid) | Space | Tiles / mirror | beat → tiles | S–M | 1 | V-4, NuVJ, Entrancer |
| Transform / rotozoom | Space | Zoom / rotation speed | — | S | 1 | NuVJ |
| Zoom pulse | Space | Amount / centre | kick | S | 1 | — |
| Slide / scroll ✓ | Space | Shift X / shift Y | — | S | 1 | NuVJ, Entrancer |
| Tunnel | Space | Depth / speed | kick → speed | S | 2 | NuVJ |
| Lens (fisheye / sphere) | Space | Strength / radius | — | S | 2 | — |
| Flip / rotate 90 | Space | Axis / step | beat → flip | S | 1 | All |
| 3D turn (perspective) | Space | Turn X / turn Y | — | S | 1 | Resolume, Modul8 |
| Polar warp | Space | Amount / turn | — | S | 2 | — |
| Radial copies | Space | Copies / spread | beat → copies | S | 2 | — |
| Infinite zoom (picture in itself) | Space | Zoom speed / copies | kick → speed | M | 3 | — |
| Wave / ripple ✓(Waves) | Distort | Frequency / height | bass → height | S | 1 | NuVJ |
| Bulge / pinch ✓ | Distort | Strength / centre | — | S | 1 | Entrancer |
| Shake | Distort | Amount / speed | kick | S | 1 | V-4, NuVJ |
| Displace | Distort | Amount / scale | bass | S | 2 | — |
| Twirl | Distort | Angle / radius | mid → angle | S | 1 | — |
| Shatter / fragments | Distort | Pieces / spread | drop → spread | M | 3 | — |
| Blur | Blur | Size / direction | — | M | 1 | — |
| Zoom / radial blur | Blur | Amount / centre | kick → amount | M | 2 | — |
| Glow / bloom | Light | Amount / threshold | energy | M | 1 | — |
| Strobe ✓ | Light | Beat division / colour | kick | S | 1 | All |
| Flash | Light | Strength / fade | drop | S | 1 | — |
| Light rays | Light | Amount / length | bass | M | 2 | — |
| Spot / vignette | Light | Size / pulse | kick → pulse | S | 2 | — |
| Glint (star sparkles) | Light | Amount / rays | hi-hat | M | 3 | — |
| Light leak | Light | Amount / colour | — | S | 2 | — |
| Drop shadow / outline (alpha) | Light | Size / colour | — | S | 2 | — |
| Edge detect / neon edges | Stylize | Strength / colour | mid | S | 1 | NuVJ |
| Pixelate / mosaic | Stylize | Size / shape | kick → size | S | 1 | — |
| Silhouette | Stylize | Threshold / colour | — | S | 2 | — |
| Halftone dots | Stylize | Size / angle | — | S | 2 | — |
| CRT / scanlines | Stylize | Lines / curve | — | S | 2 | — |
| Sharpen | Stylize | Amount / radius | — | S | 2 | — |
| Dither (1-bit / few colours) | Stylize | Levels / pattern | — | S | 2 | — |
| Emboss / relief | Stylize | Depth / angle | — | S | 3 | — |
| Toon (flat colours + lines) | Stylize | Levels / line | — | M | 3 | — |
| Text mosaic (ASCII-style) | Stylize | Size / charset | — | M | 3 | — |
| RGB split | Glitch | Distance / angle | kick → distance | S | 1 | NuVJ |
| Glitch blocks | Glitch | Amount / block size | snare | S | 2 | — |
| Tape noise (VHS-style) | Glitch | Noise / tracking | — | S | 2 | — |
| Digital grain | Glitch | Amount / size | hi-hat | S | 2 | — |
| Pixel sort | Glitch | Threshold / length | energy | L | 3 | — |
| Smear (datamosh-style) | Glitch | Amount / hold | drop → reset | M | 3 | — |
| Freeze / still | Time | Hold / release | — | S | 1 | V-4 |
| Freeze on beat | Time | Length / probability | kick | S | 2 | — |
| Stutter | Time | Length / repeats | hi-hat → trigger | S | 2 | Entrancer |
| Echo | Time | Count / spacing in beats | beat | M | 2 | — |
| Slitscan | Time | Direction / time span | bass → span | M | 3 | — |
| Echo grid | Time | Delay / tiles | — | M | 3 | Entrancer |
| Beat loop / loop roll | Time | Length in beats / catch-up | hi-hat → length | S | 1 | Resolume |
| Reverse (momentary) | Time | — / catch-up | — | S | 1 | All |
| Step (stop-motion) | Time | Frames per step / division | beat | S | 1 | — |
| Loop seam (crossfade the loop point) | Time | Frames / curve | — | S | 1 | — |
| Frame blend (smooth slow motion) | Time | Amount / — | — | S | 2 | — |
| Time ghost (delay by brightness) | Time | Depth / direction | bass | M | 3 | — |
| Trails / motion blur ✓ | Feedback | Decay / drift | — | S | 1 | NuVJ, Entrancer |
| Spiral feedback | Feedback | Zoom / twist | — | S | 2 | NuVJ |
| Bass punch | Audio | Amount / band | bass | S | 2 | NuVJ |
| Audio wobble | Audio | Amount / band | mid | S | 2 | NuVJ |
| Luma key | Key | Level / softness | — | S | 1 | NuVJ, V-4 |
| Chroma key | Key | Colour / tolerance | — | S | 1 | NuVJ, V-4 |
| Mask (shape / clip) | Key | Shape / feather | beat → size | S | 2 | — |
| Luma to alpha | Key | Level / softness | — | S | 1 | Resolume |
| Iris (shape reveal) | Key | Size / shape | kick → size | S | 2 | — |
| Crop / edge fade | Utility | Edges / softness | — | S | 1 | All |
| Motion trigger | Camera | Sensitivity / what it fires | — | S | 2 | — |
| Person cut-out (tracking) | Camera | Threshold / background | — | M | 3 | — |
| Camera echo | Camera | Length / fade | kick | S | 2 | — |
| Camera mosaic (3 cams) | Camera | Layout / switch on beat | beat | S | 2 | — |
| Hands → particles (tracking) | Tracking | Amount / colour | onset | M | 3 | — |
| Body glow (tracking) | Tracking | Width / glow | energy | M | 3 | — |

The **Time** effects marked tier 1 are the transport helpers that make any
clip loop well; they are also play modes and pad gestures
([`clips-and-playback.md`](clips-and-playback.md) §2, §8).

Mixing between layers is not an effect but part of the layer:
- blend modes (normal, add, screen, multiply, difference, lighten, luma key,
  chroma key);
- crossfade curves and wipe shapes (from the V-4 catalogue).

## 3. Generators

| Generator | Family | Knob 1 / Knob 2 | Default reaction | Tier | Seen in |
|---|---|---|---|---|---|
| Solid colour / gradient | Basic | Colour / angle | beat → shift | 1 | All |
| Test card | Basic | Pattern / grid | — | 1 | Himmel (exists) |
| Noise / fBm clouds | Texture | Scale / speed | bass → speed | 1 | — |
| Plasma | Texture | Scale / speed | mid | 1 | — |
| Lines and stripes | Pattern | Count / speed | kick | 1 | — |
| Rings / circles | Pattern | Count / pulse | kick → pulse | 1 | — |
| Shapes (polygons) | Pattern | Sides / spin | beat | 1 | NuVJ |
| Checker / grid | Pattern | Size / scroll | — | 1 | — |
| Starfield | Motion | Density / speed | energy → speed | 1 | Classic |
| Particles | Motion | Amount / gravity | onset → burst | 2 | NuVJ |
| Voronoi cells | Texture | Cells / speed | — | 2 | — |
| Audio spectrum / bars | Audio | Bands / height | spectrum | 1 | NuVJ |
| Waveform / oscilloscope | Audio | Zoom / thickness | waveform | 2 | — |
| Beat pulse / flash | Audio | Beat division / colour | beat | 1 | NuVJ |
| Text / scrolling text | Text | Size / speed | — | 2 | — |
| Tunnel / wormhole | 3D look | Speed / twist | kick → speed | 2 | Classic |
| Fractal zoom | Math | Zoom / colour | — | 3 | Classic |
| Raymarched shapes | 3D look | Shape / morph | bass → morph | 3 | — |
| Reaction-diffusion | Texture | Feed / speed | bass | 3 | — |
| Feedback source (PROGRAM fed back in) | Feedback | Zoom / turn | — | 2 | Resolume |
| Light beams / searchlights | Light | Beams / sweep | beat → sweep | 2 | Resolume |
| Live sample (recorded clip) | Source | Transport | — | 2 | Resolume, VDMX |
| ISF shader (user, add-on) | Any | Defined by the shader | — | 2 | ISF standard |

Sources that are neither effects nor generators: clips (HAP), USB cameras,
capture cards, Spout / NDI inputs and the VJ table output. Effects work on all
of them the same way.

## 4. FX pads (momentary, on LIVE)

The default set for `PAD MODE`: velocity = punch and picks the variation
(soft = the `subtle` preset, hard = `extreme`), aftertouch = the effect's
`press` target, `SHIFT` + pad = latch. Replaced per mood. Details:
[`clips-and-playback.md`](clips-and-playback.md) §8.5.

> Strobe · Flash · Stutter · Freeze · Invert · RGB split · Zoom pulse ·
> Kaleido · Mirror · Shake · Tunnel · Blur · Glitch blocks · Echo ·
> Black-out · White-out

## 5. Text generator

The text generator is a source like a clip. Dice, presets and reactions work
on it.

| Part | Options |
|---|---|
| **Text sources** | A typed list; a .txt / .csv file; the DJ's track title and artist (Pro DJ Link); BPM, clock, countdown; messages from a phone (a local web page served by Himmel, with moderation: approve before showing); OSC |
| **Styles** | Ticker (scroll), one word per beat, typewriter, kinetic stack, big centred, outline, a clip inside the letters (text as a mask), glitch letters, karaoke line, countdown / clock |
| **Look** | Font (bundled open fonts, credited), size, tracking, fill (colour / gradient / clip), outline, shadow / glow, case |
| **Place** | Like any layer: FILL ALL (across all outputs), OUT n, FIT, FREE; a ticker can run across the whole superwide canvas |
| **Reactions** | Word on the beat, flash on the kick, wobble on bass, speed on BPM |
| **Pads** | On a text layer, the CLIPS pads = 16 lines of text; tap = show that line |
| **Cost** | Glyphs from a signed-distance-field atlas built at Prepare time: one cheap pass, sharp at any size |
