// =============================================================================
//  game.cpp — clock, items, pathing, combat
//
//  Mobs steer by a BFS flow field rebuilt a few times a second rather than by
//  per-mob pathfinding. That is not an optimisation, it is the design: the
//  player spends the night rearranging the map, and a flow field is correct
//  the instant a wall goes up or a creeper blows a hole in one. Per-mob paths
//  would have to be invalidated by hand on every block change, and the whole
//  game is block changes.
// =============================================================================
#include "game.h"
#include "world.h"

#include <math.h>
#include <string.h>

namespace game {

// ---- tunables ---------------------------------------------------------------

constexpr float PLAYER_RADIUS = 0.26f;
constexpr float MOB_RADIUS    = 0.30f;
constexpr float MOVE_SPEED    = 3.1f;    // cells/second

// Looking up and down, in horizon pixels per second.
//
// Scaled with the range: the sweep is 560 pixels end to end now rather than
// 150, and at the old 130 a full look from floor to sky took four seconds. A
// look control you have to wait out is one you stop using.
//
// There is no return speed any more. The view used to drift back to the
// resting tilt whenever neither key was held, which meant the camera pushed
// back against the player the moment they stopped asking — you could not look
// up at a canopy and keep looking at it. Pitch is a held position now, exactly
// like the facing angle next to it, and holding both keys recentres it.
constexpr float PITCH_SPEED  = 320.0f;

// How hard the eye is pulled toward the height of the ground under it, and how
// much of its speed survives each tick. Critically damped would settle without
// overshoot; this is deliberately a little under-damped, so cresting a block
// gives a small hop at the top and landing gives a small dip. That overshoot is
// the whole feeling of the step.
constexpr float EYE_PULL     = 0.50f;
constexpr float EYE_DAMP     = 0.55f;
constexpr float EYE_SNAP     = 0.004f;   // below this, stop integrating
constexpr float STRAFE_SCALE  = 0.55f;   // walking backwards is deliberately slow
constexpr float TURN_SPEED    = 2.7f;    // radians/second
// How far the player can reach to mine or to place, as a distance from the eye
// to the block face — the same number for both, as Minecraft uses.
//
// The floor under this is not taste, it is geometry. At the resting tilt the
// aim ray leaves the eye at EYE = 1.2 above the ground and descends
// TILT / PROJ = 0.2375 per cell, so the crosshair meets flat ground at
// 1.2 / 0.2375 = 5.05 cells out, which is 5.19 away from the eye. A board with
// no pitch keys never leaves that tilt (see hal.h), so a reach below 5.19 would
// make open ground unmineable on it — Minecraft's own 4.5 is not available to
// us. 5.5 is the tightest round number that clears it, and the 0.31 of margin
// it leaves is what would break first if TILT or EYE were ever retuned.
constexpr float REACH         = 5.5f;
constexpr float MELEE_REACH   = 1.9f;
constexpr float MELEE_DOT     = 0.55f;   // roughly a 110 degree swing arc
constexpr int   SWING_TICKS   = 18;
constexpr int   BUILD_TICKS   = 9;       // ~6 blocks/second, fast enough to wall up
constexpr int   FLOW_PERIOD   = 20;      // rebuild pathing 3x/second
constexpr int   IFRAME_TICKS  = 26;     // immunity after a mob lands a blow
constexpr int   HIT_FLASH     = 5;      // ticks a struck mob shows white
constexpr int   WHIFF_FRAME   = 7;      // the strike frame of the swing animation
constexpr int   HURT_TICKS    = 12;
constexpr float MIN_SPAWN_DIST= 11.0f;
constexpr int   CREEPER_BLAST = 2;       // radius, in cells
constexpr float KNOCKBACK     = 0.55f;   // cells a mob is shoved by a landed hit
constexpr int   BURN_PERIOD   = 24;      // ticks between lava ticks, for anything standing in it
constexpr uint16_t STUCK_TICKS = 15 * TICK_HZ;   // give up after this long going nowhere
constexpr float STUCK_DIST     = 8.0f;           // ...but only this far out

struct MobInfo {
  int16_t  hp;
  float    speed;      // cells/second
  float    range;      // melee reach, fuse trigger, or firing range
  float    standoff;   // stops closing here and circles instead
  int16_t  damage;
  uint16_t cooldown;   // ticks between attacks, or fuse length
  uint16_t windup;     // ticks of committed telegraph before the blow lands
};

// standoff is deliberately just outside range: a mob that walks all the way
// into the player fills the screen, hides what is behind it, and reads as a
// bug. Holding at arm's length and circling reads as intent.
static const MobInfo kMob[MOB_COUNT] = {
  //  hp  speed  range  stand  dmg  cooldown  windup
  {   3,  1.35f, 1.35f, 1.25f,   1,  50,  16 },   // zombie   — the baseline pressure
  {   2,  1.85f, 1.90f, 1.35f,   4,  75,   0 },   // creeper  — fuse is its own telegraph
  {   2,  1.05f, 7.00f, 4.50f,   1, 130,  26 },   // skeleton — outranges you, draws first
};

constexpr float SEPARATION_R = 1.05f;   // how far mobs hold off each other
constexpr int   LOS_PERIOD   = 6;       // ticks between line-of-sight refreshes
constexpr float CREEPER_LUNGE = 1.45f;  // speed multiplier once the fuse is lit
constexpr uint8_t FLOW_HOLD   = 45;     // ticks on the grid after walking into something

// A creeper that has got this close and then stopped getting closer for this
// long lights its fuse anyway. The range check alone was not enough to deliver
// what the fuse is documented to be for — see the creeper branch in updateMob.
constexpr float    CREEPER_PRESS_REACH = 3.5f;
constexpr uint16_t CREEPER_PRESS_TICKS = 90;    // 1.5 s

// A siege is a trickle, not a flood. Left uncapped it fills the mob array with
// creepers that hold each OTHER at arm's length — see the same branch.
constexpr int SIEGE_CAP = 4;

static uint32_t s_aiTick = 0;


// The tool table. Mining effort is per tick against world::EFFORT_PER_TICK's
// 16, so a wooden pickaxe is exactly the tool the game used to hand out for
// free and every tier above it is new ground.
//
// Swords all mine at hand speed. That is the whole reason to have two tools
// rather than one good one: holding the sword costs you the ability to dig,
// holding the pickaxe costs you the ability to fight, and the hotbar is a
// choice you keep making instead of a label.
//
// Diamond's damage is 127 rather than 4. Nothing on the map has more than
// three hearts, so anything above three is already a one-hit kill -- naming it
// 127 says the one-hit is the POINT of the tier, and keeps it a one-hit if mob
// health is ever retuned upward.
static const ToolInfo kTool[TK_COUNT][TT_COUNT] = {
  // pickaxes:  name        dur  effort  dmg
  { { "WOOD PICK",           60,     16,   1 },
    { "STONE PICK",         132,     24,   1 },
    { "IRON PICK",          251,     40,   1 },
    { "DIAMOND PICK",      1562,     64,   1 } },
  // swords:
  { { "WOOD SWORD",          60,  HAND_EFFORT,   2 },
    { "STONE SWORD",        132,  HAND_EFFORT,   2 },
    { "IRON SWORD",         251,  HAND_EFFORT,   3 },
    { "DIAMOND SWORD",     1562,  HAND_EFFORT, 127 } },
};

const ToolInfo& toolInfo(uint8_t kind, uint8_t tier) {
  return kTool[kind < TK_COUNT ? kind : 0][tier < TT_COUNT ? tier : 0];
}

// Twelve recipes over four cells. Two things hold the table together.
//
// Every tool takes a plank as its handle, so the chain out of an empty spawn is
// always wood -> planks -> tool, and the first pickaxe is three planks from one
// log. That is the only bootstrap the game has now, and it wants to be short.
//
// No two rows are the same multiset. Worth checking by eye when adding one:
// matching is order-blind, so "2 plank" and "3 plank" are a sword and a
// pickaxe, and a fourth plank recipe would have to find a fourth count.
static const RecipeInfo kRecipe[R_COUNT] = {
  { "PLANKS",  { world::B_WOOD,  CELL_EMPTY,     CELL_EMPTY, CELL_EMPTY },
    world::B_PLANK, 3, 0 },
  { "TORCH",   { world::B_WOOD,  world::B_COAL,  CELL_EMPTY, CELL_EMPTY },
    world::B_TORCH, 4, 0 },
  { "BRICKS",  { world::B_STONE, world::B_STONE, world::B_STONE, CELL_EMPTY },
    world::B_BRICK, 3, 0 },
  { "PATCH",   { world::B_LEAVES, world::B_LEAVES, world::B_WOOD, CELL_EMPTY },
    ITEM_NONE, 0, 2 },

  { "WOOD PICK",    { world::B_PLANK, world::B_PLANK, world::B_PLANK, CELL_EMPTY },
    toolId(TK_PICK, TT_WOOD), 1, 0 },
  { "STONE PICK",   { world::B_STONE, world::B_STONE, world::B_PLANK, CELL_EMPTY },
    toolId(TK_PICK, TT_STONE), 1, 0 },
  { "IRON PICK",    { world::B_IRON, world::B_IRON, world::B_PLANK, CELL_EMPTY },
    toolId(TK_PICK, TT_IRON), 1, 0 },
  { "DIAMOND PICK", { world::B_DIAMOND, world::B_DIAMOND, world::B_PLANK, CELL_EMPTY },
    toolId(TK_PICK, TT_DIAMOND), 1, 0 },

  { "WOOD SWORD",    { world::B_PLANK, world::B_PLANK, CELL_EMPTY, CELL_EMPTY },
    toolId(TK_SWORD, TT_WOOD), 1, 0 },
  { "STONE SWORD",   { world::B_STONE, world::B_PLANK, CELL_EMPTY, CELL_EMPTY },
    toolId(TK_SWORD, TT_STONE), 1, 0 },
  { "IRON SWORD",    { world::B_IRON, world::B_PLANK, CELL_EMPTY, CELL_EMPTY },
    toolId(TK_SWORD, TT_IRON), 1, 0 },
  { "DIAMOND SWORD", { world::B_DIAMOND, world::B_PLANK, CELL_EMPTY, CELL_EMPTY },
    toolId(TK_SWORD, TT_DIAMOND), 1, 0 },
};

const RecipeInfo& recipeInfo(uint8_t r) { return kRecipe[r < R_COUNT ? r : 0]; }

// ---- items -------------------------------------------------------------------

// Every slot bare. A run opens with nothing in it, including slot 0.
static void resetSlots(State& s) {
  for (int i = 0; i < SLOT_N; ++i) { s.slot[i] = SLOT_EMPTY; s.dur[i] = 0; }
  for (int i = 0; i < GRID_N; ++i) s.grid[i] = CELL_EMPTY;
  s.sel = 0;
  s.gridSel = 0;
}

uint8_t heldItem(const State& s)  { return s.slot[s.sel % SLOT_N]; }
bool    heldIsTool(const State& s){ return isTool(heldItem(s)); }

uint8_t heldBlock(const State& s) {
  const uint8_t it = heldItem(s);
  return it < world::B_COUNT ? it : (uint8_t)world::B_COUNT;
}

uint16_t heldCount(const State& s) {
  const uint8_t b = heldBlock(s);
  return b < world::B_COUNT ? s.inv[b] : (uint16_t)0;
}

void cycleBlock(State& s, int delta) {
  s.sel = (uint8_t)((s.sel + delta % SLOT_N + SLOT_N) % SLOT_N);
}

void selectSlot(State& s, int index) {
  if (index >= 0 && index < SLOT_N) s.sel = (uint8_t)index;
}

// True if the bar has anywhere to put this material.
//
// This is the whole of what "inventory full" means here. Stacks are unbounded,
// so a material that already has a slot always fits; what runs out is slots,
// and sixteen materials against nine of them means running out is ordinary.
bool canAccept(const State& s, uint8_t mat) {
  if (mat >= world::B_COUNT) return false;
  for (int i = 0; i < SLOT_N; ++i) if (s.slot[i] == mat) return true;
  for (int i = 0; i < SLOT_N; ++i) if (s.slot[i] == SLOT_EMPTY) return true;
  return false;
}

// Puts `n` of a material into the inventory. Returns how many it took, which
// is all of them or none: stacks do not have a cap, so the only question is
// whether the material has a slot at all.
//
// It used to take them unconditionally and let the count sit in inv[] with no
// slot to show it -- "nothing is ever lost, it is only temporarily unplaceable"
// was the rule. That rule is gone, and the reason is that it was invisible: you
// mined masonry with a full bar, the game said nothing, and you owned a
// material you could not see, select or place. What will not fit is spilled on
// the floor now, where it is a thing you can look at and decide about.
static int giveItem(State& s, uint8_t mat, int n) {
  if (mat >= world::B_COUNT || n <= 0) return 0;
  for (int i = 0; i < SLOT_N; ++i)
    if (s.slot[i] == mat) { s.inv[mat] = (uint16_t)(s.inv[mat] + n); return n; }
  for (int i = 0; i < SLOT_N; ++i)
    if (s.slot[i] == SLOT_EMPTY) {
      s.slot[i] = mat;
      s.inv[mat] = (uint16_t)(s.inv[mat] + n);
      return n;
    }
  return 0;
}

// Spends from the inventory, and hands the slot back when the last one goes.
// An emptied slot has to clear rather than sit there at zero: a bar full of
// materials you no longer own is a bar with no room for the ones you do.
//
// There is no re-let pass any more, and there is nothing left for one to do.
// While giveItem could take a material with no slot to show it, freeing a slot
// meant looking for a material that had been waiting in the dark for one. Now
// stock and slot are the same fact -- inv[m] is non-zero exactly when m has a
// slot -- so an emptied slot is simply empty.
static void takeItem(State& s, uint8_t mat, int n) {
  if (mat >= world::B_COUNT) return;
  s.inv[mat] = (uint16_t)(s.inv[mat] > n ? s.inv[mat] - n : 0);
  if (s.inv[mat]) return;
  for (int i = 0; i < SLOT_N; ++i)
    if (s.slot[i] == mat) { s.slot[i] = SLOT_EMPTY; break; }
}

// Puts a freshly made tool on the bar at full durability. False when the bar
// is full, which is what makes canCraft refuse a tool recipe there.
static bool giveTool(State& s, uint8_t item) {
  for (int i = 0; i < SLOT_N; ++i) {
    if (s.slot[i] != SLOT_EMPTY) continue;
    s.slot[i] = item;
    s.dur[i] = toolInfo(toolKind(item), toolTier(item)).durability;
    return true;
  }
  return false;
}

// Spends one point of the selected tool. Returns EV_TOOL_BROKE on the point
// that finishes it, and leaves the slot empty.
static uint32_t wearTool(State& s) {
  const int i = s.sel % SLOT_N;
  if (!isTool(s.slot[i])) return 0;
  if (s.dur[i] > 1) { --s.dur[i]; return 0; }
  s.dur[i] = 0;
  s.slot[i] = SLOT_EMPTY;
  return EV_TOOL_BROKE;
}

// ---- dropped items ----------------------------------------------------------

// Defined with the rest of the effects, well below. Declared here because the
// only thing this section needs from it is one puff when lava takes an item.
static void spark(State& s, uint8_t kind, float x, float y, float z,
                  uint8_t mat, uint8_t mag);

int dropsAlive(const State& s) {
  int n = 0;
  for (int i = 0; i < MAX_DROPS; ++i) if (s.drops[i].alive) ++n;
  return n;
}

// Finds a slot in the drop array, recycling the most nearly expired one when
// every slot is taken. Dropping has to be something that always works: it is
// the only way out of a full bar, and a drop key that silently did nothing
// because sixteen other things were lying about would be the worst possible
// moment for it to fail.
static Drop& dropSlot(State& s) {
  int best = 0;
  uint16_t oldest = 0xFFFF;
  for (int i = 0; i < MAX_DROPS; ++i) {
    if (!s.drops[i].alive) return s.drops[i];
    if (s.drops[i].life < oldest) { oldest = s.drops[i].life; best = i; }
  }
  return s.drops[best];
}

// Puts an item on the floor at a point, with a velocity. Everything that
// spills goes through here: the drop key, a mined block with nowhere to go,
// and a craft result that would not fit.
static void spawnDrop(State& s, uint8_t item, uint8_t count, uint16_t dur,
                      float x, float y, float z,
                      float vx, float vy, float vz, uint8_t arm) {
  Drop& d = dropSlot(s);
  d = Drop{};
  d.x = x; d.y = y; d.z = z;
  d.vx = vx; d.vy = vy; d.vz = vz;
  d.life = (uint16_t)DROP_LIFE;
  d.dur = dur;
  d.item = item;
  d.count = count;
  d.arm = arm;
  d.alive = true;
  d.rest = false;
}

// A block that could not be carried, spilled where it was broken rather than
// thrown. No sideways velocity: it should land on the spot it came off, not
// skitter away down a slope the player then has to chase.
//
// One entity carrying the whole count, not one per block. Iron yields three,
// and three entities for one swing would churn through a sixteen-slot array in
// six blocks -- recycling, by design, throws the oldest away, so the cheapest
// way to make spilled items get lost is to spend three slots describing one
// pile of them.
static void spillAt(State& s, uint8_t mat, int n, float x, float y, float z) {
  if (n <= 0) return;
  spawnDrop(s, mat, (uint8_t)(n > 255 ? 255 : n), 0, x, y, z,
            0.0f, 0.0f, 0.0f, 0);
}

bool dropOne(State& s) {
  const int i = s.sel % SLOT_N;
  const uint8_t it = s.slot[i];
  if (it == SLOT_EMPTY) return false;

  // Thrown from the eye, along the look direction, with some loft on it. The
  // arc is what says the item left your hand rather than falling through the
  // floor at your feet.
  const float x = s.cam.px + s.cam.dx * 0.35f;
  const float y = s.cam.py + s.cam.dy * 0.35f;
  const float z = s.cam.z - 0.25f;
  const float vx = s.cam.dx * DROP_TOSS / (float)TICK_HZ;
  const float vy = s.cam.dy * DROP_TOSS / (float)TICK_HZ;
  const float vz = DROP_LOFT / (float)TICK_HZ;

  if (isTool(it)) {
    spawnDrop(s, it, 1, s.dur[i], x, y, z, vx, vy, vz, (uint8_t)DROP_ARM);
    s.slot[i] = SLOT_EMPTY;
    s.dur[i] = 0;
    return true;
  }

  if (it >= world::B_COUNT || s.inv[it] == 0) return false;
  spawnDrop(s, it, 1, 0, x, y, z, vx, vy, vz, (uint8_t)DROP_ARM);
  takeItem(s, it, 1);
  return true;
}

// Falls, settles, expires, and gets collected. One pass over sixteen entries,
// which is nothing next to the mob loop it sits beside.
static uint32_t updateDrops(State& s) {
  uint32_t ev = 0;
  constexpr float G = DROP_GRAVITY / (float)TICK_HZ / (float)TICK_HZ;

  for (int i = 0; i < MAX_DROPS; ++i) {
    Drop& d = s.drops[i];
    if (!d.alive) continue;
    if (d.arm) --d.arm;
    if (--d.life == 0) { d.alive = false; continue; }

    if (!d.rest) {
      d.vz -= G;
      // Horizontal first, and refused as a whole if the destination cell is
      // solid. An item that has hit a wall should stop against it, not slide
      // along inside it looking for a way through.
      const float nx = d.x + d.vx, ny = d.y + d.vy;
      if (!world::solidAt((int)nx, (int)ny, (int)d.z)) { d.x = nx; d.y = ny; }
      else { d.vx = 0.0f; d.vy = 0.0f; }

      d.z += d.vz;
      const int iz = (int)d.z;
      if (d.z < 1.0f || world::solidAt((int)d.x, (int)d.y, iz)) {
        // Sat down on whatever it entered. Resting ON that cell means the top
        // of it, which is one above -- the same convention feetZ uses.
        d.z = (float)(iz + 1);
        d.vx = d.vy = d.vz = 0.0f;
        d.rest = true;
      }
    }

    // Lava eats it. A pool at the bottom of a quarry is already a hazard and a
    // light; this makes it somewhere you can lose things too, which is what
    // stops it being scenery you bridge over without looking.
    if (world::isHazard(world::blockAt((int)d.x, (int)d.y, (int)d.z - 1))) {
      d.alive = false;
      spark(s, SP_BREAK, d.x, d.y, d.z, d.item < world::B_COUNT ? d.item : 0, 90);
      continue;
    }

    if (d.arm) continue;

    const float dx = d.x - s.cam.px, dy = d.y - s.cam.py;
    const float dz = d.z - (s.cam.z - 0.6f);
    if (dx * dx + dy * dy + dz * dz > DROP_REACH * DROP_REACH) continue;

    // Collected -- unless there is still nowhere to put it, in which case it
    // stays exactly where it is and you walk over it again. Refusing quietly is
    // the right answer: the item is visible, so the player can already see that
    // something is not happening and why.
    if (isTool(d.item)) {
      for (int k = 0; k < SLOT_N; ++k) {
        if (s.slot[k] != SLOT_EMPTY) continue;
        s.slot[k] = d.item;
        s.dur[k] = d.dur;
        d.alive = false;
        ev |= EV_PICKUP;
        break;
      }
    } else if (giveItem(s, d.item, d.count)) {
      d.alive = false;
      ev |= EV_PICKUP;
    }
  }
  return ev;
}

uint16_t totalBlocks(const State& s) {
  uint16_t n = 0;
  for (uint8_t m = 0; m < world::B_COUNT; ++m) n = (uint16_t)(n + s.inv[m]);
  return n;
}

// Sorts four cells so two multisets can be compared elementwise. CELL_EMPTY is
// 255 and therefore sorts last on its own, which is what lets a two-cell recipe
// and a three-cell one be told apart without counting anything.
static void sortCells(uint8_t out[GRID_N], const uint8_t in[GRID_N]) {
  for (int i = 0; i < GRID_N; ++i) out[i] = in[i];
  for (int i = 1; i < GRID_N; ++i) {           // insertion sort, four elements
    const uint8_t v = out[i];
    int j = i - 1;
    while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; --j; }
    out[j + 1] = v;
  }
}

