<h1 align="center">Card Craft</h1>

<p align="center"><b>Mine by day. Wall yourself in by night. Survive.</b></p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-M5Stack%20Cardputer-informational">
  <img alt="framework" src="https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-blue">
  <img alt="fps" src="https://img.shields.io/badge/frame%20rate-33%E2%80%9376%20fps-success">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-green">
</p>

<p align="center">
  <img src="docs/img/play.png" width="32%">
  <img src="docs/img/mobs.png" width="32%">
  <img src="docs/img/night.png" width="32%">
</p>

## The problem

A Minecraft clone on a microcontroller is usually a tech demo: a voxel renderer
that draws a world and stops there. It runs at fifteen frames a second, it has
no reason to be played twice, and the "game" is a block editor with no stakes.

The ESP32-S3 in a Cardputer has 240 MHz, no PSRAM, a 240x135 panel and about
320 KB of usable RAM. The panel is the hard limit: a full frame is 64,800 bytes
and the SPI bus runs at 40 MHz, so a frame takes **12.96 ms to transmit** no
matter what is in it. That is a hard ceiling of 77 fps, and it leaves roughly
13 ms of CPU per frame — but only if the CPU work overlaps the transfer.

## The solution

A first-person survival game with a real loop, running at **33 fps worst case
and 64 fps average**, measured on-device against a full twenty-four-mob wave.

**The loop.** Daylight is a countdown. You mine to gather blocks to build with
and ore to upgrade with, but ore only exists underground and in the open pits,
which are the last places you want to be at dusk. At night, mobs spawn at the
map edges and walk to you. Zombies press. Creepers detonate and blow a hole in
whatever you built, which is what stops walling yourself in from being a solved
strategy. Skeletons outrange you from night four. Survive to dawn and you spend
your ore on one of three upgrades. Score is nights survived; it persists to NVS.

**The world** is a 64x64 heightmap of one-metre cubes, three blocks deep and six
tall over a bedrock plane. You dig down as well as build up. Terrain is two
octaves of value noise, and on top of it go shapes noise cannot make: trees,
walled ruins you can shelter in, and open quarries cut past the stone line where
ore shows on the surface.

**The renderer** is a heightmap raycaster. A ray does not stop at the first solid
cell — it keeps stepping outward, and each cell contributes the side of its
column (split into one span per block, so a stack reads as stacked cubes) and
the top of its column seen from above. Spans are painted bottom-up while
tracking the topmost row already covered, so occlusion, early-out and
back-to-front ordering are all the same single test.

## Controls

Four buttons is the entire input budget, so the game can be ported to boards
with no keyboard. There is no look up/down: the camera carries a fixed downward
tilt, and the crosshair sits at the centre of the panel where the aim ray
actually lands.

The layout is two-handed: the right hand lives on the arrow cluster and does
all the moving and all the menu navigation, the left hand lives on `E` and `D`
and does all the acting. No key means two things, which is why `WASD` is not
also bound to movement — `D` has to mean build.

