# Changelog

## 1.7.0 — the hand you swing with

### Three held items, one hand

The pick, the sword and the empty hand had three different origins. The pick was
a stock icon, redrawn; the sword was laid out from geometry a
row at a time; and the empty hand was never art at all — four rectangles, skin, a
blue cuff and three one-pixel finger rules. Three origins is why the set never
read as a set.

The tools are a vanilla-style 32x32 pack now, and **the hand is always drawn**,
underneath whatever is held, so a pickaxe is something a fist is holding rather
than an object floating at the bottom of the screen. It is drawn first and the
tool over it, which puts the handle in front of the fingers.

The tools are **not mirrored**. The pack draws them head up-RIGHT with the
handle running down-LEFT, and that is how they are held: the handle crosses the
palm and the head stands out of the fist. An earlier pass flipped them, from back
when there was no hand and a handle had to leave by the corner of the panel
rather than enter something.

Anchors are placed off the FIST, not off the panel, and one per kind — the two
tools do not carry their handle in the same place, so one number cannot put both
across the palm.

**Hand and tool are one rigid body.** `blitHeld` used to take a `SwingFrame` and
work the arc out itself, which meant each sprite rotated about its own centre;
rotate two things about two different points and they are not one object, and the
handle walked out of the fingers as the swing progressed. `drawTool` turns the
frame into an anchor and an angle for both, about the grip they share, and
`blitHeld` just draws where it is told.

The forearm is extended to 27 rows, its last row repeated. The source sprite is
21 texels tall and ends in a closed outline, because it was drawn as an icon;
held, the swing lifts the hand and then turns it about the grip, which throws
the far end sideways as well as up, and at the source length that end came back
onto the panel and read as severed. 27 is the shortest that clears all sixteen
frames at both amplitudes. None of the fist is invented, and 15 pixels of arm
show below the palm at rest.

The grip is solved from the art rather than guessed. `blitHeld` anchors a sprite
by its centre, and the hand's centre is halfway down its cell — most of the way
into the forearm. Taking that centre as the grip put the handle across the middle
of the arm instead of through the fingers, which is exactly how it looked;
`HAND_X/Y` are now whatever places the palm texel on `GRIP_X/Y`.

A held block turns with the arm too. It used to take the `SwingFrame` and apply
its own translation, which made it the one held item that ignored the swing's
rotation. Its three faces are laid out in the cube's own axes now and the panel
is walked backwards into them — mapping forwards drops destination pixels at 1:1
and punches holes through the cube at any angle off ninety degrees, the same
reason `blitHeld` walks its destination.

### The camera looked at the floor

`TILT` was 38 pixels of resting downward tilt, and the view sat noticeably down
at the ground. It is 30 now, and `HORIZON` moves 29 -> 37.

`REACH` had to move with it, 5.5 -> 6.6. The two are a pair: the aim ray leaves
the eye at `EYE` and descends `TILT / PROJ` per cell, so a shallower tilt pushes
the point where the crosshair meets flat ground further out — 5.05 cells at 38,
6.40 at 30 — and reach has to clear that or a board with no pitch keys cannot
dig at its own feet. `test_flat_ground_at_rest_is_within_reach` is the test that
said so, and it failed on the first attempt exactly as its comment promised it
would. `PITCH_UP` and `PITCH_DOWN` are re-derived so the range stays symmetric
about level rather than about the rest pose.

Reach is the same number for placing, so the raise also lets you build a little
further out.

### A tier is a whole palette, not four swapped indices

`kMetal` in render.cpp and its hand-kept twin `kIconMetal` in ui.cpp are gone,
and so is the rule that palette entries 2..5 are the metal — a contract that was
asserted in one place and hardcoded in three others.

`make-sprites.py` emits one grid per kind plus `kPickTierPal[4]` and
`kSwordTierPal[4]`, and the caller passes a palette rather than a tier. The
pickaxe is exact: its four tiers index identically, pixel for pixel. The sword is
not, quite — its blade gains shades as the tier rises, seven colours on wood
against ten on diamond — so the diamond grid is canonical and the lower tiers
take the majority colour of each slot. One slot merges on wood and iron, two on
stone, and at 64 panel pixels none of it shows.

