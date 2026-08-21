# Changelog

## 1.4.0 — sounds, and a switch to turn them off

### The beeps were a format problem, not a tuning problem

Every sound was a short table of `{freq, ms, wave, vol}` steps handed to
`Speaker.tone()`, which loops a sixteen-sample wavetable at a frequency. Three
things that format cannot express, and every sound worth having needs at least
one of them:

- **an envelope.** Nothing decayed. Every impact ended by being cut off, which
  is what made them read as beeps more than any pitch choice did.
- **noise.** The `kNoise[16]` table was a sixteen-sample cycle played *at a
  pitch* — at 300 Hz that is a 300 Hz buzz, not noise. The explosion, the hit
  and the mining tick all depended on it and none of them got it.
- **a sweep.** An explosion is a continuous fall. This one was five tones.

Retuning the frequency tables cannot fix any of that, and adding envelopes as
micro-steps is not open either: M5Unified's per-channel request queue is two
slots deep, so a thirty-step envelope has nowhere to go.

So the waveforms are rendered offline now, by `tools/make-sfx.py`, and the device
plays them back through `Speaker.playRaw()`. An explosion is noise through a
two-pole filter sweeping 3 kHz to 200 Hz over 800 ms with a 120 to 45 Hz sub
falling underneath it and a second thump behind it. A hit is a 10 ms crack on an
80 ms thump. A mining tick is 75 ms of band-limited noise around 400 Hz.

| | before | after |
|---|---|---|
| Sound source | 16-sample wavetable at a pitch | rendered PCM, 16 kHz |
| Flash | 616 KB | 707 KB (of 3342 KB) |
| RAM | 103724 bytes | 103724 bytes |

Zero RAM because the bank lives in flash and the speaker task reads it in place,
which is the only reason this is affordable at all — the two 64.8 KB DMA
framebuffers are already the tightest allocation in the program.

The old step tables are still in `sfx.cpp`. They are the fallback for a board
whose `hal::sample()` returns false, which is the same bargain `hal.h` makes for
every other optional capability: less sound, never none.

### Repeats stopped machine-gunning

Mining fires a tick about eight times a second, and eight *identical* ticks a
second is a machine gun — the ear catches the sameness long before it catches
the sound. Mining, hits, swings and block breaks now jitter their playback rate
±8% per play, off a seeded LCG so a run is still reproducible.

### Two events that made no sound at all

`EV_ARROW_FIRE` and `EV_ARROW_HIT` had been raised by `game.cpp` since
projectiles went in and had no case in `playEvents()`: skeletons shot at you in
silence. The bow sits on the mob channel rather than the tool channel, because on
the tool channel it would lose to the player's own swings — which is exactly the
moment an incoming arrow has to be heard.

Menu cursor movement was also silent on every card. `kMenuMove` existed but was
wired only to the block-cycle key.

### SOUND: ON/OFF

On the pause card, saved to NVS. Switching it off stops whatever is in flight
rather than letting a fuse outlive the decision to silence it, and every sound
the game makes goes through `sfx::play()`, so there is one gate and nothing can
leak past it.

The pause card's `switch` used to reach QUIT through `default:`. Inserting a row
would have silently changed what QUIT meant, so it is spelled out now.

## 1.3.0 — the polish pass

Rendering, art, combat feel, particles and sound. And, found along the way, the
single most expensive line item in the frame.

### shadeFor was half the frame budget

`shadeFor` rebuilds every material's shade table from the hour of the day. It
was doing that on **every frame** — measured at **6.4 ms, rock steady** — for a
value that takes a hundred seconds to cross its entire range. Quantising the day
into 512 steps rebuilds the tables about ten times a second, which is far finer
than the eye can follow a sunset.

| | before | after |
|---|---|---|
| `shadeFor` | 6414 us/frame | 143 us/frame |
| frame rate, 24-mob wave | 40 fps average | 64 fps average |

That one change paid for everything else in this release with room to spare: the
build ends up **faster than it started** despite adding surface grain, bevels,
a selection box, particles, sprites and a projectile.

### The selection highlight was broken in six ways

`docs/img` showed the selected block as a black diamond gouged out of the grass.
That was not a style choice. The sel path wrote a near-black outline into the
first and last row of *every* selected span with no height guard, while the
ordinary cap rule three lines above it was guarded by `y1 - y0 > 2`. A grazing
top face is one or two pixels tall, so the "outline" consumed the whole span.

