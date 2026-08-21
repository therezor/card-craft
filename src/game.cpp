// =============================================================================
//  game.cpp — clock, waves, pathing, combat
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

// Looking up and down, in horizon pixels per second, and how fast the view
// drifts back to the resting tilt when neither key is held. The drift is slow
// on purpose: fast enough that you never have to think about recentring,
// slow enough that it does not fight you while you are lining up a shot.
constexpr float PITCH_SPEED  = 90.0f;
constexpr float PITCH_RETURN = 26.0f;

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
constexpr float MINE_REACH    = 6.0f;   // past where the aim ray meets flat ground
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

static uint32_t s_aiTick = 0;

// Placement stock, ordered cheapest and most plentiful first so the slot the
// player lands on by default is the one they will actually have.
const uint8_t kHotbar[HOTBAR_N] = {
  world::B_DIRT, world::B_STONE, world::B_WOOD,
  world::B_PLANK, world::B_BRICK, world::B_TORCH,
};

// Four recipes, all of them turning something the world gives you freely into
// something it does not. The torch is the important one: it is the only way to
// make ground the mobs will not spawn on.
static const RecipeInfo kRecipe[R_COUNT] = {
  { "TORCH",  "light, no spawns",
    { world::B_WOOD,   world::B_COAL   }, { 1, 1 }, world::B_TORCH, 4, 0 },
  { "PLANKS", "cheap walls",
    { world::B_WOOD,   0               }, { 1, 0 }, world::B_PLANK, 3, 0 },
  { "BRICKS", "tough walls",
    { world::B_STONE,  0               }, { 3, 0 }, world::B_BRICK, 3, 0 },
  { "PATCH",  "heal 2 hearts",
    { world::B_LEAVES, world::B_WOOD   }, { 2, 1 }, world::B_COUNT, 0, 2 },
};

const RecipeInfo& recipeInfo(uint8_t r) { return kRecipe[r < R_COUNT ? r : 0]; }

static const UpgradeInfo kUpgrade[UP_COUNT] = {
  { "TOUGHER",  "+2 max heart",  6 },
  { "PICKAXE",  "mine faster",   8 },
  { "SWORD",    "harder hits",   8 },
  { "PATCH UP", "full heal",     4 },
  { "STOCKPILE","+15 blocks",    3 },
};

const UpgradeInfo& upgradeInfo(uint8_t u) { return kUpgrade[u < UP_COUNT ? u : 0]; }

uint8_t  heldBlock(const State& s) { return kHotbar[s.hotbar % HOTBAR_N]; }
uint16_t heldCount(const State& s) { return s.inv[heldBlock(s)]; }

void cycleBlock(State& s, int delta) {
  s.hotbar = (uint8_t)((s.hotbar + delta % HOTBAR_N + HOTBAR_N) % HOTBAR_N);
}

uint16_t totalBlocks(const State& s) {
  uint16_t n = 0;
  for (int i = 0; i < HOTBAR_N; ++i) n = (uint16_t)(n + s.inv[kHotbar[i]]);
  return n;
}

bool canCraft(const State& s, uint8_t recipe) {
  if (recipe >= R_COUNT) return false;
  const RecipeInfo& r = kRecipe[recipe];
  for (int i = 0; i < 2; ++i)
    if (r.inQty[i] && s.inv[r.inMat[i]] < r.inQty[i]) return false;
  return true;
}

bool craft(State& s, uint8_t recipe) {
  if (!canCraft(s, recipe)) return false;
  const RecipeInfo& r = kRecipe[recipe];
  for (int i = 0; i < 2; ++i)
    if (r.inQty[i]) s.inv[r.inMat[i]] = (uint16_t)(s.inv[r.inMat[i]] - r.inQty[i]);
  if (r.outMat < world::B_COUNT) s.inv[r.outMat] = (uint16_t)(s.inv[r.outMat] + r.outQty);
  if (r.heal) {
    s.hp = (int16_t)(s.hp + r.heal);
    if (s.hp > s.maxHp) s.hp = s.maxHp;
  }
  return true;
}

