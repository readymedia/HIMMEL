# The effects in the NuVJ software (and what Himmel can learn from them)

The NuVJ controller came with the ArKaos NuVJ 1.0 software. Its manuals only
say there is "a wide collection of effects sorted in different categories";
they do not list them. We read the list from the owner's copy of the software:
- 40 effect definitions (`FXFiles/*.xml`), each with HLSL and Cg shaders;
- 29 generator definitions (`GENFiles/*.xml`);
- the effects compiled into `NuVJ.exe` (found by their class names and
  parameter labels);
- the mixer options from the reference manual.

The program was never run. Below, every effect is described **in our own
words**, from its name, parameters and a reading of its shader. No shader code
is copied into Himmel. Sources are in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

**How effects were used in NuVJ**
- **Cell effect:** every cell (clip) can carry one effect. The deck's
  **PMTR 1 / PMTR 2** knobs turn its parameters. When an effect has more than
  two parameters, the knobs most likely drive the first two.
- **Master effect:** one effect on the mixed output.
  - The **MASTER FX** knob (endless) chooses it.
  - The **MASTER FX** button turns it on and off.
  - **FX LEVEL** sets wet/dry.
  - Master **PMTR 1 / 2** set its parameters.
- Parameters marked *(incremental)* keep running: the knob sets a **speed**,
  not a position (rotation, scrolling, zooming).

**The last column** says what each effect could become in Himmel. We prefer
cheap single-pass shaders (goal 4: light on the GPU), and multi-pass ones only
where they are worth it.

## Shader effects (40, from `FXFiles`)

### Blur
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Gaussian Blur | Level | A soft, even blur in several passes. | Core effect: a separable blur at half resolution. |
| Directional Blur | Level, Direction | Smears the image along one angle, like motion. | Core: 1 pass, 8–16 taps along a vector. |
| Radial Blur | Level | A zoom blur out from the centre ("warp speed"). | Core: 1 pass, taps toward the centre. |

### Color
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Greyscale | – | Black and white. | Part of a colour-grade effect. |
| Invert | – | Negative image. | Toggle on the colour effect. |
| Sepiatone | – | Old-photo brown tint. | Colour-grade preset. |
| Hue Effect | Hue, Saturation | Rotates all colours around the colour wheel and changes saturation. | Core: HSV rotate. A good knob for themes. |
| Posterize | Level | Cuts the image into a few flat colour steps. | Single pass. |
| Solarize | Level, Invert | Inverts the brightest (or darkest) tones, for a darkroom look. | Single pass. |
| Color Distortion | Curve, Mode | Wraps the colours many times over (`frac` or `sin` of colour × 1..40). Psychedelic bands; colour or mono. | Single pass. Fun on BPM-driven curves. |

### Artistic
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Ascii Art | Texture, Textures Size, Transparency | Breaks the picture into cells. Each cell's brightness picks a tile from a set: ASCII letters, squares, pixels, bubbles. The original can show through. | A "mosaic of glyphs" shader with our own glyph atlas. |
| Pop Art | Texture Offset, Textures Size, Texture | Replaces brightness with a coloured pattern from a 3-D texture, a Warhol-like look. Offset cycles the colours. | Our own palette texture, single pass. |
| Cartoon | Filter Size, Edge Size, Threshold, Color Reduction | Smooths, reduces colours and draws dark outlines: a cel-shaded look. | Multi-pass (7). Only as a "heavy" effect. |
| Edge Detect | Edge Size, Threshold | Only the outlines of shapes, as lines. | Single-pass Sobel. |
| Edge Filter | Edge Size, Level | Outlines mixed back over the image (sharpen / emboss feel). | Single pass. |
| Glow | Threshold, Glow Size | Bright parts bloom and bleed light. | Bloom at quarter resolution. Popular. |
| Bumpy Surface | Light Pos X/Y/Z, Mixing factor | Treats brightness as a relief and lights it from a movable lamp: embossed metal. | Single-pass normal-from-luma. |
| Irisation | Offset, Rotation | Pulls the red, green and blue channels apart in different directions: chromatic split. | Core "RGB split". Cheap, and great on the beat. |
| Larsen Presets | Preset, Size, Transparence | Video feedback, the image inside itself with a border, like pointing a camera at its own screen. | Feedback buffer (one extra texture). |
| Digital Noiz | FX Preset | Film / video grain over the image, in colour or mono. | Single-pass noise. |
| Color Masks | Speed | An animated mask over the colours (an "anim" shape that moves with Speed). | Hard to tell exactly; we would make our own animated colour wipes. |
| Strobo | Speed *(incremental)*, Mode | Flashes the image to white or black, or inverts it, at a set rate. | Core. Sync it to the BPM in Himmel. |
| Threshold | Threshold | Pure black and white at a cut-off level: hard graphic look. | Single pass. Good for masks. |

