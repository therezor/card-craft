// =============================================================================
//  world.h — the block grid, its material table, and mining
//
//  Deliberately free of Arduino/M5GFX headers so the generator and the mining
//  arithmetic can be checked on the host (see test/test_world). The renderer
//  reads this grid; it never writes to it.
//
//  The world is a 64x64 heightmap: every cell is a stack of 0..MAX_H unit
//  cubes sitting on a solid base plane. That is not a flat maze with tall
//  walls — you walk up a one-block step, you see over a low wall, and a
//  six-high outcrop actually reads as a cliff.
//
//  A heightmap rather than a full voxel grid because it is the shape the
//  renderer can walk in one pass per column, and because the two verbs the
//  game needs — take the top block off, put one back on — are exactly the two
//  edits a heightmap supports.
//
//  On top of that, every cell may carry one *slab*: a second solid run
//  floating above the ground column with air in between. That is what a plain
//  heightmap cannot express, and it is what a roof, a bridge, a rock arch and
//  the mouth of a cave all are. One slab rather than an arbitrary list of runs
//  because one is enough for all four of those shapes, and because the cost of
//  the renderer's occlusion tracking grows with how many holes a column can
//  have in it.
//
//  Slabs are terrain, not material: they cannot be mined or built into. The
//  player walks under them.
//
//  Materials below the surface are computed, not stored: a column keeps its
//  height and the material of its top block, and everything under that is a
//  function of depth. Grass over dirt over stone over ore costs no memory and
//  cannot drift out of step with the height.
// =============================================================================
#pragma once

#include <stdint.h>

namespace world {

enum Block : uint8_t {
  B_GRASS = 0,
  B_DIRT,
  B_STONE,
  B_WOOD,
  B_LEAVES,
  B_COAL,
  B_IRON,
  B_SAND,       // desert surface
  B_SNOW,       // tundra surface
  B_BRICK,      // ruins, and craftable
  B_PLANK,      // craftable, cheap building stock
  B_TORCH,      // emits light and keeps mobs from spawning nearby
  B_LAVA,       // emits light, burns anything standing on it, cannot be mined
  B_BEDROCK,    // map border and the base plane, unbreakable
  B_COUNT
};

// Three of the surface materials are just a palette on the same terrain: what
// biome a cell is in changes what it is made of, not what shape it is. That is
// most of the visual variety for none of the generation complexity.
enum Biome : uint8_t { BIOME_PLAINS, BIOME_DESERT, BIOME_TUNDRA, BIOME_COUNT };
uint8_t biomeAt(int x, int y);

// True for materials that mark a built or grown column rather than natural
// ground. Their interiors are made of themselves down to ground level, and the
// soil profile underneath is measured from GROUND rather than from their top.
bool isStructure(uint8_t b);

// Blocks that make their own light, and how much. 0 for everything else.
uint8_t emission(uint8_t b);

// True for materials that hurt whatever is standing on them.
bool isHazard(uint8_t b);

constexpr int W = 64;    // square and power-of-two, so cell indexing is a shift
constexpr int H = 64;

// Untouched ground sits at GROUND, not at zero, which is what makes digging
// down possible: a column can lose blocks as well as gain them. Three layers
// below and six above, over a base plane of bedrock that cannot be dug out.
constexpr int GROUND = 3;
constexpr int MAX_H  = GROUND + 6;

// Mining is accumulated in "effort", not seconds, so a pickaxe upgrade is a
// multiplier on the numerator rather than a special case in the timing code.
constexpr int EFFORT_PER_TICK = 16;

struct BlockInfo {
  const char* name;
  uint8_t r, g, b;      // base colour, RGB888 — render.cpp lerps here, then packs
  uint16_t toughness;   // total effort to break; 0 means unbreakable
  uint8_t  dropBlocks;  // build material yielded
  uint8_t  dropOre;     // upgrade currency yielded

