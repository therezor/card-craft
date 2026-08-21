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
// torch 208, snow 241, lava 132, bedrock 46.
static const BlockInfo kInfo[B_COUNT] = {
  // name       r    g    b   tough  blk  ore  speckle
  { "grass",   82, 168,  62,   360,   1,   0,  40 },   // saturated green
  { "dirt",   140,  92,  50,   400,   1,   0,  52 },   // mid orange-brown
  { "stone",  128, 132, 138,  1150,   2,   0,  44 },   // neutral grey
  { "wood",    92,  60,  36,   760,   3,   0,  56 },   // very dark brown, the trunk
  { "leaves",  46, 120,  44,   220,   1,   0,  72 },   // darker than grass
  { "coal",    78,  80,  88,  1540,   1,   1, 110 },   // stone shot through with black
  { "iron",   192, 180, 166,  2100,   1,   3, 100 },   // stone shot through with pale ore
  { "sand",   228, 208, 148,   340,   1,   0,  30 },   // pale yellow, fine grained
  { "snow",   236, 242, 250,   300,   1,   0,  14 },   // white, almost smooth
  { "brick",  168,  96,  84,  1400,   2,   0,  46 },   // terracotta, not brown
  { "plank",  206, 152,  88,   470,   1,   0,  34 },   // light tan, clearly milled
  { "torch",  252, 178,  56,   200,   1,   0,   0 },   // flame orange, not sand
  { "lava",   238,  96,  30,     0,   0,   0,   0 },   // unbreakable: bridge over it
  { "bedrock", 44,  46,  52,     0,   0,   0,  60 },
};

// A structure column is made of itself all the way down to ground level: a
// brick wall is brick, a trunk is wood. A torch is deliberately NOT one — it
// is a single block sitting on top of whatever was already there, and treating
// it as a structure would make mining one reveal another torch underneath.
bool isStructure(uint8_t b) {
  return b == B_WOOD || b == B_LEAVES || b == B_BRICK || b == B_PLANK;
}

uint8_t emission(uint8_t b) {
  if (b == B_TORCH) return LIGHT_MAX;
  if (b == B_LAVA)  return LIGHT_MAX - 2;   // a glow, not a lantern
  return 0;
}

bool isHazard(uint8_t b) { return b == B_LAVA; }

const BlockInfo& info(uint8_t b) { return kInfo[b < B_COUNT ? b : 0]; }

static uint8_t g_h[W * H];      // column height, 0..MAX_H
static uint8_t g_top[W * H];    // material of the top block, or of the ground at h==0
static uint8_t g_light[W * H];  // 0..LIGHT_MAX, from torches and lava

// The optional second run per cell. Three bytes a cell for roofs, bridges,
// arches and cave mouths — the shapes a single height per column cannot make.
static uint8_t g_cBase[W * H];
static uint8_t g_cTop[W * H];   // 0 = no slab here
static uint8_t g_cMat[W * H];
static uint32_t g_seed = 1;     // kept for biomeAt, which is queried after generate

// One mining target at a time. The player can only look at one column, so a
// full damage array would be kilobytes to store a single live number — and
// "effort resets when you look away" falls out of this for free.
static int      s_mineX = -1, s_mineY = -1;
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

constexpr int   BORDER      = 2;      // full-height bedrock ring, so no ray escapes
constexpr float NOISE_SCALE = 0.13f;  // ~8 cells per lattice step
constexpr float FLAT_BELOW  = 0.52f;  // noise under this stays at height 0
constexpr int   SPAWN_CLEAR = 5;

static void placeStructures(uint32_t seed);   // defined below, after the accessors
static uint8_t surfaceOf(uint8_t biome);      // ditto
static void setSlab(int x, int y, int base, int top, uint8_t mat);

static inline int idx(int x, int y) { return y * W + x; }

