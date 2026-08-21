// =============================================================================
//  game.h — the survival simulation
//
//  Free of Arduino/M5GFX (see test/test_game). Time enters as a tick count,
//  never as millis(), so a whole night can be simulated on the host in a loop.
//
//  The loop: mine by day, wall yourself in by night, survive. Everything here
//  is sized to four buttons — there is no inventory and no crafting, just two
//  counters (blocks to build with, ore to upgrade with) and one choice at dawn.
// =============================================================================
#pragma once

#include <stdint.h>
#include "raycast.h"
#include "world.h"

namespace game {

constexpr int TICK_HZ      = 60;
constexpr int DAY_TICKS    = 60 * TICK_HZ;   // 60 s to gather
constexpr int NIGHT_TICKS  = 45 * TICK_HZ;   // 45 s to survive
constexpr int MAX_MOBS     = 24;
constexpr int TOOL_ANIM    = 16;   // frames in one pickaxe swing, ~0.27 s

enum Phase : uint8_t { PH_DAY, PH_NIGHT };

enum MobKind : uint8_t {
  MOB_ZOMBIE,     // melee, slow, night 1+
  MOB_CREEPER,    // detonates and destroys blocks, night 2+
  MOB_SKELETON,   // ranged, keeps its distance, night 4+
  MOB_COUNT
};

enum Upgrade : uint8_t {
  UP_MAXHP, UP_MINING, UP_DAMAGE, UP_HEAL, UP_BLOCKS, UP_COUNT
};

struct UpgradeInfo { const char* name; const char* detail; uint16_t cost; };
const UpgradeInfo& upgradeInfo(uint8_t u);

// ---- inventory --------------------------------------------------------------

// The materials the player can hold and place, in the order they cycle. Mining
// used to pour everything into one anonymous "blocks" counter, which made
// digging grass and digging iron feel identical; holding them separately is
// what makes the choice of *what* to dig mean anything.
constexpr int HOTBAR_N = 6;
extern const uint8_t kHotbar[HOTBAR_N];

// ---- crafting ---------------------------------------------------------------

enum Recipe : uint8_t { R_TORCH, R_PLANK, R_BRICK, R_PATCH, R_COUNT };

struct RecipeInfo {
  const char* name;
  const char* detail;
  uint8_t inMat[2], inQty[2];   // second input is unused when inQty[1] == 0
  uint8_t outMat, outQty;       // outMat == world::B_COUNT for a non-block result
  uint8_t heal;                 // hearts restored, for the ones that are not blocks
};
const RecipeInfo& recipeInfo(uint8_t r);

// Held-button state, filled by the HAL. Six flags is the whole input surface —
// see hal.h for how boards with fewer buttons reach it.
struct Input {
  bool left = false, right = false, fwd = false, back = false;
  bool act = false, build = false;
  // Optional, and deliberately last: a board with only the six above leaves
  // these false and plays exactly as it always has, at the fixed tilt.
  bool lookUp = false, lookDown = false;
};

// One-shot things that happened during a tick. main.cpp turns these into
// sounds; game.cpp never touches the speaker.
//
// Widened from uint16_t once fourteen of its sixteen bits were spoken for. It
// is returned by value and stored nowhere, so the extra two bytes cost nothing
// and rationing bits was buying nothing.
enum Event : uint32_t {
  EV_MINE_STEP  = 1u << 0,
  EV_BLOCK_BROKE= 1u << 1,
  EV_PLACE      = 1u << 2,
  EV_NO_BLOCKS  = 1u << 3,
  EV_SWING      = 1u << 4,
  EV_MOB_HIT    = 1u << 5,
  EV_MOB_DIED   = 1u << 6,
  EV_HURT       = 1u << 7,
  EV_HISS       = 1u << 8,
  EV_EXPLODE    = 1u << 9,
  EV_DUSK       = 1u << 10,
  EV_DAWN       = 1u << 11,
  EV_DIED       = 1u << 12,
  EV_TELEGRAPH  = 1u << 13,   // a mob committed to a blow that has not landed yet
  // The arm is moving. Not a sound of its own — it is what keeps the pickaxe
  // animating through a swing that connects with nothing.
  EV_SWINGING   = 1u << 17,
  EV_WHIFF      = 1u << 14,   // the strike frame of a swing that found nothing
  EV_ARROW_FIRE = 1u << 15,
  EV_ARROW_HIT  = 1u << 16,
};

// ---- effects ----------------------------------------------------------------

// A thing that happened *somewhere*. The event mask says what; this says where,
// how big, and what it was made of, which is everything a particle needs and
// none of which a bitmask can carry.
//
// Pushed by the simulation, drained by the renderer each frame. It lives here
// rather than in the renderer so game.cpp stays free of Arduino and the host
// tests can check that breaking a block emits exactly one burst at its centre.
enum SparkKind : uint8_t {
  SP_BREAK,    // a block came apart — mat says which
  SP_HIT,      // a blow landed on a mob
  SP_DEATH,    // a mob went down
  SP_BLAST,    // a creeper went off
  SP_ARROW,    // an arrow struck something
  SP_COUNT
};

struct Spark {
  float   x, y, z;
  uint8_t kind;
  uint8_t mat;    // world::Block for SP_BREAK, so shards are the block's colour
  uint8_t mag;    // 0..255, how much of it
};

// Sixteen is more than a tick can plausibly produce. It is a ring the renderer
// empties every frame, and the frame may cover up to MAX_CATCHUP ticks, so it
// has to hold a few ticks' worth. Overflow is dropped rather than grown: a lost
// puff of dust is not worth a branch in the simulation.
constexpr int MAX_SPARKS = 16;

// ---- projectiles ------------------------------------------------------------

// The skeleton used to be a hitscan: it telegraphed for 26 ticks and then took
// a heart off you from seven cells away with nothing drawn in between. There
// was no way to read it and no way to answer it. An arrow you can see coming
// and step out of is what turns that into a fight.
constexpr int   MAX_ARROWS  = 8;
constexpr float ARROW_SPEED = 9.0f;    // cells/second
constexpr int   ARROW_LIFE  = 3 * TICK_HZ;

struct Arrow {
  float   x, y, z;
  float   vx, vy, vz;
  uint16_t life;
  bool    alive;
};

struct Mob {
  float   x, y;      // z is not stored: a body always stands on world::groundAt
  int16_t hp;
  uint8_t kind;
  uint16_t timer;    // attack cooldown, or the creeper's fuse
  uint16_t windup;   // ticks left in a committed blow, 0 when not swinging
  bool    alive;
  uint8_t burn;      // ticks until lava bites again, same rule as the player
  uint16_t idle;     // ticks since it last got meaningfully closer to the player
  float   bestDist;  // the closest it has ever managed to get
  bool    los;       // clear line to the player, refreshed on a stagger
  uint8_t flowHold;  // ticks left ignoring los and pathing on the grid instead
  uint8_t side;      // which way it circles at standoff; fixed at spawn
  uint8_t hitFlash;  // ticks left showing white, so a landed blow is visible
  float   walk;      // distance travelled, for the walk cycle — see render.cpp
};

struct State {
  raycast::Camera cam;
  float    angle = 0.0f;