// ---- pathing ----------------------------------------------------------------

// 255 means unreachable. Rebuilt from the player's cell; mobs simply walk
// downhill. The queue is a plain ring of cell indices — 8 KB of static, which
// is cheaper than the branchy alternatives and never allocates mid-frame.
static uint8_t  g_flow[world::W * world::H];
static uint16_t g_queue[world::W * world::H];
static int      s_flowAge = 0;

static void rebuildFlow(int sx, int sy) {
  memset(g_flow, 0xFF, sizeof(g_flow));
  if ((unsigned)sx >= (unsigned)world::W || (unsigned)sy >= (unsigned)world::H) return;

  int head = 0, tail = 0;
  const uint16_t start = (uint16_t)(sy * world::W + sx);
  g_flow[start] = 0;
  g_queue[tail++] = start;

  // The source cell is seeded whatever its height — a creeper can leave the
  // player standing in a crater, and the mobs should still converge on them.
  while (head < tail) {
    const uint16_t idx = g_queue[head++];
    const int x = idx & (world::W - 1);
    const int y = idx >> 6;                 // world::W is 64
    const uint8_t d = g_flow[idx];
    if (d >= 254) continue;

    static const int kDx[4] = { 1, -1, 0, 0 };
    static const int kDy[4] = { 0, 0, 1, -1 };
    for (int i = 0; i < 4; ++i) {
      const int nx = x + kDx[i], ny = y + kDy[i];
      if ((unsigned)nx >= (unsigned)world::W || (unsigned)ny >= (unsigned)world::H) continue;
      const uint16_t n = (uint16_t)(ny * world::W + nx);
      if (g_flow[n] != 0xFF) continue;
      // The BFS runs outward from the player, but a mob travels the other way
      // along it — from n into this cell — so the step-up limit is tested in
      // that direction. Get it backwards and mobs happily walk up cliffs.
      if (!world::canEnter((int)world::height(nx, ny), x, y)) continue;
      g_flow[n] = (uint8_t)(d + 1);
      g_queue[tail++] = n;
    }
  }
}

static inline uint8_t flowAt(int x, int y) {
  if ((unsigned)x >= (unsigned)world::W || (unsigned)y >= (unsigned)world::H) return 0xFF;
  return g_flow[y * world::W + x];
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
    const world::Cell c = world::cellAt(cx, cy);
    if ((float)c.h > z) return false;
    // And the slab overhead, if the line passes through it. Without this a
    // skeleton shoots clean through a bridge deck it is standing under.
    if (c.slabTop && z >= (float)c.slabBase && z < (float)c.slabTop) return false;
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
  s.rng = seed ? seed : 1u;
  world::generate(seed);

  s.cam.px = (float)(world::W / 2) + 0.5f;
  s.cam.py = (float)(world::H / 2) + 0.5f;
  s.cam.z  = (float)world::groundAt(s.cam.px, s.cam.py) + raycast::EYE;
  s.eyeZ   = s.cam.z;              // start settled, not falling into the world
  s.eyeVel = 0.0f;
  s.angle  = 0.0f;
  raycast::setAngle(s.cam, s.angle);
  s.pitch  = (float)raycast::HORIZON;
  raycast::setPitch(s.cam, raycast::HORIZON);

  rebuildFlow((int)s.cam.px, (int)s.cam.py);
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
  return (uint32_t)s.night * 100u + (uint32_t)s.ore * 5u + (uint32_t)totalBlocks(s);
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

  const uint32_t r = nextRand(s) % 100u;
  if (s.night >= 4) { if (r < 45) return MOB_ZOMBIE; if (r < 78) return MOB_CREEPER; return MOB_SKELETON; }
  if (s.night >= 2) { return (r < 65) ? MOB_ZOMBIE : MOB_CREEPER; }
  return MOB_ZOMBIE;
}