### The empty hand is drawn from something now

`drawHand` calls the same blit the tools do, with the hand's own palette.

The art is a pixel-for-pixel transcription of a supplied 10x21 fist — every texel
sampled and snapped to the colours actually in that file. Eleven hand-drawn
attempts came first and every one of them was a mitten or a potato; reading the
source's own texels took one pass.

Three things fell out of it. The blit takes the art's dimensions rather than
assuming the pickaxe's, because the hand is 10x21 against the tools' 32x32 — and
both draw at SCALE 2, so the hand is a smaller sprite rather than the same sprite
in smaller pixels; two texel sizes on one panel read as a mistake. It carries a
constant `tilt` the swing's lean is added to, because the source sprite stands
straight up and a fist coming vertically out of the bottom of the panel does not
read as an arm. And it does not lean: the tools' handles run off the edge of
their own art so the panel cuts them wherever the lean puts them, but leaning the
hand swings its forearm away from the bottom edge and leaves a severed arm over
the terrain on the recovery frames.

The held block shrank from 40 pixels to 28 and moved up and left. At its old size
and anchor it covered the hand completely.

### The sword swings wider than the pickaxe

One offset table drove every held item, so a sword and a pickaxe made the same
gesture. `blitHeld` takes the swing's reach as a parameter now, in eighths: the
pickaxe keeps 12, which is the 3/2 everything used to share, and the sword takes
17. A sword is swung; a pickaxe is driven.

### A dropped pickaxe and a dropped sword were the same grey box

Dropped items were all one shape: a small billboarded cube, tinted the block's
colour or, for a tool, its tier's mid metal. Two tools of the same tier were
pixel-identical on the ground and the only way to tell them apart was to walk
over one.

A dropped tool draws its own sprite now — the same art the first-person view and
the hotbar use, shaded through `shadeMob` like everything else in the world, and
clipped by the same ground cut and slab band the cubes were. The palette is
shaded once per drop rather than per texel: eleven entries against up to 676.
Blocks keep the cube, because at a few pixels a side a 16x16 texture averages out
to the colour it already had.

## 1.6.0 — a column is a bitmask, and the view stays where you put it

### There were places the world would not let you dig

A cell was a ground column plus at most three floating **runs**, drawn from a
pool of 3,072 nodes. That cap was not an implementation detail the player never
saw: it was two rules they met. `MINE_NO_ROOM` refused a tunnel when the hole it
would leave needed a fourth run, and `PLACE_NO_ROOM` refused a shelf for the
same reason. Digging into the wrong hillside simply did not work.

A column is a `uint32_t` now, one bit a block. `MAX_H` is 32, so the world's full
height is exactly one word. Any block anywhere can be taken out or put back, a
column can have as many holes in it as it has blocks, and the walker finds its
runs with a bit scan instead of chasing a linked list. `MINE_NO_ROOM` is gone
from the codebase.

**Geometry and material are separate, and that is what makes it work.** Material
is a list of markers — "from this z upward the material is m", with a `DERIVE`
step meaning "fall back to the soil profile". Removing a block cannot change what
anything is made of, so **mining allocates nothing and can never fail**. Only
placing a *new* material draws from the pool, and only world-wide exhaustion can
refuse that. The generator's worst case over sixty seeds is 3,629 markers against
a pool of 4,096.

Two things fell out of it rather than being designed in. `matAt` and `blockAt`
had been separate functions with separate depth anchors, because a run split off
a hillside carried its own `surf` field to keep it reading grass-over-dirt; there
is one column now, so there is one answer, and the field is gone. And a roof laid
at exactly a wall's top height used to be a column *plus* a slab — two objects at
the same height — which is why `normaliseCell` existed. A mask has one
representation of a given solid, so there is nothing to normalise.

### One span per run of like blocks

