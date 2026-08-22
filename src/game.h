// =============================================================================
//  game.h — the survival simulation
//
//  Free of Arduino/M5GFX (see test/test_game). Time enters as a tick count,
//  never as millis(), so a whole night can be simulated on the host in a loop.
//
//  The loop: mine by day, wall yourself in by night, survive. There is no wave
//  counter and no shop at dawn — the night simply holds as many mobs as the dark
//  will carry, and what you get out of a day is what is in your hands.
// =============================================================================
#pragma once

#include <stdint.h>
#include "raycast.h"
#include "world.h"

namespace game {

constexpr int TICK_HZ      = 60;
// Three minutes of light, two of dark. Long enough that a day is something you
// spend rather than something you race, which is what the phase bar used to be
// compensating for: at sixty seconds the only way to know where you were in the
// day was to read a meter, and now the only way is to look at the sky.
constexpr int DAY_TICKS    = 180 * TICK_HZ;
constexpr int NIGHT_TICKS  = 120 * TICK_HZ;
constexpr int MAX_MOBS     = 24;

// How many mobs the dark holds at once. Nights used to be waves: dusk handed
// out a budget of 3 + 2 per night, the spawner paid it out and then stopped, so
// a night was a fixed quantity of monsters you could count down. This is the
// Minecraft shape instead — the dark keeps producing while there is room for
// more, and the way a night ends is that the sun comes up.
//
// Twelve, not MAX_MOBS: the array has to keep headroom above the cap for the
// siege, which ignores it. Twelve on screen is also about where the frame
// budget starts to notice.
constexpr int MOB_CAP      = 12;
constexpr int TOOL_ANIM    = 16;   // frames in one pickaxe swing, ~0.27 s

enum Phase : uint8_t { PH_DAY, PH_NIGHT };

enum MobKind : uint8_t {
  MOB_ZOMBIE,     // melee, slow, night 1+
  MOB_CREEPER,    // detonates and destroys blocks, night 2+
  MOB_SKELETON,   // ranged, keeps its distance, night 4+
  MOB_COUNT
};

// ---- items and the hotbar ---------------------------------------------------

// Nine slots along the bottom of the panel, and what is in the selected one
// decides what the two action keys do.
//
// Every slot starts empty, including the first. The pickaxe used to be pinned
// to slot 0 for the life of the run on the reasoning that a board with no
// inventory screen has to keep the tool reachable -- and that was true right
// up until the tool became something you make. A run now opens with nothing in
// your hands at all, and the first pickaxe is the first thing the day is for.
constexpr int SLOT_N = 9;

// What a slot can hold, as one byte. Three bands, and the order matters:
// materials occupy 0..B_COUNT-1, so every `it < world::B_COUNT` test in the
// codebase still means exactly "is this a block" without being touched.
constexpr uint8_t TOOL_BASE  = 100;   // ..107, see toolId below
constexpr uint8_t SLOT_EMPTY = 201;

enum ToolKind : uint8_t { TK_PICK, TK_SWORD, TK_COUNT };
enum ToolTier : uint8_t { TT_WOOD, TT_STONE, TT_IRON, TT_DIAMOND, TT_COUNT };

constexpr uint8_t toolId(uint8_t kind, uint8_t tier) {
  return (uint8_t)(TOOL_BASE + kind * TT_COUNT + tier);
}
// How many tool ids the band holds. Named rather than written inline as
// TK_COUNT * TT_COUNT, because multiplying two different enum types together
// is deprecated in C++20 and the compiler is right to say so.
constexpr int TOOL_N = (int)TK_COUNT * (int)TT_COUNT;

constexpr bool isTool(uint8_t it) {
  return it >= TOOL_BASE && it < TOOL_BASE + TOOL_N;
}
constexpr uint8_t toolKind(uint8_t it) { return (uint8_t)((it - TOOL_BASE) / TT_COUNT); }
constexpr uint8_t toolTier(uint8_t it) { return (uint8_t)((it - TOOL_BASE) % TT_COUNT); }

// What a tool is worth in the two things a tool does. Mining is effort per
// tick against world::EFFORT_PER_TICK's 16; melee is hearts off a mob.
struct ToolInfo {
  const char* name;
  uint16_t durability;   // blocks broken, or blows landed, before it is gone
  uint8_t  effort;       // mining effort per tick
  int16_t  damage;       // melee damage
};
const ToolInfo& toolInfo(uint8_t kind, uint8_t tier);

// How much slower bare hands are than a wooden pickaxe. Minecraft's own ratio
// is about five for stone, and it is the number that gives the tool a reason to
// exist: without it "what is selected" is a label rather than a choice. It is
// also what paces the opening minute now that a run starts empty -- wood comes
// away in about four seconds by hand, stone in six.
constexpr int HAND_DIVISOR = 5;

// Bare hands, and anything held that is not a pickaxe. Named because three
// places need the same number and one of them is a test.
constexpr int HAND_EFFORT = world::EFFORT_PER_TICK / HAND_DIVISOR;
constexpr int16_t HAND_DAMAGE = 1;

// ---- crafting ---------------------------------------------------------------

// Twelve recipes over a 2x2 grid. Shapeless: what a recipe wants is a multiset
// of up to four materials, and where in the grid they sit is not part of it.
// Shape would buy nothing at this size -- with four cells there is exactly one
// interesting shaped recipe per multiset -- and it would cost the player four
// chances to get a layout subtly wrong on a keyboard with no pointer.
enum Recipe : uint8_t {
  R_PLANK, R_TORCH, R_BRICK, R_PATCH,
  R_PICK_WOOD, R_PICK_STONE, R_PICK_IRON, R_PICK_DIAMOND,
  R_SWORD_WOOD, R_SWORD_STONE, R_SWORD_IRON, R_SWORD_DIAMOND,
  R_COUNT
};

constexpr int     GRID_N     = 4;     // 2x2

// The craft card's cursor visits more than the four cells: the result slot is
// where a craft is committed, and the recipe book is opened from a row under
// the grid. Making them focus stops rather than dedicated keys is what lets the
// whole card be driven by four arrows and ENTER.
constexpr uint8_t GRID_FOCUS_OUT  = 4;
constexpr uint8_t GRID_FOCUS_BOOK = 5;
constexpr uint8_t GRID_FOCUS_N    = 6;
constexpr uint8_t CELL_EMPTY = 255;   // a bare cell; not a material, not a tool
constexpr uint8_t R_NONE     = 255;   // matchGrid found nothing

// outItem is a world::Block, a tool id, or ITEM_NONE for a recipe whose whole
// result is the heal. One field rather than a block-or-tool pair because the
// three bands are already disjoint by construction -- see TOOL_BASE.
constexpr uint8_t ITEM_NONE = 254;

struct RecipeInfo {
  const char* name;
  uint8_t cells[GRID_N];   // materials or CELL_EMPTY; order is not significant
  uint8_t outItem;
  uint8_t outQty;
  uint8_t heal;            // hearts restored, for the ones that are not items
};
const RecipeInfo& recipeInfo(uint8_t r);

// Which recipe a grid spells, or R_NONE. Says nothing about whether the player
// can afford it -- the grid is an arrangement, and the cells are not spent
// until the craft commits. See canAfford.
uint8_t matchGrid(const uint8_t grid[GRID_N]);

// Held-button state, filled by the HAL. Six flags is the whole input surface —
// see hal.h for how boards with fewer buttons reach it.
struct Input {
  bool left = false, right = false, fwd = false, back = false;
  bool act = false, build = false;
  // Held, not an edge: the repeat that empties a stack is paced by the
  // simulation (see DROP_PERIOD), so a board only has to say whether the key
  // is down. A board with no key to spare leaves it false and simply cannot
  // throw anything away, which costs it nothing else.
  bool drop = false;
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
  // A mob made a noise for no reason. Minecraft's night is mostly this — the
  // moan that tells you something is out there before it is on top of you.
  EV_MOB_IDLE   = 1u << 20,
  // The arm is moving. Not a sound of its own — it is what keeps the pickaxe
  // animating through a swing that connects with nothing.
  EV_SWINGING   = 1u << 17,
  EV_WHIFF      = 1u << 14,   // the strike frame of a swing that found nothing
  EV_ARROW_FIRE = 1u << 15,
  EV_ARROW_HIT  = 1u << 16,
  // Aimed somewhere a block cannot go: off the map, inside a body, or — while
  // the world is still a heightmap — out in mid-air where nothing can hold it.
  EV_CANT_PLACE = 1u << 18,
  // The world has no run left to describe the hole a cut would leave, or the
  // block a build would need. A wall the player has honeycombed too finely.
  EV_NO_ROOM    = 1u << 19,
  // A tool spent its last point of durability and left the hand holding it.
  EV_TOOL_BROKE = 1u << 21,
  EV_DROP       = 1u << 22,   // something left the bar and is now on the floor
  EV_PICKUP     = 1u << 23,   // ...and something was walked over and taken back
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

// ---- dropped items ----------------------------------------------------------

// A thing on the floor. Thrown out of the bar by the drop key, or spilled by a
// mined block that had nowhere to go, and picked back up by walking over it.
//
// This is what makes the nine slots a real capacity rather than a display
// quirk. Before it, a material with no slot went into inv[] and simply stopped
// being mentioned -- held, uncountable and unplaceable, with nothing on screen
// to say so. Now the bar IS the inventory, and what will not fit lands at your
// feet where you can see it.
constexpr int MAX_DROPS = 16;

// A minute and a half. Long enough to go and empty a slot and come back, short
// enough that a morning's spillage is not still lying there at dusk.
constexpr int DROP_LIFE   = 90 * TICK_HZ;
// Ticks before a fresh drop can be collected. Without it the pickup test fires
// on the same tick the throw does and the item never leaves your hand.
constexpr int DROP_ARM    = 1 * TICK_HZ;
// Ticks between repeats while held, so about seven a second. The gap is
// actually this plus one -- the throw happens on the tick the timer reads zero
// and then the timer counts this many down -- which is worth knowing before
// retuning it, and not worth an off-by-one in the tap path to correct.
constexpr int DROP_PERIOD = 8;

constexpr float DROP_TOSS    = 3.6f;  // cells/second, thrown forward
constexpr float DROP_LOFT    = 2.4f;  // cells/second, thrown up
constexpr float DROP_GRAVITY = 26.0f; // cells/second/second
constexpr float DROP_REACH   = 1.10f; // how close you have to be to collect one

struct Drop {
  float    x, y, z;
  float    vx, vy, vz;
  uint16_t life;      // ticks left before it goes
  uint16_t dur;       // durability, when item is a tool
  uint8_t  item;      // a world::Block or a tool id -- the same byte a slot holds
  uint8_t  count;     // always 1 for a tool
  uint8_t  arm;       // ticks until it can be picked up
  bool     alive;
  bool     rest;      // settled on a surface, no longer falling
};

struct Mob {
  float   x, y;
  // The surface it is standing on. It used to be derived from the terrain on
  // the spot, on the reasoning that a body always stands on world::groundAt —
  // which stopped being true the moment a cell could offer more than one place
  // to stand. A mob on a bridge deck and a mob under it are in the same cell.
  uint8_t z;
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
  uint16_t voice;    // ticks until it makes a noise of its own accord
  uint16_t press;    // ticks a creeper has spent close but unable to close
};

struct State {
  raycast::Camera cam;
  float    angle = 0.0f;