uint8_t matchGrid(const uint8_t grid[GRID_N]) {
  uint8_t a[GRID_N];
  sortCells(a, grid);
  if (a[0] == CELL_EMPTY) return R_NONE;       // nothing laid out at all
  for (uint8_t r = 0; r < R_COUNT; ++r) {
    uint8_t b[GRID_N];
    sortCells(b, kRecipe[r].cells);
    bool same = true;
    for (int i = 0; i < GRID_N && same; ++i) same = (a[i] == b[i]);
    if (same) return r;
  }
  return R_NONE;
}

// How many of `mat` a set of four cells asks for.
static int cellCount(const uint8_t cells[GRID_N], uint8_t mat) {
  int n = 0;
  for (int i = 0; i < GRID_N; ++i) if (cells[i] == mat) ++n;
  return n;
}

// True if inv[] covers every material in `cells`. Counted per distinct
// material rather than per cell, because takeItem clamps at zero silently --
// spending three planks one at a time when you own one leaves you owning none
// and holding a pickaxe you did not pay for.
static bool coversCells(const State& s, const uint8_t cells[GRID_N]) {
  for (int i = 0; i < GRID_N; ++i) {
    const uint8_t m = cells[i];
    if (m == CELL_EMPTY) continue;
    if (m >= world::B_COUNT) return false;
    if (s.inv[m] < cellCount(cells, m)) return false;
  }
  return true;
}