static inline bool outside(int x, int y) {
  return (unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H;
}

void generate(uint32_t seed) {
  g_seed = seed;
  memset(g_cBase, 0, sizeof(g_cBase));
  memset(g_cTop,  0, sizeof(g_cTop));
  memset(g_cMat,  0, sizeof(g_cMat));
  s_mineX = s_mineY = -1;
  s_effort = 0;

  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);
      if (x < BORDER || y < BORDER || x >= W - BORDER || y >= H - BORDER) {
        g_h[i] = MAX_H;
        g_top[i] = B_BEDROCK;
        continue;
      }

      const float fx = (float)x * NOISE_SCALE, fy = (float)y * NOISE_SCALE;
      // The second octave breaks up the smooth blobs the first alone produces,
      // which otherwise look like a lava lamp rather than rock.
      const float n = valueNoise(fx, fy, seed) * 0.68f
                    + valueNoise(fx * 2.9f, fy * 2.9f, seed ^ 0x9e3779b9u) * 0.32f;

      int rise = 0;
      if (n > FLAT_BELOW) {
        rise = 1 + (int)((n - FLAT_BELOW) * (1.0f / (1.0f - FLAT_BELOW))
                         * (float)(MAX_H - GROUND));
      }
      int h = GROUND + rise;
      if (h > MAX_H) h = MAX_H;

      const uint8_t surface = surfaceOf(biomeAt(x, y));
      uint8_t top = surface;
      if (rise >= 4) {
        top = B_STONE;                 // exposed rock on the high ground
      } else if (rise >= 2) {
        top = (latticef(x, y, seed ^ 0xD1D7u) < 0.4f) ? B_DIRT : surface;
      }

      g_h[i] = (uint8_t)h;
      g_top[i] = top;
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
      g_h[idx(x, y)] = GROUND;
      g_top[idx(x, y)] = surfaceOf(biomeAt(x, y));
    }
  }
}


// ---- structures -------------------------------------------------------------
//
// Placed after the noise, onto ground the noise left flat. Each one is a shape
// the terrain generator could never produce on its own, and each one is there
// for a reason the player will feel: a tree is the best block-per-effort trade
// in the game, a ruin is shelter you did not have to build, and a quarry is
// the only place ore is visible from the surface.

static inline bool flatAt(int x, int y) {
  return !isBorder(x, y) && g_h[idx(x, y)] == GROUND && !isStructure(g_top[idx(x, y)]);
}

static bool flatArea(int x0, int y0, int w, int d) {
  for (int y = y0; y < y0 + d; ++y)
    for (int x = x0; x < x0 + w; ++x)
      if (!flatAt(x, y)) return false;
  return true;
}

static void setCol(int x, int y, int h, uint8_t top) {
  if (outside(x, y) || isBorder(x, y)) return;
  g_h[idx(x, y)] = (uint8_t)(h < 0 ? 0 : (h > MAX_H ? MAX_H : h));
  g_top[idx(x, y)] = top;
}

// A trunk with a canopy on top. In a heightmap the crown cannot overhang the
// trunk, so height does the work instead: a tall centre column capped with two
// blocks of leaves, and shorter leafy columns beside it to break the outline
// into something that reads as a tree rather than a post.
static bool placeTree(int x, int y, uint32_t& rng) {
  if (!flatAt(x, y)) return false;
  setCol(x, y, GROUND + 4, B_LEAVES);
  static const int kDx[4] = { 1, -1, 0, 0 };
  static const int kDy[4] = { 0, 0, 1, -1 };
  for (int i = 0; i < 4; ++i) {
    rng = rng * 1664525u + 1013904223u;
    if ((rng >> 16) % 100u >= 45u) continue;     // roughly half get a skirt
    const int nx = x + kDx[i], ny = y + kDy[i];
    if (flatAt(nx, ny)) setCol(nx, ny, GROUND + 2, B_LEAVES);
  }
  return true;
}