Every block of every column used to be its own candidate: a loop turn, two
clamps and a full trip through the clipper, about 30,000 times a frame. A run of
like blocks is one span now. The pixels are identical, because a face's texture
coordinate is a function of world height — `v` keeps accumulating and wraps every
tile, which is once a block, exactly as before.

The bevels between blocks therefore fall *inside* a span. The obvious way to find
them is to watch `v` wrap, and it was measured at **+726 µs a frame against the
183 µs the merging saves** — a compare and an unpredictable branch on every wall
pixel is dearer than the clipper calls it pays for. They are computed
arithmetically outside the loop instead.

### `-flto` replaced by a unity build

The reason `-flto` does not work here is documented and has nothing to do with
this code: the plugin must be on the link line to resolve `src/`'s objects, which
collects the Arduino core, where it drops `app_main()` as unreferenced. Handing
the compiler one translation unit instead of four gets the same inlining without
a link line being involved. `src/unity.cpp`, worth about 1.5% of CPU.

### The benchmark was not comparable, and a fixed seed was not the fix

It already used a constant seed. But the simulation advanced by *elapsed time*,
so a build that rendered more slowly took more catch-up ticks per frame, walked
further per frame, and within seconds was looking at a different scene. Render
cost feeds tick rate feeds camera position feeds render cost. Two runs of the
**same build** came back 12% apart on frame rate and 33% apart on CPU.

The bench loop runs exactly one tick a frame and ignores the clock. Runs now
agree to within 0.3%. Every figure measured before this was noise.

Also: the `cardputer-dev` and `cardputer-notex` builds did not link at all.
`render.h` declared four `g_floor*` telemetry counters that nothing defined. They
are defined now, and counted at the call site rather than inside
`drawFloorSpan` — that function is `noinline` precisely because it is one
register short of spilling its inner loop, and four counters live across it would
put it back over.

### The view pushed back against you

Pitch drifted home to the resting tilt whenever neither look key was held, so you
could look up at a canopy but not keep looking at it. It is a held position now,
the same as the facing angle; holding both keys still recentres.

The range went with it. It was about 25° up and 12° down, and the reason given
for keeping down short was that down fills the screen with near geometry and
that is where the frame time is. That does not survive contact with the walker:
looking steeply down, every ray meets the floor within a cell or two and the DDA
stops almost immediately. The expensive view is the one *across* a landscape,
which is the resting tilt. Both stops are about 60° now, symmetric about level.

### Grass had stripes on it

The 16x16 grass tile is dirt with a green lip — exactly right for a wall, and
wrong from above, because the floor sampler indexes the same tile by world XY and
drew that lip as a green stripe every sixteen texels across open ground.

Materials can carry a second tile for their top face now, and grass is the first
to. It costs no palette entries: grass's first four entries were already the
greens the lip is drawn with. Sparse on purpose — stone is stone whichever way
you look at it, and a second tile for every material would double a texel table
that has to live in SRAM.

### Measured

Fixed-seed night-10 wave, 40 windows of 30 frames, with the deterministic bench:

| | fps worst | mean | CPU µs | world µs |
|---|---|---|---|---|
| before | 32 | 46.9 | 15,553 | 12,487 |
| after | 32 | 48.5 | 14,912 | 11,919 |

The frame is now CPU-bound rather than panel-bound — 14.9 ms of processor against
a 12.98 ms transfer — which is the opposite of what the README used to say. The
textured floors are what moved it.

RAM is the binding constraint, not the frame rate. The two framebuffers need
129,600 bytes of contiguous internal DMA memory, and if `reserve()` cannot get
them the panel says NO DMA MEMORY and the board hangs while USB still enumerates
— which looks like a dead serial link, not an allocation failure. The first
bitmask build did exactly that. Packing the surface material and the torch light
into one byte brought it to +16 KB over the old world, with 36 KB of heap left.

## 1.5.0 — a block you can point at, a column with holes in it, and textures

### Mining took the wrong block, and building put it in the wrong place