// True if the bar has room for a tool, or the recipe does not make one.
static bool hasRoomFor(const State& s, const RecipeInfo& r) {
  if (!isTool(r.outItem)) return true;
  for (int i = 0; i < SLOT_N; ++i) if (s.slot[i] == SLOT_EMPTY) return true;
  return false;
}

bool canCraft(const State& s, uint8_t recipe) {
  if (recipe >= R_COUNT) return false;
  const RecipeInfo& r = kRecipe[recipe];
  return coversCells(s, r.cells) && hasRoomFor(s, r);
}

bool canAffordGrid(const State& s) {
  const uint8_t r = matchGrid(s.grid);
  if (r == R_NONE) return false;
  return coversCells(s, s.grid) && hasRoomFor(s, kRecipe[r]);
}

// Grants a recipe's result. Split out so craft() and craftGrid() cannot drift
// on what "the output" means -- there are three kinds of it now.
static bool grantResult(State& s, const RecipeInfo& r) {
  if (isTool(r.outItem)) {
    if (!giveTool(s, r.outItem)) return false;
  } else if (r.outItem < world::B_COUNT) {
    // Usually there is room, because spending the inputs has just freed the
    // slot they were sitting in. When there is not -- crafting from materials
    // you still have plenty of -- the result lands at your feet rather than
    // being quietly binned. Crafting must never destroy what it just made.
    const int took = giveItem(s, r.outItem, r.outQty);
    if (took < r.outQty)
      spillAt(s, r.outItem, r.outQty - took, s.cam.px, s.cam.py, s.cam.z - 0.4f);
  }
  if (r.heal) {
    s.hp = (int16_t)(s.hp + r.heal);
    if (s.hp > s.maxHp) s.hp = s.maxHp;
  }
  return true;
}

bool craft(State& s, uint8_t recipe) {
  if (!canCraft(s, recipe)) return false;
  const RecipeInfo& r = kRecipe[recipe];
  // Safe to spend first: canCraft has already established both that the
  // inventory covers the cells and that a tool result has a slot waiting, so
  // grantResult below cannot be the thing that fails.
  for (int i = 0; i < GRID_N; ++i)
    if (r.cells[i] != CELL_EMPTY) takeItem(s, r.cells[i], 1);
  return grantResult(s, r);
}

bool craftGrid(State& s) {
  const uint8_t r = matchGrid(s.grid);
  if (r == R_NONE) return false;
  if (!coversCells(s, s.grid) || !hasRoomFor(s, kRecipe[r])) return false;
  for (int i = 0; i < GRID_N; ++i)
    if (s.grid[i] != CELL_EMPTY) takeItem(s, s.grid[i], 1);
  const bool ok = grantResult(s, kRecipe[r]);
  for (int i = 0; i < GRID_N; ++i) s.grid[i] = CELL_EMPTY;
  return ok;
}

bool fillGrid(State& s, uint8_t recipe) {
  if (!canCraft(s, recipe)) return false;
  for (int i = 0; i < GRID_N; ++i) s.grid[i] = kRecipe[recipe].cells[i];
  s.gridSel = 0;
  return true;
}

// Where each arrow goes from each stop. Written out rather than computed,
// because the card is not a rectangle -- it is a 2x2 grid with a result slot
// beside it and a book row under it:
//
//     [0][1]   [OUT]
//     [2][3]
//     [  RECIPES  ]
//
// Every cell of the table lands somewhere real, so no arrow is ever a no-op.
// A dead key on a card reads as the card being broken.
static const uint8_t kGridNav[GRID_FOCUS_N][4] = {
  //            left  right   up   down
  /* 0     */ {    4,     1,    5,     2 },
  /* 1     */ {    0,     4,    5,     3 },
  /* 2     */ {    4,     3,    0,     5 },
  /* 3     */ {    2,     4,    1,     5 },
  /* OUT   */ {    1,     0,    5,     5 },
  /* BOOK  */ {    4,     4,    2,     0 },
};

void gridMove(State& s, int dx, int dy) {
  if (s.gridSel >= GRID_FOCUS_N) s.gridSel = 0;
  int dir = -1;
  if (dx < 0) dir = 0;
  else if (dx > 0) dir = 1;
  else if (dy < 0) dir = 2;
  else if (dy > 0) dir = 3;
  if (dir < 0) return;
  s.gridSel = kGridNav[s.gridSel][dir];
}

// Cycles one cell through the materials the player actually holds, plus empty.
// Only what is held: a grid that can name a material you have none of is a grid
// that spends most of its keypresses on things you cannot make.
void gridCycle(State& s, int delta) {
  // Only a cell holds a material. On the result slot or the book row there is
  // nothing to cycle, and quietly editing cell 0 from the far side of the card
  // is the sort of thing a modulo would have done here.
  if (!gridOnCell(s)) return;
  uint8_t opts[world::B_COUNT + 1];
  int n = 0;
  opts[n++] = CELL_EMPTY;
  for (uint8_t m = 0; m < world::B_COUNT; ++m) if (s.inv[m]) opts[n++] = m;

  const uint8_t cur = s.grid[s.gridSel];
  int at = 0;
  for (int i = 0; i < n; ++i) if (opts[i] == cur) { at = i; break; }
  at = (at + delta % n + n) % n;
  s.grid[s.gridSel] = opts[at];
}

// ---- pathing ----------------------------------------------------------------

// 255 means unreachable. Rebuilt from the player's cell; mobs simply walk
// downhill. The queue is a plain ring of cell indices — 8 KB of static, which
// is cheaper than the branchy alternatives and never allocates mid-frame.
// The pathing field. One entry per cell: the high byte is the step distance
// from the player (255 = never reached), the low byte is the surface height
// that distance was measured at.
//
// One surface per cell, not one per run. The surface a cell is FIRST reached at
// is by construction the one on the shortest route, so a second slot could only
// ever describe a longer way to the same place. What that gives up is a cell
// where a ground-level route and a bridge-deck route are both useful — and
// there the mobs take the ground and mill about, which is what they did before
// any of this. What it buys is fitting in memory: four surfaces a cell with an
// exactly-sized queue is 108 KB, and this board has none of that spare.
static uint16_t g_flow[world::W * world::H];

// A ring, not one slot per cell. A breadth-first sweep of a 96x96 grid never
// has more than a couple of frontier rings pending at once — a few hundred
// cells — so an exactly-sized queue was eighteen kilobytes standing idle.
// Overflow drops the cell, which reads as unreachable: the pathing degrades
// rather than corrupting, and the field is rebuilt three times a second
// anyway. Shrink this first if RAM runs short.
constexpr int FLOW_QUEUE = 2048;
static uint16_t g_queue[FLOW_QUEUE];