// A walled enclosure with one doorway. No roof: a heightmap has no way to put
// a block over open air. What it gives the player is four walls taller than a
// mob can step, which is the entire point of running to one at dusk.
static bool placeRuin(int x0, int y0, int w, int d, uint32_t& rng) {
  if (!flatArea(x0 - 1, y0 - 1, w + 2, d + 2)) return false;

  rng = rng * 1664525u + 1013904223u;
  const int side = (int)((rng >> 16) % 4u);
  rng = rng * 1664525u + 1013904223u;
  const int doorW = 1 + (int)((rng >> 16) % 2u);      // one or two cells wide
  rng = rng * 1664525u + 1013904223u;
  const int doorAt = 1 + (int)((rng >> 16) % (uint32_t)((side < 2 ? w : d) - doorW - 1));

  for (int y = y0; y < y0 + d; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      const bool edge = (x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + d - 1);
      if (!edge) continue;

      // Punch the doorway out of the chosen wall.
      bool door = false;
      const int ox = x - x0, oy = y - y0;
      if      (side == 0 && oy == 0     && ox >= doorAt && ox < doorAt + doorW) door = true;
      else if (side == 1 && oy == d - 1 && ox >= doorAt && ox < doorAt + doorW) door = true;
      else if (side == 2 && ox == 0     && oy >= doorAt && oy < doorAt + doorW) door = true;
      else if (side == 3 && ox == w - 1 && oy >= doorAt && oy < doorAt + doorW) door = true;
      if (door) continue;

      setCol(x, y, GROUND + 3, B_BRICK);
    }
  }

  // Roof it. This is the shape a plain heightmap cannot make: a slab of brick
  // over open air, with a floor you can stand on underneath. It is also the
  // only fully enclosed shelter in the game — mobs cannot spawn under it once
  // it is lit, and they cannot drop in from above.
  for (int y = y0 + 1; y < y0 + d - 1; ++y)
    for (int x = x0 + 1; x < x0 + w - 1; ++x)
      setSlab(x, y, GROUND + 3, GROUND + 4, B_BRICK);
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
      if (isStructure(g_top[i]) || g_h[i] > GROUND + 1) return false;
    }

  for (int y = cy - r; y <= cy + r; ++y) {
    for (int x = cx - r; x <= cx + r; ++x) {
      const int dx = x - cx, dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r) continue;
      // Terraced rather than a smooth bowl, so the walls are one-block steps
      // the player can actually climb back out of.
      const int down = r - (int)(sqrtf((float)d2) + 0.5f) + 1;
      int h = GROUND - down;
      if (h < 1) h = 1;
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
      if (isStructure(g_top[idx(x, y)]) || g_cTop[idx(x, y)]) return false;
      if (g_h[idx(x, y)] > GROUND + 1) return false;
    }

  const int deck = GROUND + 3;
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
    if (isStructure(g_top[idx(x, y)]) || g_cTop[idx(x, y)]) return false;
    // It has to run into a hill, or there is nothing to be inside of.
    if (g_h[idx(x, y)] < GROUND + (i > 0 ? 3 : 1)) return false;
  }

  for (int i = 0; i < len; ++i) {
    const int x = alongX ? cx + i : cx;
    const int y = alongX ? cy : cy + i;
    const int roof = (int)g_h[idx(x, y)];
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

  // Each structure gets many attempts rather than one. A single random throw
  // lands on ground too steep or too crowded most of the time, and the map is
  // much worse for silently ending up with no ruin on it at all.
  constexpr int TRIES = 60;

  // Ruins first: they need the largest clear footprint, and if the trees get
  // in ahead of them there is nowhere left to put one.
  for (int want = 0; want < 4; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int w = 5 + next(3), d = 5 + next(3);
      if (placeRuin(BORDER + 1 + next(W - 2 * BORDER - w - 2),
                    BORDER + 1 + next(H - 2 * BORDER - d - 2), w, d, rng)) break;
    }
  }
  for (int want = 0; want < 5; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int r = 2 + next(2);
      if (placeQuarry(BORDER + r + 1 + next(W - 2 * BORDER - 2 * r - 2),
                      BORDER + r + 1 + next(H - 2 * BORDER - 2 * r - 2), r, rng)) break;
    }
  }
  // Arches and cave mouths before the trees, for the same reason ruins go
  // first: they need clear ground and the trees will take all of it.
  for (int want = 0; want < 3; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int len = 5 + next(3);
      if (placeArch(BORDER + len + next(W - 2 * BORDER - 2 * len),
                    BORDER + len + next(H - 2 * BORDER - 2 * len),
                    len, next(2) == 0, rng)) break;
    }
  }
  for (int want = 0; want < 5; ++want) {
    for (int i = 0; i < TRIES; ++i) {
      const int len = 4 + next(4);
      if (placeCave(BORDER + 1 + next(W - 2 * BORDER - len - 2),
                    BORDER + 1 + next(H - 2 * BORDER - len - 2),
                    len, next(2) == 0, rng)) break;
    }
  }

  for (int i = 0; i < 110; ++i)
    placeTree(BORDER + next(W - 2 * BORDER), BORDER + next(H - 2 * BORDER), rng);
}

// ---- access -----------------------------------------------------------------

uint8_t height(int x, int y) {
  if (outside(x, y)) return MAX_H;            // the world is walled, not open
  return g_h[idx(x, y)];
}

bool isBorder(int x, int y) {
  if (outside(x, y)) return true;
  return g_top[idx(x, y)] == B_BEDROCK;
}