  // Ten hearts, Minecraft's number. It used to be six, on the reasoning that a
  // short bar makes the +2 upgrade read as a real gain — and that upgrade is
  // gone, so what is left is a fixed bar, and a fixed bar of six against mobs
  // that hit for up to four is two mistakes from over.
  int16_t  hp = 10, maxHp = 10;
  uint16_t inv[world::B_COUNT] = {0};   // how much of each material is held

  // What is in each hotbar slot: a tool id, SLOT_EMPTY, or a world::Block.
  //
  // For materials the counts live in inv[], but the two are now the same fact:
  // inv[m] is non-zero exactly when m has a slot. A material used to be able to
  // sit in inv[] with nothing on the bar to show it -- held, uncountable and
  // unplaceable -- and what closed that hole was giving the bar a real
  // capacity: a material with nowhere to go is spilled on the floor as a Drop
  // instead of swallowed. A tool was always this way round: the slot IS the
  // tool, there is nowhere else for it to be, and that is why crafting one
  // needs a free slot.
  uint8_t  slot[SLOT_N];
  uint8_t  sel = 0;                     // which slot is selected, 0..SLOT_N-1

  // Durability left, per slot. Parallel to slot[] rather than a field on an
  // item struct, because a slot is one byte everywhere else in the program and
  // widening it would touch the hotbar, the renderer and every block test.
  // Meaningless -- and not read -- where slot[i] is not a tool.
  uint16_t dur[SLOT_N] = {0};