- **No vertical edges**, because a column cannot see its neighbours — and its
  neighbours are being walked by the other core. Now a post-pass after the join,
  where the extents are complete and one sweep can see the whole silhouette.
- **No height guard**, so short spans inverted to a dark blob.
- **Rules at the occlusion seam**: the rows recorded were the *clipped* ones, so
  a partly hidden target drew a line hanging in mid-air. Each column now records
  the block's own projection as well as what survived the clipper, and a
  boundary is ruled only where the two agree.
- **A cell dug to height 0 was invisible.** It emitted no spans at all, so the
  floor of a pit rendered as the background gradient and the far wall stretched
  down across the hole. Standing in a quarry, 46 of 135 rows were painted; now
  134 are.
- **Slabs lit up.** Aiming at the cell under a bridge highlighted the deck over
  it — a block that cannot be mined and was not the target.
- **Selected blocks lost their dither and their grain**, because the highlight
  was a separate loop. It is a table swap now, which also takes a branch out of
  the hottest loop in the program.

The outline colour is chosen per material by luminance *as drawn*, not as
tabulated: grass is a mid-tone at noon and nearly black at midnight, and one
constant was wrong for half the palette at any given hour.

The first version of the post-pass cost 1.5 ms average and 5.7 ms on a frame
where the target filled the panel, because it walked every row of every selected
column and hashed every pixel for the crack stipple. Rewritten to be
proportional to the outline rather than the area: **77 us**.

### Night was unreadable, and torches could not fix it

A night frame was a black rectangle. `s_amb` bottomed out at 0.34 to prevent
exactly that, but the face multiplier compounds with it and the fog target was
near-black too, so both ends of every ramp landed in the same 8-27 range.

Worse, torch light is applied by pulling a block toward the near end of the fog
ramp — and at midnight that end *is* the block at ambient. A torch could clear
haze and nothing more. Standing beside one looked almost exactly like standing
in the open, which quietly made the torch recipe pointless.

The ramp now extends *below* band 0 with six brighter-than-ambient rungs, so the
same single subtraction has somewhere brighter to pull toward. Zero extra
per-pixel cost. Night also gained a moon-blue floor, and the player carries a
small light so the two cells at their feet are always readable.

### Blocks read as blocks

Every vertical face carries grain: a shared 8x8 noise tile, with the amplitude
per material, so snow stays smooth and coal reads as stone shot through with
black — which is what finally tells coal and iron apart from the stone they sit
in. It is cheap because a vertical face is hit at one horizontal position per
screen column, so a face needs no interpolation across a span, only an index
down it.

The single dark rule between stacked blocks became a bevel: a lit edge on top
and a contact shadow underneath. A six-high stack now reads as six cubes rather
than as one tall face with lines ruled across it.

Tops get no per-pixel work at all. Rows 29 to 134 are 78% of the panel and are
almost entirely floor, which is exactly where the floor-merge measurement said
the budget dies.

### Sprites are authored pixel art

Mobs were nine axis-aligned rectangles scaled continuously, so every edge
crawled as one walked toward you and small ones dissolved into mush. They are
indexed bitmaps now — `tools/make-sprites.py`, reviewable ASCII art, packed by a
machine that does not make bit-order mistakes, exactly as `make-font.py` already
did for the font. Outline baked into the art, three or more shades per surface,
two walk frames driven by distance travelled rather than by a timer.

The pickaxe was drawn at runtime from rotated squares: a correct silhouette with
nothing inside it. It is art now too, and it took four goes to make it read as a
pickaxe rather than as a bent stick. Every failure was visible from across the
room:

- **The open V between the head's lower arm and the haft is the silhouette.**
  Filled in, what is left is a walking stick.
- **The head is in front of the handle it is fixed to.** Drawing the haft over
  it punched a hole clean through the blade where the two cross, and the tool
  looked snapped.
- **The head has to taper to a point at both ends**, and the two prongs have to
  meet at a real angle. At constant thickness, or with both arms at the same
  slope, it stops being a pickaxe head and becomes a blade.
- **The head starts grey.** It was the wood tier's colour, which is the colour of
  the haft, and a head matching its handle reads as one object however good the
  silhouette is.