Both verbs worked on the *column*, not on a block. `mine()` always popped the
top of the stack, so aiming at the foot of a six-high wall took the block off
its top — six blocks above where the crosshair was. `place()` only ever grew a
column by one, so there was no way to put a block against a wall, under an
overhang, or out in front of you. `State::aimOnTop` had been computed and
stored since the beginning and never once read.

`pick()` now returns a `Hit`: the block, which of its faces was struck, that
face's outward normal, and the distance. Building goes at `hit + normal`, which
is Minecraft's rule and has the useful property that a placed block always
touches something solid by construction — so there is no separate "is it
supported" test to get wrong.

The refusals are explicit and all tested: out of the world, beyond reach, into
something already solid, into the player's own body, or into a mob. The body
test replaces an older rule that refused only the cell the player stood in,
which ignored the block over their head — and what it was really guarding
against was pillaring straight up out of a wave, which a body-sized volume
guards properly.

### `Span::sel` had two levels, and now has one

The second level existed only because the block you were pointing at and the
block about to break were different things. They are the same block now, so the
outline follows it and the whole two-level scheme is gone — along with the case
where the outline ran off the top of the panel while you stood beside a tall
wall.

### Reach was measured against the wrong distance

`MINE_REACH` was compared against the DDA's horizontal distance, so reach grew
as the player looked down — the one direction it should shrink. It is `REACH`
now, 5.5 world units from the eye.

The floor under that number is geometry, not taste. At the resting tilt the aim
ray leaves the eye at `EYE` = 1.2 and descends `TILT / PROJ` = 0.2375 per cell,
so the crosshair meets flat ground 5.05 cells out — 5.19 from the eye. A board
with no pitch keys never leaves that tilt, so Minecraft's own 4.5 would make
open ground unmineable on it. 5.5 is the tightest round number that clears it.

Tightening it exposed an older bug immediately: for a hit on top of a column the
reach test compared `dFar`, the far edge of the cell the ray crosses, rather
than where the ray actually meets the ground. Reach overshot by up to a whole
cell, invisibly, while the limit was loose enough not to matter.

### Digging down only ever found dirt

`matAt` measured soil depth from the column's **current** height, and mining is
what changes that height. Take one block off and the layer beneath became the
new "one below the top", which is the dirt band — so it was dirt again, and
again, all the way to bedrock. Digging could never reach stone, coal or iron.

Cliff *faces* rendered the bands correctly, which made it worse: you could see
the ore, mine it, and get dirt. Ore was obtainable only where the generator had
written it onto a surface, which put the whole upgrade economy nearly out of
reach of the pick.

It is the same mistake the leaf band made and that this file already names —
anchoring a band to a height the column does not remember. `g_surf` is the
anchor: one byte a cell, the height the generator left the column at. Over a
1,064-column sample, digging now yields 1,055 coal and 390 iron worth 2,225 ore
where it used to yield none.

### A column is a list of runs now

A cell was one height plus one optional *slab*, and the slab was terrain: the
generator could make one, the player could not mine it or build into it. So the
world had the shape of levels and denied the player access to it.

The three slab arrays are replaced by a pool with a per-cell list head — 3,072
nodes of six bytes, plus an index per cell. A fixed array of three runs a cell
would have been 110 KB, which this board does not have; the pool costs a node
only for cells that carry one, and almost none do.

What that buys is the two verbs a heightmap cannot express. Mining the middle of
a column **splits** it: the part above goes on standing as a run, which is a
tunnel through a hillside. Placing a block in mid-air against a face creates a
run, which is a floor, a roof or a bridge. Filling the hole back in **merges**
the run into the column, because there has to be exactly one way to describe a
given solid or every later edit has to guess which of two objects it meant.

A run cut out of terrain carries the surface its materials are measured from, so
the piece left hanging over your tunnel is still grass over dirt over stone
rather than one flat colour. Mining at z=4 drops iron and z=5 in the remnant
still reads as iron.

When a split would need a run the cell cannot hold, it is refused — and refused
*before* any effort is banked, so a player never grinds three seconds into a
block that then will not come out.

The walker barely changed. `castColumn` already looped over a `Run` array; it
just happened to be built with exactly two entries in it.