static void spawnMob(State& s) {
  int slot = -1;
  for (int i = 0; i < MAX_MOBS; ++i) if (!s.mobs[i].alive) { slot = i; break; }
  if (slot < 0) return;

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
    if (s.spawnBudget) --s.spawnBudget;
    return;
  }
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
  return (float)world::groundAt(s.cam.px, s.cam.py) - (float)world::groundAt(m.x, m.y);
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
  spark(s, SP_BLAST, m.x, m.y, (float)world::groundAt(m.x, m.y) + 0.6f, 0, 255);
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

  const float mz = (float)world::groundAt(m.x, m.y) + 1.0f;
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
    const bool inGround = a.z < (float)c.h;
    const bool inSlab   = c.slabTop && a.z >= (float)c.slabBase && a.z < (float)c.slabTop;
    if (a.z < 0.0f || inGround || inSlab) {
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
  if (world::isHazard(world::topMat((int)m.x, (int)m.y))) {
    if (m.burn == 0) {
      m.burn = BURN_PERIOD;
      if (--m.hp <= 0) { m.alive = false; return ev | EV_MOB_DIED; }
    }
  }
  if (m.burn) --m.burn;

  // Line of sight is refreshed on a stagger rather than every tick: it costs a
  // walk along the segment, and with a full wave that is the one piece of mob
  // work that would actually show up in the frame time.
  if (((s_aiTick + (uint32_t)idx) % LOS_PERIOD) == 0) {
    const float mz = (float)world::groundAt(m.x, m.y) + 0.9f;
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
    if (m.timer && m.hp > 0) {
      lunging = true;               // fuse lit: it commits and closes fast
      if (m.timer == 1) return detonate(s, m);
    } else if (reach < mi.range) {
      // Deliberately not gated on line of sight. A creeper pressed against the
      // far side of a wall you built is exactly the situation its fuse exists
      // for: it cannot see you, it detonates anyway, and the wall goes with it.
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
  slide(m.x, m.y, wantX * v, wantY * v, MOB_RADIUS, (int)world::groundAt(m.x, m.y));
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
    m.alive = false;
    if (s.spawnBudget < MAX_MOBS) ++s.spawnBudget;
  }
  return ev;
}

// ---- player -----------------------------------------------------------------

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
      m.hp -= (int16_t)(1 + s.damageLevel);
      m.hitFlash = HIT_FLASH;
      kick(s, 5);
      spark(s, SP_HIT, m.x, m.y, (float)world::groundAt(m.x, m.y) + 0.95f, 0, 120);
      ev |= EV_SWING | EV_MOB_HIT;

      // Shove it back along the swing. Without this a hit has no consequence
      // you can see — the mob keeps standing exactly where it was and combat
      // is just two health bars ticking down. Knockback also buys the player a
      // moment, which is what makes backing out of a crowd possible at all.
      const float kx = m.x - s.cam.px, ky = m.y - s.cam.py;
      const float kl = sqrtf(kx * kx + ky * ky);
      if (kl > 0.001f) {
        slide(m.x, m.y, kx / kl * KNOCKBACK, ky / kl * KNOCKBACK,
              MOB_RADIUS, (int)world::groundAt(m.x, m.y));
        m.windup = 0;              // interrupted mid-swing
      }
      if (m.hp <= 0) {
        // A creeper killed mid-fuse still goes off. Disarming one is a reward
        // for hitting it early, not for hitting it at all.
        if (m.kind == MOB_CREEPER && m.timer) ev |= detonate(s, m);
        else {
          m.alive = false;
          spark(s, SP_DEATH, m.x, m.y, (float)world::groundAt(m.x, m.y) + 0.8f, 0, 200);
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

  uint8_t dropM = 0, dropB = 0, dropO = 0;
  const int effort = world::EFFORT_PER_TICK + s.miningLevel * 8;
  if (world::mine(s.aimX, s.aimY, effort, dropM, dropB, dropO)) {
    s.inv[dropM] = (uint16_t)(s.inv[dropM] + dropB);
    s.ore        = (uint16_t)(s.ore + dropO);
    // Shards take the colour of what came off, so digging coal out of a stone
    // face looks different from digging the face itself.
    spark(s, SP_BREAK, (float)s.aimX + 0.5f, (float)s.aimY + 0.5f,
          (float)world::height(s.aimX, s.aimY) + 0.5f, dropM, 200);
    ev |= EV_BLOCK_BROKE;
  } else {
    ev |= EV_MINE_STEP;
  }
  return ev;
}

// ---- tick -------------------------------------------------------------------

uint32_t tick(State& s, const Input& in) {
  if (s.dead || s.awaitingUpgrade) return 0;

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
    } else {
      // Drift home. Everything about aiming and mining reach is tuned at the
      // resting tilt, so that is where the view belongs when nobody is asking
      // for anything else.
      const float back = PITCH_RETURN / (float)TICK_HZ;
      const float d = (float)raycast::HORIZON - h;
      if (d > back)       h += back;
      else if (d < -back) h -= back;
      else                h = (float)raycast::HORIZON;
    }
    const float lo = (float)(raycast::HORIZON - raycast::PITCH_RANGE);
    const float hi = (float)(raycast::HORIZON + raycast::PITCH_RANGE);
    s.pitch = h < lo ? lo : (h > hi ? hi : h);
    raycast::setPitch(s.cam, (int)(s.pitch + 0.5f));
  }

  float fwd = 0.0f;
  if (in.fwd)  fwd += 1.0f;
  if (in.back) fwd -= STRAFE_SCALE;
  if (fwd != 0.0f) {
    const float v = fwd * MOVE_SPEED / (float)TICK_HZ;
    slide(s.cam.px, s.cam.py, s.cam.dx * v, s.cam.dy * v, PLAYER_RADIUS,
          (int)world::groundAt(s.cam.px, s.cam.py));
  }

  // The eye chases the ground under it instead of being pinned to it. A step up
  // is already automatic — world::STEP_UP lets a body walk onto a one-block
  // rise without asking — but assigning the new height outright moved the view
  // a whole world unit between two frames, and what the player saw was a jump
  // cut, not a step. The spring below is that same automatic step with the
  // motion put back into it.
  {
    const float target = (float)world::groundAt(s.cam.px, s.cam.py) + raycast::EYE;
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
  int hx, hy;
  bool onTop = false;
  const bool hit = raycast::pick(s.cam, MINE_REACH, hx, hy, onTop);
  if (hit && !world::isBorder(hx, hy)) {
    if (s.aimValid && (hx != s.aimX || hy != s.aimY)) world::resetDamage(s.aimX, s.aimY);
    s.aimValid = true; s.aimOnTop = onTop;
    s.aimX = (int16_t)hx; s.aimY = (int16_t)hy;
  } else {
    if (s.aimValid) world::resetDamage(s.aimX, s.aimY);
    s.aimValid = false;
  }

  // -- act / build
  if (in.act) ev |= playerAct(s);
  else if (s.aimValid) world::resetDamage(s.aimX, s.aimY);

  if (s.swingCooldown == 0 && in.build) {
    const uint8_t held = heldBlock(s);
    if (s.inv[held] == 0) {
      ev |= EV_NO_BLOCKS;
    } else if (s.aimValid) {
      // Never build on the cell the player is standing in: with a heightmap
      // that would jack them up a block, and repeated it would let them
      // pillar out of every wave for free.
      const bool ownCell = ((int)s.cam.px == s.aimX && (int)s.cam.py == s.aimY);
      if (!ownCell && world::place(s.aimX, s.aimY, held)) {
        --s.inv[held];
        s.swingCooldown = BUILD_TICKS;
        s_flowAge = FLOW_PERIOD;      // the map changed; repath next tick
        ev |= EV_PLACE;
      }
    }
  }
  s.aimDamage = s.aimValid ? world::damage(s.aimX, s.aimY) : 0;

  // The pickaxe swings while it is doing something and drops to rest the
  // moment it is not, so the animation reads as work rather than as idle
  // fidgeting. It free-runs rather than being retriggered per hit, which is
  // what keeps a continuous mine looking like a steady rhythm.
  if (ev & (EV_MINE_STEP | EV_BLOCK_BROKE | EV_SWING | EV_MOB_HIT | EV_SWINGING))
    s.toolPhase = (uint8_t)((s.toolPhase + 1) % TOOL_ANIM);
  else
    s.toolPhase = 0;

  // -- standing in the fire
  // Lava is unbreakable, so the only answers are to bridge over it or to get
  // off it. Damage on a timer rather than per tick, or a single misstep at six
  // hearts would be instantly fatal.
  if (world::isHazard(world::topMat((int)s.cam.px, (int)s.cam.py))) {
    if (s.burn == 0) { s.burn = 24; ev |= burnPlayer(s, 1); }
  }
  if (s.burn) --s.burn;
  if (s.dead) return ev | EV_DIED;

  // -- pathing and mobs
  if (++s_flowAge >= FLOW_PERIOD) {
    s_flowAge = 0;
    rebuildFlow((int)s.cam.px, (int)s.cam.py);
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
      // Pressure ramps with the night count but is capped, because past a
      // point the flow field funnels them into a queue and it stops mattering.
      const int budget = 3 + (int)s.night * 2;
      s.spawnBudget = (uint8_t)(budget > MAX_MOBS ? MAX_MOBS : budget);
      s.spawnTimer = 0;
      ev |= EV_DUSK;
    }
  } else {
    // A siege does not run out. The night's budget is a pacing device for an
    // ordinary night; a player who has sealed themselves in has opted out of
    // that, so the director keeps sending the one thing that can reach them
    // until the wall is open again.
    if (s.sealedTicks > SEALED_TRIGGER && s.spawnBudget == 0
        && (s.sealedTicks % (2u * TICK_HZ)) == 0) {
      s.spawnBudget = 1;
    }

    if (s.spawnBudget && s.spawnTimer == 0) {
      spawnMob(s);
      uint16_t gap = (uint16_t)(50 - (s.night > 8 ? 8 : s.night) * 3);
      if (s.sealedTicks > SEALED_TRIGGER) gap /= 2;   // sealed in: they come faster
      s.spawnTimer = gap;
    } else if (s.spawnTimer) {
      --s.spawnTimer;
    }
    if (s.phaseTick >= (uint32_t)NIGHT_TICKS) {
      s.phase = PH_DAY;
      s.phaseTick = 0;
      ++s.night;
      for (int i = 0; i < MAX_MOBS; ++i) s.mobs[i].alive = false;   // they burn off
      s.spawnBudget = 0;
      s.sealedTicks = 0;

      // Three distinct offers, drawn without replacement.
      uint8_t pool[UP_COUNT];
      for (uint8_t i = 0; i < UP_COUNT; ++i) pool[i] = i;
      for (uint8_t i = UP_COUNT - 1; i > 0; --i) {
        const uint8_t j = (uint8_t)(nextRand(s) % (uint32_t)(i + 1));
        const uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
      }
      s.offer[0] = pool[0]; s.offer[1] = pool[1]; s.offer[2] = pool[2];
      s.awaitingUpgrade = true;
      ev |= EV_DAWN;
    }
  }
  return ev;
}

void chooseUpgrade(State& s, uint8_t upgrade) {
  if (upgrade < UP_COUNT && s.ore >= kUpgrade[upgrade].cost) {
    s.ore = (uint16_t)(s.ore - kUpgrade[upgrade].cost);
    switch (upgrade) {
      case UP_MAXHP:  s.maxHp += 2; s.hp += 2; break;
      case UP_MINING: if (s.miningLevel < 12) ++s.miningLevel; break;
      case UP_DAMAGE: if (s.damageLevel < 12) ++s.damageLevel; break;
      case UP_HEAL:   s.hp = s.maxHp; break;
      case UP_BLOCKS: s.inv[world::B_PLANK] = (uint16_t)(s.inv[world::B_PLANK] + 15); break;
      default: break;
    }
  }
  // Always resumes — an unaffordable pick is a skip, so a broke player is
  // never stuck on this screen.
  s.awaitingUpgrade = false;
}

}  // namespace game
