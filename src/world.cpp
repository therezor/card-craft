// =============================================================================
//  world.cpp — material table, terrain generation, mining
//
//  Terrain is two octaves of value noise mapped through a threshold: a bit
//  over half the map stays at height 0 so there is somewhere to walk and to be
//  caught in the open, and the rest ramps up into outcrops. Flat-then-ramp
//  rather than a straight scaling of the noise, because noise mapped directly
//  to height gives rolling dunes with nowhere flat and nothing to hide behind.
// =============================================================================
#include "world.h"

#include <math.h>
#include <string.h>

namespace world {

// Colours are chosen for separation *after* distance shading flattens them
// together — hence grass being unnaturally saturated, and coal not being black
// (a black block is indistinguishable from a far-away anything).
// Toughness is in effort, and the player supplies EFFORT_PER_TICK of it at
// TICK_HZ — so 960/second at level zero, and a value here divided by 960 is
// roughly the seconds that block takes by hand. These are set so grass comes
// away in under half a second and iron takes a committed two, because with
// one-metre cubes rather than full-height walls the old numbers broke eight
// blocks a second and mining had no weight at all.
// Distance fog pulls every colour toward the same grey, so blocks have to be
// separated by *brightness* as much as by hue — two materials the same hue and
// the same value are the same block as soon as they are six cells away. The
// warm family in particular used to be four near-identical browns (dirt, wood,
// plank, iron); they are now spread from very dark to very light.
//
// Rough luminance of each, for reference: wood 67, coal 80, dirt 102, brick
// 116, leaves 90, grass 132, stone 131, plank 155, iron 179, sand 208,
// torch 208, snow 241, lava 132, bedrock 46, masonry 103.
//
// Masonry is cut stone and it is deliberately not B_STONE. Two reasons, and
// the second is the one that forces it. It is darker and cooler than natural
// rock, so a tower standing against a stone hillside is still a tower. And it
// is a *structure* material: matAt makes a structure column out of itself all
// the way down, while a non-structure one gets the soil profile — so a keep
// built out of B_STONE would have had two courses of dirt inside it, one
// block under its own battlements.
static const BlockInfo kInfo[B_COUNT] = {
  // name       r    g    b   tough  blk
  { "grass",   82, 168,  62,   360,   1 },   // saturated green
  { "dirt",   140,  92,  50,   400,   1 },   // mid orange-brown
  { "stone",  128, 132, 138,  1150,   2 },   // neutral grey
  { "wood",    92,  60,  36,   760,   3 },   // very dark brown, the trunk
  { "leaves",  46, 120,  44,   220,   1 },   // darker than grass
  // Coal and iron carry their old ore yields as block yields now. They are the
  // slowest things on the map to dig and the only reason to go down, so a
  // single lump for two thousand effort would make the mine not worth entering.
  { "coal",    78,  80,  88,  1540,   2 },   // stone shot through with black
  { "iron",   192, 180, 166,  2100,   3 },   // stone shot through with pale ore
  { "sand",   228, 208, 148,   340,   1 },   // pale yellow, fine grained
  { "snow",   236, 242, 250,   300,   1 },   // white, almost smooth
  { "brick",  168,  96,  84,  1400,   2 },   // terracotta, not brown
  { "plank",  206, 152,  88,   470,   1 },   // light tan, clearly milled
  { "masonry", 96, 104, 122,  1500,   2 },   // cut stone, cool and dark
  { "torch",  252, 178,  56,   200,   1 },   // flame orange, not sand
  { "lava",   238,  96,  30,     0,   0 },   // unbreakable: bridge over it
  { "bedrock", 44,  46,  52,     0,   0 },
  // The deep reward. Toughness above iron's so the tier that mines it fastest
  // is the one it makes, and a single block per break because three would make
  // one lucky seam kit you out for the whole run. Cyan because every other
  // material on the map is warm or grey, and its luminance (~195) sits in the
  // gap between iron's 179 and sand's 208 -- fog flattens hue long before it
  // flattens brightness, so the gap is what keeps it readable at range.
  { "diamond",110, 232, 228,  3000,   1 },   // pale cyan, unmistakable in stone
};

// A structure column is made of itself all the way down to ground level: a
// brick wall is brick, a trunk is wood. A torch is deliberately NOT one — it
// is a single block sitting on top of whatever was already there, and treating
// it as a structure would make mining one reveal another torch underneath.
bool isStructure(uint8_t b) {
  return b == B_WOOD || b == B_LEAVES || b == B_BRICK || b == B_PLANK
      || b == B_MASONRY;
}

uint8_t emission(uint8_t b) {
  if (b == B_TORCH) return LIGHT_MAX;
  if (b == B_LAVA)  return LIGHT_MAX - 2;   // a glow, not a lantern
  return 0;
}

bool isHazard(uint8_t b) { return b == B_LAVA; }

const BlockInfo& info(uint8_t b) { return kInfo[b < B_COUNT ? b : 0]; }

static inline int idx(int x, int y) { return y * W + x; }

static inline bool outside(int x, int y) {
  return (unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H;
}

// ---- storage ----------------------------------------------------------------
//
// A column is a 32-bit occupancy mask, one bit a block, and MAX_H is exactly
// 32 so the world's full height is exactly one word.
//
// What this replaces is a heightmap plus a pool of up to three floating "runs"
// a cell could carry above it. That model could not describe a fourth hole, and
// the two refusals the game had to carry -- MINE_NO_ROOM and PLACE_NO_ROOM --
// were both the pool saying so. A bitmask has no such limit: any block anywhere
// can be taken out or put back, a column can be swiss cheese, and the walker
// finds its runs with a bit scan instead of chasing a linked list.
//
// Geometry and material are separate concerns here, which is the other half of
// the change. Removing a block cannot alter what anything is made of, so mining
// -- the common edit, and the one that used to be refused -- never allocates.
static uint32_t g_solid[W * H];

// The height the terrain generator left this column at, and the material of the
// block it left on top. Together they anchor the soil profile: grass over dirt
// over stone over ore falls out of depth below g_surf, so it costs no memory
// and cannot drift out of step with the terrain.
//
// Both are terrain, not state: mining and building do not move them. That is
// the fix for a bug this file already documents -- measuring depth from the
// CURRENT height meant digging down re-exposed the dirt band forever and stone
// was unreachable. g_surf is the anchor; nothing but the generator writes it.
static uint8_t g_surf[W * H];

// The natural surface material and the torch light, one byte for both.
//
// Neither needs eight bits: a material is one of fifteen and light runs 0..6,
// so they are four bits and three. Splitting them into two arrays cost nine
// kilobytes for six wasted bits a cell, and nine kilobytes is most of the
// margin the two framebuffers leave -- they are 129,600 bytes of the board's
// internal RAM and they have to be contiguous, so what is left over decides
// whether the game boots at all rather than merely how much it can hold.
constexpr uint8_t SMAT_MASK  = 0x0F;
constexpr uint8_t LIGHT_SHIFT = 4;
static uint8_t g_cell[W * H];

static inline uint8_t smatOf(int i)  { return (uint8_t)(g_cell[i] & SMAT_MASK); }
static inline uint8_t lightOf(int i) { return (uint8_t)(g_cell[i] >> LIGHT_SHIFT); }
static inline void setSmat(int i, uint8_t m) {
  g_cell[i] = (uint8_t)((g_cell[i] & ~SMAT_MASK) | (m & SMAT_MASK));
}
static inline void setLight(int i, uint8_t v) {
  g_cell[i] = (uint8_t)((g_cell[i] & SMAT_MASK) | (uint8_t)(v << LIGHT_SHIFT));
}

// ---- material markers -------------------------------------------------------
//
// Everything the depth profile cannot derive -- a trunk, a canopy, a brick
// wall, a plank floor, anything a player placed -- is a step function over z,
// stored as its steps.
//
// A marker says "from this z upward, the material is m", until the next marker.
// M_DERIVE means "from here upward, go back to the soil profile", and it is
// load-bearing rather than a tidy default: putting a plank into a tunnel mined
// out below the surface writes (z, PLANK) AND (z+1, DERIVE), because without
// the second one the plank's material would run on up over untouched ground.
//
// Why a marker list and not a fixed pair of material bands per cell: material
// identity is functional here, not cosmetic. emission(), isHazard() and
// rebuildLight() all key off it, so a scheme that has to coerce a third
// material into one of two bands does not merely mis-colour a block -- it can
// light a column that has no torch in it, or turn lava inert. There is no
// coercion in this one, so there is no such case to get wrong.
//
// The pool is global rather than per-cell, and mining never draws from it, so
// exhaustion needs thousands of distinct placements rather than four in one
// column. marksFree() is on the dev overlay so that stays visible.
constexpr uint8_t  M_DERIVE   = 255;
constexpr int      MARK_POOL_N = 4096;
constexpr uint16_t MARK_NIL   = 0xFFFF;

struct MatMark {
  uint8_t  base;   // material applies from here upward...
  uint8_t  mat;    // ...and is this, or M_DERIVE for "use the soil profile"
  uint16_t next;   // pool index, ascending by base, MARK_NIL ends the list
};

static MatMark  g_mark[MARK_POOL_N];
static uint16_t g_mhead[W * H];
static uint16_t g_freeMark = MARK_NIL;

static void marksInit() {
  for (int i = 0; i < MARK_POOL_N - 1; ++i) g_mark[i].next = (uint16_t)(i + 1);
  g_mark[MARK_POOL_N - 1].next = MARK_NIL;
  g_freeMark = 0;
  for (int i = 0; i < W * H; ++i) g_mhead[i] = MARK_NIL;
}

static uint16_t markAlloc() {
  if (g_freeMark == MARK_NIL) return MARK_NIL;
  const uint16_t n = g_freeMark;
  g_freeMark = g_mark[n].next;
  return n;
}

static void markRelease(uint16_t n) {
  g_mark[n].next = g_freeMark;
  g_freeMark = n;
}

static void markClear(int i) {
  uint16_t n = g_mhead[i];
  while (n != MARK_NIL) { const uint16_t nx = g_mark[n].next; markRelease(n); n = nx; }
  g_mhead[i] = MARK_NIL;
}

int marksFree() {
  int k = 0;
  for (uint16_t n = g_freeMark; n != MARK_NIL; n = g_mark[n].next) ++k;
  return k;
}

// The material the step function reads at z, before the soil profile is
// consulted: the highest marker at or below z.
static inline uint8_t markAt(int i, int z) {
  uint8_t m = M_DERIVE;
  for (uint16_t n = g_mhead[i]; n != MARK_NIL; n = g_mark[n].next) {
    if ((int)g_mark[n].base > z) break;      // sorted ascending
    m = g_mark[n].mat;
  }
  return m;
}

// Drops steps that say nothing: a marker equal to the one before it, a run of
// M_DERIVE at the bottom (which is what the absence of any marker already
// means), and any marker whose whole range has no solid block left in it.
//
// That last clause is what keeps the pool from being a slow leak. "Mining never
// allocates" is true, but without this it would still only ever grow: dig a
// built wall away and its markers would describe air forever.
static void markNormalise(int i) {
  const uint32_t solid = g_solid[i];
  uint16_t* link = &g_mhead[i];
  uint8_t   prev = M_DERIVE;
  while (*link != MARK_NIL) {
    const uint16_t n = *link;
    const int base = (int)g_mark[n].base;
    const uint16_t nx = g_mark[n].next;
    const int top = (nx == MARK_NIL) ? MAX_H : (int)g_mark[nx].base;

    // Rows this step covers, as a mask. Empty range or no solid bits under it
    // means the step describes nothing.
    const uint32_t span = (base >= top || base >= MAX_H)
        ? 0u
        : (((top >= MAX_H) ? ~0u : ((1u << top) - 1u)) & ~((1u << base) - 1u));

    if (g_mark[n].mat == prev || (span & solid) == 0u) {
      *link = nx;
      markRelease(n);
      continue;
    }
    prev = g_mark[n].mat;
    link = &g_mark[n].next;
  }
}

// Makes the step function read `mat` over [a, b), leaving what it read at and
// above b untouched. Returns false only if the pool is empty.
static bool markSetRange(int i, int a, int b, uint8_t mat) {
  if (a < 0) a = 0;
  if (b > MAX_H) b = MAX_H;
  if (a >= b) return true;

  const uint8_t after = markAt(i, b);        // read before anything moves

  // Drop every step strictly inside (a, b] -- they are about to be overwritten
  // or replaced by the closing marker below.
  uint16_t* link = &g_mhead[i];
  while (*link != MARK_NIL) {
    const uint16_t n = *link;
    const int base = (int)g_mark[n].base;
    if (base >= a && base <= b) { *link = g_mark[n].next; markRelease(n); continue; }
    if (base > b) break;
    link = &g_mark[n].next;
  }

  // Insert (a, mat), then (b, after) where b is still inside the world.
  const int nOpen = (b < MAX_H && after != mat) ? 2 : 1;
  uint16_t a0 = markAlloc();
  uint16_t a1 = (nOpen == 2) ? markAlloc() : MARK_NIL;
  if (a0 == MARK_NIL || (nOpen == 2 && a1 == MARK_NIL)) {
    if (a0 != MARK_NIL) markRelease(a0);
    if (a1 != MARK_NIL) markRelease(a1);
    return false;
  }

  g_mark[a0] = { (uint8_t)a, mat, MARK_NIL };
  link = &g_mhead[i];
  while (*link != MARK_NIL && (int)g_mark[*link].base < a) link = &g_mark[*link].next;
  g_mark[a0].next = *link;
  *link = a0;

  if (nOpen == 2) {
    g_mark[a1] = { (uint8_t)b, after, MARK_NIL };
    link = &g_mark[a0].next;
    while (*link != MARK_NIL && (int)g_mark[*link].base < b) link = &g_mark[*link].next;
    g_mark[a1].next = *link;
    *link = a1;
  }

  markNormalise(i);
  return true;
}

// ---- column geometry --------------------------------------------------------

// Blocks in the contiguous run resting on the base plane. This is what `height`
// meant when a column was a height, and most of the game still asks for it --
// but it is now derived from the mask rather than stored, so it cannot disagree
// with what is actually solid.
static inline int colHeight(uint32_t m) {
  const uint32_t gaps = ~m;
  return gaps == 0u ? MAX_H : __builtin_ctz(gaps);
}

// ---- derived accessors ------------------------------------------------------
//
// The old model stored a column height and a top material. Both are still the
// question most of the game asks, so both survive as names -- but they are read
// off the mask and the markers now rather than stored, so they cannot fall out
// of step with what is actually solid. That was a real class of bug here: mine()
// had to recompute the revealed material BEFORE moving the column, because
// asking afterwards asked about the block it had just removed.
static inline int ghAt(int i) { return colHeight(g_solid[i]); }

static uint8_t matAtIdx(int i, int z);          // defined with the access group

static inline uint8_t gtopAt(int i) {
  const int h = ghAt(i);
  return h == 0 ? smatOf(i) : matAtIdx(i, h - 1);
}

// The lowest solid block at or above z, or 255 for open sky. This is the "how
// much room is over this floor" question every movement rule asks.
//
// Deliberately not MAX_H when there is nothing up there: MAX_H is a real height
// a column can reach, so returning it would make the top HEADROOM blocks of the
// world behave as though something were resting on them.
// True where the cell carries anything above its ground column -- a roof, a
// bridge deck, a tree crown, a shelf a player built. What used to be "this cell
// has a run in its list".
static inline bool hasFloating(int i) {
  const int h = ghAt(i);
  return h < MAX_H && (g_solid[i] >> h) != 0u;
}

static inline int ceilingAbove(int i, int z) {
  if (z >= MAX_H) return 255;
  const uint32_t m = g_solid[i] & (z <= 0 ? ~0u : ~((1u << z) - 1u));
  return m ? __builtin_ctz(m) : 255;
}

// The run of solid blocks containing z, or an empty range if z is air.
// Both ends are found with a single bit scan; on Xtensa NSAU makes each of
// these one instruction.
static inline void runAround(uint32_t m, int z, int& base, int& top) {
  if (z < 0 || z >= MAX_H || !((m >> z) & 1u)) { base = top = 0; return; }
  const uint32_t below = ~m & ((1u << z) - 1u);         // gaps under z
  base = below ? (32 - __builtin_clz(below)) : 0;
  const uint32_t above = ~m >> z;                        // gaps at or above z
  top = above ? z + __builtin_ctz(above) : MAX_H;
}

// Sets the mask bits over [a, b). Geometry only -- material is the markers'
// business, and keeping the two apart is what makes mining allocation-free.
static inline void solidFill(int i, int a, int b) {
  if (a < 0) a = 0;
  if (b > MAX_H) b = MAX_H;
  if (a >= b) return;
  g_solid[i] |= (((b >= MAX_H) ? ~0u : ((1u << b) - 1u)) & ~((1u << a) - 1u));
}

static inline void solidClear(int i, int a, int b) {
  // Layer zero is bedrock and never comes out, whatever asks. Minecraft's floor
  // is a course of bedrock for the same reason: without it the bottom of a pit
  // is not a block, and a thing that is not a block has no texture to draw, no
  // face to outline and nothing for a pick to bite on. It used to be a "base
  // plane" that the renderer and the picker each had to special-case, and the
  // selection box could not be drawn on it at all because no span belonged to
  // it. Making it an ordinary block deletes all of that: it is unbreakable
  // because its toughness is zero, which is a rule the world already had.
  if (a < 1) a = 1;
  if (b > MAX_H) b = MAX_H;
  if (a >= b) return;
  g_solid[i] &= ~(((b >= MAX_H) ? ~0u : ((1u << b) - 1u)) & ~((1u << a) - 1u));
}

// Reshapes the terrain itself: the column becomes h blocks of natural ground
// with `mat` on top, and the soil anchor moves with it.
//
// Separate from setCol below, which stacks a material on top of terrain rather
// than being terrain. The generator does both -- levelArea cuts a building
// platform, placeTree grows a trunk -- and they are not the same edit: only
// this one is allowed to move g_surf.
static void setTerrain(int x, int y, int h, uint8_t mat) {
  if (outside(x, y)) return;
  if (h < 1) h = 1;              // the bedrock course; see solidClear
  if (h > MAX_H) h = MAX_H;
  const int i = idx(x, y);
  g_solid[i] = (h >= MAX_H) ? ~0u : ((1u << h) - 1u);
  g_surf[i]  = (uint8_t)h;
  setSmat(i, mat);
  markClear(i);
}
int32_t g_seed = 1;     // kept for biomeAt, which is queried after generate

// One mining target at a time. The player can only look at one column, so a
// full damage array would be kilobytes to store a single live number — and
// "effort resets when you look away" falls out of this for free.
static int      s_mineX = -1, s_mineY = -1, s_mineZ = -1;
static uint16_t s_effort = 0;

// ---- noise ------------------------------------------------------------------

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du;
  x ^= x >> 15; x *= 0x846ca68bu;
  x ^= x >> 16; return x;
}

// [0,1) from a lattice point. The odd multipliers are large primes; they keep
// x and y from aliasing onto each other along the diagonal.
static inline float latticef(int x, int y, uint32_t seed) {
  uint32_t h = hash32((uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u ^ seed);
  return (float)(h >> 8) * (1.0f / 16777216.0f);
}

// The raw hash, for callers that only want to compare against a threshold.
// matAt is on the render path — it runs for every block face drawn — and the
// float conversion and compare that latticef adds is pure overhead there.
static inline uint32_t latticeu(int x, int y, uint32_t seed) {
  return hash32((uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u ^ seed);
}

static float valueNoise(float x, float y, uint32_t seed) {
  const int xi = (int)floorf(x), yi = (int)floorf(y);
  const float xf = x - (float)xi, yf = y - (float)yi;
  const float u = xf * xf * (3.0f - 2.0f * xf);   // smoothstep, so hills have
  const float v = yf * yf * (3.0f - 2.0f * yf);   // rounded shoulders not facets
  const float a = latticef(xi,     yi,     seed);
  const float b = latticef(xi + 1, yi,     seed);
  const float c = latticef(xi,     yi + 1, seed);
  const float d = latticef(xi + 1, yi + 1, seed);
  const float top = a + (b - a) * u;
  const float bot = c + (d - c) * u;
  return top + (bot - top) * v;
}

// ---- generation -------------------------------------------------------------

constexpr int   BORDER      = 2;       // full-height bedrock ring, so no ray escapes
constexpr float NOISE_SCALE = 0.075f;  // ~13 cells per lattice step
constexpr float FLAT_BELOW  = 0.40f;   // noise under this stays at ground level
constexpr int   SPAWN_CLEAR = 7;

// How the noise above FLAT_BELOW maps to height. Not linear: a straight ramp
// over twenty-four blocks puts most of the map halfway up a slope, and a world
// with nowhere flat is a world with nowhere to be caught in the open and
// nowhere to build. The exponent, with the per-biome amplitude below, is what
// keeps the bulk of the map low and the tall ground rare — lowland with
// outcrops in it rather than dunes everywhere.
constexpr float RISE_CURVE  = 1.0f;

static void placeStructures(uint32_t seed);   // defined below, after the accessors
static void dropOrphanCanopy(int cx, int cy);  // ditto
static float biomeNoise(int x, int y);         // ditto
static void terrainShape(float bn, float& amp, float& rough);
static uint8_t surfaceOf(uint8_t biome);      // ditto
static void setSlab(int x, int y, int base, int top, uint8_t mat);


void generate(uint32_t seed) {
  g_seed = seed;
  marksInit();
  s_mineX = s_mineY = s_mineZ = -1;
  s_effort = 0;

  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);
      if (x < BORDER || y < BORDER || x >= W - BORDER || y >= H - BORDER) {
        g_solid[i] = ~0u;
        g_surf[i] = MAX_H;
        setSmat(i, B_BEDROCK);
        continue;
      }

      const float fx = (float)x * NOISE_SCALE, fy = (float)y * NOISE_SCALE;
      // The second octave breaks up the smooth blobs the first alone produces,
      // which otherwise look like a lava lamp rather than rock. How much of it
      // goes in is the biome's business: a dune is the first octave nearly
      // alone, and a tundra peak is most of the second.
      const uint8_t biome = biomeAt(x, y);
      float amp, rough;
      terrainShape(biomeNoise(x, y), amp, rough);
      const float n = valueNoise(fx, fy, seed) * (1.0f - rough)
                    + valueNoise(fx * 2.9f, fy * 2.9f, seed ^ 0x9e3779b9u) * rough;

      int rise = 0;
      if (n > FLAT_BELOW) {
        const float t = (n - FLAT_BELOW) * (1.0f / (1.0f - FLAT_BELOW));
        rise = 1 + (int)(powf(t, RISE_CURVE) * amp * (float)(MAX_H - GROUND - 1));
      }
      int h = GROUND + rise;
      if (h > MAX_H) h = MAX_H;

      // The rock and snow lines are heights now, not noise thresholds. On a
      // world six blocks tall those were the same thing; on one twenty-four
      // blocks tall a peak has to change material as it climbs, or a mountain
      // is a grass cone. Snow above the rock, in every biome — ground high
      // enough is above the snow line whatever grows at its foot, and that is
      // the single strongest cue that the world has real height now.
      const uint8_t surface = surfaceOf(biome);
      uint8_t top = surface;
      if (rise >= 16) {
        top = B_SNOW;
      } else if (rise >= 10) {
        top = B_STONE;                 // exposed rock on the high ground
      } else if (rise >= 4) {
        top = (latticef(x, y, seed ^ 0xD1D7u) < 0.4f) ? B_DIRT : surface;
      }

      g_solid[i] = (h >= MAX_H) ? ~0u : ((1u << h) - 1u);
      g_surf[i] = (uint8_t)h;
      setSmat(i, top);
    }
  }