  // How strongly this material breaks up across its own face, 0..255. Not a
  // texture: one shared noise tile is shaded per material at this amplitude, so
  // stone reads as grain and snow stays smooth for the cost of one byte. It is
  // also what finally tells coal and iron apart from the stone they sit in,
  // which no amount of tuning three flat greys ever managed.
  uint8_t  speckle;
};

const BlockInfo& info(uint8_t b);

// ---- grid -------------------------------------------------------------------

// Deterministic for a given seed — test_world locks this down.
//
// Terrain is noise; on top of it go hand-shaped structures — trees, walled
// ruins to shelter in, and open quarries cut down past the stone line. A
// heightmap cannot express a roof or a cave mouth (both need an overhang), so
// a ruin is a doorway in a wall with the sky above it and a "cave" is an open
// pit. That is the honest version of those shapes in this world.
void generate(uint32_t seed);

// Out-of-bounds reads report a full-height bedrock wall rather than failing, so
// neither the ray stepper nor the mob pathing needs a bounds check inside its
// inner loop.
uint8_t height(int x, int y);

// Material of layer z (0 is the block resting on the base plane). Layers at or
// above the column height report the material of the base plane.
uint8_t matAt(int x, int y, int z);

// Material of the topmost block, or of the ground itself where height is 0.
uint8_t topMat(int x, int y);

bool    isBorder(int x, int y);

// Everything the renderer needs about one cell, fetched in a single call.
//
// The walker used to ask five separate questions per grid step — height, slab
// base, slab top, slab material, light — each a cross-module call that has to
// re-check bounds and recompute the index, and none of which the compiler can
// inline because they live in another translation unit. At ~30 cells a column
// and 240 columns that was the single largest line item in a frame.
struct Cell {
  uint8_t h;          // ground column height
  uint8_t top;        // material of its top block
  uint8_t light;      // 0..LIGHT_MAX
  uint8_t slabBase;   // slab occupies [slabBase, slabTop)
  uint8_t slabTop;    // 0 = no slab
  uint8_t slabMat;
};
Cell cellAt(int x, int y);

// ---- slabs ------------------------------------------------------------------

// A cell's slab occupies [slabBase, slabTop). slabTop == 0 means there is none.
// The gap between the ground column and slabBase is what you walk through.
uint8_t slabBase(int x, int y);
uint8_t slabTop(int x, int y);
uint8_t slabMat(int x, int y);
bool    hasSlab(int x, int y);

// Headroom a body needs between the floor and anything above it.
constexpr int HEADROOM = 2;

// True if a body could stand on this cell at all: inside the map, and with
// room between its floor and any slab over it.
bool standable(int x, int y);

// Places a slab directly. Only the generator and the developer build makes
// slabs — the player cannot, which is why this is not part of place().
void devSlab(int x, int y, int base, int top, uint8_t mat);

// ---- light ------------------------------------------------------------------

constexpr int LIGHT_MAX = 6;   // radius in cells, and the brightest level

// 0 in the open at night, up to LIGHT_MAX next to a torch. Used by the
// renderer to lift a block out of the fog and by the spawner to refuse to put
// a mob anywhere the player has lit.
uint8_t light(int x, int y);

// Rebuilt from scratch whenever a light source appears or goes. A full sweep
// of the grid is a few thousand operations and happens only on an edit, which
// is far cheaper than keeping an incremental source list correct across
// mining, building and creeper blasts.
void rebuildLight();

// ---- movement ---------------------------------------------------------------

constexpr int STEP_UP = 1;    // blocks that can be walked up without stopping

// True if a body standing at height `fromH` can enter this cell: the step up
// must be no more than STEP_UP. Stepping down is always allowed.
bool canEnter(int fromH, int x, int y);

// True if a body of radius r centred here clears every cell it overlaps.
bool fits(int fromH, float px, float py, float r);

// Height of the surface a body at (px, py) stands on.
uint8_t groundAt(float px, float py);

// ---- mining -----------------------------------------------------------------

// Adds effort to a column. Returns true on the tick its top block comes off,
// and only then writes the yields. Effort is discarded when the player looks
// away (resetDamage), which is what makes a tough block a commitment.
// dropMat is what came off, so the caller can put it in an inventory rather
// than adding it to one anonymous pile.
bool mine(int x, int y, int effort,
          uint8_t& dropMat, uint8_t& dropBlocks, uint8_t& dropOre);
void resetDamage(int x, int y);

// 0..255 progress toward breaking, for the crosshair arc.
uint8_t damage(int x, int y);

// Adds a block on top of a column. False if it is already at MAX_H or is the
// map border.
bool place(int x, int y, uint8_t mat);

// Creeper blast: knocks blocks off every column within radius, most at the
// centre. Returns how many blocks it destroyed.
int explode(int cx, int cy, int radius);

}  // namespace world