**Pixel art is never rotated.** Three ways of leaning the tool through its swing
were tried on the device and all three were worse than not leaning it at all.
Rotating and walking the destination drops source texels at any angle off ninety
degrees, and a one-texel outline tears open — the tool came apart mid-swing.
Walking the source instead with oversized blocks closes the holes and smears the
edges into lumps. A per-row shear keeps every texel square but slides the head
sideways off its own handle, which looks like the tool bending. The sprite is
blitted at whole-pixel offsets now and the swing is pure motion; the arc in
`kSwing` is nineteen pixels across and twenty-eight down, and on its own it reads
as a swing perfectly well.

The art is laid out from its geometry and pasted into `make-sprites.py` as
literal ASCII, because placing sloped runs a row at a time produced off-by-one
edges every single time, and because the art still has to be reviewable in a
diff — which is the entire point of that file.

### The fight

- **A swing that missed did nothing at all** — no event, no sound, and the
  pickaxe did not even move, because `playerAct` returned 0 when the arc was
  empty. It swings now, rate-limited by the animation rather than by the swing
  cooldown, so missing never locks you out of connecting.
- **The skeleton was a hitscan.** It telegraphed for 26 ticks and then took a
  heart off you from seven cells away with nothing drawn in between. It looses a
  real arrow now: dodgeable by stepping aside, and stoppable by a wall.
- **No invulnerability frames.** Two mobs landing blows a tick apart took two
  hearts with no window to answer in, and nothing else in the game paced them.
  Lava deliberately bypasses the window: standing in it is a choice being made
  again every tick.
- **Struck mobs flash white and jolt.** The flash alone reads as a lighting
  glitch; the recoil is what makes it a hit.
- **Camera kick** on blows and explosions, applied to a *copy* of the camera —
  shaking the real one would drag the aim ray with it and break reproducibility.

### Particles

Block shards in the colour of what broke, hit sparks, death puffs, blast debris
and arrow strikes. World-space points projected exactly as mobs are and clipped
against the same occlusion table, including the slab band — so nothing shows
through a bridge deck.

The event mask says *what* happened and cannot say *where*, so the simulation
hands over a small ring of positioned effects alongside it. The pool has its own
random number generator: cosmetics must never draw from `State::rng`, or the
number of sparks on screen would change the simulation.

### Sound

The old audio was one square-wave beep per frame, chosen from a priority chain
that discarded the other twelve events. The comment explaining why said layering
"produces mush on a single piezo channel" — but the Cardputer's speaker is an
I2S amplifier (M5Unified brings it up on BCK 41 / WS 43 / DATA 42), and
`Speaker_Class` mixes eight virtual channels and takes a single-cycle wavetable.
All of it was available from the start; the game just never asked.

`src/sfx.cpp` is a non-blocking sequencer over cues built from steps, on
channels assigned by role so a cue can never cut a more important one. Mining
while a creeper hisses at you is two sounds now, which matters: the hiss is the
only warning you get, and it used to be silenced by a pickaxe.

### Looking up and down, and the step

Pitch is a y-shear on a horizon the camera now carries, with the aim slope
derived from it so the reticle and the aim ray can never disagree. Bounded,
drifting back to the resting tilt, and mapped to R and F — a board with no keys
to spare leaves the flags false and plays exactly as before.

Walking up a block was already automatic, but `cam.z` was assigned outright, so
the view moved a whole world unit between two frames. What that looked like was
a jump cut. It is a slightly under-damped spring now: nine ticks to settle, with
a small hop at the crest and a dip on landing.

**94 host tests**, up from 73.

## 1.2.2

**Billboards are occluded by overhangs.** The occlusion table records the
topmost row painted by *ground* geometry, deliberately ignoring slabs — a mob
standing under a bridge is plainly visible and must not be clipped by the deck
over it. But a deck *between* the camera and something farther away does hide
the part of it behind the deck, and a single topmost row cannot express that.
Slabs now contribute their own band per column and distance bucket, and a
billboard clips against both: what is left of a mob on the far side of a bridge
is the piece above the deck and the piece below it.

## 1.2.1 — mob audit

Six bugs found by probing the mob code rather than reading it. Four were the
same mistake in four places: **range was measured in two dimensions** in a game
that had grown a third.

**Melee reached straight up a cliff.** A zombie at the foot of a five-block
pillar was "1.2 cells away" from a player standing on top of it and hit them
through the rock — which defeats the entire point of building height. Anything
that reaches out and touches now measures feet-to-feet in three dimensions.
Steering stays two-dimensional, because pathing is a grid.

**So did the player's swing**, in the other direction. Same rule both ways.