  placeStructures(seed);
  rebuildLight();

  // Last, so it clears anything a structure dropped on the spawn point.
  // Guarantee a flat landing pad, otherwise generate() can drop the player
  // inside a hill.
  const int cx = W / 2, cy = H / 2;
  for (int y = cy - SPAWN_CLEAR; y <= cy + SPAWN_CLEAR; ++y) {
    for (int x = cx - SPAWN_CLEAR; x <= cx + SPAWN_CLEAR; ++x) {
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > SPAWN_CLEAR * SPAWN_CLEAR) continue;
      if (isBorder(x, y)) continue;
      // Everything in the column, not just the ground: clearing the column and
      // leaving what floated over it behind is how you get a roof, an eave or a
      // tree crown hanging over the landing pad with nothing under it. One mask
      // write now says all of that.
      setTerrain(x, y, GROUND, surfaceOf(biomeAt(x, y)));
    }
  }
  // A tree standing just inside the pad hangs its crown just outside it, and
  // wiping the trunk on its own leaves the crown in the air. The sweep looks at
  // the ring of cells around each one it is given, so running it over the pad
  // covers everything the pad could have held up.
  for (int y = cy - SPAWN_CLEAR; y <= cy + SPAWN_CLEAR; ++y)
    for (int x = cx - SPAWN_CLEAR; x <= cx + SPAWN_CLEAR; ++x)
      dropOrphanCanopy(x, y);

}


