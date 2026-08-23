// =============================================================================
//  world.h — the block grid, its material table, and mining
//
//  Deliberately free of Arduino/M5GFX headers so the generator and the mining
//  arithmetic can be checked on the host (see test/test_world). The renderer
//  reads this grid; it never writes to it.
//
//  The world is a 96x96 heightmap: every cell is a stack of 0..MAX_H unit
//  cubes sitting on a solid base plane. That is not a flat maze with tall
//  walls — you walk up a one-block step, you see over a low wall, and a
//  twenty-high outcrop actually reads as a cliff.
//
//  A column is a 32-bit occupancy mask, one bit a block, and MAX_H is exactly
//  32 so a column is exactly one word. Any block anywhere can be taken out or
//  put back: a tunnel, an overhang, a bridge, a roof, a cave mouth and a tower
//  are all just bits, and a column can have as many holes in it as it likes.
//
//  Geometry and material are kept apart, which is the other half of it:
//  removing a block cannot change what anything is made of, so mining
//  allocates nothing and can never be refused for want of room.
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
  B_MASONRY,    // cut stone: towers, castles. Structural, unlike B_STONE
  B_TORCH,      // emits light and keeps mobs from spawning nearby
  B_LAVA,       // emits light, burns anything standing on it, cannot be mined
  B_BEDROCK,    // map border and the base plane, unbreakable
  // Appended rather than filed next to the other ores, so that adding it
  // renumbers nothing. It is the SIXTEENTH material, and sixteen is the whole
  // of what the four-bit surface-material field in g_cell can hold -- see
  // SMAT_MASK in world.cpp. A seventeenth means repacking that byte first.
  B_DIAMOND,    // rare, and only in the deepest layers
  B_COUNT
};

// A biome changes the shape of the ground as well as its colour: each carries
// its own amplitude and roughness, so plains are lowland you get caught in the
// open on, desert rolls in long smooth dunes, and tundra climbs into jagged
// peaks with a snow line on them. Colour alone gives every horizon the same
// shape, which is the opposite of what a biome is for.
//
// Forest shares the plains surface and the plains ground: what makes it a
// forest is that the trees are dense enough to lose a mob in.
enum Biome : uint8_t {
  BIOME_PLAINS, BIOME_FOREST, BIOME_DESERT, BIOME_TUNDRA, BIOME_COUNT
};
uint8_t biomeAt(int x, int y);

// True for materials that mark a built or grown column rather than natural
// ground. Their interiors are made of themselves down to ground level, and the
// soil profile underneath is measured from GROUND rather than from their top.
bool isStructure(uint8_t b);

// Blocks that make their own light, and how much. 0 for everything else.
uint8_t emission(uint8_t b);

// True for materials that hurt whatever is standing on them.
bool isHazard(uint8_t b);

// Square, and deliberately not a power of two. 128 does not fit: six bytes a
// cell against two 64.8 KB framebuffers leaves 128x128 nineteen kilobytes short
// of the S3's internal RAM. Nothing may assume a shift — the flow field's cell
// decode divides, and a constant divide is a multiply anyway.
constexpr int W = 96;
constexpr int H = 96;

// Untouched ground sits at GROUND, not at zero, which is what makes digging
// down possible: a column can lose blocks as well as gain them. Eight layers
// below and twenty-four above, over a base plane of bedrock that cannot be dug
// out.
//
// Generous on purpose. Height is the one dimension that is free here — a column
// is one word whether it counts to nine or to thirty-two — and nothing built out
// of three blocks has a silhouette. This buys towers, cliffs, tall trees and a
// mine you can descend, for no memory at all.
constexpr int GROUND = 8;
constexpr int MAX_H  = GROUND + 24;

// How deep diamond goes, and how much of it there is. Public because the host
// tests assert the depth rule directly -- that no diamond is ever reachable
// above this, which is what keeps it out of the four-bit surface field.
//
// Layer 0 is bedrock, so three layers is z 1..3: the floor of the deepest hole
// the map has. At 0.5% of the lattice range a column that goes all the way down
// is usually three layers of stone, and a seam is a thing you go looking for.
constexpr int      DIAMOND_MAX_Z  = 3;
constexpr uint32_t DIAMOND_RARITY = 21474836u;   // 0.005 of the range

// Mining is accumulated in "effort", not seconds, so a pickaxe upgrade is a
// multiplier on the numerator rather than a special case in the timing code.
constexpr int EFFORT_PER_TICK = 16;

struct BlockInfo {
  const char* name;
  // Base colour, RGB888. It is what a block fades to at distance, what fills
  // its top face, and what its break-particles are made of — and it is entry 0
  // of the material's texture palette, which tools/make-textures.py checks
  // against this table so the two cannot drift apart.
  uint8_t r, g, b;
  uint16_t toughness;   // total effort to break; 0 means unbreakable
  // How many of itself a block yields when it comes off. Coal and iron are
  // items you hold like any other, so their richness is spelled here rather
  // than in an abstract currency.
  uint8_t  dropBlocks;