**Creeper blasts ignored height** — one at the foot of a pillar damaged a player
eight blocks above it. Standing on top of something is real cover now.

**Skeletons shot through slabs.** Line of sight tested the ground column and
never the slab over it, so an archer under a bridge could fire through the deck.

**Lava burned only the player.** Mobs walked through it untouched, which made a
pool a hazard to route around rather than something to back a wave into. It now
burns anything standing in it, on the same timer.

**Building ignored slabs entirely.** A column could be raised straight through a
bridge deck, or the gap under one bricked shut around whatever was standing in
it — leaving a body in a cell the movement rules call impossible to occupy. An
hour of simulated play hit that state 2504 times; it is now zero. You can still
build under a bridge, just not seal it.

**Mobs that could never arrive held a slot all night.** One in a pocket the flow
field cannot leave — behind a ridge it cannot climb — works at the barrier
forever, so the wave that actually reached the player was quietly smaller than
the one the director budgeted for. They now give up after fifteen seconds of no
progress and hand the slot back. Measured as *progress*, not as movement: a mob
boxed in still shuffles inside its cell every tick. Distance-gated, so a mob
working at a wall the player has just sealed themselves behind is left alone.

Not a bug, checked and confirmed: creeper craters are terraced in one-block
steps, so nothing gets trapped in its own blast hole.


## 1.2.0

### Overhangs

**Every cell may carry a slab** — a second solid run floating over its ground
column. Ruins are roofed, hillsides have tunnels bored into them, and arches
span the lowland. Occlusion became a list of unpainted row ranges rather than a
single topmost row, because the moment a world has an overhang you can see
ground, then sky under a bridge, then the bridge, and a column's painted region
is no longer contiguous. A fast path for the single-range case keeps the common
column as cheap as it was.

### Both cores

Columns are independent, so core 0 takes the even ones and core 1 the odd ones
with no locking at all. Interleaved rather than split down the middle: a
contiguous split leaves one core filling a cliff while the other looks at sky.

### Crafting, inventory, torches, lava, biomes

Mining files drops by material instead of pouring everything into one counter,
and the build key places what is held. Four recipes, the important one being
the torch: lit ground is ground mobs will not spawn on, which is the first
reason to build anything other than a wall. Lava lights the pit it sits in,
burns whatever stands on it and cannot be mined — bridge it or go around.
Three biomes put sand and snow on the same terrain the noise already made.

### Fixes

**Mining never changed a surface.** `matAt` reads the column's height and top
material, so resolving the revealed block *after* the column had shrunk asked
"what is on top of this column" and got back the block that had just been
removed. Digging grass off a hill left grass, and taking a torch down left
another torch under it.

**Two brown families.** Dirt, wood, plank and iron ore were four near-identical
mid-browns, which distance fog then flattened into one. The palette is now
separated by brightness as well as hue.

### Rendering

Ordered 2x2 dithering between fog bands, so sixteen distance steps across a
ground plane stop reading as visible arcs. Fewer hearts to start with — six, not
ten — so the +2 upgrade is a real gain.


## 1.1.0

### Mobs

**They stop at arm's length.** Every kind now has a standoff just outside its
reach and circles there instead of closing, so a wave surrounds the player
rather than queueing up in front of them — and nothing walks into the camera
and fills the screen with one sprite any more.

**Attacks are announced.** A blow winds up, holds the mob still while it
commits, and can be walked out of; a skeleton's shot can be broken by stepping
behind cover between the draw and the release. A hit that landed the instant a
mob was in range was unreadable.

**Two pathing bugs, both older than this release.** Mobs picked the single
lowest-cost neighbouring cell and walked to its centre — so an east step
targeted the `y` they already had, and they locked to one axis and crabbed
sideways forever. They now descend the gradient of the whole neighbourhood and
move in both axes at once. Separately, a body could end up overlapping a cell
it was not allowed to enter (walk up a step, find the next cell two higher) and
from then on every move was refused and it was wedged there for the night; it
now eases back to the middle of the cell it is standing in.

**They look like something.** Two legs with a gap between them, arms, and a
face — a creeper is unmistakable now, and it strobes while its fuse burns. A
dark silhouette pass keeps them from dissolving into terrain of a similar
colour, and faces are dropped below the size where they would just be specks.