// ---- structures -------------------------------------------------------------
//
// Placed after the noise, onto ground the noise left flat. Each one is a shape
// the terrain generator could never produce on its own, and each one is there
// for a reason the player will feel: a tree is the best block-per-effort trade
// in the game, a house is shelter that is already lit, a castle is the landmark
// you navigate by, and a quarry is the only place ore is visible from the
// surface.
//
// All of it is sized against a world twenty-four blocks tall. At the six it
// used to be, every one of these was three or four blocks and the whole map
// read as a scattering of large cubes — which is a resolution problem and not
// a modelling one. A keep is eighteen blocks now, a tree is nine, and the
// blocks look small because there are enough of them to make a shape.

// Free ground: at the untouched height, not built on, and — the part that is
// easy to forget — nothing already hanging over it. Without the slab test a
// tree grows through a bridge deck and a house is built under an arch, because
// a slab leaves the column beneath it looking exactly like open field.
static inline bool flatAt(int x, int y) {
  if (isBorder(x, y)) return false;
  const int i = idx(x, y);
  return ghAt(i) == GROUND && !isStructure(gtopAt(i)) && !hasFloating(i);
}

static bool flatArea(int x0, int y0, int w, int d) {
  for (int y = y0; y < y0 + d; ++y)
    for (int x = x0; x < x0 + w; ++x)
      if (!flatAt(x, y)) return false;
  return true;
}

// Ground that is not necessarily flat but is clear: nothing built, nothing
// hanging, and no more slope than `drop` across it. What a tower footing needs,
// where a house needs a level floor.
static bool clearArea(int x0, int y0, int w, int d, int drop) {
  int lo = MAX_H, hi = 0;
  for (int y = y0; y < y0 + d; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      if (isBorder(x, y)) return false;
      const int i = idx(x, y);
      if (isStructure(gtopAt(i)) || hasFloating(i)) return false;
      if (ghAt(i) < lo) lo = ghAt(i);
      if (ghAt(i) > hi) hi = ghAt(i);
    }
  return hi - lo <= drop;
}

static void setCol(int x, int y, int h, uint8_t top);

// Cuts a level platform and reports the height it settled on.
//
// Buildings used to demand ground already at exactly GROUND. That was nearly
// free on a world six blocks tall, where four fifths of the map was dead flat;
// on one that actually has hills it meant three seeds in ten had no house on
// them at all and two had no castle, because the footprint never found a
// perfectly level patch big enough. A builder picks a spot and cuts a
// platform, so this does too — and a village on a levelled shelf is a better
// thing to look at than a village that is not there.
//
// The mean rather than the minimum, so a site is half cut and half filled and
// the platform does not sit in a pit. Cutting exposes whatever was under the
// column; filling puts the local surface material down.
static int levelArea(int x0, int y0, int w, int d) {
  int sum = 0, n = 0;
  for (int y = y0; y < y0 + d; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      if (outside(x, y)) continue;
      sum += ghAt(idx(x, y));
      ++n;
    }
  if (n == 0) return GROUND;
  const int h = (sum + n / 2) / n;
  for (int y = y0; y < y0 + d; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      if (outside(x, y) || isBorder(x, y)) continue;
      const int i = idx(x, y);
      const int cur = ghAt(i);
      if (cur == h) continue;
      // The biome surface either way, cut or filled. Exposing the subsoil
      // where the platform was cut is both uglier — a village yard half grass
      // and half dirt — and a quiet lie about the world: every other patch of
      // ground at the untouched height shows what the biome grows, and code
      // that reads the map has no way to know this one is different.
      setCol(x, y, h, surfaceOf(biomeAt(x, y)));
    }
  return h;
}

// Raises (or cuts) a column to h blocks with `top` on its cap.
//
// Terrain underneath is left alone: g_surf does not move, so a trunk grown on a
// hillside still has that hillside's soil profile under it rather than becoming
// wood all the way down. What used to make that work was matAt special-casing
// isStructure() materials; it is markers now, and the marker says exactly the
// same thing the special case did.
static void setCol(int x, int y, int h, uint8_t top) {
  if (outside(x, y) || isBorder(x, y)) return;
  if (h < 1) h = 1;              // the bedrock course; see solidClear
  if (h > MAX_H) h = MAX_H;
  const int i = idx(x, y);
  const int surf = (int)g_surf[i];

  g_solid[i] = (h >= MAX_H) ? ~0u : ((1u << h) - 1u);

  if (isStructure(top)) {
    // A tree is a trunk under its canopy, not leaves on a dirt plug, and a ruin
    // wall is brick to the ground: a structure's interior is made of itself.
    // One block of cap material, the body below it -- and leaves cap a trunk.
    const uint8_t body = (top == B_LEAVES) ? B_WOOD : top;
    markSetRange(i, GROUND < h - 1 ? GROUND : h - 1, h - 1, body);
    markSetRange(i, h - 1, h, top);
  } else {
    // Not a structure: everything from the natural surface up is this material,
    // and a column cut BELOW the surface still shows it on the freshly cut cap.
    markSetRange(i, surf < h - 1 ? surf : h - 1, h, top);
  }
}

static inline int groundOf(int x, int y) {
  return outside(x, y) ? GROUND : ghAt(idx(x, y));
}

// ---- trees ------------------------------------------------------------------