Cell cellAt(int x, int y) {
  Cell c;
  if (outside(x, y)) {
    // Out of bounds reads report the wall that rings the map, so the walker
    // needs no bounds check of its own.
    c.h = MAX_H; c.top = B_BEDROCK; c.light = 0;
    c.slabBase = 0; c.slabTop = 0; c.slabMat = B_STONE;
    return c;
  }
  const int i = idx(x, y);
  c.h = g_h[i]; c.top = g_top[i]; c.light = g_light[i];
  c.slabBase = g_cBase[i]; c.slabTop = g_cTop[i]; c.slabMat = g_cMat[i];
  return c;
}

uint8_t slabBase(int x, int y) { return outside(x, y) ? 0 : g_cBase[idx(x, y)]; }
uint8_t slabTop (int x, int y) { return outside(x, y) ? 0 : g_cTop [idx(x, y)]; }
uint8_t slabMat (int x, int y) { return outside(x, y) ? B_STONE : g_cMat[idx(x, y)]; }
bool    hasSlab (int x, int y) { return slabTop(x, y) != 0; }

static void setSlab(int x, int y, int base, int top, uint8_t mat) {
  if (outside(x, y) || isBorder(x, y)) return;
  if (top > MAX_H) top = MAX_H;
  if (base < 0) base = 0;
  if (top <= base) return;
  const int i = idx(x, y);
  g_cBase[i] = (uint8_t)base;
  g_cTop[i]  = (uint8_t)top;
  g_cMat[i]  = mat;
}

void devSlab(int x, int y, int base, int top, uint8_t mat) {
  setSlab(x, y, base, top, mat);
}

uint8_t topMat(int x, int y) {
  if (outside(x, y)) return B_BEDROCK;
  return g_top[idx(x, y)];
}

uint8_t matAt(int x, int y, int z) {
  if (outside(x, y)) return B_BEDROCK;
  const int i = idx(x, y);
  const int h = g_h[i];
  if (z < 0 || z >= h) return B_BEDROCK;      // the base plane under everything
  if (z == h - 1) return g_top[i];            // the face the player sees and mines

  const uint8_t top = g_top[i];
  if (isStructure(top)) {
    // A tree is a trunk under its canopy, not leaves on a dirt plug; a ruin
    // wall is brick to the ground. Everything a structure stands on is normal
    // soil, measured from ground level rather than from the structure's top.
    if (z >= GROUND) {
      if (top == B_LEAVES) return (z >= h - 2) ? B_LEAVES : B_WOOD;
      return top;
    }
  }

  // Below the surface, material is a function of depth: a band of dirt, then
  // stone, with ore seeded by position so a given column always digs the same.
  const int surface = isStructure(top) ? GROUND : h;
  const int depth = surface - 1 - z;
  if (depth < 0) return B_DIRT;
  if (depth <= 1) return B_DIRT;
  const uint32_t o = latticeu(x * 7 + z, y * 13 - z, 0x0C0A1u);
  if (o < 214748365u) return B_IRON;    // 0.050 of the range
  if (o < 794568365u) return B_COAL;    // 0.185
  return B_STONE;
}

// ---- biomes -----------------------------------------------------------------

// One extra octave of noise at a much longer wavelength than the terrain, so a
// biome is a region many hills across rather than a per-cell speckle.
uint8_t biomeAt(int x, int y) {
  const float n = valueNoise((float)x * 0.035f, (float)y * 0.035f, g_seed ^ 0xB10E5u);
  if (n < 0.40f) return BIOME_DESERT;
  if (n > 0.62f) return BIOME_TUNDRA;
  return BIOME_PLAINS;
}

// The surface material for a biome. Everything below the surface is the same
// rock and soil everywhere — a desert is a skin, not a different world.
static uint8_t surfaceOf(uint8_t biome) {
  switch (biome) {
    case BIOME_DESERT: return B_SAND;
    case BIOME_TUNDRA: return B_SNOW;
    default:           return B_GRASS;
  }
}

// ---- light ------------------------------------------------------------------

uint8_t light(int x, int y) {
  if (outside(x, y)) return 0;
  return g_light[idx(x, y)];
}

void rebuildLight() {
  memset(g_light, 0, sizeof(g_light));
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const uint8_t e = emission(g_top[idx(x, y)]);
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
          if (v > g_light[idx(nx, ny)]) g_light[idx(nx, ny)] = v;
        }
      }
    }
  }
}

// ---- movement ---------------------------------------------------------------

bool canEnter(int fromH, int x, int y) {
  if (outside(x, y)) return false;
  const int i = idx(x, y);
  const int floorH = (int)g_h[i];
  if (floorH - fromH > STEP_UP) return false;
  // Under a slab there has to be room to stand up. Without this a body walks
  // into a bridge pier and ends up inside the deck.
  if (g_cTop[i] && (int)g_cBase[i] - floorH < HEADROOM) return false;
  return true;
}