### Split
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Mirror | Horizontal Pos, Vertical Pos | Mirrors the image around a movable cross: 2 or 4 symmetric parts. | Core, cheap. |
| Checker | Split, Offset | Cuts the image into a checkerboard and shifts every other square sideways. | Single pass. |
| Tiling | Size | Repeats the image as a grid of small copies. | Core. Pairs with a "repeat count" knob. |
| Video Split | Horizontal, Vertical | Splits the image into strips or tiles (repeat in x / y). | Same shader as Tiling, two axes. |
| Scroller | Speed X / Speed Y *(incremental)*, Altitude | The image scrolls endlessly and wraps around. | Core: UV offset over time. |

### 3D
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Tunnel / Tunnel 2 | Speed *(incremental)*, Tiling, Transparency, Curve, Light Position *(incremental)* | Flies through a tube lined with the video. Curve bends the tube; a light moves along it. | A single-pass polar-UV tunnel. Classic VJ look. |
| Plane | Speed, Rotation *(incremental)*, Altitude, Tiling | The video as an endless floor seen in perspective, sliding and turning (a Mode 7 look). | Single pass. |
| Plane Rotation | Rotation Y *(incremental)*, Tiling | A flat card with the video spinning around its vertical axis. | Our quad transform: almost free. |
| Cube Inside | Rotation X / Y *(incremental)*, Tiling | The camera sits inside a cube whose walls show the video, slowly turning. | Single pass (ray-box). |
| Sphere Inside | Rotation X / Y *(incremental)*, Tiling | The same, inside a sphere. | Single pass (equirect lookup). |
| Ripple | Wave Level, Wave Size, Wave Speed *(incremental)* | Water ripples move over the image. | Single pass. |
| RotoZoom | Zoom Speed, Rotation Speed *(both incremental)* | Endless zoom and spin of a tiled image: demo-scene rotozoomer. | Single pass. |

### Sound controlled
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Bass Zoom / Bass Zoom 2 | Release | The picture punches in on every bass hit and falls back at the Release speed. Two variants of the movement. | Core. Himmel's audio analysis (bass band + onset) drives the zoom. |

### Animations
| Effect | Parameters | What it does | In Himmel |
|---|---|---|---|
| Zoom Out | Speed | The picture shrinks away into the distance and repeats (an "anim" shape). | A preset of our transform effect. |

## Effects compiled into the program (15)

These have no XML file. We know them by name and by their parameter labels.

| Effect | Parameters | What it most likely does | In Himmel |
|---|---|---|---|
| Comic | Speed, BPM Sync On/Off | Comic-book look: flat colours and halftone, animated, optionally in time with the BPM. | Posterize + halftone dots, single pass. |
| Larsen | Rotation, Size | Simple video feedback that turns and scales each frame into the next: a spiral tunnel. | Feedback buffer. |
| EarthQuake | Update Speed | Shakes the picture in jumps (camera shake). | Transform jitter, almost free. Lovely on onsets. |
| Mirror | X, Y | The built-in variant of Mirror. | See Mirror. |
| Irisation | Offset | The built-in RGB split. | See Irisation. |
| Split Scrolling | Mode | Strips of the picture scroll in opposite directions. | Single pass. |
| Kaleido | Divisions | Kaleidoscope with N mirrored wedges. | Core. A favourite. |
| RotoZoom | Zoom Speed, Rotation Speed | The built-in rotozoomer. | See above. |
| Twirl Feedback | Blur level | Feedback that swirls around the centre and blurs: a smoky vortex. | Feedback buffer + rotate + blur. |
| Tiling | Level | The built-in tiling. | See Tiling. |
| Stroboscope | Hard/soft | Strobe with hard flashes or soft pulses. | See Strobo. |
| RGB Cycle | Pattern | The colour channels swap and cycle in patterns. | Single pass, BPM-driven. |
| Strips | – | Cuts the image into strips that shift or flicker. | Single pass. |
| Invert | Burn | Negative, with a "burn" control. | Part of the colour effect. |
| Motion Blur | – | Blends each frame with the last ones: trails. | Feedback buffer with decay. Cheap. |

Icons also exist for **Contrast**, **Cropping**, **Fish Eye** and
**Larsen Presets Borders**. We found no code for them, so they may be unused.

## Generators (29 "Live Generators", from `GENFiles`)

Generators make pictures by themselves. Most of them listen to the audio input
(bass, mid, high, volume, beat).