  // The 2x2 crafting grid and its cursor. Held in the simulation rather than
  // in the craft screen so the host tests can lay out a recipe and commit it
  // without a renderer, which is where every crafting test below lives.
  //
  // Filled with CELL_EMPTY by begin(), not by the initialiser above: CELL_EMPTY
  // is 255 and a zeroed grid would read as four cells of grass.
  uint8_t  grid[GRID_N];
  uint8_t  gridSel = 0;

  uint16_t burn = 0;                    // ticks until lava bites again
  uint16_t night = 1;

  uint8_t  phase = PH_DAY;
  uint32_t phaseTick = 0;

  Mob      mobs[MAX_MOBS];
  uint16_t sealedTicks = 0;    // how long no mob has been able to path to you
  uint16_t spawnTimer = 0;
  // Consecutive spawn attempts that found nowhere to put a mob. See the clock
  // in tick() for why the spawner has to be able to say "nowhere" out loud.
  uint8_t  spawnFails = 0;
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
  Drop     drops[MAX_DROPS];
  // Ticks until the drop key may fire again. Zero while the key is up, so the
  // next tap is instant and only a held key pays the repeat interval.
  uint8_t  dropTimer = 0;
  Spark    sparks[MAX_SPARKS];
  uint8_t  sparkN = 0;         // reset by whoever drains it, once a frame
  uint8_t  toolPhase = 0;      // frame of the pickaxe swing, 0 = at rest