bool fits(int fromH, float px, float py, float r) {
  const int x0 = (int)floorf(px - r), x1 = (int)floorf(px + r);
  const int y0 = (int)floorf(py - r), y1 = (int)floorf(py + r);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      if (!canEnter(fromH, x, y)) return false;
  return true;
}

uint8_t groundAt(float px, float py) {
  return height((int)floorf(px), (int)floorf(py));
}

// ---- mining -----------------------------------------------------------------

bool mine(int x, int y, int effort,
          uint8_t& dropMat, uint8_t& dropBlocks, uint8_t& dropOre) {
  if (outside(x, y) || isBorder(x, y)) return false;
  const int i = idx(x, y);
  if (g_h[i] == 0) return false;                       // nothing left to take

  const uint8_t m = g_top[i];
  const BlockInfo& bi = kInfo[m];
  if (bi.toughness == 0) return false;

  if (x != s_mineX || y != s_mineY) { s_mineX = x; s_mineY = y; s_effort = 0; }

  const int acc = (int)s_effort + effort;
  if (acc < (int)bi.toughness) { s_effort = (uint16_t)acc; return false; }

  dropMat    = m;
  dropBlocks = bi.dropBlocks;
  dropOre    = bi.dropOre;

  // What is revealed has to be worked out BEFORE the column shrinks. matAt
  // reads g_h and g_top, so calling it after the decrement asks "what is the
  // top of this column" and gets back the material that was just removed —
  // which meant mining never changed a surface at all: digging the grass off a
  // hill left grass, and taking a torch down left another torch.
  const int newH = (int)g_h[i] - 1;
  const uint8_t newTop = (newH == 0) ? surfaceOf(biomeAt(x, y))
                                     : matAt(x, y, newH - 1);
  g_h[i] = (uint8_t)newH;
  g_top[i] = newTop;
  s_mineX = s_mineY = -1;
  s_effort = 0;
  if (emission(m)) rebuildLight();       // a torch just came down
  return true;
}

void resetDamage(int x, int y) {
  if (x == s_mineX && y == s_mineY) { s_mineX = s_mineY = -1; s_effort = 0; }
}

uint8_t damage(int x, int y) {
  if (x != s_mineX || y != s_mineY) return 0;
  if (outside(x, y) || g_h[idx(x, y)] == 0) return 0;
  const uint16_t t = kInfo[g_top[idx(x, y)]].toughness;
  if (t == 0) return 0;
  const uint32_t d = (uint32_t)s_effort * 255u / t;
  return (uint8_t)(d > 255 ? 255 : d);
}

bool standable(int x, int y) {
  if (outside(x, y)) return false;
  const int i = idx(x, y);
  return !g_cTop[i] || (int)g_cBase[i] - (int)g_h[i] >= HEADROOM;
}

bool place(int x, int y, uint8_t mat) {
  if (outside(x, y) || isBorder(x, y)) return false;
  const int i = idx(x, y);
  if (g_h[i] >= MAX_H) return false;
  // Never build a floor up so close to a slab that nothing can stand between
  // them. place() used to ignore slabs completely, so a column could be raised
  // straight through a bridge deck, and the gap under one could be bricked
  // shut around whatever was standing in it — leaving a body in a cell the
  // movement rules say is impossible to be in.
  if (g_cTop[i] && (int)g_h[i] + 1 > (int)g_cBase[i] - HEADROOM) return false;
  g_h[i] = (uint8_t)(g_h[i] + 1);
  g_top[i] = mat;
  if (emission(mat)) rebuildLight();
  return true;
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
      if (g_h[i] == 0) continue;
      // Deepest at the centre, one block at the rim: a blast should leave a
      // crater, not a cylinder punched out of the ground.
      int take = radius - (int)(sqrtf((float)d2)) + 1;
      if (take > (int)g_h[i]) take = g_h[i];
      if (take <= 0) continue;
      // Same ordering as mine(): resolve the revealed material first.
      const int newH = (int)g_h[i] - take;
      const uint8_t newTop = (newH == 0) ? surfaceOf(biomeAt(x, y))
                                         : matAt(x, y, newH - 1);
      g_h[i] = (uint8_t)newH;
      g_top[i] = newTop;
      n += take;
    }
  }
  if (n) { resetDamage(s_mineX, s_mineY); rebuildLight(); }
  return n;
}

}  // namespace world