constexpr uint16_t FLOW_NONE = 0xFFFF;
static inline uint16_t flowPack(int d, int z) {
  return (uint16_t)(((d & 0xFF) << 8) | (z & 0xFF));
}
static inline uint8_t flowDist(uint16_t v) { return (uint8_t)(v >> 8); }
static inline uint8_t flowSurf(uint16_t v) { return (uint8_t)(v & 0xFF); }

static void rebuildFlow(int sx, int sy, int sz) {
  memset(g_flow, 0xFF, sizeof(g_flow));
  if ((unsigned)sx >= (unsigned)world::W || (unsigned)sy >= (unsigned)world::H) return;

  int head = 0, count = 0;
  const uint16_t start = (uint16_t)(sy * world::W + sx);
  g_flow[start] = flowPack(0, sz);
  g_queue[count++] = start;

  // The source cell is seeded whatever its height — a creeper can leave the
  // player standing in a crater, and the mobs should still converge on them.
  while (count > 0) {
    const uint16_t idx = g_queue[head];
    head = (head + 1) % FLOW_QUEUE;
    --count;
    // Divide, not shift: world::W is 96 and not a power of two. It is a
    // constant, so the compiler turns both of these into a multiply and a
    // shift anyway.
    const int x = idx % world::W;
    const int y = idx / world::W;
    const uint8_t d = flowDist(g_flow[idx]);
    const int     z = (int)flowSurf(g_flow[idx]);
    if (d >= 254) continue;

    static const int kDx[4] = { 1, -1, 0, 0 };
    static const int kDy[4] = { 0, 0, 1, -1 };
    for (int i = 0; i < 4; ++i) {
      const int nx = x + kDx[i], ny = y + kDy[i];
      if ((unsigned)nx >= (unsigned)world::W || (unsigned)ny >= (unsigned)world::H) continue;
      const uint16_t n = (uint16_t)(ny * world::W + nx);
      if (g_flow[n] != FLOW_NONE) continue;

      // Where a body standing here would come to rest over there.
      const uint8_t nz = world::surfaceUnder(nx, ny, z);
      if (nz == world::NO_SURFACE) continue;

      // The sweep runs outward from the player, but a mob travels the other
      // way along it — from n back into this cell — so the step has to be
      // legal in that direction too. Get this backwards and mobs walk up
      // cliffs. Asking it as "and could it get back?" is also what keeps a
      // two-high wall unclimbable from both sides, which is what the whole
      // wall-yourself-in loop rests on.
      if (world::surfaceUnder(x, y, (int)nz) != (uint8_t)z) continue;

      g_flow[n] = flowPack(d + 1, nz);
      if (count < FLOW_QUEUE) {
        g_queue[(head + count) % FLOW_QUEUE] = n;
        ++count;
      }
    }
  }
}

static inline uint8_t flowAt(int x, int y) {
  if ((unsigned)x >= (unsigned)world::W || (unsigned)y >= (unsigned)world::H) return 0xFF;
  return flowDist(g_flow[y * world::W + x]);
}

// The height the field expects a body to be at in this cell, so a mob that
// steps into it lands where the route was measured.
// Ticks since the field was last rebuilt. FLOW_PERIOD apart, except when an
// edit to the world forces it early.
static int s_flowAge = 0;

static inline uint8_t flowZAt(int x, int y) {
  if ((unsigned)x >= (unsigned)world::W || (unsigned)y >= (unsigned)world::H)
    return world::NO_SURFACE;
  const uint16_t v = g_flow[y * world::W + x];
  return v == FLOW_NONE ? world::NO_SURFACE : flowSurf(v);
}

// ---- helpers ----------------------------------------------------------------

static inline uint32_t nextRand(State& s) {
  s.rng ^= s.rng << 13; s.rng ^= s.rng >> 17; s.rng ^= s.rng << 5;
  return s.rng;
}

// Height-aware: the segment is sampled in three dimensions, so a skeleton
// cannot shoot over a one-block lip it is standing behind, nor through a
// six-block outcrop it happens to be level with.
static bool lineOfSight(float x0, float y0, float z0, float x1, float y1, float z1) {
  const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
  const int steps = (int)(sqrtf(dx * dx + dy * dy) * 4.0f) + 1;
  for (int i = 1; i < steps; ++i) {
    const float t = (float)i / (float)steps;
    const int cx = (int)floorf(x0 + dx * t), cy = (int)floorf(y0 + dy * t);
    const float z = z0 + dz * t;
    // Solid anywhere the line passes through, ground or overhead alike --
    // without the second part a skeleton shoots clean through a bridge deck it
    // is standing under. One bit test now says both.
    if (z < 0.0f) return false;
    const world::Cell c = world::cellAt(cx, cy);
    const int zi = (int)z;
    if (zi >= world::MAX_H) continue;
    if ((c.solid >> zi) & 1u) return false;
  }
  return true;
}

// Per-axis so a body slides along a wall instead of sticking to it. Sticking
// is fatal here — the player backing into a corner during a wave must not stop.
//
// fromH is the height the body is currently standing on: a one-block rise is
// walked up without noticing, anything taller stops it. That is the whole
// vertical movement model, and it is why a two-high wall is worth building.
static void slide(float& px, float& py, float vx, float vy, float r, int fromH) {
  // Already overlapping something it should not be. This happens when the
  // ground under a body changes — it walks up a step and the cell beyond is
  // suddenly two higher, or a creeper drops the floor out from under it — and
  // once it does, every move is refused and the body is wedged there for the
  // rest of the night. Ease back toward the middle of the cell it is actually
  // standing in, which is by definition somewhere it may be.
  if (!world::fits(fromH, px, py, r)) {
    px += (floorf(px) + 0.5f - px) * 0.25f;
    py += (floorf(py) + 0.5f - py) * 0.25f;
    return;
  }
  if (vx != 0.0f && world::fits(fromH, px + vx, py, r)) px += vx;
  if (vy != 0.0f && world::fits(fromH, px, py + vy, r)) py += vy;
}

// ---- lifecycle --------------------------------------------------------------

void begin(State& s, uint32_t seed) {
  s = State{};
  resetSlots(s);
  s.rng = seed ? seed : 1u;
  world::generate(seed);

  s.cam.px = (float)(world::W / 2) + 0.5f;
  s.cam.py = (float)(world::H / 2) + 0.5f;
  s.feetZ  = world::groundAt(s.cam.px, s.cam.py);
  s.cam.z  = (float)s.feetZ + raycast::EYE;
  s.eyeZ   = s.cam.z;              // start settled, not falling into the world
  s.eyeVel = 0.0f;
  s.angle  = 0.0f;
  raycast::setAngle(s.cam, s.angle);
  s.pitch  = (float)raycast::HORIZON;
  raycast::setPitch(s.cam, raycast::HORIZON);

  rebuildFlow((int)s.cam.px, (int)s.cam.py, (int)s.feetZ);
  s_flowAge = 0;
}

float daylight(const State& s) {
  if (s.phase != PH_DAY) return 0.0f;
  const float t = (float)s.phaseTick / (float)DAY_TICKS;
  if (t < 0.12f) return t / 0.12f;             // sunrise
  if (t > 0.75f) return (1.0f - t) / 0.25f;    // sunset — the panic window
  return 1.0f;
}

uint32_t score(const State& s) {
  // Surviving is still worth most. Ore used to be a separate currency counted
  // here at five apiece; it is an item now, so the two metals are weighted
  // where the currency was rather than counting as one block each.
  // Five apiece, which is what the ore currency was worth. The yields were
  // moved onto the blocks one for one — an iron block gave 3 ore and now gives
  // 3 iron — so weighting the items the same keeps a run's score comparable
  // with the ones already in NVS.
  //
  // Diamond is weighted at twenty-five. It drops one to iron's three, sits in
  // the bottom three layers at half a percent, and costs three thousand effort
  // a block, so five apiece would have made a seam worth less than the stone
  // dug through to reach it.
  return (uint32_t)s.night * 100u
       + (uint32_t)s.inv[world::B_DIAMOND] * 25u
       + (uint32_t)s.inv[world::B_IRON] * 5u
       + (uint32_t)s.inv[world::B_COAL] * 5u
       + (uint32_t)totalBlocks(s);
}

// ---- spawning ---------------------------------------------------------------

// How long the player has to be unreachable before the game answers. Short
// enough that turtling never feels safe, long enough that ducking behind a
// rock for a moment does not summon anything.
constexpr uint16_t SEALED_TRIGGER = 2 * TICK_HZ;

// Is anything actually able to walk to the player right now? The flow field
// already knows: it is a BFS from the player over cells a body can enter, so a
// mob standing on an unreachable cell has been walled out.
static void updateSealed(State& s) {
  int alive = 0, reachable = 0;
  for (int i = 0; i < MAX_MOBS; ++i) {
    if (!s.mobs[i].alive) continue;
    ++alive;
    if (flowAt((int)s.mobs[i].x, (int)s.mobs[i].y) != 0xFF) ++reachable;
  }
  if (alive > 0 && reachable == 0) {
    if (s.sealedTicks < 0xFFF0) ++s.sealedTicks;
  } else if (s.sealedTicks) {
    --s.sealedTicks;
  }
}

static uint8_t pickKind(State& s) {
  // Walled in: send the one thing that can do something about it. This is the
  // answer to turtling rather than giving every mob a pickaxe — a zombie that
  // chews through walls is just a slower zombie, whereas a creeper arriving
  // because you sealed yourself in is a consequence you can see coming and
  // choose to fight instead.
  if (s.sealedTicks > SEALED_TRIGGER) return MOB_CREEPER;

  // A flat mix from the first night. It used to unlock creepers on night two
  // and skeletons on night four, which is a wave-game's ramp: the pressure came
  // from a counter rather than from the dark. Minecraft's night one has all
  // three in it, and what makes a later night harder is that you are further
  // from home and deeper in a hole.
  const uint32_t r = nextRand(s) % 100u;
  if (r < 45) return MOB_ZOMBIE;
  if (r < 75) return MOB_SKELETON;
  return MOB_CREEPER;
}