**Sealing yourself in has an answer.** When nothing can path to the player, the
director sends creepers and keeps sending them, and their fuse is deliberately
not gated on line of sight — one pressed against the far side of your wall goes
off anyway, and the wall goes with it. This is why zombies were not given
pickaxes: a mob that chews through walls is just a slower mob, whereas a
creeper arriving *because* you sealed yourself in is a consequence you can see
coming and choose to fight instead.

### Controls

**Two-handed.** The right hand lives on the arrow cluster and does all the
moving and menu navigation; the left hand lives on `E` and `D` and does all the
acting. `WASD` is no longer bound to movement, because `D` has to mean build
and a key cannot mean two things.

**A pause menu**, and a `ui::Menu` type behind it — a card with a title, a
cursor and rows, used by both the pause screen and the dawn card. The cursor
wraps, the card sizes itself to its contents, and it renders over the frozen
world rather than cutting to a flat background, so the player comes back
knowing where they were standing.

### Rendering

**Blocks read as blocks.** A dark rule along the top edge of every side band,
so a six-high column is six cubes rather than one tall face, and a small
deterministic colour wobble per block so a large stone face is not a flat slab.
Both fade into the fog with everything else.

**A sun, a moon and stars**, fixed to compass bearings rather than to the
screen, so they slide past as the player turns and work as a compass. They are
clipped against the terrain's own occlusion table.


## 1.0.0

First release. A first-person survival game for the M5Stack Cardputer and
Cardputer ADV, running at 50 fps worst case against a 30 fps target.

### Game

**Night survival.** Daylight is a countdown: mine for blocks to build with and
ore to upgrade with, then survive the night. Mobs spawn at the map edges and
path to the player with a BFS flow field rebuilt three times a second — which
is the design, not an optimisation, because the player spends the night
rearranging the map and a flow field is correct the instant a wall goes up.
Zombies from night one, creepers from night two, skeletons from night four.
Surviving pays out one of three upgrades. Score persists to NVS.

**Creepers destroy blocks.** Without that, walling yourself in is a solved
strategy and the building half of the game has no opponent.

### World

**A 64x64 heightmap**, three blocks below ground level and six above, over an
unbreakable bedrock plane — so digging down is as available as building up.
Terrain is two octaves of value noise thresholded into flat ground and outcrops
rather than scaled directly to height, which would give rolling dunes with
nowhere flat and nothing to hide behind.

**Structures** the noise cannot produce: trees with canopies over trunks, walled
ruins with a doorway to shelter in, and terraced quarries cut past the stone
line where ore is visible from the surface. Materials below a column's surface
are computed from depth rather than stored, so grass over dirt over stone over
ore costs no memory and cannot drift out of step with the height.

### Rendering

**A heightmap raycaster.** Rays do not stop at the first solid cell; they walk
outward, and every cell contributes its column's side — one span per block, so
a stack reads as stacked cubes — and its column's top. Painting bottom-up while
tracking the topmost covered row makes occlusion, early-out and draw ordering
one test.

**The frame is one DMA transfer.** Two raw `lgfx::swap565_t` framebuffers with
the byte swap baked into the shade tables, so the DMA path never converts a
pixel. Holding the SPI transaction open across frames took the frame time from
16.0 ms to 12.98 ms — the transfer is now genuinely concurrent with the next
frame's CPU work rather than serialised after it.

**Shading is the art direction.** With no textures, three face brightnesses and
sixteen distance bands are the only things giving a cube its edges and the world
its depth. The tables are rebuilt every frame from the day/night clock, so the
sky, the fog and every block recolour together through sunset for a few hundred
lerps.

### Interface

**Four buttons.** No look up/down; the camera carries a fixed downward tilt and
the crosshair sits at the centre of the panel, where the aim ray actually lands.
The block under it is lit and outlined so there is never a question about what
a swing will take. The HUD is a 13-pixel strip; how late in the day it is, and
whether you are hurt, are told by the picture instead.

**A pickaxe** in the corner of the frame, swinging when it is working and at
rest when it is not, its head coloured by mining level — the only place in the
game where an upgrade is visible as an object rather than as a line on a menu
you have already closed.

### Project

**42 host tests** over the three modules that contain no hardware: terrain
generation, the material profile, mining and placing, the DDA's edge cases
(axis-aligned rays, a camera buried inside a column), and whole simulated nights
including whether mobs still close in on a player who has walled themselves in.

**A `cardputer-dev` env** carrying the frame-time overlay, frame timings over
USB, a scripted play session for reproducible benchmarks, and a screenshot
responder. None of it is compiled into the shipped build.