### Walking on what you built

`surfaceUnder` replaces the assumption that a body stands on the ground column.
It answers "where would a body at this height come to rest in that cell" — the
highest solid top within `STEP_UP`, with `HEADROOM` over it — and the top of a
run counts, which is the whole of standing on a floor you laid.

`STEP_UP` is still one, so a two-high wall is still unclimbable and walling
yourself in still works. A staircase is climbable, which is the point.

Bodies also un-wedge now: nothing reachable within a step means the world moved
rather than the body — a column grew underneath, or a blast dropped the floor —
and snapping to the terrain is the only answer that does not leave a player
standing inside the pillar that just rose around them.

### Mobs path over it too

The flow field's nodes are standable surfaces rather than cells. Each entry is
a distance and the height that distance was measured at, and the step test runs
in **both** directions — the sweep goes outward from the player but a mob
travels inward along it, so asking "and could it get back?" is what keeps a
two-high wall unclimbable from either side.

One surface per cell, not one per run: the surface a cell is first reached at is
by construction the one on the shortest route, so a second slot could only
describe a longer way to the same place. Four surfaces a cell with an
exactly-sized queue is 108 KB, and that is not available.

The queue is a bounded ring instead. A breadth-first sweep of a 96x96 grid never
has more than a couple of frontier rings pending, so the old exactly-sized queue
was eighteen kilobytes standing idle. Overflow drops the cell, which reads as
unreachable — the pathing degrades rather than corrupts, and it is rebuilt three
times a second anyway.

Net effect on memory: the 3-D field is **5 KB smaller** than the 2-D one it
replaced.

Mobs spawn on the terrain, never on a run, so a roof is shelter rather than a
landing pad.

### Blocks have textures

`world.h` was careful to call the old speckle "not a texture", and it was not
one: a single shared 8x8 noise tile, carrying no colour of its own, indexed by
**screen row** — so the pattern slid across a block as the camera moved instead
of sitting on it.

Two things changed and neither costs a per-pixel branch. The tile is per
material and 16x16, and the byte it holds now selects one of eight **authored
colours** rather than one of four brightness steps of a single one. `shadeFor`
runs each of those eight through exactly the ambient/fog/torch pipeline it used
to run one colour through, so the table is the same shape and the inner loop is
the same two loads. That is the difference between grain and a texture: an
amplitude ramp of one colour cannot draw a grass side, because a grass side is
brown with a green lip on it.

The other half is a real wall-space `v`. A block is one world unit and pitch is
a shear rather than a rotation, so one unit is always `PROJ/dist` pixels — the
step is one multiply off a reciprocal the cell already has, and there is no
divide per span. It is carried on the painter rather than passed to it, because
`paint()` runs tens of thousands of times a frame.

`u` indexes one tile across the face now, not eight. The old comment arguing for
eight was right for a tile indexed by screen row — at one tile per face a texel
was ten pixels wide against one tall, which draws as brushed metal — but with a
real `v` the texels are square by construction, and one tile per block is what
makes brick courses line up and a grass lip sit on the top edge instead of eight
of them.

Top and underside faces stay on the constant-colour path. They are most of the
panel and texturing them needs per-row floor casting, which this repo measured
at 10.5 ms against 4.3 ms and wrote down so it would not be tried again.

**Texels have to live in RAM.** Left in `.rodata` they are external flash behind
the instruction cache, and the wall loop reads them at a position that jumps
with the material and the ray angle — close to the worst pattern a cache can be
given. Measured, that cost 1.5 ms a frame; four kilobytes of SRAM buys all of it
back. The 64-byte tile this replaced never showed the problem because 64 bytes
stay resident whatever you do to them.

### The benchmark measured a different world every time

`b` called `startRun()`, which seeds from `esp_random()`. Two runs of the *same
build* came back at 37 fps and 75 fps purely because one was looking at a house
and the other at open ground, and several apparent regressions during this work
were nothing but that. It generates a fixed seed now and always starts a fresh
run, and two consecutive measurements agree to within 0.2%.