// A trunk with a canopy that hangs out over open ground.
//
// The old tree was a column of leaves with shorter columns of leaves beside it,
// and it read as a shrub for a reason no amount of tuning was going to fix: a
// canopy is leaves *over air*, one cell out from a trunk that is not underneath
// them, and a heightmap cannot say that. Leaves standing on the ground are a
// bush however tall you make them.
//
// Slabs can say it — the same second run per cell that roofs a ruin and spans
// an arch. So the crown is slabs, and what you get is a tree you walk *under*
// rather than a green post you walk around.
//
// A Minecraft tree: a short trunk carrying a crown four layers deep and five
// cells across, not a mast with a plate on top.
//
// The shape this replaces was a nine-to-eleven block trunk under a crown three
// deep, which from any distance reads as two or three green blocks on a stick.
// Almost all of that was trunk. Minecraft's oak is the other way round — six
// logs and a crown that is most of what you see — and the reason is that the
// crown is what a tree IS at a distance. The canopy is also what makes a forest
// somewhere you can lose a mob rather than a field of poles.
//
// Layers, measured from the first z above the top log, exactly as Minecraft
// stacks them:
//
//     trunkTop      a plus: the four orthogonal neighbours and the trunk cap
//     trunkTop-1    3x3
//     trunkTop-2    5x5, corners off
//     trunkTop-3    5x5, corners off
//
// A cell's leaves are one contiguous run, so each gets a single setSlab rather
// than a layer at a time — the runs would merge into exactly this anyway.
//
// The crown comes down when the trunk does, but not all at once: see
// world::decayTick.
static bool placeTree(int x, int y, uint32_t& rng) {
  if (!flatAt(x, y)) return false;

  const uint8_t biome = biomeAt(x, y);
  if (biome == BIOME_DESERT) return false;          // cactus country; see below

  rng = rng * 1664525u + 1013904223u;
  // A tundra spruce is taller and narrower than a plains oak. At the size a
  // tree occupies six cells away the only thing that separates two of them is
  // the outline, so the variants differ in shape and not in colour.
  const bool pine = (biome == BIOME_TUNDRA);
  const int logs = (pine ? 9 : 6) + (int)((rng >> 16) % 2u);
  const int trunkTop = GROUND + logs;               // first z above the top log

  // Where the crown starts. Three below the top log for an oak leaves three or
  // four blocks of clear air under the eaves — HEADROOM is two, so the player
  // and the mobs both walk under it and a canopy is shade rather than a wall.
  const int skirt = pine ? trunkTop - 5 : trunkTop - 3;

  setCol(x, y, trunkTop, B_WOOD);                   // wood all the way down
  // The trunk's own cap: exactly one block of leaves, oak or spruce. One,
  // because the whole column below it is trunk and the rest of the game reads
  // a tree as "wood under a single leaf cap" — chop through two and the second
  // one is a leaf you got where a log should have been.
  setSlab(x, y, trunkTop, trunkTop + 1, B_LEAVES);

  // A canopy cell is refused where the ground would come up into it, and where
  // it would leave too little air underneath to walk through. The old rule
  // wanted dead flat ground under every leaf, which is why a tree on any kind
  // of slope lost half its crown; this wants only the clearance that makes a
  // canopy shade rather than a wall.
  auto leaves = [](int nx, int ny, int base, int top) {
    if (outside(nx, ny) || isBorder(nx, ny)) return;
    // Somewhere a body could already stand, and still somewhere a body can
    // stand once the leaves are over it. The second test alone is not enough:
    // the cell may already be roofed by a neighbouring crown lower than this
    // one, and adding to it would not be what made it unwalkable but would
    // certainly be found holding the bag.
    if (!standable(nx, ny)) return;
    if (base - (int)ghAt(idx(nx, ny)) < HEADROOM) return;
    setSlab(nx, ny, base, top, B_LEAVES);
  };

  static const int kR1[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
  // The outer ring without its four corners: a full 5x5 crown is a square, and
  // a square is the one shape a tree never is.
  static const int kR2[12][2] = { {2,0},{-2,0},{0,2},{0,-2},
                                  {2,1},{2,-1},{-2,1},{-2,-1},
                                  {1,2},{-1,2},{1,-2},{-1,-2} };

  for (int i = 0; i < 8; ++i) {
    const int nx = x + kR1[i][0], ny = y + kR1[i][1];
    // Orthogonal neighbours reach the top layer; the diagonals stop one short,
    // which is what turns the crown's cap into a plus instead of a block.
    const int top = (i < 4) ? trunkTop + 1 : trunkTop;
    leaves(nx, ny, skirt, pine ? trunkTop - 1 : top);
  }

  for (int i = 0; i < 12; ++i) {
    rng = rng * 1664525u + 1013904223u;
    if ((rng >> 16) % 100u >= 82u) continue;        // ragged, not stamped
    const int nx = x + kR2[i][0], ny = y + kR2[i][1];
    // Two layers for an oak, and for a spruce a skirt that stops well below
    // the middle of the tree so the silhouette tapers.
    leaves(nx, ny, skirt, pine ? skirt + 2 : trunkTop - 1);
  }
  return true;
}

// Clears leaves that have nothing left to hang from.
//
// This runs at GENERATION time only, and the distinction is the whole point of
// it. The generator cuts platforms and clears a spawn pad out from under trees
// it has already grown, and a world handed to the player with crowns floating
// over nothing is a world that looks broken before it is touched.
//
// What it deliberately does NOT do any more is run when the player chops. Cut
// the trunk out from under an oak in Minecraft and the crown stays where it is;
// you climb it, or you knock it down a block at a time, or you leave it. Doing
// this on every mine meant a whole canopy winked out on the frame the last log
// came off, which is not a thing that happens in the game this is imitating and
// took the harvest with it.
//
// Tested per canopy cell rather than per trunk, because two trees can stand
// close enough to share one and only one of them is being cut down. A slab is
// held up if any column within two cells is wood or leaves and still reaches
// the slab's underside — which is the actual rule, with no height constant in
// it to drift out of step with placeTree.
static void dropOrphanCanopy(int cx, int cy) {
  for (int y = cy - 2; y <= cy + 2; ++y)
    for (int x = cx - 2; x <= cx + 2; ++x) {
      if (outside(x, y)) continue;
      const int i = idx(x, y);
      // Every floating run of this cell, not just the first: a cell can carry
      // any number of them now, and a crown left hanging is exactly what this
      // sweep is for. Runs are found by bit scan, walking up from the top of
      // the ground column.
      for (int z = ghAt(i); z < MAX_H; ) {
        if (!((g_solid[i] >> z) & 1u)) { ++z; continue; }
        int base, top;
        runAround(g_solid[i], z, base, top);
        z = top;
        if (matAtIdx(i, base) != B_LEAVES) continue;

        bool held = false;
        for (int ty = y - 2; ty <= y + 2 && !held; ++ty)
          for (int tx = x - 2; tx <= x + 2 && !held; ++tx) {
            if (outside(tx, ty)) continue;
            const int j = idx(tx, ty);
            held = (gtopAt(j) == B_WOOD || gtopAt(j) == B_LEAVES)
                   && ghAt(j) >= base;
          }
        if (held) continue;
        solidClear(i, base, top);
        markNormalise(i);
      }
    }
}

// ---- scatter ----------------------------------------------------------------
//
// The small stuff, one or two cells each. It is what stops a biome reading as
// an empty field with the interesting things parked on it: you should be able
// to tell where you are from the litter, not only from the colour of the floor.

// Sand column with arms, which is as close to a saguaro as one cell gets. The
// arms are slabs, so they stand off the trunk with air under them.
static bool placeCactus(int x, int y, uint32_t& rng) {
  if (!flatAt(x, y) || biomeAt(x, y) != BIOME_DESERT) return false;
  rng = rng * 1664525u + 1013904223u;
  // Four blocks at the shortest, because the arms hang two below the top and
  // anything less than HEADROOM over the sand is an arm you walk into rather
  // than under. Every slab in this generator has to clear that, or the map
  // grows obstacles that look like scenery.
  const int top = GROUND + 4 + (int)((rng >> 16) % 3u);
  setCol(x, y, top, B_LEAVES);            // the one green thing in a desert
  static const int kD[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
  for (int i = 0; i < 4; ++i) {
    rng = rng * 1664525u + 1013904223u;
    if ((rng >> 16) % 100u >= 38u) continue;
    const int nx = x + kD[i][0], ny = y + kD[i][1];
    if (flatAt(nx, ny)) setSlab(nx, ny, top - 2, top - 1, B_LEAVES);
  }
  return true;
}

// A lump of rock two or three cells across, domed rather than cubic.
static bool placeBoulder(int cx, int cy, uint32_t& rng) {
  rng = rng * 1664525u + 1013904223u;
  const int r = 1 + (int)((rng >> 16) & 1u);
  if (!clearArea(cx - r, cy - r, 2 * r + 1, 2 * r + 1, 1)) return false;
  for (int y = cy - r; y <= cy + r; ++y)
    for (int x = cx - r; x <= cx + r; ++x) {
      const int dx = x - cx, dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r + 1) continue;
      setCol(x, y, groundOf(x, y) + (d2 == 0 ? 3 : (d2 <= 1 ? 2 : 1)), B_STONE);
    }
  return true;
}

// A rock needle: one cell wide and eight to fourteen tall. Useless, unmissable,
// and the cheapest landmark in the generator.
static bool placeSpire(int x, int y, uint32_t& rng) {
  if (!clearArea(x - 1, y - 1, 3, 3, 2)) return false;
  rng = rng * 1664525u + 1013904223u;
  const int h = 8 + (int)((rng >> 16) % 7u);
  const int base = groundOf(x, y);
  setCol(x, y, base + h, B_STONE);
  // A shoulder or two, so it tapers instead of standing like a chimney.
  static const int kD[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
  for (int i = 0; i < 4; ++i) {
    rng = rng * 1664525u + 1013904223u;
    if ((rng >> 16) % 100u >= 50u) continue;
    const int nx = x + kD[i][0], ny = y + kD[i][1];
    if (!outside(nx, ny) && !isBorder(nx, ny)
        && !hasFloating(idx(nx, ny)))
      setCol(nx, ny, base + 2 + (int)((rng >> 8) % (uint32_t)(h / 2)), B_STONE);
  }
  return true;
}

// A dead trunk with no crown. Tundra only, where the pines thin out.
static bool placeSnag(int x, int y, uint32_t& rng) {
  if (!flatAt(x, y) || biomeAt(x, y) != BIOME_TUNDRA) return false;
  rng = rng * 1664525u + 1013904223u;
  // Deliberately clear of GROUND + 5, which is a house's corner post: two bare
  // wood columns of the same height are the same thing to anything reading the
  // map, tests included.
  setCol(x, y, GROUND + 6 + (int)((rng >> 16) % 3u), B_WOOD);
  return true;
}

// ---- buildings --------------------------------------------------------------

// A house: three materials at three brightnesses, a door you walk through,
// windows you can see through and cannot climb, a roof that overhangs its own
// walls, and a torch burning inside it.
//
// Every one of those is there to be read at distance rather than to be pretty.
// Fog pulls every colour toward the same grey, so the three materials are
// picked from opposite ends of the luminance list at the top of this file —
// wood 67 for the corner posts, plank 155 for the walls, brick 116 for the
// roof — and what survives ten cells of haze is a light box with dark posts at
// its corners. The eave is what tells a roof from a lid. The torch is why you
// run for one at dusk: it lights the inside, and the spawner will not put a
// mob anywhere lit.
static bool placeHouse(int x0, int y0, int w, int d, uint32_t& rng) {
  // Two cells of margin rather than one: the roof overhangs by a cell, and an
  // eave landing on somebody else's wall is a cell with two things in it and
  // room for one.
  if (!clearArea(x0 - 2, y0 - 2, w + 4, d + 4, 4)) return false;
  const int base = levelArea(x0 - 2, y0 - 2, w + 4, d + 4);
  if (base + 6 > MAX_H || base < 2) return false;

  rng = rng * 1664525u + 1013904223u;
  const int side = (int)((rng >> 16) % 4u);
  rng = rng * 1664525u + 1013904223u;
  const int doorAt = 1 + (int)((rng >> 16) % (uint32_t)((side < 2 ? w : d) - 2));

  const int wallTop = base + 5;
  int doorX = -1, doorY = -1;

  for (int y = y0; y < y0 + d; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      const int ox = x - x0, oy = y - y0;
      const bool ex = (ox == 0 || ox == w - 1);
      const bool ey = (oy == 0 || oy == d - 1);
      if (!ex && !ey) continue;                     // the floor, left as it is

      if (ex && ey) { setCol(x, y, wallTop, B_WOOD); continue; }   // corner post

      const int wall  = (oy == 0) ? 0 : (oy == d - 1) ? 1 : (ox == 0) ? 2 : 3;
      const int along = (wall < 2) ? ox : oy;
      const int span  = (wall < 2) ? w  : d;

      if (wall == side && along == doorAt) {        // the threshold, left open
        doorX = x; doorY = y;
        continue;
      }
      // One window per wall, in the middle of it, and two courses off the
      // ground. STEP_UP is one — so a zombie can see you through it and cannot
      // come through it, which is the whole reason a window is not a doorway.
      if (along == span / 2) { setCol(x, y, base + 3, B_PLANK); continue; }

      setCol(x, y, wallTop, B_PLANK);
    }
  }

  // The lintel over the door and the eave over the lintel are one run, because
  // a cell carries one slab. Split into two, the roof pass below would
  // overwrite whichever went down first and the eave would have a hole in it
  // exactly where the door is. Its underside sits HEADROOM above the
  // threshold, which is what canEnter asks for and is therefore walkable.
  if (doorX >= 0) setSlab(doorX, doorY, base + HEADROOM, wallTop + 1, B_PLANK);

  // The roof, the wall course it caps, and one ring of eave past that. Cells
  // that already carry a slab are skipped — the door's lintel is one, and so
  // is an arch deck if a footprint ever overlaps one.
  for (int y = y0 - 1; y <= y0 + d; ++y)
    for (int x = x0 - 1; x <= x0 + w; ++x) {
      if (outside(x, y) || isBorder(x, y) || hasFloating(idx(x, y))) continue;
      setSlab(x, y, wallTop, wallTop + 1, B_BRICK);
    }

  return true;
}

// Three to five houses around a common yard. One house on a hillside is a
// curiosity; a village is a place -- but it is an unlit one. Nothing on the
// map emits any more except lava, because light is the one thing the player
// has to make, and a village that came with its own torches was four free
// ones sitting in a building you could also shelter in.
static bool placeVillage(int cx, int cy, uint32_t& rng) {
  constexpr int SPAN = 13;                 // yard plus a house on each side
  if (!clearArea(cx - SPAN, cy - SPAN, 2 * SPAN + 1, 2 * SPAN + 1, 5)) return false;
  const int base = levelArea(cx - SPAN, cy - SPAN, 2 * SPAN + 1, 2 * SPAN + 1);
  if (base + 6 > MAX_H || base < 2) return false;

  int built = 0;
  static const int kAt[6][2] = { {-11,-9}, {3,-9}, {-11,4}, {3,4}, {-4,-12}, {-4,7} };
  rng = rng * 1664525u + 1013904223u;
  const int want = 3 + (int)((rng >> 16) % 3u);
  for (int i = 0; i < 6 && built < want; ++i) {
    rng = rng * 1664525u + 1013904223u;
    const int w = 6 + (int)((rng >> 16) % 2u);
    rng = rng * 1664525u + 1013904223u;
    const int d = 6 + (int)((rng >> 16) % 2u);
    if (placeHouse(cx + kAt[i][0], cy + kAt[i][1], w, d, rng)) ++built;
  }
  return built >= 2;
}

// A ruin: a brick enclosure with one doorway, a slab roof over it, and a course
// or two knocked out of a wall. Four walls taller than a mob can step, which is
// the entire point of running to one at dusk.
//
// The gaps are not damage for its own sake. A ruin and the house above it are
// the same silhouette at the distance either is first seen from, and a broken
// wall is the one difference that survives being ten cells away and half in
// fog. One is shelter you repair; the other is shelter you can just walk into.
static bool placeRuin(int x0, int y0, int w, int d, uint32_t& rng) {
  if (!clearArea(x0 - 1, y0 - 1, w + 2, d + 2, 4)) return false;
  const int base = levelArea(x0 - 1, y0 - 1, w + 2, d + 2);
  if (base + 6 > MAX_H || base < 2) return false;

  rng = rng * 1664525u + 1013904223u;
  const int side = (int)((rng >> 16) % 4u);
  rng = rng * 1664525u + 1013904223u;
  const int doorW = 1 + (int)((rng >> 16) % 2u);      // one or two cells wide
  rng = rng * 1664525u + 1013904223u;
  const int doorAt = 1 + (int)((rng >> 16) % (uint32_t)((side < 2 ? w : d) - doorW - 1));

  const int wallTop = base + 5;
  for (int y = y0; y < y0 + d; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      const bool edge = (x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + d - 1);
      if (!edge) continue;

      bool door = false;
      const int ox = x - x0, oy = y - y0;
      if      (side == 0 && oy == 0     && ox >= doorAt && ox < doorAt + doorW) door = true;
      else if (side == 1 && oy == d - 1 && ox >= doorAt && ox < doorAt + doorW) door = true;
      else if (side == 2 && ox == 0     && oy >= doorAt && oy < doorAt + doorW) door = true;
      else if (side == 3 && ox == w - 1 && oy >= doorAt && oy < doorAt + doorW) door = true;
      if (door) continue;

      setCol(x, y, wallTop, B_BRICK);
    }
  }

  // Knock a course or two out. Never a corner, and never so low that a mob can
  // step in: two blocks of wall are left, so what the gap costs the player is
  // the look of the thing rather than the shelter.
  rng = rng * 1664525u + 1013904223u;
  const int gaps = 1 + (int)((rng >> 16) & 1u);
  for (int g = 0; g < gaps; ++g) {
    rng = rng * 1664525u + 1013904223u;
    const int gx = x0 + 1 + (int)((rng >> 16) % (uint32_t)(w - 2));
    rng = rng * 1664525u + 1013904223u;
    const int gy = ((rng >> 16) & 1u) ? y0 : y0 + d - 1;
    const int gi = idx(gx, gy);
    if (ghAt(gi) == wallTop && gtopAt(gi) == B_BRICK)
      setCol(gx, gy, base + 2, B_BRICK);
  }

  // Roof it. This is the shape a plain heightmap cannot make: a slab of brick
  // over open air, with a floor you can stand on underneath. It is also the
  // only fully enclosed shelter in the game — mobs cannot spawn under it once
  // it is lit, and they cannot drop in from above.
  for (int y = y0 + 1; y < y0 + d - 1; ++y)
    for (int x = x0 + 1; x < x0 + w - 1; ++x)
      setSlab(x, y, wallTop, wallTop + 1, B_BRICK);
  return true;
}

