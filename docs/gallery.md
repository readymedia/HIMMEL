# Gallery

Screenshots of what Himmel looks like today, and of the parts that are still
only design mockups. Taken on 2026-11-11.

- **Weapon simulators** are real, working tools: the browser live view
  (`tools/mk2_live.py`) draws each controller from its profile and lets you
  play it with the mouse. The same page shows the real hardware when it is
  plugged in. How it works: [weapons/README.md](../weapons/README.md).
- **GUI mockups** are design sketches of the planned operator GUI. They are
  not running code yet.

The panels are Himmel's own drawings. They carry a "HIMMEL · ..." badge
instead of the maker's logo, and there are no photos of the real hardware.

## Weapons (controllers)

### Maschine MK2: weapon no. 1, the main instrument

![MK2 simulator](images/weapon-mk2.png)

Knob page on the two 1-bit screens, pads lit by pressure, Group A held.
Verified on the owner's MK2; `himmel.exe --sim` plays the engine from this
page.

### ROLI Lightpad Block: weapon no. 2

![Lightpad simulator](images/weapon-lightpad.png)

The 15 × 15 LED surface in rainbow mode. Multi-touch with pressure; Himmel
uploads its own LED renderer to the block.

### ROLI LUMI Keys: weapon no. 3

![LUMI simulator](images/weapon-lumi.png)

24 keys with an RGB LED per key, driven by Himmel.

### Numark NuVJ: weapon no. 5

![NuVJ simulator](images/weapon-nuvj.png)

Two decks, crossfader and jogs. Himmel writes the LCD text and LEDs; checked
on the owner's unit.

The NuVJ also has a **classic two-deck view** that the GUI and the controller
play together (preview pictures are 2D placeholders until the engine renders
them):

![Classic decks view](images/vj-classic-decks.png)

### Akai APC20: weapon no. 9, a secondary weapon

![APC20 simulator](images/weapon-apc20.png)

Clip launch grid, scene buttons and faders. Simulated only so far (the unit
needs a power adapter the owner does not have yet).

### Work in progress on other branches

These weapons are drawn and simulated on their own branches and are not
merged yet.

| Korg KF4 (no. 6) | Roland V-4 (no. 7) |
|:-:|:-:|
| ![KF4 simulator](images/weapon-kf4.png) | ![V-4 simulator](images/weapon-v4.png) |
| A video mixer with no data port: Himmel simulates its picture | 4-channel video mixer over MIDI |

| Livid Ohm64 (no. 8) | Entrancer touch FX (no. 4) |
|:-:|:-:|
| ![Ohm64 simulator](images/weapon-ohm64.png) | ![Entrancer simulator](images/weapon-entrancer.png) |
| 64-button grid, knobs and faders, all LEDs on | Touch-pad effects box; a Lightpad can be its pad |

## Operator GUI (mockups)

The planned operator GUI, in English, with dark, light and night themes.
Source files: [`docs/gui-mockups/`](gui-mockups/).

### Perform

| Dark | Light | Night |
|:-:|:-:|:-:|
| ![Perform, dark](images/gui-perform-dark.png) | ![Perform, light](images/gui-perform-light.png) | ![Perform, night](images/gui-perform-night.png) |

Setlist and cues, program and preview, outputs, scenes, layers with knob
rings, audio and sync, and the Smart Random / Safe Scene assist.

### Setup

| Outputs | Projection mapping |
|:-:|:-:|
| ![Setup outputs](images/gui-setup-outputs.png) | ![Setup mapping](images/gui-setup-mapping.png) |

### Prepare

| Library | Setlist and cues |
|:-:|:-:|
| ![Library](images/gui-library.png) | ![Setlist](images/gui-setlist.png) |

### Effects and help

| Effect browser | Help |
|:-:|:-:|
| ![Effect browser](images/gui-effect-browser.png) | ![Help](images/gui-help.png) |

## Other mockups

### MK2 and the three-screen wall

![MK2 and 3-screen wall mockup](images/mockup-mk2-wall.png)

The first interactive mockup: [`docs/vj-mockup.html`](vj-mockup.html).

### The VJ table (design proposal)

![VJ table mockup](images/mockup-vj-table.png)

The boxes you own, patched with cables on a table. This is a proposal on its
own branch; nothing in the engine does this yet.
