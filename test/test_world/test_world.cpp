// Host-side tests for the world grid: terrain generation, the material
// profile under a column, and the two edits the game is built on.
// Run with:  pio test -e native
#include <unity.h>

#include "world.h"

using namespace world;

// ---- generation -------------------------------------------------------------

// A run is meant to be reproducible from its seed. If generate() ever picks up
// state that outlives it, two calls with the same seed stop matching and a
// reported score means nothing.
static void test_generate_is_deterministic(void) {
  generate(9001);
  uint8_t h1[W * H], m1[W * H];
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) { h1[y * W + x] = height(x, y); m1[y * W + x] = topMat(x, y); }

  generate(1234);          // a different world in between
  generate(9001);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      TEST_ASSERT_EQUAL_UINT8(h1[y * W + x], height(x, y));
      TEST_ASSERT_EQUAL_UINT8(m1[y * W + x], topMat(x, y));
    }
}

// The border has to be full height as well as bedrock: a ray that escapes the
// grid must be stopped by something the walker can draw, not by a bounds check.
static void test_border_is_full_height_bedrock(void) {
  generate(7);
  TEST_ASSERT_TRUE(isBorder(0, 0));
  TEST_ASSERT_TRUE(isBorder(W - 1, H - 1));
  TEST_ASSERT_EQUAL_UINT8(MAX_H, height(0, 0));
  TEST_ASSERT_EQUAL_UINT8(B_BEDROCK, topMat(0, 0));
  // Out of bounds reads report the same wall rather than failing.
  TEST_ASSERT_EQUAL_UINT8(MAX_H, height(-5, 3));
  TEST_ASSERT_EQUAL_UINT8(MAX_H, height(W + 40, H + 40));
  TEST_ASSERT_TRUE(isBorder(-1, -1));
}

// generate() used to be able to drop the player inside a hill. The spawn pad
// is cleared last, after the structures, precisely so a tree or a ruin cannot
// land on it.
static void test_spawn_pad_is_clear(void) {
  for (uint32_t seed = 1; seed <= 25; ++seed) {
    generate(seed);
    const int cx = W / 2, cy = H / 2;
    for (int y = cy - 3; y <= cy + 3; ++y)
      for (int x = cx - 3; x <= cx + 3; ++x)
        TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  }
}

// Nothing natural may sit below ground level, or the player starts the run
// already in a hole they cannot see out of.
static void test_terrain_never_starts_below_ground(void) {
  for (uint32_t seed = 1; seed <= 10; ++seed) {
    generate(seed);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        TEST_ASSERT_TRUE(height(x, y) <= MAX_H);
  }
}

// ---- material profile -------------------------------------------------------

// A tree is a canopy over a trunk over ordinary soil. It used to be leaves
// sitting on a plug of dirt, because the soil profile was measured from the
// column top rather than from ground level.
//
// Only the tall centre column of a tree has a trunk: the short leafy skirts
// beside it are two blocks of canopy resting straight on the ground, so the
// trunk assertion is made against height, not against every leaf column.
static void test_tree_is_wood_under_leaves(void) {
  generate(4242);
  int canopies = 0, trunks = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (topMat(x, y) != B_LEAVES) continue;
      const int h = height(x, y);
      ++canopies;
      TEST_ASSERT_EQUAL_UINT8(B_LEAVES, matAt(x, y, h - 1));
      TEST_ASSERT_EQUAL_UINT8(B_LEAVES, matAt(x, y, h - 2));
      // Everything between ground level and the canopy is trunk.
      for (int z = GROUND; z < h - 2; ++z) {
        TEST_ASSERT_EQUAL_UINT8(B_WOOD, matAt(x, y, z));
        ++trunks;
      }
      // Under ground level it is ordinary soil again, not more trunk.
      TEST_ASSERT_NOT_EQUAL(B_WOOD, matAt(x, y, GROUND - 1));
      TEST_ASSERT_NOT_EQUAL(B_WOOD, matAt(x, y, 0));
    }
  }
  TEST_ASSERT_TRUE(canopies > 0);
  TEST_ASSERT_TRUE(trunks > 0);       // at least one tree was tall enough
}