static bool spawnMob(State& s) {
  int slot = -1;
  for (int i = 0; i < MAX_MOBS; ++i) if (!s.mobs[i].alive) { slot = i; break; }
  if (slot < 0) return false;

  // A sealed-in player makes every cell on the map unreachable — the flow
  // field is a BFS out of their own cell and it cannot leave the box. Holding
  // the usual reachability rule there means the siege can never be spawned at
  // all, which is precisely the case it exists for. So when the walls are up,
  // drop the requirement and put it near the wall instead of across the map:
  // it has nowhere to path to anyway, it only has to arrive and go off.
  const bool siege = s.sealedTicks > SEALED_TRIGGER;
  const float minDist = siege ? 4.5f : MIN_SPAWN_DIST;

  // Spawn onto reachable ground, far enough away that the player hears it
  // coming rather than finding it already on top of them.
  for (int tries = 0; tries < 64; ++tries) {
    const int x = (int)(nextRand(s) % (uint32_t)world::W);
    const int y = (int)(nextRand(s) % (uint32_t)world::H);
    if (world::isBorder(x, y)) continue;
    // Somewhere a body can actually stand. The siege path skips the flow-field
    // check, which is what used to let a creeper be dropped into the gap under
    // a bridge too shallow to stand in.
    if (!world::standable(x, y)) continue;
    // Nothing spawns on lit ground. This is the whole point of a torch, and
    // the reason building has a use beyond walls.
    if (world::light(x, y) > 0) continue;
    if (!siege && flowAt(x, y) == 0xFF) continue;
    const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    const float dx = fx - s.cam.px, dy = fy - s.cam.py;
    const float d2 = dx * dx + dy * dy;
    if (d2 < minDist * minDist) continue;
    if (siege && d2 > 12.0f * 12.0f) continue;   // near the wall, not across the map

    Mob& m = s.mobs[slot];
    m.alive = true;
    m.kind  = pickKind(s);
    m.hp    = kMob[m.kind].hp;
    m.x = fx; m.y = fy;
    // Spawned onto the terrain, never onto a player's roof. A wave that can
    // appear on top of what you built is not a wave you can wall out.
    m.z = world::groundAt(fx, fy);
    m.timer = 0;
    m.windup = 0;
    m.burn = 0;
    m.idle = 0;
    m.bestDist = 1e9f;
    m.los = false;
    m.flowHold = 0;
    // Fixed at spawn so a mob circles one way consistently. Picking per tick
    // would make it jitter on the spot instead of orbiting.
    m.side = (uint8_t)(nextRand(s) & 1u);
    // Staggered at spawn rather than started at zero, or a night's worth of
    // mobs would all moan on the same tick for the whole night.
    m.press = 0;
    m.voice = (uint16_t)(TICK_HZ + nextRand(s) % (uint32_t)(5 * TICK_HZ));
    return true;
  }
  return false;
}

// ---- mobs -------------------------------------------------------------------

// Feet-to-feet height difference between a body and the player, and the true
// separation including it.
//
// Range checks used to be flat: a zombie at the foot of a five-block pillar was
// "1.2 cells away" from a player standing on top of it and could hit them
// through the rock. Steering stays two-dimensional — pathing is a grid — but
// anything that reaches out and touches the player has to measure in three.
// Feet are read from the grid, not inferred from the eye. The eye is eased
// toward the ground rather than pinned to it, so subtracting EYE from it would
// make reach wobble by a fraction of a block for a few ticks after every step.
static inline float feetDz(const State& s, const Mob& m) {
  return (float)s.feetZ - (float)m.z;
}
static inline float reachOf(const State& s, const Mob& m, float flat) {
  const float dz = feetDz(s, m);
  return sqrtf(flat * flat + dz * dz);
}

// Where something happened, for the renderer to throw particles at. Dropped
// silently when full: a lost puff of dust is not worth a branch in the sim.
static void spark(State& s, uint8_t kind, float x, float y, float z,
                  uint8_t mat, uint8_t mag) {
  if (s.sparkN >= MAX_SPARKS) return;
  Spark& sp = s.sparks[s.sparkN++];
  sp.x = x; sp.y = y; sp.z = z;
  sp.kind = kind; sp.mat = mat; sp.mag = mag;
}

static void kick(State& s, uint8_t amount) {
  if (amount > s.shake) s.shake = amount;
}

// Damage from a mob. Swallowed while the player is still reeling from the last
// one: two mobs landing blows a tick apart used to take two hearts with no
// window to answer in, and nothing else in the game paced them.
static uint32_t hurtPlayer(State& s, int16_t amount) {
  if (s.iframes) return 0;
  s.hp -= amount;
  s.hurtFlash = HURT_TICKS;
  s.iframes = IFRAME_TICKS;
  kick(s, 10);
  if (s.hp <= 0) { s.hp = 0; s.dead = true; return EV_HURT | EV_DIED; }
  return EV_HURT;
}

// Lava, and anything else the player is standing in rather than being hit by.
// It deliberately bypasses the i-frame window: standing in lava is a choice
// being made again every tick, and its own burn timer already paces it.
static uint32_t burnPlayer(State& s, int16_t amount) {
  s.hp -= amount;
  s.hurtFlash = HURT_TICKS;
  if (s.hp <= 0) { s.hp = 0; s.dead = true; return EV_HURT | EV_DIED; }
  return EV_HURT;
}

static uint32_t detonate(State& s, Mob& m) {
  world::explode((int)m.x, (int)m.y, CREEPER_BLAST);
  uint32_t ev = EV_EXPLODE;
  spark(s, SP_BLAST, m.x, m.y, (float)m.z + 0.6f, 0, 255);
  kick(s, 22);
  const float dx = s.cam.px - m.x, dy = s.cam.py - m.y;
  // Three-dimensional, so standing on top of a pillar is real cover from a
  // blast at its foot rather than merely looking like it.
  const float dzf = feetDz(s, m);
  const float d = sqrtf(dx * dx + dy * dy + dzf * dzf);
  if (d < (float)CREEPER_BLAST + 1.0f) {
    // Falls off with distance, so backing away from a lit creeper is a real
    // and readable choice rather than a coin flip.
    const float f = 1.0f - d / ((float)CREEPER_BLAST + 1.0f);
    const int16_t dmg = (int16_t)(kMob[MOB_CREEPER].damage * f + 0.5f);
    if (dmg > 0) ev |= hurtPlayer(s, dmg);
  }
  m.alive = false;
  return ev;
}

// How hard this mob is pushed away from its neighbours. Without it a wave
// collapses into a single stack walking the same line, which looks like one
// mob with a depth bug and lets the player kill six things with one swing.
static void separation(const State& s, int self, float& sx, float& sy) {
  const Mob& me = s.mobs[self];
  for (int i = 0; i < MAX_MOBS; ++i) {
    if (i == self) continue;
    const Mob& o = s.mobs[i];
    if (!o.alive) continue;
    const float dx = me.x - o.x, dy = me.y - o.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 > SEPARATION_R * SEPARATION_R || d2 < 1e-5f) continue;
    const float d = sqrtf(d2);
    const float w = (SEPARATION_R - d) / SEPARATION_R;
    sx += dx / d * w;
    sy += dy / d * w;
  }
}

// Loosed from the mob's chest toward the player's eye. Aimed where the player
// is, not where they will be — leading the shot would make it undodgeable,
// which is the thing being fixed.
static uint32_t fireArrow(State& s, const Mob& m) {
  int slot = -1;
  for (int i = 0; i < MAX_ARROWS; ++i)
    if (!s.arrows[i].alive) { slot = i; break; }
  if (slot < 0) return 0;

  const float mz = (float)m.z + 1.0f;
  float dx = s.cam.px - m.x, dy = s.cam.py - m.y, dz = s.cam.z - mz;
  const float d = sqrtf(dx * dx + dy * dy + dz * dz);
  if (d < 0.001f) return 0;
  const float k = ARROW_SPEED / (float)TICK_HZ / d;

  Arrow& a = s.arrows[slot];
  a.x = m.x; a.y = m.y; a.z = mz;
  a.vx = dx * k; a.vy = dy * k; a.vz = dz * k;
  a.life = ARROW_LIFE;
  a.alive = true;
  return EV_ARROW_FIRE;
}

// One step per tick, stopping at the player or at anything solid. Terrain is
// tested against the column it is passing over, so an arrow is absorbed by a
// wall you put between yourself and the archer — which is the other answer to
// a skeleton, and the one that makes building matter at range.
static uint32_t updateArrows(State& s) {
  uint32_t ev = 0;
  for (int i = 0; i < MAX_ARROWS; ++i) {
    Arrow& a = s.arrows[i];
    if (!a.alive) continue;
    if (--a.life == 0) { a.alive = false; continue; }

    a.x += a.vx; a.y += a.vy; a.z += a.vz;

    const float dx = a.x - s.cam.px, dy = a.y - s.cam.py, dz = a.z - s.cam.z;
    if (dx * dx + dy * dy + dz * dz < (PLAYER_RADIUS + 0.22f) * (PLAYER_RADIUS + 0.22f)) {
      a.alive = false;
      spark(s, SP_ARROW, a.x, a.y, a.z, 0, 140);
      ev |= EV_ARROW_HIT | hurtPlayer(s, kMob[MOB_SKELETON].damage);
      continue;
    }

    const int cx = (int)a.x, cy = (int)a.y;
    const world::Cell c = world::cellAt(cx, cy);
    const int az = (int)a.z;
    const bool hit = a.z < 0.0f
                  || (az < world::MAX_H && ((c.solid >> az) & 1u));
    if (hit) {
      a.alive = false;
      spark(s, SP_ARROW, a.x, a.y, a.z, 0, 80);
      ev |= EV_ARROW_HIT;
    }
  }
  return ev;
}