// ---- masonry ----------------------------------------------------------------

// Crenellations: the top course of a wall, every other cell a block higher.
// Two blocks of difference read as a notch at any distance the wall is visible
// from; one reads as a rounding error.
static inline void crenellate(int x, int y, int top) {
  setCol(x, y, top + (((x + y) & 1) ? 2 : 0), B_MASONRY);
}

// A keep: a solid block of cut stone, fourteen to eighteen blocks up, notched
// at the top, with a stair winding round the outside of it.
//
// The stair is the reason this is worth building rather than drawing. STEP_UP
// is one block, so a ring of cells each one higher than the last is climbable
// by construction — and by anything else with legs, which is the trade. What
// you get is height you can fight from and a thing on the skyline you can
// navigate by, and both of those were impossible in a world nine blocks tall.
static bool placeTower(int cx, int cy, uint32_t& rng) {
  constexpr int R = 2;                     // a 5x5 keep
  if (!clearArea(cx - R - 2, cy - R - 2, 2 * R + 5, 2 * R + 5, 5)) return false;
  const int base = levelArea(cx - R - 2, cy - R - 2, 2 * R + 5, 2 * R + 5);
  if (base < 2) return false;
  rng = rng * 1664525u + 1013904223u;
  const int top = base + 14 + (int)((rng >> 16) % 5u);
  if (top + 2 > MAX_H) return false;

  for (int y = cy - R; y <= cy + R; ++y)
    for (int x = cx - R; x <= cx + R; ++x) {
      const bool edge = (x == cx - R || x == cx + R || y == cy - R || y == cy + R);
      if (edge) crenellate(x, y, top);
      else      setCol(x, y, top, B_MASONRY);      // the roof you stand on
    }

  // The stair, one cell out from the wall, winding anticlockwise. It stops
  // where it meets the parapet rather than running past it into the air.
  static const int kRing[24][2] = {
    {-3,-3},{-2,-3},{-1,-3},{0,-3},{1,-3},{2,-3},{3,-3},
    {3,-2},{3,-1},{3,0},{3,1},{3,2},{3,3},
    {2,3},{1,3},{0,3},{-1,3},{-2,3},{-3,3},
    {-3,2},{-3,1},{-3,0},{-3,-1},{-3,-2}
  };
  for (int i = 0; i < 24; ++i) {
    const int h = base + 1 + i;
    if (h >= top) break;
    setCol(cx + kRing[i][0], cy + kRing[i][1], h, B_MASONRY);
  }
  return true;
}

// A castle: four corner towers, a curtain wall between them, and a gatehouse
// you walk through. The one thing on the map big enough that you see it before
// you can see what it is made of.
static bool placeCastle(int x0, int y0, int sz, uint32_t& rng) {
  if (!clearArea(x0 - 1, y0 - 1, sz + 2, sz + 2, 6)) return false;
  const int base = levelArea(x0 - 1, y0 - 1, sz + 2, sz + 2);
  if (base < 2) return false;
  const int curtain = base + 8;
  const int tower   = base + 15;
  if (tower + 2 > MAX_H) return false;

  rng = rng * 1664525u + 1013904223u;
  const int gate = (int)((rng >> 16) % 4u);          // which wall the gate is in
  const int gateAt = sz / 2;

  for (int y = y0; y < y0 + sz; ++y) {
    for (int x = x0; x < x0 + sz; ++x) {
      const int ox = x - x0, oy = y - y0;
      const bool ex = (ox <= 1 || ox >= sz - 2);
      const bool ey = (oy <= 1 || oy >= sz - 2);
      if (ex && ey) { crenellate(x, y, tower); continue; }   // corner tower, 2x2

      const bool edge = (ox == 0 || ox == sz - 1 || oy == 0 || oy == sz - 1);
      if (!edge) continue;                                   // the bailey

      const int wall  = (oy == 0) ? 0 : (oy == sz - 1) ? 1 : (ox == 0) ? 2 : 3;
      const int along = (wall < 2) ? ox : oy;
      // The gate: a two-cell opening with the wall carried over it on a slab,
      // so it is an arch rather than a hole. One slab per cell, so the whole
      // thickness of the wall above the gate is that one run.
      if (wall == gate && (along == gateAt || along == gateAt + 1)) {
        setSlab(x, y, base + HEADROOM, curtain + 1, B_MASONRY);
        continue;
      }
      crenellate(x, y, curtain);
    }
  }

  // The bailey is bare. A castle is eight blocks of cut stone and nothing else
  // -- it is shelter, not a lit camp, and what you do with the dark inside it
  // is your own problem.
  return true;
}