| Action | Cardputer | Hand |
|---|---|---|
| Move, back | `;` `.` (up/down arrows) | right |
| Turn | `,` `/` (left/right arrows) | right |
| Mine, attack | `E` (or `SPACE`) — hold it; the target block is lit and outlined | left |
| Build | `D` — stacks one block on the column you are aiming at | left |
| Pause | `` ` `` or `TAB` | left |
| Confirm | `E` or `ENTER` | left |

Building on your own cell is refused: with a heightmap that would jack you up a
block at a time and let you pillar out of every wave for free.

## Performance

Measured on a Cardputer ADV with the `cardputer-dev` build, which prints frame
timings over USB and answers `b` with a scripted play session. That session
jumps straight to a late-night wave, because a benchmark that measures night one
measures five mobs and the frame rate that has to hold is the one with a full
field of twenty-four.

| | worst | average | best |
|---|---|---|---|
| Night, full 24-mob wave | 33 fps | 64 fps | 76 fps |

CPU averages 9.6 ms against a 12.98 ms transfer, so most frames are limited by
the panel rather than by the processor and sit at the 76 fps ceiling. The worst
case is a view filled with close geometry, where the column walker alone reaches
22 ms.

The single largest saving was not in the renderer at all. `shadeFor` rebuilds
every material's shade table from the hour of the day, and it was doing so on
every frame — **6.4 ms, half the budget, recomputing a value that takes a hundred
seconds to cross its range.** Quantising the day into 512 steps rebuilds it
about ten times a second, which is finer than the eye can follow a sunset:

| | before | after |
|---|---|---|
| `shadeFor` | 6414 us/frame | 143 us/frame |
| frame rate | 40 fps average | 64 fps average |

Four things buy the rest:

- **Pre-swapped framebuffers.** The ST7789 wants big-endian RGB565. Typing the
  buffers as `lgfx::swap565_t` and baking the swap into the shade tables means
  `pushImageDMA` takes the `pixelcopy_t::no_convert` path and nothing in a hot
  loop ever swaps a byte.
- **A held SPI transaction.** `startWrite`/`endWrite` are reference-counted in
  LovyanGFX, so holding one open across frames stops the `endWrite` inside
  `pushImageDMA` from ending the transfer and waiting on it. This alone moved the
  frame time from 16.0 ms to 12.98 ms — the CPU work now genuinely overlaps the
  transfer instead of running after it.
- **Two buffers**, so the frame being built is never the frame being sent.
- **Both cores.** Columns are independent — each walks the world read-only and
  writes only its own pixels — so core 0 takes the even columns and core 1 the
  odd ones, with no locking and a two-notification handshake. Interleaved, not
  split down the middle: a contiguous split leaves one core staring at a cliff
  filling its whole stripe while the other looks at sky, and the frame costs
  whatever the slower half costs.

Two things that were tried and measured as losses, and are documented in the
source so they are not tried again:

- **Merging consecutive floor cells into one span** (Doom's floor casting).
  A/B on a fixed scene: 10.5 ms merged against 4.3 ms unmerged. Doom's version
  pays because it replaces per-column ray work; this replaced an already-cheap
  flat fill over the largest area of the screen with a per-row shade
  computation, and the pixels cost far more than the spans saved.
- **`-flto`.** It has to be on the link line to resolve `src/`'s objects, which
  collects the Arduino core, where it drops `app_main()` as unreferenced —
  the only caller is inside a precompiled archive the optimiser cannot see, and
  `--undefined=app_main` does not save it. The inlining it would have bought was
  got instead by batching the per-cell world lookups into one `world::cellAt()`.

The walker itself carries the reciprocal from each cell's far edge to the next
cell's near edge (one divide per cell, not two), steps block bands by
subtraction rather than reprojecting each one, skips a column's whole side when
even its top is already covered, and stops at 17 cells because the shade table
saturates at 16.

## Build

```sh
pio run -e cardputer -t upload          # the game
pio test -e native                      # 94 host tests, no hardware needed
pio run -e cardputer-dev -t upload      # + frame-time overlay, telemetry, screenshots
./tools/grab-screenshots.py             # pull PNGs off the dev build
./tools/make-font.py                    # regenerate src/font5x7.h from its glyph art
```

One binary runs on both the Cardputer and the Cardputer ADV: they share the
StampS3 module, M5GFX autodetects the panel, and M5Cardputer picks the keyboard
controller (IO matrix or TCA8418) at runtime.

## Layout

```
src/
  main.cpp          screens, the fixed 60 Hz timestep, audio
  world.h/.cpp      PURE  heightmap, materials, terrain + structures, mining
  raycast.h/.cpp    PURE  camera -> screen spans
  game.h/.cpp       PURE  clock, waves, flow-field pathing, combat, upgrades
  render.h/.cpp     framebuffers, shade tables, spans, billboards, the pickaxe
  ui.h/.cpp         HUD and the title/upgrade/death cards
  font5x7.h         generated by tools/make-font.py
  screenshot.h/.cpp  #ifdef DEV_SERIAL
  hal/hal.h         board contract — six held buttons and four edges
  hal/hal_cardputer.cpp
test/               Unity suites for the three pure modules
```

`world`, `raycast` and `game` include no Arduino or M5GFX headers, so the
generator, the DDA and a whole night of simulation all run on the host. Time
enters the simulation as a tick count, never as `millis()`.

## Adding a board

Board specifics live behind `src/hal/hal.h`. Nothing in `src/` outside `hal/`
includes a board library.

1. Write `src/hal/hal_<board>.cpp` wrapped in `#if defined(BOARD_<X>)`,
   implementing `begin()`, `display()`, `update()`, `buttons()`, `caps()`,
   `boardName()`, `beep()` and `voice()`.
2. Map whatever input the board has onto the eight held flags and nine edges in
   `hal::Buttons`, and name those controls in `hal::Caps` so the on-screen hints
   match the hardware. A board with four buttons maps `LEFT/RIGHT/FWD/ACT` and
   reaches `BUILD` with an `L+R` chord; a three-button board auto-walks forward.

   Everything past that core degrades to less game, never to a broken one. No
   keys for `lookUp`/`lookDown` means the fixed downward tilt the game shipped
   with, and `caps().kLook` set to `nullptr` so the title screen does not offer
   a control that is not there. A `voice()` that ignores its channel and wave
   and calls `beep()` loses the layering and the timbre, not the sound.
3. Add an env to `platformio.ini` with `-D BOARD_<X>`.

A different panel size is a two-line change in `raycast.h`; nothing else in the
codebase names a pixel count.

## Overhangs

Every cell may carry one **slab**: a second solid run floating above its ground
column with air in between. That is what a plain heightmap cannot express, and
it is what a roof, a bridge, a rock arch and the mouth of a cave all are. Ruins
are roofed, hillsides have tunnels bored into them, and arches span the lowland.

Making that work meant replacing the renderer's occlusion — a single "topmost
row painted so far" — with a list of still-unpainted row ranges. It has to be a
list: the moment a world can have an overhang, you can see the ground, then a
gap of sky under a bridge, then the bridge deck, and a column's painted region
is no longer one contiguous run. Almost every candidate span still takes a fast
path for the one-range case, which is what keeps it affordable.

One slab per cell rather than an arbitrary list of runs, because one covers all
four of those shapes and the occlusion cost grows with how many holes a column
can have. Slabs are terrain, not material: you walk under them, you cannot mine
them.

Mobs are clipped correctly against them: the occlusion table records ground
geometry only, so a mob under a bridge is not hidden by the deck over it, while
slabs contribute a separate band so a mob on the *far* side of one is.

**A caveat worth knowing.** The camera has a fixed downward tilt and no pitch
control, so anything above eye level lives in the top 38 rows of the panel. An
overhang directly above you is off the top of the screen — you see one by
approaching it, not by standing under it. Cave mouths read best, because you
walk into them at ground level.

---

<p align="center">MIT · made by <b>REZOR</b> · rezor.me</p>