They are built from a few modules:
- `ShapeAudio`: a waveform or spectrum as a line or a filled shape;
- `ParticleAudio` / `AnimatedParticles`: sprites;
- `Spectrum3D`: a spectrum as a 3-D grid;
- `Deformation`: whirl, expand, rotate;
- `Fade`: trails;
- `Volume`: a textured 3-D volume;
- `AnimatedPlane`;
- `DistortedShape`;
- `BeatStrob`;
- cameras that rotate, zoom or move on the beat.

The `GENERATORS_HOWTO.txt` in the software explains the format.

| Generator | What it shows |
|---|---|
| Simple Waveform | The sound's waveform as a line. |
| Waveform 2D Filled | A filled waveform. |
| Spectrum 2D Filled | A filled frequency spectrum. |
| n0s Spectrum | Several spectrum layers, deformed and stacked. |
| Bass Scope | Two audio shapes with trails that pulse with the volume. |
| Rolling Waveforms | Four waveforms rolling in 3-D, with trails. |
| Animated Spectrum | The spectrum as a 3-D landscape on a moving plane. |
| Waveform 2D Filled In Cube | A filled waveform, bent into a deformed cube. |
| Cube | A 3-D cube built from the audio shape, with trails. |
| Distorted Shape 1 / 2 | A blob that wobbles with the sound, with trails. |
| Kaleido Combo | A 3-D spectrum on a plane, then kaleidoscoped. |
| Laser Light | Audio lines as laser beams with long glowing trails. |
| Galactic Ring | Two rings of particles that pulse with the music. |
| Bumper Dots | Dots that jump with the audio. |
| Animated Balls | Bouncing balls (particles) with an effect and trails. |
| Particles Field | A field of square particles flying past. |
| Crypted Particles | Two layers of grid-textured particles: a "data" look. |
| OVNI | A UFO-like pulsing object of particles, with a camera on the beat. |
| Pulsating Ball | A glowing ball that breathes with the volume. |
| Pulsating Square / 3 Pulsating Square | One or three textured squares pulsing, with trails. |
| Polka Dots | Layers of dots in 3-D, with trails. |
| Sixties Shapes | Psychedelic 60s shapes in three layers, with trails. |
| Moire | Two zebra patterns over each other: moiré interference that moves with the audio. |
| Liquid Texture | A flowing texture in 3-D layers. |
| Color Feed | Colour fields fed back into themselves, slowly changing. |
| Speed Ball | Audio shapes on moving planes, with trails: a busy, fast look. |
| Beat Strob | White flashes on the beat. |

## The mixer (from the reference manual and the program)

**Copy modes:** how deck B is blended onto deck A. The crossfader sets the
amount.
- **Copy:** an average of A and B.
- **Add:** brighter where either is bright.
- **Subtract:** B taken away from A.
- **Multiply:** only where both are bright.
- **Maximum:** the brighter pixel of A or B (by luminance).
- **Minimum:** the darker pixel.

**Masking (keying) on deck B**
- **Luminance key** or **chrominance (colour) key**.
- Two knobs set the **centre** and the **width** of the brightness or hue range
  that turns transparent, so A shows through.

**Transitions (crossfade shapes)**
- Linear, exponential and smooth-exponential transparency.
- Rectangular and circular masks, hard or smooth.

**Playback:** one shot, loop or ping-pong. A **background** visual sits under
deck A.

**Output**
- Brightness and contrast on the master.
- **INSTA BLACK:** black, or a chosen background.
- **INSTA A / B:** cut to one deck with an auto-fade speed.
- **Fader start:** moving the crossfader starts the other deck's clip.

**Audio:** bass, mid, high, volume, "bass pulse" and BPM (from the audio input
or tapped).

## What Himmel takes from this

1. **A first effect set:**
   - one cheap pass each: Hue / colour grade, Mirror, Kaleido, Tiling,
     RGB split (Irisation), Strobe (BPM), Tunnel, RotoZoom, Ripple, Edge,
     Posterize, Threshold, Scroller;
   - with a feedback buffer: Motion Blur, Larsen / Twirl feedback;
   - audio: Bass Zoom and EarthQuake, both driven by the onset / bass band.
2. **Two parameters per effect on two knobs, and incremental "speed"
   parameters.**
   - This is exactly the NuVJ's PMTR 1 / 2 and the MK2's knob pages.
   - Every Himmel effect should name its two "main" parameters, and say which
     ones are speeds.
3. **Mixer:**
   - copy modes and luma / chroma key on the deck mixer;
   - crossfade curves and mask wipes as transitions.
4. **Generators:** audio-reactive, built from a few modules (shapes,
   particles, trails, cameras on the beat). That matches Himmel's plan for
   cheap generators.

All of these are written fresh for Himmel, in HLSL or ISF.