  // What the crosshair is on this tick, for the HUD.
  bool     aimValid = false;
  bool     aimOnTop = false;   // came down on a column top rather than its side
  int16_t  aimX = 0, aimY = 0, aimZ = 0;
  // The outward normal of the face being pointed at. A block built against
  // that face goes at aim + this, which is the whole of Minecraft's placement
  // rule and the reason the hit has to carry a face at all.
  int8_t   aimNX = 0, aimNY = 0, aimNZ = 0;
  uint8_t  aimDamage = 0;

  // The surface the player is standing on. Not derivable from the cell any
  // more: a cell can offer several, and which one you are on depends on which
  // one you walked in at. This is what makes a floor you built somewhere you
  // can stand rather than scenery.
  uint8_t  feetZ = 0;

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

  // What the sounds this tick were ABOUT. An event is a bit and a bit cannot
  // carry a subject, so the three cues that need one — which material is being
  // dug, which mob was hit, which mob spoke — leave it here. Written by tick(),
  // read by whoever turns events into sound, and meaningless without the
  // matching event bit.
  uint8_t  sfxDigMat = 0;      // with EV_MINE_STEP / EV_BLOCK_BROKE / EV_PLACE
  uint8_t  sfxHitKind = 0;     // with EV_MOB_HIT
  uint8_t  sfxDiedKind = 0;    // with EV_MOB_DIED
  uint8_t  sfxIdleKind = 0;    // with EV_MOB_IDLE

  uint32_t rng = 1;            // in-state, so a run is reproducible on the host
};

// Fresh run: generates the world, places the player, resets every counter.
void begin(State& s, uint32_t seed);

// Advances one 1/TICK_HZ step. Returns an OR of Event bits.
uint32_t tick(State& s, const Input& in);

// Moves the selection along the bar, wrapping. Skips nothing: an empty slot is
// still selectable, because an empty hand is a thing you can choose to hold.
void cycleBlock(State& s, int delta);

// Selects a slot outright, the way a number key does. Out-of-range is ignored.
void selectSlot(State& s, int index);

// What is in the selected slot: a tool id, SLOT_EMPTY, or a world::Block.
uint8_t heldItem(const State& s);
bool    heldIsTool(const State& s);


// The material the player would place right now, and how many are left. The
// material is world::B_COUNT when the selection is not a block at all.
uint8_t heldBlock(const State& s);
uint16_t heldCount(const State& s);

// Throws one of whatever is selected onto the floor in front of the player.
// A tool goes whole, carrying its remaining durability with it. False when
// there is nothing in the hand or nowhere to put it.
bool dropOne(State& s);

// True if the bar could accept this material: either it already has a slot, or
// there is a free one for it to claim. What makes "full" mean something.
bool canAccept(const State& s, uint8_t mat);

// How many items are lying on the floor right now.
int dropsAlive(const State& s);

// Everything placeable, added up — for the HUD and the score.
uint16_t totalBlocks(const State& s);

// True if the inventory holds every cell of the recipe, and -- for a recipe
// whose result is a tool -- there is a free slot to put it in. A tool has no
// count in inv[], so unlike a material it cannot wait for a slot to free.
bool canCraft(const State& s, uint8_t recipe);

// True if the inventory covers what is actually laid out in the grid. Separate
// from canCraft because the grid can spell a recipe the player cannot pay for:
// every cell cycles through held materials independently, so one plank will
// happily fill three cells.
bool canAffordGrid(const State& s);

// Crafts whatever the grid currently spells, clearing it. False, and nothing
// spent, if the grid matches nothing or the player cannot afford it.
bool craftGrid(State& s);

// Lays a recipe's cells into the grid. False if unaffordable; the grid is left
// as it was rather than half-filled.
bool fillGrid(State& s, uint8_t recipe);

// Moves the grid cursor, wrapping, and cycles one cell through the materials
// currently held (and empty). The two things the craft card's keys do.
void gridMove(State& s, int dx, int dy);
void gridCycle(State& s, int delta);

// True while the cursor is on one of the four cells, rather than on the result
// slot or the book row. What ENTER means depends on it.
inline bool gridOnCell(const State& s) { return s.gridSel < GRID_N; }

// Consumes the inputs and grants the output. No-op if unaffordable.
bool craft(State& s, uint8_t recipe);

// 0 at dawn, 1 at dusk — drives the sky/fog lerp in render.cpp.
float daylight(const State& s);

uint32_t score(const State& s);

}  // namespace game