  // Six hearts, not ten. The +2 upgrade is the main thing ore buys early, and
  // it only reads as a real gain if the bar it grows is short to begin with.
  int16_t  hp = 6, maxHp = 6;
  uint16_t ore = 0;
  uint16_t inv[world::B_COUNT] = {0};   // how much of each material is held
  uint8_t  hotbar = 0;                  // index into kHotbar
  uint16_t burn = 0;                    // ticks until lava bites again
  uint16_t night = 1;

  uint8_t  phase = PH_DAY;
  uint32_t phaseTick = 0;

  // Upgrades, as plain multipliers so the mining and combat code stays
  // branch-free about which upgrades the player happens to own.
  uint8_t  miningLevel = 0;
  uint8_t  damageLevel = 0;

  Mob      mobs[MAX_MOBS];
  uint16_t sealedTicks = 0;    // how long no mob has been able to path to you
  uint8_t  spawnBudget = 0;    // mobs still owed to the current night
  uint16_t spawnTimer = 0;
  uint16_t swingCooldown = 0;
  uint16_t hurtFlash = 0;      // ticks of red vignette left

  // Ticks of immunity to mob damage. Not to lava: standing in lava is a choice
  // the player keeps making every tick, and the burn timer already paces it.
  // Without this a crowd chain-stuns you with no answer, because nothing else
  // in the game limits how often two mobs can land blows on the same frame.
  uint16_t iframes = 0;

  // Camera kick, decaying. Applied to a copy of the camera when drawing, never
  // to the camera itself — shaking the real one would drag the aim ray with it
  // and make the run unreproducible on the host.
  uint8_t  shake = 0;

  Arrow    arrows[MAX_ARROWS];
  Spark    sparks[MAX_SPARKS];
  uint8_t  sparkN = 0;         // reset by whoever drains it, once a frame
  uint8_t  toolPhase = 0;      // frame of the pickaxe swing, 0 = at rest

  // What the crosshair is on this tick, for the HUD.
  bool     aimValid = false;
  bool     aimOnTop = false;   // came down on a column top rather than its side
  int16_t  aimX = 0, aimY = 0;
  uint8_t  aimDamage = 0;

  // Eye height, eased toward the ground the body is standing on rather than
  // snapped to it. The snap was a teleport: step up one block and the whole
  // view jumped a full world unit in a single frame, which reads as a glitch
  // rather than as a step. Easing it is what turns walking up a block into a
  // hop you can feel.
  float    eyeZ = 0.0f;
  float    eyeVel = 0.0f;

  // Where the view is pointed, as a horizon row. Kept here rather than only in
  // the camera because it is fractional while it drifts, and the camera's copy
  // is a whole pixel.
  float    pitch = (float)raycast::HORIZON;

  bool     dead = false;
  bool     awaitingUpgrade = false;
  uint8_t  offer[3] = {0, 0, 0};

  uint32_t rng = 1;            // in-state, so a run is reproducible on the host
};

// Fresh run: generates the world, places the player, resets every counter.
void begin(State& s, uint32_t seed);

// Advances one 1/TICK_HZ step. Returns an OR of Event bits.
uint32_t tick(State& s, const Input& in);

// Applies the chosen dawn upgrade and resumes play. No-op if unaffordable.
void chooseUpgrade(State& s, uint8_t upgrade);

// Cycles the placement material. Skips nothing: an empty slot is still
// selectable, so the hotbar order never shifts under the player.
void cycleBlock(State& s, int delta);

// The material the player would place right now, and how many are left.
uint8_t heldBlock(const State& s);
uint16_t heldCount(const State& s);

// Everything placeable, added up — for the HUD and the score.
uint16_t totalBlocks(const State& s);

// True if the recipe's inputs are all in the inventory.
bool canCraft(const State& s, uint8_t recipe);

// Consumes the inputs and grants the output. No-op if unaffordable.
bool craft(State& s, uint8_t recipe);

// 0 at dawn, 1 at dusk — drives the sky/fog lerp in render.cpp.
float daylight(const State& s);

uint32_t score(const State& s);

}  // namespace game