// Ore only exists below the dirt band, which is what makes it worth digging
// for rather than something you trip over on the surface.
static void test_ore_is_below_the_dirt_band(void) {
  generate(31337);
  TEST_ASSERT_EQUAL_UINT8(B_DIRT, matAt(20, 20, GROUND - 2));
  bool sawOre = false;
  for (int y = 2; y < H - 2 && !sawOre; ++y)
    for (int x = 2; x < W - 2 && !sawOre; ++x)
      if (matAt(x, y, 0) == B_COAL || matAt(x, y, 0) == B_IRON) sawOre = true;
  TEST_ASSERT_TRUE(sawOre);
}

// Layers at or above the column height are the base plane, not a stale read of
// whatever the top material happened to be.
static void test_above_column_is_base_plane(void) {
  generate(5);
  const int h = height(30, 30);
  TEST_ASSERT_EQUAL_UINT8(B_BEDROCK, matAt(30, 30, h));
  TEST_ASSERT_EQUAL_UINT8(B_BEDROCK, matAt(30, 30, h + 3));
  TEST_ASSERT_EQUAL_UINT8(B_BEDROCK, matAt(30, 30, -1));
}

// ---- mining -----------------------------------------------------------------

static void test_mine_takes_exactly_one_block(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  const int before = height(x, y);
  uint8_t m = 0, b = 0, o = 0;
  int guard = 0;
  while (!mine(x, y, 32, m, b, o)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_INT(before - 1, height(x, y));
  TEST_ASSERT_TRUE(b > 0);
}

// matAt reads g_h and g_top, so working out the revealed material after the
// column has already shrunk asks "what is the top of this column" and gets
// back the block that was just removed. The surface then never changed at all:
// digging the grass off a hill left grass, and taking a torch down left
// another torch underneath it.
static void test_mining_reveals_what_is_underneath(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  uint8_t m = 0, b = 0, o = 0;

  TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  const uint8_t surface = topMat(x, y);
  int guard = 0;
  while (!mine(x, y, 64, m, b, o)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_UINT8(surface, m);              // took the surface block
  TEST_ASSERT_NOT_EQUAL(surface, topMat(x, y));     // and something else is now on top
  TEST_ASSERT_EQUAL_UINT8(B_DIRT, topMat(x, y));

  // A torch is a single block on top of something, not a column of torches.
  TEST_ASSERT_TRUE(place(x, y, B_TORCH));
  guard = 0;
  while (!mine(x, y, 64, m, b, o)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_UINT8(B_TORCH, m);
  TEST_ASSERT_NOT_EQUAL(B_TORCH, topMat(x, y));
}

// Effort is per-target. Looking at another block and back has to start over,
// or a player could chip at every block in a wall and break them all at once.
static void test_mining_effort_resets_on_new_target(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  uint8_t m = 0, b = 0, o = 0;
  TEST_ASSERT_FALSE(mine(x, y, 100, m, b, o));
  TEST_ASSERT_TRUE(damage(x, y) > 0);
  TEST_ASSERT_FALSE(mine(x + 1, y, 16, m, b, o));      // look away
  TEST_ASSERT_EQUAL_UINT8(0, damage(x, y));         // progress discarded
  resetDamage(x + 1, y);
  TEST_ASSERT_EQUAL_UINT8(0, damage(x + 1, y));
}

// The whole point of the ground level offset: a column can go below it.
static void test_can_dig_below_ground_level(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  uint8_t m = 0, b = 0, o = 0;
  for (int dug = 0; dug < GROUND; ++dug) {
    int guard = 0;
    while (!mine(x, y, 64, m, b, o)) TEST_ASSERT_TRUE(++guard < 500);
  }
  TEST_ASSERT_EQUAL_UINT8(0, height(x, y));
  // The base plane is bedrock and cannot be taken any further.
  TEST_ASSERT_FALSE(mine(x, y, 100000, m, b, o));
}

static void test_border_is_unbreakable(void) {
  generate(11);
  uint8_t m = 0, b = 0, o = 0;
  const uint8_t before = height(0, 5);
  TEST_ASSERT_FALSE(mine(0, 5, 1000000, m, b, o));
  TEST_ASSERT_EQUAL_UINT8(before, height(0, 5));
  TEST_ASSERT_FALSE(place(0, 5, B_PLANK));
}

// ---- placing ----------------------------------------------------------------

static void test_place_stacks_and_caps(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  const int start = height(x, y);
  TEST_ASSERT_TRUE(place(x, y, B_PLANK));
  TEST_ASSERT_EQUAL_INT(start + 1, height(x, y));
  TEST_ASSERT_EQUAL_UINT8(B_PLANK, topMat(x, y));
  int guard = 0;
  while (place(x, y, B_PLANK)) TEST_ASSERT_TRUE(++guard < 64);
  TEST_ASSERT_EQUAL_UINT8(MAX_H, height(x, y));
}

// ---- movement ---------------------------------------------------------------

// One block up is walked without noticing; two stops you. That rule is the
// only reason a two-high wall is worth the blocks it costs.
static void test_step_up_limit(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  TEST_ASSERT_TRUE(canEnter(GROUND, x + 1, y));
  place(x + 1, y, B_PLANK);                    // one up
  TEST_ASSERT_TRUE(canEnter(GROUND, x + 1, y));
  place(x + 1, y, B_PLANK);                    // two up
  TEST_ASSERT_FALSE(canEnter(GROUND, x + 1, y));
  // Down is always free, however far.
  TEST_ASSERT_TRUE(canEnter(GROUND + 4, x + 1, y));
}

static void test_fits_checks_every_overlapped_cell(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  const float px = (float)x + 0.5f, py = (float)y + 0.5f;
  TEST_ASSERT_TRUE(fits(GROUND, px, py, 0.3f));
  place(x + 1, y, B_PLANK);
  place(x + 1, y, B_PLANK);                    // a two-high block to the east
  TEST_ASSERT_TRUE(fits(GROUND, px, py, 0.3f));        // still clear of it
  TEST_ASSERT_FALSE(fits(GROUND, px + 0.45f, py, 0.3f));  // now overlapping it
}

// ---- explosions -------------------------------------------------------------

// A blast should leave a crater, not a cylinder: deepest at the centre,
// one block at the rim.
static void test_explode_is_deepest_at_the_centre(void) {
  generate(11);
  const int cx = W / 2, cy = H / 2;
  const int before = height(cx, cy);
  const int n = explode(cx, cy, 2);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(height(cx, cy) < before);
  TEST_ASSERT_TRUE(height(cx, cy) <= height(cx + 2, cy));
  // Bedrock survives it.
  const uint8_t border = height(0, 5);
  explode(0, 5, 3);
  TEST_ASSERT_EQUAL_UINT8(border, height(0, 5));
}

// ---- biomes and light -------------------------------------------------------

// All three biomes have to actually appear, or the second noise channel is
// tuned wrong and the whole map is one palette.
static void test_all_biomes_appear(void) {
  bool seen[BIOME_COUNT] = { false, false, false };
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    generate(seed);
    for (int y = 2; y < H - 2; ++y)
      for (int x = 2; x < W - 2; ++x)
        seen[biomeAt(x, y)] = true;
  }
  for (int i = 0; i < BIOME_COUNT; ++i) TEST_ASSERT_TRUE(seen[i]);
}

static void test_biome_sets_the_surface_material(void) {
  generate(4242);
  for (int y = 2; y < H - 2; ++y) {
    for (int x = 2; x < W - 2; ++x) {
      if (height(x, y) != GROUND) continue;         // untouched flat ground only
      const uint8_t top = topMat(x, y);
      if (isStructure(top)) continue;
      // A cave floor is at ground level too, but it is cut rock: whatever the
      // hill had at that depth, which is legitimately stone or ore.
      if (hasSlab(x, y)) continue;
      switch (biomeAt(x, y)) {
        case BIOME_DESERT: TEST_ASSERT_EQUAL_UINT8(B_SAND,  top); break;
        case BIOME_TUNDRA: TEST_ASSERT_EQUAL_UINT8(B_SNOW,  top); break;
        default:           TEST_ASSERT_EQUAL_UINT8(B_GRASS, top); break;
      }
    }
  }
}

// Light falls off with distance and follows the source when it moves.
static void test_light_follows_its_source(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  TEST_ASSERT_EQUAL_UINT8(0, light(x, y));

  TEST_ASSERT_TRUE(place(x, y, B_TORCH));
  TEST_ASSERT_EQUAL_UINT8(LIGHT_MAX, light(x, y));
  TEST_ASSERT_TRUE(light(x + 2, y) > 0);
  TEST_ASSERT_TRUE(light(x + 2, y) < light(x, y));
  TEST_ASSERT_EQUAL_UINT8(0, light(x + LIGHT_MAX + 1, y));

  // Mine it back out and the light has to go with it.
  uint8_t m = 0, b = 0, o = 0;
  int guard = 0;
  while (!mine(x, y, 64, m, b, o)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_UINT8(B_TORCH, m);
  TEST_ASSERT_EQUAL_UINT8(0, light(x, y));
}

// Lava lights the pit it sits in, which is what makes a quarry readable at
// night, and it can never be mined away.
static void test_lava_glows_and_is_unbreakable(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  place(x, y, B_LAVA);
  TEST_ASSERT_TRUE(light(x, y) > 0);
  TEST_ASSERT_TRUE(isHazard(B_LAVA));
  TEST_ASSERT_FALSE(isHazard(B_STONE));
  uint8_t m = 0, b = 0, o = 0;
  TEST_ASSERT_FALSE(mine(x, y, 1000000, m, b, o));
}

// place() used to ignore slabs completely, so a column could be raised
// straight through a bridge deck, and the gap under one could be bricked shut
// around whatever was standing in it — leaving a body in a cell the movement
// rules say is impossible to occupy. A soak run found 2504 instances of it.
static void test_cannot_build_into_a_slab(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  devSlab(x, y, GROUND + HEADROOM + 1, GROUND + HEADROOM + 3, B_STONE);

  // One course still fits under it.
  TEST_ASSERT_TRUE(place(x, y, B_PLANK));
  TEST_ASSERT_TRUE(standable(x, y));
  // The next would leave no room to stand, so it is refused.
  TEST_ASSERT_FALSE(place(x, y, B_PLANK));
  TEST_ASSERT_TRUE(standable(x, y));
  TEST_ASSERT_EQUAL_UINT8(GROUND + 1, height(x, y));
}

static void test_standable_reports_headroom(void) {
  generate(11);
  const int x = W / 2 + 3, y = H / 2;
  TEST_ASSERT_TRUE(standable(x, y));            // open sky
  devSlab(x, y, GROUND + HEADROOM, GROUND + HEADROOM + 2, B_STONE);
  TEST_ASSERT_TRUE(standable(x, y));            // exactly enough
  devSlab(x, y, GROUND + 1, GROUND + 3, B_STONE);
  TEST_ASSERT_FALSE(standable(x, y));           // too low to stand under
  TEST_ASSERT_FALSE(standable(-1, -1));
}

static void test_degenerate_input(void) {
  generate(0);                                  // seed 0 must still produce a map
  TEST_ASSERT_EQUAL_UINT8(GROUND, height(W / 2, H / 2));
  uint8_t m = 0, b = 0, o = 0;
  TEST_ASSERT_FALSE(mine(-1, -1, 100, m, b, o));
  TEST_ASSERT_FALSE(place(-1, -1, B_PLANK));
  TEST_ASSERT_EQUAL_INT(0, explode(-50, -50, 2));
  TEST_ASSERT_EQUAL_UINT8(0, damage(-1, -1));
  TEST_ASSERT_FALSE(fits(GROUND, -3.0f, -3.0f, 0.3f));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_generate_is_deterministic);
  RUN_TEST(test_border_is_full_height_bedrock);
  RUN_TEST(test_spawn_pad_is_clear);
  RUN_TEST(test_terrain_never_starts_below_ground);
  RUN_TEST(test_tree_is_wood_under_leaves);
  RUN_TEST(test_ore_is_below_the_dirt_band);
  RUN_TEST(test_above_column_is_base_plane);
  RUN_TEST(test_mine_takes_exactly_one_block);
  RUN_TEST(test_mining_reveals_what_is_underneath);
  RUN_TEST(test_mining_effort_resets_on_new_target);
  RUN_TEST(test_can_dig_below_ground_level);
  RUN_TEST(test_border_is_unbreakable);
  RUN_TEST(test_place_stacks_and_caps);
  RUN_TEST(test_step_up_limit);
  RUN_TEST(test_fits_checks_every_overlapped_cell);
  RUN_TEST(test_explode_is_deepest_at_the_centre);
  RUN_TEST(test_all_biomes_appear);
  RUN_TEST(test_biome_sets_the_surface_material);
  RUN_TEST(test_light_follows_its_source);
  RUN_TEST(test_lava_glows_and_is_unbreakable);
  RUN_TEST(test_cannot_build_into_a_slab);
  RUN_TEST(test_standable_reports_headroom);
  RUN_TEST(test_degenerate_input);
  return UNITY_END();
}