With that, the cost of textures on one fixed world, against the same build with
the sampling compiled out (`-e cardputer-notex`, which exists for exactly this):

| | no textures | textures | delta |
|---|---|---|---|
| fps, mean over the walk | 64.4 | 62.2 | −3.4% |
| walker, mean | 7,030 µs | 7,543 µs | +513 µs |

The dev build also reports free heap on its telemetry line, because the two
figures a build report gives you — static bytes used, and a total the
framebuffers are not counted against — cannot be subtracted to get it.

## 1.4.1 — the pickaxe looks like a pickaxe, and zombies swing

### The pickaxe was a hammer

The head was a straight bar with matching tapers on both ends, mounted at an
angle, with the haft running behind it. Every one of those choices reads as a
mallet rather than a pickaxe.

Replaced with a stock 32x32 icon — an arched
head with two different ends and a haft that comes through it, drawn by someone
who draws these for a living.

It could not be dropped in as-is. `'a'..'d'` are not free colours: `drawTool`
swaps exactly those four for the tool tier, so the head has to be all four of
them and the haft none of them. The import classifies metal from wood on
saturation, ranks each into its own ramp, and puts the outline ring back the way
the house style wants it, so the art in `make-sprites.py` is still reviewable
ASCII and the tier recolour still works.

Kept at the artist's native 32x32 rather than squeezed into the old 24x24 —
downscaling pixel art by three quarters visibly flattened the arch, which was
the whole reason for the change. `drawTool` drops from three panel pixels per
texel to two to suit, which lands the tool at 64 pixels tall against the 72 it
used to be. The swing offsets are in panel pixels and do not scale, so the arc
is exactly the size it was.

### The swing leans now

`SwingFrame.ang` had been in the table since the swing was authored and had
never been read — the comment beside it said so, and said why: three ways of
leaning the sprite were tried on the device and all three were worse than not
leaning it.

All three walked the *source*. Rotating source texels into the destination
drops destination pixels at any angle off ninety degrees, so a one-texel
outline tears open and the tool comes apart mid-swing; oversized blocks close
the holes and smear the edges into lumps; a per-row shear keeps the texels
square but slides the head off its own handle.

The fourth way walks the *destination* instead: every panel pixel the tool
could cover, rotated backwards to ask which source texel it came from. A hole
is impossible by construction, because the loop is over the pixels being filled
rather than the texels doing the filling. Nearest-neighbour, so no colour is
invented and the palette stays the nine entries it was.

The scan box is sized from the angle rather than fixed at the diagonal — 34
pixels of reach at rest against 46 fully leaned — so the fifteen barely-leaned
frames do not pay for the slack the one extreme frame needs. Both source
coordinates are linear across a row, so the inner loop is two adds and a
bounds test.

### The tool sits lower

The anchor moves from 96 to 108. The head lives in the top-left of the art, so
the anchor sits well below where the head lands; at 108 the art's centre is past
the bottom edge and what is in frame is the head and the top of the haft, which
is all a first-person tool should show. Lower was tried and the downswing takes
the head off the bottom of the panel.

### Zombies telegraph with their arms, not by standing still

A committed blow already froze the mob for sixteen ticks before it landed —
that freeze *was* the entire visual telegraph, and it is not one. A zombie
winding up to hit you looked exactly like a zombie that had stopped walking,
and the walk cycle froze with it, so nothing on screen moved at all.

Two attack poses now: the arms come in to the body to wind up, then are thrown
out past the silhouette to strike, sixteen pixels wide against the walk's
fourteen. At sixteen pixels there is no room for a subtle gesture, so the
change is in the outline where it survives being three cells away.

The strike runs the last six ticks of the windup and ends as the blow lands, so
the arms are still out when the screen kicks.

`MobArt` grew a `walk` field: how many of a family's frames are the walk cycle.
The renderer picks a stride with a modulo, and without it a zombie would have
thrown a punch every other step. Creepers and skeletons declare all their frames
as walk frames and are unchanged — a creeper's fuse is its own telegraph, and
the skeleton's draw is still only a pause.

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