static uint32_t updateMob(State& s, int idx) {
  Mob& m = s.mobs[idx];
  uint32_t ev = 0;
  const MobInfo& mi = kMob[m.kind];
  const float dx = s.cam.px - m.x, dy = s.cam.py - m.y;
  const float dist = sqrtf(dx * dx + dy * dy);
  const float invD = (dist > 0.001f) ? 1.0f / dist : 0.0f;

  const float reach = reachOf(s, m, dist);

  if (m.timer) --m.timer;

  // Lava burns whatever is standing in it, mob or player. It used to burn only
  // the player, which made a lava pool a hazard to walk around rather than
  // something to back a wave into.
  // What it is standing ON, not what the ground column happens to be topped
  // with. See the player's copy of this below for the bug that distinction
  // fixes; a mob chasing you across your own bridge had exactly the same one.
  if (world::isHazard(world::blockAt((int)m.x, (int)m.y, (int)m.z - 1))) {
    if (m.burn == 0) {
      m.burn = BURN_PERIOD;
      if (--m.hp <= 0) {
        m.alive = false;
        s.sfxDiedKind = m.kind;
        return ev | EV_MOB_DIED;
      }
    }
  }
  if (m.burn) --m.burn;

  // Line of sight is refreshed on a stagger rather than every tick: it costs a
  // walk along the segment, and with a full wave that is the one piece of mob
  // work that would actually show up in the frame time.
  if (((s_aiTick + (uint32_t)idx) % LOS_PERIOD) == 0) {
    const float mz = (float)m.z + 0.9f;
    m.los = lineOfSight(m.x, m.y, mz, s.cam.px, s.cam.py, s.cam.z);
  }

  // -- committed blows ---------------------------------------------------------
  // A blow that lands the instant a mob is in range is unreadable and feels
  // unfair. Every attack is announced, holds the mob still while it commits,
  // and can be walked out of — or, for the skeleton, broken by stepping behind
  // something. That is most of what makes them feel deliberate.
  if (m.windup) {
    if (--m.windup == 0) {
      if (m.kind == MOB_SKELETON) {
        // It looses an arrow rather than dealing damage where it stands. The
        // hitscan version took a heart off you from seven cells away with
        // nothing drawn in between: no way to read it, and no way to answer it.
        // A shot that takes half a second to arrive can be stepped out of, and
        // that is what makes a skeleton a fight rather than a tax on standing
        // in the open.
        if (m.los) ev |= fireArrow(s, m);
      } else {
        const bool stillThere = reachOf(s, m, dist) <= mi.range + PLAYER_RADIUS;
        if (stillThere) ev |= hurtPlayer(s, mi.damage);
      }
      m.timer = mi.cooldown;
    }
    return ev;                      // committed: no steering this tick
  }

  bool lunging = false;
  if (m.kind == MOB_CREEPER) {
    // How long it has been near the player without managing to arrive. In the
    // open this never matters: a creeper crosses the gap from PRESS_REACH to
    // its own range in under a second, so it fuses on range as it always did.
    if (reach < CREEPER_PRESS_REACH) ++m.press; else m.press = 0;

    if (m.timer && m.hp > 0) {
      lunging = true;               // fuse lit: it commits and closes fast
      if (m.timer == 1) return detonate(s, m);
    } else if (reach < mi.range || m.press > CREEPER_PRESS_TICKS) {
      // Deliberately not gated on line of sight. A creeper pressed against the
      // far side of a wall you built is exactly the situation its fuse exists
      // for: it cannot see you, it detonates anyway, and the wall goes with it.
      //
      // The press clause is what makes that true rather than merely intended.
      // On range alone the answer to a wall could be jammed by the crowd it
      // summoned: mobs hold each other off at SEPARATION_R, so seventeen
      // creepers stacked against one wall all sat at 2.1 to 2.7 cells with the
      // fuse needing 1.9, and none of them ever went off. Something that has
      // got within three and a half cells of you and then stopped getting
      // closer has found what it came for, whether that is your wall or the
      // back of another creeper.
      m.timer = mi.cooldown;
      ev |= EV_HISS;
    }
  } else if (!m.timer && reach < mi.range && (m.kind != MOB_SKELETON || m.los)) {
    m.windup = mi.windup;
    ev |= EV_TELEGRAPH;
    if (m.windup) return ev;
  }

  // -- steering ----------------------------------------------------------------
  float wantX = 0.0f, wantY = 0.0f;

  // With a clear line, walk straight at the player. The flow field is a grid,
  // so following it in the open produces a four-directional stagger; it earns
  // its keep only when there is something to walk around.
  //
  // flowHold is what stops that being a downgrade. Sight and passage are not
  // the same test — a mob can see the player clean over a two-block ridge it
  // has no way to climb — so walking into something drops it back onto the
  // grid for long enough to actually route around, rather than pressing into
  // the obstacle for the rest of the night.
  if (m.flowHold) --m.flowHold;
  if (m.los && !m.flowHold) {
    wantX = dx * invD; wantY = dy * invD;
  } else {
    // Descend the gradient of the whole 4-neighbourhood, not the single best
    // cell. Picking one neighbour and walking to its centre means an east step
    // targets the same y the body already has, so it locks to one axis and
    // crabs sideways forever; the gradient gives a real diagonal and moves in
    // both axes at once.
    const int cx = (int)m.x, cy = (int)m.y;
    const int here = (int)flowAt(cx, cy);
    // Unreachable neighbours are treated as steeply uphill rather than
    // skipped, so the gradient pushes away from them instead of ignoring them.
    auto fv = [here](int x, int y) {
      const uint8_t f = flowAt(x, y);
      return (float)(f == 0xFF ? here + 8 : (int)f);
    };
    const float gx = fv(cx + 1, cy) - fv(cx - 1, cy);
    const float gy = fv(cx, cy + 1) - fv(cx, cy - 1);
    const float gl = sqrtf(gx * gx + gy * gy);
    if (gl > 0.001f) {
      wantX = -gx / gl; wantY = -gy / gl;
    } else {
      // Flat field: walled off with nowhere downhill. Press straight at the
      // player so a creeper that gets here still detonates against the wall.
      wantX = dx * invD; wantY = dy * invD;
    }
  }

  // Inside its standoff a mob circles instead of closing, easing outward if it
  // has been crowded in. A ring of them ends up surrounding the player rather
  // than queueing up in front — the same behaviour that makes them hard to
  // fight one at a time.
  const float standoff = lunging ? mi.standoff * 0.55f : mi.standoff;
  if (dist < standoff) {
    const float turn = m.side ? 1.0f : -1.0f;
    const float tanX = -dy * invD * turn, tanY = dx * invD * turn;
    const float push = (standoff - dist) / standoff;
    wantX = tanX * 0.85f - dx * invD * push * 0.9f;
    wantY = tanY * 0.85f - dy * invD * push * 0.9f;
  }

  float sepX = 0.0f, sepY = 0.0f;
  separation(s, idx, sepX, sepY);
  wantX += sepX * 1.15f;
  wantY += sepY * 1.15f;

  const float wl = sqrtf(wantX * wantX + wantY * wantY);
  if (wl > 0.001f) { wantX /= wl; wantY /= wl; }

  float speed = mi.speed;
  if (lunging) speed *= CREEPER_LUNGE;
  const float v = speed / (float)TICK_HZ;

  const float wasX = m.x, wasY = m.y;
  slide(m.x, m.y, wantX * v, wantY * v, MOB_RADIUS, (int)m.z);
  // Where the body ended up standing. A mob can now be somewhere the terrain
  // alone does not describe — on a bridge, on a roof, on a floor the player
  // built — so its height is carried rather than recomputed.
  {
    const uint8_t sfc = world::surfaceUnder((int)m.x, (int)m.y, (int)m.z);
    m.z = (sfc == world::NO_SURFACE) ? world::groundAt(m.x, m.y) : sfc;
  }
  // How far it has actually walked, so its legs match its speed instead of a
  // timer. Kept on the mob rather than in the renderer because slots are
  // recycled, and a respawned mob would otherwise inherit a stale stride.
  m.walk += sqrtf((m.x - wasX) * (m.x - wasX) + (m.y - wasY) * (m.y - wasY));

  // Made no real headway: something is in the way that steering cannot see.
  const float mx = m.x - wasX, my = m.y - wasY;
  if (mx * mx + my * my < (v * 0.3f) * (v * 0.3f)) m.flowHold = FLOW_HOLD;

  // A mob in a pocket the flow field cannot reach out of — behind a ridge it
  // cannot climb, say — works at the barrier forever. It never arrives, but it
  // holds one of the night's slots, so the wave that actually reaches the
  // player is quietly smaller than the one the director budgeted for. Give up
  // and hand the slot back so a replacement can be sent somewhere useful.
  //
  // Measured as progress, not as movement: a mob boxed in still shuffles
  // around inside its cell every tick, so "did it move" never accumulates.
  // What matters is whether it has got any closer than it ever has before.
  //
  // Distance-gated, because a mob working at a wall the player has just sealed
  // themselves behind is doing exactly what it should be doing.
  const float post = sqrtf((s.cam.px - m.x) * (s.cam.px - m.x)
                         + (s.cam.py - m.y) * (s.cam.py - m.y));
  if (post < m.bestDist - 0.5f) {
    m.bestDist = post;
    m.idle = 0;
  } else if (post > STUCK_DIST && ++m.idle > STUCK_TICKS) {
    // It gave its budget slot back too, when there was a budget. Under a cap
    // there is nothing to hand back: dying drops the live count, and the
    // spawner notices on its next tick.
    m.alive = false;
  }
  return ev;
}

// ---- player -----------------------------------------------------------------

// True if a body of radius r centred at (bx, by), standing with its feet at
// bz, occupies the block (x, y, z). A body is a column HEADROOM tall, so this
// is the test that stops a player sealing themselves inside their own build.
static bool bodyOccupies(float bx, float by, int bz, float r,
                         int x, int y, int z) {
  if (z < bz || z >= bz + world::HEADROOM) return false;
  // Nearest point of the cell to the body's centre, which is the cheap way to
  // ask "does this disc overlap this square" without a per-corner test.
  const float cx = bx < (float)x ? (float)x : (bx > (float)x + 1.0f ? (float)x + 1.0f : bx);
  const float cy = by < (float)y ? (float)y : (by > (float)y + 1.0f ? (float)y + 1.0f : by);
  const float dx = bx - cx, dy = by - cy;
  return dx * dx + dy * dy < r * r;
}