  // How broken up a surface looks is carried by the per-material texture in
  // textures.h, in colour rather than in noise amplitude — which is what lets
  // coal read as black lumps in grey rock instead of as darker grey.
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
// Asking separately — height, slab base, slab top, slab material, light — is
// five cross-module calls per grid step, each re-checking bounds and
// recomputing the index, at ~30 cells a column and 240 columns a frame.
struct RunView { uint8_t base, top, mat; };   // occupies [base, top)

// Everything the renderer needs about one cell, fetched in a single call.
//
// `solid` is the whole column: bit z is set where (x, y, z) is a block. MAX_H
// is 32, so a column is exactly one word, and the runs a walker wants come out
// of it with a bit scan rather than a list walk.
//
// A mask has no cap: a column can have as many holes in it as it has blocks,
// so no tunnel or shelf can ever be refused for want of room.
struct Cell {
  uint32_t solid;     // bit z = (x, y, z) is solid
  uint8_t  h;         // blocks in the run resting on the base plane
  uint8_t  top;       // material of that run's top block
  uint8_t  light;     // 0..LIGHT_MAX
  uint8_t  surf;      // the height the generator left this column at
};
Cell cellAt(int x, int y);

// Copies this cell's floating runs into `out`, ascending by base, and returns
// how many. `cap` is the size of `out`: a cell's run count is unbounded now, so
// the caller states what it can take rather than the world declaring a limit.
int runsAt(int x, int y, RunView* out, int cap);

// True if (x, y, z) is solid, wherever that solidity comes from.
bool solidAt(int x, int y, int z);

// Material occupying (x, y, z), or B_BEDROCK where nothing does.
uint8_t blockAt(int x, int y, int z);

// Material markers still unused. The dev build watches this: a generator that
// has quietly run out stops making tree crowns, and that should be visible
// rather than mysterious.
//
// Only distinct materials cost one. Mining never allocates -- taking a block
// out cannot change what anything is made of -- which is why the refusal this
// replaces is gone.
int marksFree();

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

// Returned by surfaceUnder where a body cannot stand at all.
constexpr uint8_t NO_SURFACE = 255;

// The surface a body currently at `fromZ` would come to rest on in this cell:
// the highest solid top no more than STEP_UP above it, with HEADROOM of air
// over it. Stepping down is unbounded; stepping up is not.
//
// The top of a run counts, which is what lets a player stand on a floor they
// built. It is also what keeps a two-high wall unclimbable — its top is two
// above your feet, and STEP_UP is one — so walling yourself in still works.
uint8_t surfaceUnder(int x, int y, int fromZ);

// True if a body standing at `fromZ` can enter this cell at all.
bool canEnter(int fromZ, int x, int y);

// True if a body of radius r centred here clears every cell it overlaps.
bool fits(int fromZ, float px, float py, float r);

// Height of the surface a body at (px, py) stands on, given where its feet are
// now. The two-argument form answers for the ground column alone, which is
// what anything that only ever stands on terrain wants.
uint8_t groundAt(float px, float py, int fromZ);
uint8_t groundAt(float px, float py);

// ---- mining -----------------------------------------------------------------

enum MineResult : uint8_t {
  MINE_PROGRESS,   // effort banked, the block is still standing
  MINE_BROKE,      // it came off this tick, and the drops are written
  MINE_NOTHING,    // air, out of bounds, or unbreakable
};

// Adds effort to one block. Returns MINE_BROKE on the tick it comes off, and
// only then writes the yields. Effort is discarded when the player looks away
// (resetDamage), which is what makes a tough block a commitment. dropMat is
// what came off, so the caller can put it in an inventory rather than on one
// anonymous pile.
//
// Taking a block out of the middle of a column splits it: the part above goes
// on standing. That is a tunnel. A column is a bitmask, so a hole is a cleared
// bit and mining cannot be refused for want of room.
MineResult mine(int x, int y, int z, int effort,
                uint8_t& dropMat, uint8_t& dropBlocks);

// Takes the top block off a column. Most callers mean this and should not have
// to work out which z it is.
//
// Named rather than overloaded on arity, and that is not style. It was
// `mine(x, y, effort, mat, blocks)` next to `mine(x, y, z, effort, mat,
// blocks)` — two signatures a single int apart — so a stale six-argument call
// left over from when there was a third out-param bound silently to the OTHER
// overload, mining z = 100000 and reporting nothing. It compiles, it runs, and
// the `while (height > GROUND)` loop around it never terminates. That happened
// three separate times during one refactor before the names were split.
bool mineTop(int x, int y, int effort,
             uint8_t& dropMat, uint8_t& dropBlocks);

void resetDamage(int x, int y, int z);
void resetDamage(int x, int y);

// 0..255 progress toward breaking, for the crosshair arc.
uint8_t damage(int x, int y, int z);
uint8_t damage(int x, int y);

enum PlaceResult : uint8_t {
  PLACE_OK,
  PLACE_OCCUPIED,  // already solid, out of the world, or above MAX_H
  PLACE_NO_ROOM,   // the material-marker pool is exhausted, world-wide
  PLACE_REFUSED,   // the border, or on top of a light source
};

// Puts a block at an exact position, joining it to whatever it touches. This
// is what lets a player build out into the air: a floor, a roof, a bridge.
PlaceResult place(int x, int y, int z, uint8_t mat);

// Adds a block on top of a column. False if it is already at MAX_H or is the
// map border.
bool place(int x, int y, uint8_t mat);

// Creeper blast: knocks blocks off every column within radius, most at the
// centre. Returns how many blocks it destroyed.
int explode(int cx, int cy, int radius);

}  // namespace world
