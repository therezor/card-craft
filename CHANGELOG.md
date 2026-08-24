# Changelog

## 1.0.1

- The keyboard no longer allocates while a key is held. Reading the key state
  copied a structure containing three vectors, so holding any key ran three
  allocations and three frees every frame — and that was the only place the
  game reached the allocator at all while you were playing.
- Developer builds report the heap low-water mark, a heap integrity check, the
  margin left on both task stacks, and how much of the placement-marker pool is
  spare. A freeze that happens once now leaves something behind to read.

This came out of a report of the game freezing when the craft card was opened.
That could not be reproduced, and nothing here is proven to be its cause: the
heap measures 35.5 KB free and does not move when the card opens, and both
stacks have room to spare. What the keyboard fix removes is the one place in
the frame where a memory problem could have surfaced, which happens to be a
keypress — the same keypress that opens the craft card.

## 1.0.0 — first public release

A first-person survival game for the M5Stack Cardputer. Mine a world made of
blocks, build something to hide in, and see how many nights you last.

- Mine and build anywhere. Every block can come out and go back somewhere else,
  so tunnels, shelves, roofs and holes are never refused.
- Craft from shapes. A pickaxe is a wide head over a handle, a sword is a blade
  over a handle. Start with nothing and work up through stone, iron and diamond.
- Survive the night. Zombies wind up before they swing, skeletons shoot from
  range, creepers take out whatever you just built. Torches keep ground clear.
- Sixteen cells of draw distance, day and night, music under the daylight, and
  a score counted in nights survived.

Runs on both the Cardputer and the Cardputer ADV from one build.

---

## Development

Everything below is the build history from before the first release. These
numbers were never published and are kept only for the record.

### 1.9.0

- Fewer mobs at night, and they spawn much further away, so they walk in out of
  the dark instead of appearing at the edge of what you can see.

### 1.8.1

- Music under the daylight. Night stays quiet, because the mobs are the sound.
- Fog is clearer up close and fully closed at the far end.
- Planks cost two logs and pay four.

### 1.8.0

- New key layout: the left hand acts, the right hand moves.
- Jump, which goes up and forward from one press.
- Recipes are shapes you lay out in the grid, not just piles of material.
- A recipe book you can browse and load a recipe straight from.
- Blocks are drawn as their real art on the hotbar and in the crafting card.

### 1.7.0

- You can see what you are holding: the pickaxe, the sword and your empty hand
  are drawn in first person and swing when you use them.
- Each tier has its own metal colour.
- A dropped tool looks like that tool instead of a grey box.

### 1.6.0

- Mine and build anywhere. Any block can come out or go back, so tunnels,
  shelves and holes are never refused.
- The view stays where you point it.

### 1.5.0

- Block textures.
- Mining takes the block you are aiming at, and building puts one against the
  face you are aiming at — so you can dig into a wall and build outward.
- Digging down finds dirt, then stone, then ore.
- You can walk on and under what you build.

### 1.4.1

- Zombies wind up before they hit, so a blow can be read and stepped out of.

### 1.4.0

- Real sounds for mining, building, fighting and every mob, with a SOUND
  setting on the pause card.

### 1.3.0

- Night is dark but readable, and torches light the ground around them.
- Look up and down.
- The block under the crosshair is outlined and cracks as you mine it.
- Hand-drawn mob sprites, and particles when something breaks or bleeds.

### 1.2.2

- Fixes.

### 1.2.1

- Mob behaviour pass.

### 1.2.0

- Overhangs: roofs, bridges, arches and cave mouths.
- Crafting, an inventory, torches, lava and biomes.
- The renderer uses both cores.

### 1.1.0

- Mobs, with pathing that follows you through the world.
- Control pass.

### 1.0.0

- First release: a 96x96 block world, mining and building, day and night, and
  a score counted in nights survived.