// Puts a block against the face the crosshair is on.
//
// The target is always hit + normal, so a placed block always touches
// something solid by construction — which is why place-against-face is the
// primitive here and place-at-cell is not. It is also why there is no separate
// "is it supported" rule to get wrong.
static uint32_t placeAgainstFace(State& s, uint8_t held) {
  const int tx = (int)s.aimX + s.aimNX;
  const int ty = (int)s.aimY + s.aimNY;
  const int tz = (int)s.aimZ + s.aimNZ;

  if (tz < 0 || tz >= world::MAX_H)      return EV_CANT_PLACE;
  if (world::isBorder(tx, ty))           return EV_CANT_PLACE;

  // Not inside the player, and not inside anything alive. This replaces an
  // older test that refused only the cell the player was standing in — which
  // was both too weak (it ignored the block over their head) and aimed at the
  // wrong thing: what it was really guarding against was pillaring straight up
  // out of a wave, and a body-sized volume guards that properly.
  if (bodyOccupies(s.cam.px, s.cam.py, (int)s.feetZ, PLAYER_RADIUS, tx, ty, tz))
    return EV_CANT_PLACE;
  for (int i = 0; i < MAX_MOBS; ++i) {
    const Mob& m = s.mobs[i];
    if (!m.alive) continue;
    if (bodyOccupies(m.x, m.y, (int)m.z, MOB_RADIUS, tx, ty, tz))
      return EV_CANT_PLACE;
  }

  switch (world::place(tx, ty, tz, held)) {
    case world::PLACE_OK:      break;
    case world::PLACE_NO_ROOM: return EV_NO_ROOM;
    default:                   return EV_CANT_PLACE;
  }

  takeItem(s, held, 1);
  s.sfxDigMat = held;           // the place cue is material-classed too
  s.swingCooldown = BUILD_TICKS;
  s_flowAge = FLOW_PERIOD;      // the map changed; repath next tick
  return EV_PLACE;
}

static uint32_t playerAct(State& s) {
  uint32_t ev = 0;

  // A mob in the swing arc takes priority over the block behind it — otherwise
  // ACT would mine a wall while a zombie chews on you.
  if (!s.swingCooldown) {
    int target = -1;
    float bestD = MELEE_REACH;
    for (int i = 0; i < MAX_MOBS; ++i) {
      Mob& m = s.mobs[i];
      if (!m.alive) continue;
      const float dx = m.x - s.cam.px, dy = m.y - s.cam.py;
      const float flat = sqrtf(dx * dx + dy * dy);
      // Same rule in both directions: if a mob cannot reach up a cliff to hit
      // the player, the player cannot reach down one to hit it either.
      const float d = reachOf(s, m, flat);
      if (d > bestD || flat < 0.001f) continue;
      if ((dx / flat) * s.cam.dx + (dy / flat) * s.cam.dy < MELEE_DOT) continue;
      bestD = d; target = i;
    }
    if (target >= 0) {
      s.swingCooldown = SWING_TICKS;
      Mob& m = s.mobs[target];
      // What is in the hand decides the blow. A pickaxe is a fist with a
      // handle here -- it swings for the same one heart bare hands do, which
      // is what makes carrying a sword through a night worth a slot.
      const uint8_t held = heldItem(s);
      const bool sword = isTool(held) && toolKind(held) == TK_SWORD;
      m.hp = (int16_t)(m.hp - (sword ? toolInfo(TK_SWORD, toolTier(held)).damage
                                     : HAND_DAMAGE));
      // Wear on the blow that LANDED, not on the swing. A whiff costs nothing;
      // see the miss branch below, which never reaches this.
      if (sword) ev |= wearTool(s);
      m.hitFlash = HIT_FLASH;
      kick(s, 5);
      spark(s, SP_HIT, m.x, m.y, (float)m.z + 0.95f, 0, 120);
      s.sfxHitKind = m.kind;
      ev |= EV_SWING | EV_MOB_HIT;

      // Shove it back along the swing. Without this a hit has no consequence
      // you can see — the mob keeps standing exactly where it was and combat
      // is just two health bars ticking down. Knockback also buys the player a
      // moment, which is what makes backing out of a crowd possible at all.
      const float kx = m.x - s.cam.px, ky = m.y - s.cam.py;
      const float kl = sqrtf(kx * kx + ky * ky);
      if (kl > 0.001f) {
        slide(m.x, m.y, kx / kl * KNOCKBACK, ky / kl * KNOCKBACK,
              MOB_RADIUS, (int)m.z);
        m.windup = 0;              // interrupted mid-swing
      }
      if (m.hp <= 0) {
        // A creeper killed mid-fuse still goes off. Disarming one is a reward
        // for hitting it early, not for hitting it at all.
        if (m.kind == MOB_CREEPER && m.timer) ev |= detonate(s, m);
        else {
          m.alive = false;
          spark(s, SP_DEATH, m.x, m.y, (float)m.z + 0.8f, 0, 200);
          s.sfxDiedKind = m.kind;
          ev |= EV_MOB_DIED;
        }
      }
      return ev;
    }
  }

  // Nothing in the arc and nothing under the crosshair: the swing still happens.
  // It used to return silently, so holding the attack button in open air moved
  // nothing, made no sound, and did not even animate the pickaxe — the one
  // input the player presses most often did nothing at all when it missed.
  //
  // Rate-limited by the animation rather than by the swing cooldown: an
  // eighteen-tick lockout for hitting air would make combat sluggish the moment
  // a mob stepped into range mid-swing. This fires once per swing, about four
  // a second, and never blocks a real one.
  if (!s.aimValid) {
    ev |= EV_SWINGING;                 // keeps the tool animating, see tick()
    if (s.toolPhase == WHIFF_FRAME) ev |= EV_WHIFF;
    return ev;
  }

  uint8_t dropM = 0, dropB = 0;

  // What is in your hand decides how fast this goes. Bare hands and a fistful
  // of dirt are the same thing — Minecraft's rule, and the one that makes the
  // hotbar a choice rather than a label: keeping the pickaxe selected costs you
  // the ability to place, and placing costs you the ability to dig quickly.
  const uint8_t held = heldItem(s);
  int effort = HAND_EFFORT;
  // Any tool answers from the table, not just a pickaxe. A sword's entry is
  // already HAND_EFFORT, so this changes nothing today -- it means the table
  // stays the single place a tool's digging speed is written down, instead of
  // half of it living in the condition here.
  if (isTool(held)) effort = toolInfo(toolKind(held), toolTier(held)).effort;
  if (effort < 1) effort = 1;          // never zero, or a slot would be a wall

  // Named before the swing lands, so the dig cue knows what it is chipping at
  // even on the ticks that break nothing.
  s.sfxDigMat = world::blockAt(s.aimX, s.aimY, s.aimZ);

  // The block the crosshair is on, not the top of the column it belongs to.
  // Those used to be different things, and aiming at the foot of a six-high
  // wall took the block off its top.
  switch (world::mine(s.aimX, s.aimY, s.aimZ, effort, dropM, dropB)) {
    case world::MINE_BROKE: {
      // Grass gives dirt, as it does in Minecraft. It is also one material
      // fewer competing for the eight slots the bar has for thirteen.
      const uint8_t yield = (dropM == world::B_GRASS) ? (uint8_t)world::B_DIRT
                                                      : dropM;
      // What the bar cannot take is spilled on the spot rather than vanishing
      // into a count with no slot to show it. Mining a material you have no
      // room for is now a visible event with a thing you can go and pick up,
      // which is the whole point of the change.
      const int took = giveItem(s, yield, dropB);
      if (took < dropB)
        spillAt(s, yield, dropB - took, (float)s.aimX + 0.5f,
                (float)s.aimY + 0.5f, (float)s.aimZ + 0.5f);
      // A pickaxe is spent per block broken, not per tick of effort: a tough
      // block and a soft one cost it the same, which is what makes a better
      // tier worth more than its speed.
      if (isTool(held) && toolKind(held) == TK_PICK) ev |= wearTool(s);
      // Shards take the colour of what came off, so digging coal out of a
      // stone face looks different from digging the face itself.
      spark(s, SP_BREAK, (float)s.aimX + 0.5f, (float)s.aimY + 0.5f,
            (float)s.aimZ + 0.5f, dropM, 200);
      s_flowAge = FLOW_PERIOD;      // the map changed; repath next tick
      ev |= EV_BLOCK_BROKE;
      break;
    }
    default:
      ev |= EV_MINE_STEP;
      break;
  }
  return ev;
}

// ---- tick -------------------------------------------------------------------