// An open pit cut past the stone line. Not a cave — nothing here can roof one
// over — but it does the job a cave does in this game: it is where ore shows
// on the surface, and it is a hole you can be cornered in.
static bool placeQuarry(int cx, int cy, int r, uint32_t& rng) {
  // Some quarries have found something. A pool of lava at the bottom of a pit
  // is a light source, a hazard and a reason to carry blocks to bridge with,
  // all from one material — and it makes the ore down there cost something.
  rng = rng * 1664525u + 1013904223u;
  const bool molten = ((rng >> 16) % 100u) < 45u;

  // Lowland only, and clear of anything already built. Not required to be dead
  // flat — cutting into the side of a rise is what a quarry looks like — but a
  // tall hill would leave a cliff the player cannot climb back out of.
  for (int y = cy - r; y <= cy + r; ++y)
    for (int x = cx - r; x <= cx + r; ++x) {
      if (outside(x, y) || isBorder(x, y)) return false;
      const int i = idx(x, y);
      // Not onto ground that has already been cut. A quarry reads the material
      // it exposes out of the column it is about to shorten, so a second one
      // laid over the floor of the first asks for the material of a layer that
      // is no longer there — and matAt answers B_BEDROCK, which isBorder() then
      // reports as the edge of the map. The result was a patch of unmineable,
      // unclearable "border" in the middle of the world, and the spawn pad
      // skipped over it because that is exactly what it is told to do with
      // border cells.
      if (isStructure(gtopAt(i)) || hasFloating(i)) return false;
      if (ghAt(i) > GROUND + 3 || ghAt(i) < GROUND) return false;
    }

  for (int y = cy - r; y <= cy + r; ++y) {
    for (int x = cx - r; x <= cx + r; ++x) {
      const int dx = x - cx, dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r) continue;
      // Terraced rather than a smooth bowl, so the walls are one-block steps
      // the player can actually climb back out of.
      // Two courses per terrace step, so a pit on a world twenty-four blocks
      // tall is a pit and not a scuff. Still one block at a time to climb out
      // of, because each terrace is two cells wide.
      const int down = (r - (int)(sqrtf((float)d2) + 0.5f) + 1) * 2;
      int h = GROUND - down;
      if (h < 1) h = 1;
      // A quarry only ever cuts down. Belt and braces against the above: the
      // material comes from a layer of the column as it stands, so raising one
      // here would be reading past its top.
      if (h >= ghAt(idx(x, y))) continue;
      setCol(x, y, h, matAt(x, y, h - 1));
      if (molten && d2 <= 1) setCol(x, y, h, B_LAVA);
    }
  }
  return true;
}

// A rock arch: a span of stone bridging two points of high ground, with a gap
// underneath you can walk through. Purely a landmark, and the clearest possible
// demonstration that the world is no longer a pure heightmap.
static bool placeArch(int cx, int cy, int len, bool alongX, uint32_t& rng) {
  const int hx = alongX ? len / 2 : 1, hy = alongX ? 1 : len / 2;
  for (int y = cy - hy; y <= cy + hy; ++y)
    for (int x = cx - hx; x <= cx + hx; ++x) {
      if (outside(x, y) || isBorder(x, y)) return false;
      if (isStructure(gtopAt(idx(x, y))) || hasFloating(idx(x, y))) return false;
      if (ghAt(idx(x, y)) > GROUND + 2) return false;
    }

  const int deck = GROUND + 4;
  for (int i = -len / 2; i <= len / 2; ++i) {
    const int x = alongX ? cx + i : cx;
    const int y = alongX ? cy : cy + i;
    const bool foot = (i == -len / 2 || i == len / 2);
    if (foot) {
      setCol(x, y, deck + 1, B_STONE);            // the piers hold it up
    } else {
      setSlab(x, y, deck, deck + 1, B_STONE);     // and the deck floats
    }
  }
  (void)rng;
  return true;
}

// A tunnel bored into a hillside: the floor is cut down to ground level and a
// slab of the hill is left over the top of it. Not a cave system — nothing
// here can branch or turn — but it is a mouth you walk into, out of the light.
static bool placeCave(int cx, int cy, int len, bool alongX, uint32_t& rng) {
  for (int i = 0; i < len; ++i) {
    const int x = alongX ? cx + i : cx;
    const int y = alongX ? cy : cy + i;
    if (outside(x, y) || isBorder(x, y)) return false;
    if (isStructure(gtopAt(idx(x, y))) || hasFloating(idx(x, y))) return false;
    // It has to run into a hill, or there is nothing to be inside of — and
    // tall enough at EVERY cell, the mouth included. The mouth used to be
    // allowed down to GROUND + 1, where the roof lands at or under the floor
    // plus HEADROOM and setSlab quietly declines to make it: the floor was
    // still cut, so what the generator produced was a bare patch of hillside
    // stone at ground level with open sky over it. Expressed against HEADROOM
    // rather than as a 3, because it is setSlab's rule that decides this.
    if (ghAt(idx(x, y)) < GROUND + HEADROOM + 1) return false;
  }

  for (int i = 0; i < len; ++i) {
    const int x = alongX ? cx + i : cx;
    const int y = alongX ? cy : cy + i;
    const int roof = ghAt(idx(x, y));
    setCol(x, y, GROUND, matAt(x, y, GROUND - 1));
    setSlab(x, y, GROUND + HEADROOM, roof, B_STONE);
  }
  (void)rng;
  return true;
}

static void placeStructures(uint32_t seed) {
  uint32_t rng = seed ^ 0xB10CC0DEu;
  auto next = [&rng](uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (int)((rng >> 16) % n);
  };
  // Somewhere inside the border, with `pad` cells to spare on both sides.
  auto spot = [&next](int pad) {
    return BORDER + pad + next(W - 2 * BORDER - 2 * pad);
  };

  // Each structure gets many attempts rather than one. A single random throw
  // lands on ground too steep or too crowded most of the time, and the map is
  // much worse for silently ending up with no castle on it at all.
  //
  // Every placement hoists its random draws into named locals instead of
  // calling next() twice inside one argument list. Sibling arguments are
  // indeterminately sequenced, so which draw happened first was the compiler's
  // choice — and a generator deterministic per compiler rather than per seed is
  // not deterministic, which is the one thing test_world exists to check and
  // the one thing it could not have caught.
  constexpr int TRIES = 90;

  // Strictly largest footprint first. Everything here competes for the same
  // flat lowland and whatever runs last gets what is left, which is why the
  // trees — of which there are a hundred and which need one cell each — go at
  // the very end.
  for (int want = 0; want < 2; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int sz = 13 + next(5);
      const int x = spot(sz / 2 + 2) - sz / 2;
      const int y = spot(sz / 2 + 2) - sz / 2;
      if (placeCastle(x, y, sz, rng)) break;
    }
  }
  for (int i = 0; i < TRIES; ++i) {
    const int x = spot(15), y = spot(15);
    if (placeVillage(x, y, rng)) break;
  }
  for (int want = 0; want < 4; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int x = spot(6), y = spot(6);
      if (placeTower(x, y, rng)) break;
    }
  }
  for (int want = 0; want < 3; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int w = 6 + next(3);
      const int d = 6 + next(3);
      const int x = spot(w / 2 + 3) - w / 2;
      const int y = spot(d / 2 + 3) - d / 2;
      if (placeHouse(x, y, w, d, rng)) break;
    }
  }
  for (int want = 0; want < 4; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int w = 6 + next(4);
      const int d = 6 + next(4);
      const int x = spot(w / 2 + 2) - w / 2;
      const int y = spot(d / 2 + 2) - d / 2;
      if (placeRuin(x, y, w, d, rng)) break;
    }
  }
  for (int want = 0; want < 7; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int r = 3 + next(3);
      const int x = spot(r + 2), y = spot(r + 2);
      if (placeQuarry(x, y, r, rng)) break;
    }
  }
  // Arches and cave mouths before the scatter, for the same reason the
  // buildings go first: they need clear ground and the small stuff takes it.
  for (int want = 0; want < 4; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int len = 6 + next(4);
      const int x = spot(len + 1), y = spot(len + 1);
      const bool alongX = next(2) == 0;
      if (placeArch(x, y, len, alongX, rng)) break;
    }
  }
  for (int want = 0; want < 6; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int len = 5 + next(5);
      const int x = spot(len + 2), y = spot(len + 2);
      const bool alongX = next(2) == 0;
      if (placeCave(x, y, len, alongX, rng)) break;
    }
  }

  // Landmarks that want high ground rather than flat: after the things that
  // need lowland, before the things that will carpet it.
  for (int want = 0; want < 10; ++want) {
    for (int i = 0; i < 30; ++i) {
      const int x = spot(3), y = spot(3);
      if (placeSpire(x, y, rng)) break;
    }
  }
  for (int i = 0; i < 90; ++i) {
    const int x = spot(3), y = spot(3);
    placeBoulder(x, y, rng);
  }

  // The scatter and the trees, last and by the hundred. Each throw is one cell
  // and most of them land on something already taken, which is what makes the
  // distribution follow the terrain rather than the random number generator.
  for (int i = 0; i < 260; ++i) {
    const int x = spot(3), y = spot(3);
    placeCactus(x, y, rng);
  }
  for (int i = 0; i < 120; ++i) {
    const int x = spot(3), y = spot(3);
    placeSnag(x, y, rng);
  }
  // Woods rather than an orchard, and it is the mask that does it rather than
  // the count: a uniform scatter puts a tree every few cells everywhere, which
  // gives no stands to walk into and no clearings to be caught in. Forest is
  // the biome where the mask is generous; plains get a copse here and there.
  for (int i = 0; i < 900; ++i) {
    const int x = spot(3), y = spot(3);
    const float grove = valueNoise((float)x * 0.10f, (float)y * 0.10f, seed ^ 0x7A3Eu);
    const float gate = (biomeAt(x, y) == BIOME_FOREST) ? 0.36f : 0.56f;
    if (grove < gate) continue;
    placeTree(x, y, rng);
  }
}