uint32_t tick(State& s, const Input& in) {
  if (s.dead) return 0;

  uint32_t ev = 0;
  if (s.swingCooldown) --s.swingCooldown;
  if (s.hurtFlash)     --s.hurtFlash;
  if (s.iframes)       --s.iframes;
  if (s.shake)         --s.shake;
  for (int i = 0; i < MAX_MOBS; ++i)
    if (s.mobs[i].hitFlash) --s.mobs[i].hitFlash;

  // -- look and move
  const float turn = TURN_SPEED / (float)TICK_HZ;
  if (in.left)  s.angle -= turn;
  if (in.right) s.angle += turn;
  raycast::setAngle(s.cam, s.angle);

  // Pitch is a horizon offset, not a rotation — see raycast::Camera. Holding
  // both keys recentres, which costs no extra key on a board that has two to
  // spare and none at all on a board that has neither.
  {
    const float step = PITCH_SPEED / (float)TICK_HZ;
    float h = s.pitch;
    if (in.lookUp && in.lookDown) {
      h = (float)raycast::HORIZON;
    } else if (in.lookUp) {
      h += step;
    } else if (in.lookDown) {
      h -= step;
    }
    // ...and otherwise it stays where it was put. See PITCH_SPEED.
    const float lo = (float)(raycast::HORIZON - raycast::PITCH_DOWN);
    const float hi = (float)(raycast::HORIZON + raycast::PITCH_UP);
    s.pitch = h < lo ? lo : (h > hi ? hi : h);
    raycast::setPitch(s.cam, (int)(s.pitch + 0.5f));
  }

  float fwd = 0.0f;
  if (in.fwd)  fwd += 1.0f;
  if (in.back) fwd -= STRAFE_SCALE;
  if (fwd != 0.0f) {
    const float v = fwd * MOVE_SPEED / (float)TICK_HZ;
    slide(s.cam.px, s.cam.py, s.cam.dx * v, s.cam.dy * v, PLAYER_RADIUS,
          (int)s.feetZ);
  }

  // The eye chases the ground under it instead of being pinned to it. A step up
  // is already automatic — world::STEP_UP lets a body walk onto a one-block
  // rise without asking — but assigning the new height outright moved the view
  // a whole world unit between two frames, and what the player saw was a jump
  // cut, not a step. The spring below is that same automatic step with the
  // motion put back into it.
  {
    // Where the feet are is a fact about the world and is resolved first; the
    // eye then chases it. Keeping the two separate is what lets a bridge deck
    // be walked onto — the surface can jump a block while the view does not.
    const uint8_t sfc = world::surfaceUnder((int)s.cam.px, (int)s.cam.py,
                                            (int)s.feetZ);
    // Nothing within a step means the world moved rather than the body: a
    // column grew under the player, or a blast dropped the floor away. Snapping
    // to the terrain is what un-wedges them — left at a stale height they stand
    // inside the pillar that just rose around them, and every later step-up
    // test is measured from a height they are not at.
    s.feetZ = (sfc == world::NO_SURFACE)
                ? world::groundAt(s.cam.px, s.cam.py)
                : sfc;
    const float target = (float)s.feetZ + raycast::EYE;
    const float d = target - s.eyeZ;
    if (d * d < EYE_SNAP * EYE_SNAP && s.eyeVel * s.eyeVel < EYE_SNAP * EYE_SNAP) {
      s.eyeZ = target;
      s.eyeVel = 0.0f;
    } else {
      s.eyeVel = (s.eyeVel + d * EYE_PULL) * EYE_DAMP;
      s.eyeZ += s.eyeVel;
    }
    s.cam.z = s.eyeZ;
  }

  // -- what the crosshair is on
  raycast::Hit hit;
  const bool got = raycast::pick(s.cam, REACH, hit);
  if (got && !world::isBorder(hit.x, hit.y)) {
    if (s.aimValid && (hit.x != s.aimX || hit.y != s.aimY || hit.z != s.aimZ))
      world::resetDamage(s.aimX, s.aimY, s.aimZ);
    s.aimValid = true;
    s.aimX = hit.x; s.aimY = hit.y; s.aimZ = hit.z;
    s.aimNX = hit.nx; s.aimNY = hit.ny; s.aimNZ = hit.nz;
    s.aimOnTop = (hit.face == raycast::F_TOP);
  } else {
    if (s.aimValid) world::resetDamage(s.aimX, s.aimY, s.aimZ);
    s.aimValid = false;
  }

  // -- act / build
  if (in.act) ev |= playerAct(s);
  else if (s.aimValid) world::resetDamage(s.aimX, s.aimY, s.aimZ);

  if (s.swingCooldown == 0 && in.build) {
    // A pickaxe and an empty hand both build nothing. That refusal IS the item
    // system from the player's side: the bar is not decoration, and the answer
    // to "why did nothing happen" is visible at the bottom of the screen.
    const uint8_t held = heldBlock(s);
    if (held >= world::B_COUNT || s.inv[held] == 0) {
      ev |= EV_NO_BLOCKS;
    } else if (s.aimValid) {
      // The block goes against the face that was hit, not on top of whatever
      // column the crosshair happened to land on. That is the whole difference
      // between building and growing a heightmap: aim at the side of a wall and
      // the block lands beside it, aim at its top and it lands on top.
      ev |= placeAgainstFace(s, held);
    }
  }
  s.aimDamage = s.aimValid ? world::damage(s.aimX, s.aimY, s.aimZ) : 0;

  // The pickaxe swings while it is doing something and drops to rest the
  // moment it is not, so the animation reads as work rather than as idle
  // fidgeting. It free-runs rather than being retriggered per hit, which is
  // what keeps a continuous mine looking like a steady rhythm.
  if (ev & (EV_MINE_STEP | EV_BLOCK_BROKE | EV_SWING | EV_MOB_HIT | EV_SWINGING))
    s.toolPhase = (uint8_t)((s.toolPhase + 1) % TOOL_ANIM);
  else
    s.toolPhase = 0;

  // -- what left the hand, and what is lying about
  // The drop key. Tapping it fires at once because the timer is zero whenever
  // the key is up; holding it pays DROP_PERIOD between each, which is what
  // turns "empty this slot" into one long press instead of sixty taps.
  if (!in.drop) {
    s.dropTimer = 0;
  } else if (s.dropTimer) {
    --s.dropTimer;
  } else {
    if (dropOne(s)) ev |= EV_DROP;
    s.dropTimer = (uint8_t)DROP_PERIOD;
  }

  ev |= updateDrops(s);

  // -- standing in the fire
  // Lava is unbreakable, so the only answers are to bridge over it or to get
  // off it. Damage on a timer rather than per tick, or a single misstep at six
  // hearts would be instantly fatal.
  // The block under the player's feet, which is not the same question as what
  // the ground column is topped with. topMat() only ever answers for the
  // terrain, so bridging over a lava pool — building a floor and standing on
  // it, which is the whole answer the game offers to lava — kept burning you
  // through your own bridge. feetZ is the surface actually being stood on, and
  // the block below it is what that surface is made of.
  if (world::isHazard(world::blockAt((int)s.cam.px, (int)s.cam.py,
                                     (int)s.feetZ - 1))) {
    if (s.burn == 0) { s.burn = 24; ev |= burnPlayer(s, 1); }
  }
  if (s.burn) --s.burn;
  if (s.dead) return ev | EV_DIED;

  // -- pathing and mobs
  if (++s_flowAge >= FLOW_PERIOD) {
    s_flowAge = 0;
    rebuildFlow((int)s.cam.px, (int)s.cam.py, (int)s.feetZ);
  }
  // Counted every tick, not once per flow rebuild: the field is only rebuilt
  // three times a second, so counting there would make SEALED_TRIGGER twenty
  // times longer than it reads and the siege would never arrive inside a night.
  updateSealed(s);
  ++s_aiTick;
  for (int i = 0; i < MAX_MOBS; ++i)
    if (s.mobs[i].alive) ev |= updateMob(s, i);
  ev |= updateArrows(s);

  if (s.dead) return ev | EV_DIED;

  // -- clock
  ++s.phaseTick;
  if (s.phase == PH_DAY) {
    if (s.phaseTick >= (uint32_t)DAY_TICKS) {
      s.phase = PH_NIGHT;
      s.phaseTick = 0;
      s.spawnTimer = 0;
      ev |= EV_DUSK;
    }
  } else {
    // The dark holds as many as it can and keeps topping itself up. This used
    // to be a budget handed out at dusk — 3 + 2 per night, paid out and then
    // exhausted — so a night was a countable quantity of monsters, and once you
    // had killed them all the rest of the night was empty. A cap has no such
    // shape: the pressure is constant while it is dark and it stops when the
    // sun comes up, which is the only thing that should end a night.
    int alive = 0;
    for (int i = 0; i < MAX_MOBS; ++i) if (s.mobs[i].alive) ++alive;

    // A sealed-in player has opted out of the ordinary cap: the director keeps
    // sending the one thing that can do something about a wall. Its own cap is
    // much lower, because what a siege needs is for a creeper to get to the
    // wall, and a queue of them behind it is the thing that stops one.
    const bool siege = s.sealedTicks > SEALED_TRIGGER;
    if (s.spawnTimer) {
      --s.spawnTimer;
    } else if (alive < (siege ? SIEGE_CAP : MOB_CAP)) {
      // A fixed cadence, not one that ramps with the night count. What paces a
      // night now is how fast the player clears the cap, not a difficulty dial.
      s.spawnTimer = siege ? (uint16_t)(TICK_HZ / 2) : (uint16_t)35;

      if (spawnMob(s)) {
        s.spawnFails = 0;
      } else if (++s.spawnFails >= 3) {
        // Nowhere to put one, three times running. The ordinary spawn rule
        // needs somewhere a mob could walk to the player FROM, and there are
        // two ways for that to stop existing: the player walls themselves in,
        // or — the one that actually bit — a night of creepers craters the
        // ground they are standing on and leaves them at the bottom of a pit
        // nothing can path into.
        //
        // updateSealed cannot see either case here, because it works by
        // watching live mobs fail to reach you and there are none left to
        // watch. Without this the night simply goes quiet for good: the
        // spawner refuses every position, nothing is alive to notice, and the
        // dark stops producing until dawn. Saying so out loud is what lets the
        // siege path — which puts a creeper near the wall and skips the
        // reachability test — take over, exactly as it does for a wall.
        // Comfortably past the trigger rather than one tick over it:
        // updateSealed decays this every tick that nothing alive is failing to
        // reach the player, and with nothing alive at all that is every tick.
        // Set to trigger+1 it falls back under before the next spawn attempt
        // comes round, and the siege never actually fires.
        s.sealedTicks = (uint16_t)(SEALED_TRIGGER + 2 * TICK_HZ);
        s.spawnFails = 0;
      }
    }

    if (s.phaseTick >= (uint32_t)NIGHT_TICKS) {
      s.phase = PH_DAY;
      s.phaseTick = 0;
      ++s.night;
      for (int i = 0; i < MAX_MOBS; ++i) s.mobs[i].alive = false;   // they burn off
      s.sealedTicks = 0;
      ev |= EV_DAWN;
    }
  }

  // -- mobs talking to themselves
  //
  // One a tick at most: a dozen zombies whose timers happen to land together
  // would otherwise stack a dozen moans into one frame, and the mixer has four
  // channels. Losing the others costs nothing — the point of the sound is that
  // something is out there, and one voice says that as well as six.
  for (int i = 0; i < MAX_MOBS; ++i) {
    Mob& m = s.mobs[i];
    if (!m.alive || m.voice == 0) continue;
    if (--m.voice) continue;
    m.voice = (uint16_t)(3 * TICK_HZ + nextRand(s) % (uint32_t)(7 * TICK_HZ));
    if (ev & EV_MOB_IDLE) continue;              // one voice a tick
    // Only within earshot. A moan from a mob on the far side of the map is a
    // sound with no information in it.
    const float dx = m.x - s.cam.px, dy = m.y - s.cam.py;
    if (dx * dx + dy * dy > 18.0f * 18.0f) continue;
    s.sfxIdleKind = m.kind;
    ev |= EV_MOB_IDLE;
  }
  return ev;
}

}  // namespace game