// ---- access -----------------------------------------------------------------

uint8_t height(int x, int y) {
  if (outside(x, y)) return MAX_H;            // the world is walled, not open
  return (uint8_t)ghAt(idx(x, y));
}

bool isBorder(int x, int y) {
  if (outside(x, y)) return true;
  return smatOf(idx(x, y)) == B_BEDROCK;
}

// Material at z, with no solidity test: the step function first, then the soil
// profile the markers defer to.
//
// This is one function where there used to be two. matAt answered for the
// ground column and blockAt answered for the runs floating over it, each with
// its own depth anchor -- a run carried a `surf` field for exactly that, so a
// piece split off a hillside still read grass over dirt over stone. With the
// mask there is no such distinction: a block is at a z, and z is all the depth
// anchor a soil profile needs.
static uint8_t matAtIdx(int i, int z) {
  // The floor of the world, under everything and answerable before the markers
  // are consulted: a player cannot place over it because it is never air, and
  // the soil profile below has nothing to say about a layer that is bedrock by
  // definition.
  if (z <= 0) return B_BEDROCK;

  const uint8_t m = markAt(i, z);
  if (m != M_DERIVE) return m;

  // Below the surface, material is a function of depth: the biome's own ground
  // on top, a band of dirt, then stone with ore seeded by position so a given
  // column always digs the same.
  //
  // Measured from g_surf, the height the generator left this column at. See
  // g_surf for what measuring from the current height did.
  const int depth = (int)g_surf[i] - 1 - z;
  if (depth <= 0) return smatOf(i);
  if (depth == 1) return B_DIRT;
  const int x = i % W, y = i / W;
  // Diamond, before the ore bands and after the two depth branches above. Both
  // positions are load-bearing. After, because a column whose ground sits low
  // -- a desert floor, the bottom of a quarry -- has its surface and its dirt
  // layer inside the depth band, and answering diamond there would put it on
  // open ground and into g_cell's four-bit surface field, which has no room to
  // describe it. Before the ore bands, because it is the rarest thing here and
  // a band checked later can only ever be what the earlier ones did not claim.
  //
  // Its own lattice seed rather than a slice off the end of iron's range: one
  // field asked twice at the same point is not independent, and carving the
  // diamond band out of iron's would quietly thin the iron in exactly the
  // layers a player digs to for diamond.
  if (z <= DIAMOND_MAX_Z) {
    const uint32_t d = latticeu(x * 11 - z, y * 5 + z, 0x1D0B3u);
    if (d < DIAMOND_RARITY) return B_DIAMOND;
  }
  const uint32_t o = latticeu(x * 7 + z, y * 13 - z, 0x0C0A1u);
  if (o < 214748365u) return B_IRON;    // 0.050 of the range
  if (o < 794568365u) return B_COAL;    // 0.185
  return B_STONE;
}

Cell cellAt(int x, int y) {
  Cell c;
  if (outside(x, y)) {
    // Out of bounds reads report the wall that rings the map, so the walker
    // needs no bounds check of its own.
    c.solid = ~0u; c.h = MAX_H; c.top = B_BEDROCK; c.light = 0; c.surf = MAX_H;
    return c;
  }
  const int i = idx(x, y);
  // The whole column in one 32-bit load. This call exists because five separate
  // cross-module questions per grid step was the largest line item in a frame;
  // the mask answers all the geometry ones at once, and there is no longer a
  // run list to copy out -- which also means no cap on what it can describe.
  c.solid = g_solid[i];
  c.h     = (uint8_t)colHeight(c.solid);
  c.top   = c.h == 0 ? smatOf(i) : matAtIdx(i, c.h - 1);
  c.light = lightOf(i);
  c.surf  = g_surf[i];
  return c;
}

// The lowest floating run of a cell, under the name the rest of the codebase
// already uses for it. While the generator was the only thing making runs there
// was never more than one, so for every caller that predates the change this is
// the same answer it always got.
static inline void lowestFloat(int i, int& base, int& top) {
  const int h = ghAt(i);
  const uint32_t above = (h >= MAX_H) ? 0u : (g_solid[i] & ~((1u << h) - 1u));
  if (!above) { base = top = 0; return; }
  base = __builtin_ctz(above);
  runAround(g_solid[i], base, base, top);
}

uint8_t slabBase(int x, int y) {
  if (outside(x, y)) return 0;
  int b, t; lowestFloat(idx(x, y), b, t);
  return (uint8_t)b;
}
uint8_t slabTop(int x, int y) {
  if (outside(x, y)) return 0;
  int b, t; lowestFloat(idx(x, y), b, t);
  return (uint8_t)t;
}
uint8_t slabMat(int x, int y) {
  if (outside(x, y)) return B_STONE;
  const int i = idx(x, y);
  int b, t; lowestFloat(i, b, t);
  return t == 0 ? B_STONE : matAtIdx(i, t - 1);
}
bool hasSlab(int x, int y) { return slabTop(x, y) != 0; }

// Every floating run of a cell, ascending. `cap` is the caller's array size:
// a column's run count is unbounded now, so the caller has to say how many it
// can take rather than the world declaring a limit it no longer has.
int runsAt(int x, int y, RunView* out, int cap) {
  if (outside(x, y) || cap <= 0) return 0;
  const int i = idx(x, y);
  int k = 0;
  for (int z = ghAt(i); z < MAX_H && k < cap; ) {
    if (!((g_solid[i] >> z) & 1u)) { ++z; continue; }
    int base, top;
    runAround(g_solid[i], z, base, top);
    out[k++] = { (uint8_t)base, (uint8_t)top, matAtIdx(i, top - 1) };
    z = top;
  }
  return k;
}

// True if the block at (x, y, z) is solid. One bit test -- the whole reason the
// refusals could go.
bool solidAt(int x, int y, int z) {
  if (outside(x, y)) return true;          // the border is solid all the way up
  if (z < 0) return true;                  // the base plane
  if (z >= MAX_H) return false;
  return ((g_solid[idx(x, y)] >> z) & 1u) != 0u;
}

// Material of whatever occupies (x, y, z). B_BEDROCK for air, matching matAt's
// convention for "nothing here".
uint8_t blockAt(int x, int y, int z) {
  if (outside(x, y)) return B_BEDROCK;
  if (z < 0 || z >= MAX_H) return B_BEDROCK;
  const int i = idx(x, y);
  if (!((g_solid[i] >> z) & 1u)) return B_BEDROCK;
  return matAtIdx(i, z);
}

static void setSlab(int x, int y, int base, int top, uint8_t mat) {
  if (outside(x, y) || isBorder(x, y)) return;
  if (top > MAX_H) top = MAX_H;
  if (base < 0) base = 0;
  if (top <= base) return;
  const int i = idx(x, y);
  // The generator used to have exactly one slot per cell, so a second call
  // silently replaced the first. Keeping that: any floating run this one
  // overlaps is cleared whole before it goes in. The generator leans on it --
  // placeTree lays its crown down in layers over the same cells, and a house
  // sets its eaves and then its roof -- and made the old last-call-wins rule
  // part of those shapes.
  const int h = ghAt(i);
  for (int z = h; z < MAX_H; ) {
    if (!((g_solid[i] >> z) & 1u)) { ++z; continue; }
    int rb, rt;
    runAround(g_solid[i], z, rb, rt);
    z = rt;
    if (rb < top && rt > base) solidClear(i, rb, rt);
  }
  solidFill(i, base, top);
  markSetRange(i, base, top, mat);
}

void devSlab(int x, int y, int base, int top, uint8_t mat) {
  setSlab(x, y, base, top, mat);
}

uint8_t topMat(int x, int y) {
  if (outside(x, y)) return B_BEDROCK;
  return gtopAt(idx(x, y));
}

// Air reports the base plane, which is this file's long-standing convention for
// "nothing here" and what every caller already tests against.
//
// matAt and blockAt have converged: they used to differ because matAt answered
// for the ground column and blockAt for the runs floating over it, each with
// its own depth anchor. There is one column now, so there is one answer.
uint8_t matAt(int x, int y, int z) {
  return blockAt(x, y, z);
}

// ---- biomes -----------------------------------------------------------------

// One extra octave of noise at a much longer wavelength than the terrain, so a
// biome is a region you walk across rather than a speckle.
static float biomeNoise(int x, int y) {
  return valueNoise((float)x * 0.028f, (float)y * 0.028f, g_seed ^ 0xB10E5u);
}

uint8_t biomeAt(int x, int y) {
  const float n = biomeNoise(x, y);
  if (n < 0.38f) return BIOME_DESERT;
  if (n > 0.66f) return BIOME_TUNDRA;
  return (n > 0.52f) ? BIOME_FOREST : BIOME_PLAINS;
}

static uint8_t surfaceOf(uint8_t biome) {
  switch (biome) {
    case BIOME_DESERT: return B_SAND;
    case BIOME_TUNDRA: return B_SNOW;
    default:           return B_GRASS;      // plains and forest share it
  }
}

// How hard the ground is allowed to climb here, as a fraction of the full
// range, and how much of the high octave goes into it.
//
// Continuous in the biome noise rather than switched on the enum: a step in
// amplitude at a biome edge is a cliff running the whole length of the border,
// and no amount of tuning the two sides hides it. So the thresholds above pick
// the *material*, which can change abruptly without looking wrong, and the
// numbers below pick the *shape*, which cannot.
//
// Roughness is the number that decides whether the map stays walkable. It is
// the weight on the octave three times finer than the terrain, so it is what
// puts a two-block step between one cell and the next — and STEP_UP is one. At
// roughness much past this, a fifth of the ground stops being reachable from
// spawn at all; the generator will happily build a castle you cannot walk to.
static void terrainShape(float bn, float& amp, float& rough) {
  if (bn < 0.38f) {                       // desert: long, smooth, low dunes
    amp   = 0.32f;
    rough = 0.12f;
  } else if (bn < 0.52f) {                // plains: lowland with outcrops
    const float t = (bn - 0.38f) * (1.0f / 0.14f);
    amp   = 0.32f + 0.20f * t;
    rough = 0.12f + 0.20f * t;
  } else if (bn < 0.66f) {                // forest: as plains, a touch rolling
    amp   = 0.58f;
    rough = 0.32f;
  } else {                                // tundra: peaks, and broken ones
    const float t = (bn - 0.66f) * (1.0f / 0.34f);
    amp   = 0.58f + 0.80f * t * t;
    rough = 0.32f + 0.22f * t;
  }
}

// ---- light ------------------------------------------------------------------

uint8_t light(int x, int y) {
  if (outside(x, y)) return 0;
  return lightOf(idx(x, y));
}

void rebuildLight() {
  for (int i = 0; i < W * H; ++i) setLight(i, 0);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const uint8_t e = emission(gtopAt(idx(x, y)));
      if (!e) continue;
      // Splat a diamond rather than a circle: it costs a subtraction instead
      // of a square root and at this radius the two are indistinguishable.
      for (int dy = -LIGHT_MAX; dy <= LIGHT_MAX; ++dy) {
        for (int dx = -LIGHT_MAX; dx <= LIGHT_MAX; ++dx) {
          const int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
          if (d > (int)e) continue;
          const int nx = x + dx, ny = y + dy;
          if (outside(nx, ny)) continue;
          const uint8_t v = (uint8_t)((int)e - d);
          const int j = idx(nx, ny);
          if (v > lightOf(j)) setLight(j, v);
        }
      }
    }
  }
}

// ---- movement ---------------------------------------------------------------

uint8_t surfaceUnder(int x, int y, int fromZ) {
  if (outside(x, y)) return NO_SURFACE;
  const int i = idx(x, y);

  // A body may step up by STEP_UP and down by any amount, so every solid top
  // at or below that ceiling is a candidate and the highest one wins. The
  // ground column is one candidate; the top of each run is another, and that
  // is the whole of "you can stand on what you built".
  // The highest run top at or below the ceiling. Scanned down from the ceiling
  // rather than up through a list: the first solid block at or below `limit`
  // is the top block of the run a body would come to rest on, so its top is
  // one above it, and there is nothing to iterate.
  const int limit = fromZ + STEP_UP;
  const uint32_t below = g_solid[i] & ((limit >= MAX_H) ? ~0u
                                                        : ((1u << limit) - 1u));
  // No solid under the ceiling at all still leaves the base plane to stand on,
  // which is what a column dug out to nothing is.
  const int best = below ? (32 - __builtin_clz(below)) : 0;

  // ...and there has to be room to stand up on it. Without this a body walks
  // into a bridge pier and ends up inside the deck.
  //
  // Only the winning candidate is tested, not each in turn: a surface with
  // something pressed down on top of it is not somewhere to stand, and falling
  // through to the next one down would mean passing through whatever blocked
  // it.
  if (ceilingAbove(i, best) - best < HEADROOM) return NO_SURFACE;
  return (uint8_t)best;
}

bool canEnter(int fromZ, int x, int y) {
  return surfaceUnder(x, y, fromZ) != NO_SURFACE;
}

bool fits(int fromZ, float px, float py, float r) {
  const int x0 = (int)floorf(px - r), x1 = (int)floorf(px + r);
  const int y0 = (int)floorf(py - r), y1 = (int)floorf(py + r);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      if (!canEnter(fromZ, x, y)) return false;
  return true;
}

uint8_t groundAt(float px, float py, int fromZ) {
  const uint8_t sfc = surfaceUnder((int)floorf(px), (int)floorf(py), fromZ);
  // Nowhere to stand is not a height. Leaving the body where it was is the
  // only answer that does not teleport it, and fits() is what stops it getting
  // there in the first place.
  return sfc == NO_SURFACE ? (uint8_t)fromZ : sfc;
}

uint8_t groundAt(float px, float py) {
  return height((int)floorf(px), (int)floorf(py));
}

// ---- mining -----------------------------------------------------------------

// Merges any run that has come to rest directly on the ground column into it,
// There is no cell to normalise any more, and no room to run out of.
//
// The old model needed both: a solid could be described either as "column to 9"
// or as "column to 6 plus a run from 6", so every edit had to fold the two back
// together, and every edit that split a column had to ask first whether the
// cell had a spare node to describe the hole with. A mask has exactly one
// representation of a given solid, so there is nothing to fold, and clearing a
// bit cannot fail.

MineResult mine(int x, int y, int z, int effort,
                uint8_t& dropMat, uint8_t& dropBlocks) {
  if (outside(x, y) || isBorder(x, y)) return MINE_NOTHING;
  const int i = idx(x, y);
  if (!solidAt(x, y, z)) return MINE_NOTHING;

  const uint8_t m = blockAt(x, y, z);
  const BlockInfo& bi = kInfo[m];
  if (bi.toughness == 0) return MINE_NOTHING;

  // There is no MINE_NO_ROOM here any more, and its absence is the point of the
  // whole model. It used to be asked before any effort was banked -- "would the
  // hole this leaves need a run this cell cannot hold" -- because a cell could
  // only describe three holes, and digging into the wrong hillside was simply
  // refused. Taking a block out is one cleared bit now. It cannot fail.

  if (x != s_mineX || y != s_mineY || z != s_mineZ) {
    s_mineX = x; s_mineY = y; s_mineZ = z; s_effort = 0;
  }
  const int acc = (int)s_effort + effort;
  if (acc < (int)bi.toughness) { s_effort = (uint16_t)acc; return MINE_PROGRESS; }

  dropMat    = m;
  dropBlocks = bi.dropBlocks;

  // The edit itself. Everything the old code did around this -- recomputing the
  // revealed material before the column moved, splitting a run in two, carrying
  // the upper piece's soil anchor across -- was bookkeeping the two-part
  // representation forced on it. Geometry is one bit and material is untouched,
  // so a mined block is exactly this:
  solidClear(i, z, z + 1);
  markNormalise(i);

  s_mineX = s_mineY = s_mineZ = -1;
  s_effort = 0;
  // Leaves are NOT swept here. Chopping the trunk out from under a crown used
  // to delete the whole crown on the spot; in Minecraft it stays, and so does
  // this one. See dropOrphanCanopy, which is now generator-only.
  if (emission(m)) rebuildLight();       // a torch just came down
  return MINE_BROKE;
}

// The old shape, kept because most callers mean "take the top block off this
// column" and should not have to work out which z that is.
bool mineTop(int x, int y, int effort,
             uint8_t& dropMat, uint8_t& dropBlocks) {
  if (outside(x, y)) return false;
  const int h = ghAt(idx(x, y));
  if (h == 0) return false;
  return mine(x, y, h - 1, effort, dropMat, dropBlocks) == MINE_BROKE;
}

void resetDamage(int x, int y, int z) {
  if (x == s_mineX && y == s_mineY && (z < 0 || z == s_mineZ)) {
    s_mineX = s_mineY = s_mineZ = -1;
    s_effort = 0;
  }
}
void resetDamage(int x, int y) { resetDamage(x, y, -1); }

uint8_t damage(int x, int y, int z) {
  if (x != s_mineX || y != s_mineY) return 0;
  if (z >= 0 && z != s_mineZ) return 0;
  if (outside(x, y) || !solidAt(x, y, s_mineZ)) return 0;
  const uint16_t t = kInfo[blockAt(x, y, s_mineZ)].toughness;
  if (t == 0) return 0;
  const uint32_t d = (uint32_t)s_effort * 255u / t;
  return (uint8_t)(d > 255 ? 255 : d);
}
uint8_t damage(int x, int y) { return damage(x, y, -1); }

bool standable(int x, int y) {
  if (outside(x, y)) return false;
  const int i = idx(x, y);
  return ceilingAbove(i, ghAt(i)) - ghAt(i) >= HEADROOM;
}

PlaceResult place(int x, int y, int z, uint8_t mat) {
  if (outside(x, y) || isBorder(x, y)) return PLACE_REFUSED;
  if (z < 0 || z >= MAX_H) return PLACE_OCCUPIED;
  if (solidAt(x, y, z)) return PLACE_OCCUPIED;
  const int i = idx(x, y);

  // Nothing may be built on a light source. It is the Minecraft rule, and it
  // also keeps every emitter at the top of a run where rebuildLight can find it.
  if (z > 0 && emission(blockAt(x, y, z - 1))) return PLACE_REFUSED;

  // Geometry first, then material -- and the material is written for exactly
  // this one block. That second part matters below the natural surface: putting
  // a plank into a mined-out tunnel has to say "plank at z" AND "back to the
  // soil profile at z + 1", or the plank's material runs on up over untouched
  // ground. markSetRange writes both.
  //
  // The only way this fails is the marker pool being empty world-wide, which
  // takes thousands of distinct placements -- not the four-in-one-column that
  // PLACE_NO_ROOM used to mean.
  // The bit goes down BEFORE the marker, and the order is load-bearing:
  // markSetRange normalises, and normalising drops any marker whose range holds
  // no solid block. Written the other way round the marker describes air for
  // the instant it exists, is collected on the spot, and the block comes out as
  // whatever the soil profile says instead of what was placed.
  solidFill(i, z, z + 1);
  if (!markSetRange(i, z, z + 1, mat)) { solidClear(i, z, z + 1); return PLACE_NO_ROOM; }

  if (emission(mat)) rebuildLight();
  return PLACE_OK;
}

// The old shape: put one block on top of this column.
bool place(int x, int y, uint8_t mat) {
  if (outside(x, y)) return false;
  const int i = idx(x, y);
  if (ghAt(i) >= MAX_H) return false;
  // Never build a floor up so close to something overhead that nothing can
  // stand between them, which would seal a body into a cell the movement rules
  // say is impossible to be in.
  if (ceilingAbove(i, ghAt(i) + 1) - (ghAt(i) + 1) < HEADROOM) return false;
  return place(x, y, ghAt(i), mat) == PLACE_OK;
}

int explode(int cx, int cy, int radius) {
  int n = 0;
  const int r2 = radius * radius;
  for (int y = cy - radius; y <= cy + radius; ++y) {
    for (int x = cx - radius; x <= cx + radius; ++x) {
      const int dx = x - cx, dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r2 || outside(x, y) || isBorder(x, y)) continue;
      const int i = idx(x, y);
      if (ghAt(i) == 0) continue;
      // Deepest at the centre, one block at the rim: a blast should leave a
      // crater, not a cylinder punched out of the ground.
      int take = radius - (int)(sqrtf((float)d2)) + 1;
      if (take > ghAt(i)) take = ghAt(i);
      if (take <= 0) continue;
      // No revealed-material dance: what is under the crater was already
      // whatever it was, and clearing bits does not change it.
      solidClear(i, ghAt(i) - take, ghAt(i));
      markNormalise(i);
      n += take;
    }
  }
  // A blast leaves whatever it did not reach standing, leaves included. It used
  // to sweep the canopy afterwards, which meant a creeper going off under a
  // tree took the tree with it.
  if (n) { resetDamage(s_mineX, s_mineY); rebuildLight(); }
  return n;
}

}  // namespace world
