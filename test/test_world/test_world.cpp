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
//
// The slab as well as the column. The clear used to reset height and material
// and leave the second run behind, which put a roof, an eave or a tree crown
// over the landing pad with nothing underneath holding it — rare while only
// ruins had slabs, routine now that every tree does.
static void test_spawn_pad_is_clear(void) {
  for (uint32_t seed = 1; seed <= 25; ++seed) {
    generate(seed);
    const int cx = W / 2, cy = H / 2;
    for (int y = cy - 3; y <= cy + 3; ++y)
      for (int x = cx - 3; x <= cx + 3; ++x) {
        TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
        TEST_ASSERT_FALSE(hasSlab(x, y));
      }
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

// A tree is a cap of leaves over a trunk over ordinary soil. It used to be
// leaves sitting on a plug of dirt, because the soil profile was measured from
// the column top rather than from ground level.
static void test_tree_is_wood_under_leaves(void) {
  generate(4242);
  int canopies = 0, trunks = 0;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (topMat(x, y) != B_LEAVES) continue;
      const int h = height(x, y);
      ++canopies;
      TEST_ASSERT_EQUAL_UINT8(B_LEAVES, matAt(x, y, h - 1));
      // Everything between ground level and the cap is trunk.
      for (int z = GROUND; z < h - 1; ++z) {
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

// And the trunk you can see is the trunk you get. The leaf band used to be the
// top *two* layers of the column, measured from a height that mining changes:
// take the cap off and the block under it became the new top two, so the leaves
// walked down the trunk ahead of the pick and a whole tree yielded nothing but
// leaves. B_WOOD was in the material table, described as the trunk, drawn on
// the side of every tree, and could not be obtained at all.
static void test_chopping_a_tree_actually_yields_wood(void) {
  generate(4242);
  int tx = -1, ty = -1;
  for (int y = 1; y < H - 1 && tx < 0; ++y)
    for (int x = 1; x < W - 1 && tx < 0; ++x)
      if (topMat(x, y) == B_LEAVES && height(x, y) > GROUND + 2) { tx = x; ty = y; }
  TEST_ASSERT_TRUE(tx > 0);

  uint8_t dm, db;
  int leaves = 0, wood = 0;
  while (height(tx, ty) > GROUND)
    if (mineTop(tx, ty, 100000, dm, db)) {
      if (dm == B_LEAVES) ++leaves;
      if (dm == B_WOOD)   ++wood;
    }
  TEST_ASSERT_EQUAL_INT(1, leaves);       // one block of cap
  TEST_ASSERT_TRUE(wood >= 2);            // and the rest of it is trunk
}

// ---- canopies ---------------------------------------------------------------

// The canopy is the whole point of the tree: leaves over open air, a cell out
// from a trunk that is not underneath them. A heightmap cannot say that, and a
// column of leaves standing on the ground is a bush however tall you build it —
// which is what the old tree was. It is a slab, the same second run per cell
// that roofs a ruin, and it has to leave room to walk under.
static void test_a_tree_canopy_hangs_over_walkable_ground(void) {
  int canopies = 0;
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    generate(seed);
    for (int y = 1; y < H - 1; ++y)
      for (int x = 1; x < W - 1; ++x) {
        if (!hasSlab(x, y) || slabMat(x, y) != B_LEAVES) continue;
        ++canopies;
        TEST_ASSERT_TRUE(slabBase(x, y) > height(x, y));   // over air
        TEST_ASSERT_TRUE(standable(x, y));                 // with headroom
        // Enterable from the ground under it, which is not the same as from
        // GROUND. A crown is allowed to hang over a slope now — the old rule
        // wanted dead-flat ground under every leaf and that is why a tree on
        // any kind of hill came out half bald — so asking whether a body at
        // the global ground level could step in was asking about the hill.
        TEST_ASSERT_TRUE(canEnter((int)height(x, y), x, y));
        // A trunk near it, reaching its underside, holding it up. Searched two
        // cells out, which is one more than a crown now needs: the crown is
        // three across since the scale-down, so every leaf is orthogonally or
        // diagonally adjacent to its own trunk. Left at two deliberately — it
        // is the radius dropOrphanCanopy sweeps, and a wider search can only
        // ever accept a crown a narrower one would.
        //
        // Leaves and not wood, and dropOrphanCanopy agrees with that now: a
        // standing tree wears its leaf cap, so B_LEAVES is what the top of a
        // live trunk looks like. A bare wood column is a snag or a house
        // corner post, and neither of those ever grew a canopy.
        bool held = false;
        for (int dy = -2; dy <= 2 && !held; ++dy)
          for (int dx = -2; dx <= 2 && !held; ++dx)
            held = topMat(x + dx, y + dy) == B_LEAVES
                   && height(x + dx, y + dy) >= slabBase(x, y);
        TEST_ASSERT_TRUE(held);
      }
  }
  TEST_ASSERT_TRUE(canopies > 0);
}

// Fell the tree and the crown stays exactly where it was.
//
// This test used to assert the opposite, and the opposite is what the game
// did: cutting the last log deleted the whole canopy on the same tick. That is
// not what Minecraft does — a chopped oak leaves a crown hanging in the air
// that you can climb into, harvest, or ignore — and it took the leaves out of
// the player's reach in the process. The sweep that did it still exists and is
// still correct, but it belongs to worldgen, where a platform cut out from
// under a tree really would leave a crown over nothing.
static void test_felling_a_tree_leaves_its_canopy_standing(void) {
  generate(4242);
  int tx = -1, ty = -1;
  for (int y = 5; y < H - 5 && tx < 0; ++y)
    for (int x = 5; x < W - 5 && tx < 0; ++x) {
      if (topMat(x, y) != B_LEAVES || height(x, y) <= GROUND + 2) continue;
      // A tree standing on its own: no other trunk within four cells, so every
      // leaf slab inside its own crown can only have been held up by this one.
      int slabs = 0, others = 0;
      for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
          if ((dx || dy) && topMat(x + dx, y + dy) == B_LEAVES) ++others;
          if (dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2
              && hasSlab(x + dx, y + dy) && slabMat(x + dx, y + dy) == B_LEAVES)
            ++slabs;
        }
      if (slabs > 0 && others == 0) { tx = x; ty = y; }
    }
  TEST_ASSERT_TRUE(tx > 0);

  int before = 0;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx)
      if (hasSlab(tx + dx, ty + dy) && slabMat(tx + dx, ty + dy) == B_LEAVES)
        ++before;
  TEST_ASSERT_TRUE(before > 0);

  uint8_t dm, db;
  while (height(tx, ty) > GROUND) mineTop(tx, ty, 100000, dm, db);
  TEST_ASSERT_EQUAL_UINT8(GROUND, height(tx, ty));    // the trunk really is gone

  int after = 0;
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx)
      if (hasSlab(tx + dx, ty + dy) && slabMat(tx + dx, ty + dy) == B_LEAVES)
        ++after;
  TEST_ASSERT_EQUAL_INT(before, after);               // and the crown is still up
}

// ---- buildings --------------------------------------------------------------

// A house is three materials at three brightnesses, and that is what has to be
// true for it to read as a house six cells away in fog rather than as the ruin
// standing next to it. The two are the same silhouette on purpose, so material
// is the only thing this can ask about.
// The block under a roofed column's brick cap, or B_BEDROCK where there is no
// such thing. A wall and the roof over it are one column now, so "what is this
// wall made of" is a question about the block below the cap.
static uint8_t underRoof(int x, int y) {
  const int h = (int)height(x, y);
  if (h < 2 || blockAt(x, y, h - 1) != B_BRICK) return B_BEDROCK;
  return blockAt(x, y, h - 2);
}

static void test_a_house_has_posts_walls_and_a_roof(void) {
  int posts = 0, roofs = 0;
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    generate(seed);
    for (int y = 1; y < H - 1; ++y)
      for (int x = 1; x < W - 1; ++x) {
        // A corner post is a wood column with the brick roof sitting on it.
        // Not identified by height: a building levels its own site, so its
        // walls stand on whatever that site settled at and not on GROUND. And
        // not by material alone either — a dead tundra snag is a bare wood
        // column too, which is exactly why the roof is part of the test.
        //
        // Read as "brick with wood directly under it" rather than "a wood
        // column carrying a slab". Those used to be different things: a roof
        // laid at exactly the wall's top height was a separate object floating
        // at the height the column ended, and hasSlab() found it. A column is
        // a bitmask now, so a roof resting on a wall is simply the next block
        // up — which is what it always was in the world, and what the test
        // should have been asking about.
        const int h = (int)height(x, y);
        if (h < 2) continue;
        if (blockAt(x, y, h - 1) != B_BRICK) continue;
        if (blockAt(x, y, h - 2) != B_WOOD) continue;
        ++posts;
        // A corner post has plank wall running away from it. Only along one
        // edge, necessarily: the other one can be the doorway, whose threshold
        // is left at ground level with whatever the biome puts there.
        int walls = 0;
        for (int d = -1; d <= 1; d += 2) {
          if (underRoof(x + d, y) == B_PLANK) ++walls;
          if (underRoof(x, y + d) == B_PLANK) ++walls;
        }
        TEST_ASSERT_TRUE(walls >= 1);
        ++roofs;
      }
  }
  TEST_ASSERT_TRUE(posts > 0);
  TEST_ASSERT_TRUE(roofs > 0);
}

// You walk in through the door and you do not climb in through the window.
// That is the entire difference between the two, and it is a step-up rule and a
// headroom rule rather than anything about how either one looks.
static void test_a_house_door_admits_and_its_windows_do_not(void) {
  int doors = 0, windows = 0;
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    generate(seed);
    for (int y = 1; y < H - 1; ++y)
      for (int x = 1; x < W - 1; ++x) {
        // A window sill: plank, with the roof course two above it and nothing
        // between. The floor of the room it looks into is two blocks below.
        if (topMat(x, y) == B_PLANK && hasSlab(x, y)
            && slabMat(x, y) == B_BRICK && slabBase(x, y) == height(x, y) + 2) {
          ++windows;
          TEST_ASSERT_FALSE(canEnter(height(x, y) - 2, x, y));   // too tall to step
          continue;
        }
        // A threshold: a plank lintel over floor-level ground.
        if (hasSlab(x, y) && slabMat(x, y) == B_PLANK) {
          ++doors;
          const int floorH = (int)height(x, y);
          TEST_ASSERT_TRUE(canEnter(floorH, x, y));       // and walk through
          TEST_ASSERT_TRUE(standable(x, y));
          // The lintel and the eave over it are one run, not two: a cell holds
          // one slab, so a separate eave would have overwritten this or left a
          // hole in the roofline exactly where the door is.
          //
          // Five, not six. The run is [base + HEADROOM, wallTop + 1) and
          // wallTop came down a course when the houses were scaled to the
          // ten-cell draw distance, so its top followed.
          TEST_ASSERT_TRUE(slabTop(x, y) >= floorH + 5);
        }
      }
  }
  TEST_ASSERT_TRUE(doors > 0);
  TEST_ASSERT_TRUE(windows > 0);
}

static void test_ore_is_below_the_dirt_band(void) {
  generate(31337);
  // An untouched column, found rather than assumed: a fixed cell can land in a
  // quarry, which is cut well below GROUND and is bedrock at this depth.
  int ux = -1, uy = -1;
  for (int y = 3; y < H - 3 && ux < 0; ++y)
    for (int x = 3; x < W - 3 && ux < 0; ++x)
      if (height(x, y) == GROUND && !isStructure(topMat(x, y)) && !hasSlab(x, y))
        { ux = x; uy = y; }
  TEST_ASSERT_TRUE(ux > 0);
  TEST_ASSERT_EQUAL_UINT8(B_DIRT, matAt(ux, uy, GROUND - 2));
  // Sampled a course above the floor: layer zero is bedrock everywhere now, so
  // asking it about ore asks about the one layer that can never hold any.
  bool sawOre = false;
  for (int y = 2; y < H - 2 && !sawOre; ++y)
    for (int x = 2; x < W - 2 && !sawOre; ++x)
      if (matAt(x, y, 1) == B_COAL || matAt(x, y, 1) == B_IRON) sawOre = true;
  TEST_ASSERT_TRUE(sawOre);
}

// Light is craft-only now, so nothing the generator builds may emit any. The
// three places that used to -- a house's interior torch, the village path, and
// the castle brazier -- were free light sitting inside the best shelter on the
// map, which is exactly the pressure the torch recipe is supposed to create.
//
// Swept over seeds rather than checked on one: village, house and castle all
// have placement tests that fail on some maps, so a single seed could pass by
// simply not having built the thing that used to carry the torch.
static void test_the_generator_places_no_light(void) {
  for (uint32_t seed = 1; seed <= 25; ++seed) {
    generate(seed);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (int z = 0; z < MAX_H; ++z) {
          if (!solidAt(x, y, z)) continue;
          TEST_ASSERT_NOT_EQUAL_UINT8(B_TORCH, matAt(x, y, z));
        }
  }
}

// Diamond is deep and rare, and both halves matter: deep is what makes it worth
// digging for, and rare is what stops one seam from kitting out the whole run.
static void test_diamond_is_deep_and_rarer_than_iron(void) {
  int diamond = 0, iron = 0, deepest = -1;
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    generate(seed);
    for (int y = 2; y < H - 2; ++y)
      for (int x = 2; x < W - 2; ++x)
        for (int z = 1; z < MAX_H; ++z) {
          const uint8_t m = matAt(x, y, z);
          if (m == B_IRON) ++iron;
          if (m != B_DIAMOND) continue;
          ++diamond;
          if (z > deepest) deepest = z;
        }
  }
  TEST_ASSERT_TRUE(diamond > 0);                     // it exists...
  TEST_ASSERT_TRUE(diamond < iron);                  // ...and is the scarcer one
  TEST_ASSERT_TRUE(deepest <= DIAMOND_MAX_Z);        // never above its band
}

// The invariant that lets B_DIAMOND be the sixteenth material at all.
//
// g_cell packs the natural surface material into four bits, so ids run 0..15
// and diamond takes the very last one. That is only sound while diamond is
// never a generator SURFACE: it is answered by the depth profile, and setSmat
// is only ever handed a biome surface or bedrock. If it could reach that field
// the id would have to survive a round trip through a byte that is exactly
// full, and isBorder -- which reads the same field -- would start answering
// about the wrong cells.
//
// Asserted as a depth bound, because that is what makes it structural rather
// than incidental: the generator leaves terrain at GROUND (8) and diamond
// cannot exist above DIAMOND_MAX_Z (3), so no cut, quarry or cave can bring
// the two together.
//
// Note that diamond DOES show at the floor of a deep quarry, and should: that
// is where ore surfaces, and iron and coal do exactly the same thing. Exposed
// is not the same as stored -- topMat computes that answer from depth and
// never reads the packed field.
static void test_diamond_cannot_reach_the_surface_field(void) {
  for (uint32_t seed = 1; seed <= 12; ++seed) {
    generate(seed);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (int z = DIAMOND_MAX_Z + 1; z < MAX_H; ++z)
          TEST_ASSERT_NOT_EQUAL_UINT8(B_DIAMOND, matAt(x, y, z));
  }

  // And the field itself is still intact: the border ring is exactly the cells
  // isBorder names, which it decides by reading the packed surface material.
  generate(7);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const bool edge = (x == 0 || y == 0 || x == W - 1 || y == H - 1);
      if (edge) TEST_ASSERT_TRUE(isBorder(x, y));
    }
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
  uint8_t m = 0, b = 0;
  int guard = 0;
  while (!mineTop(x, y, 32, m, b)) TEST_ASSERT_TRUE(++guard < 500);
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
  uint8_t m = 0, b = 0;

  TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  const uint8_t surface = topMat(x, y);
  int guard = 0;
  while (!mineTop(x, y, 64, m, b)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_UINT8(surface, m);              // took the surface block
  TEST_ASSERT_NOT_EQUAL(surface, topMat(x, y));     // and something else is now on top
  TEST_ASSERT_EQUAL_UINT8(B_DIRT, topMat(x, y));

  // A torch is a single block on top of something, not a column of torches.
  TEST_ASSERT_TRUE(place(x, y, B_TORCH));
  guard = 0;
  while (!mineTop(x, y, 64, m, b)) TEST_ASSERT_TRUE(++guard < 500);
  TEST_ASSERT_EQUAL_UINT8(B_TORCH, m);
  TEST_ASSERT_NOT_EQUAL(B_TORCH, topMat(x, y));
}

// Effort is per-target. Looking at another block and back has to start over,
// or a player could chip at every block in a wall and break them all at once.
static void test_mining_effort_resets_on_new_target(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  uint8_t m = 0, b = 0;
  TEST_ASSERT_FALSE(mineTop(x, y, 100, m, b));
  TEST_ASSERT_TRUE(damage(x, y) > 0);
  TEST_ASSERT_FALSE(mineTop(x + 1, y, 16, m, b));      // look away
  TEST_ASSERT_EQUAL_UINT8(0, damage(x, y));         // progress discarded
  resetDamage(x + 1, y);
  TEST_ASSERT_EQUAL_UINT8(0, damage(x + 1, y));
}

// The whole point of the ground level offset: a column can go below it.
static void test_can_dig_below_ground_level(void) {
  generate(11);
  const int x = W / 2, y = H / 2;
  TEST_ASSERT_EQUAL_UINT8(GROUND, height(x, y));
  uint8_t m = 0, b = 0;
  // GROUND - 1 blocks come out, not GROUND: the bottom course is bedrock and
  // stays. The floor of a pit is a real block now rather than an abstract base
  // plane the renderer and the picker each had to know about — which is what
  // gives it a texture to draw and a face the selection box can be put on.
  for (int dug = 0; dug < GROUND - 1; ++dug) {
    int guard = 0;
    while (!mineTop(x, y, 64, m, b)) TEST_ASSERT_TRUE(++guard < 500);
  }
  TEST_ASSERT_EQUAL_UINT8(1, height(x, y));
  TEST_ASSERT_EQUAL_UINT8(B_BEDROCK, matAt(x, y, 0));
  // And it cannot be taken any further.
  TEST_ASSERT_FALSE(mineTop(x, y, 100000, m, b));
}

static void test_border_is_unbreakable(void) {
  generate(11);
  uint8_t m = 0, b = 0;
  const uint8_t before = height(0, 5);
  TEST_ASSERT_FALSE(mineTop(0, 5, 1000000, m, b));
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
  // Somewhere dark, found rather than assumed: the generator lights its own
  // villages and castles now, and one of them can sit near the middle of the
  // map where this used to just take the centre cell.
  int x = -1, y = -1;
  for (int j = 3; j < H - 3 && x < 0; ++j)
    for (int i = 3; i < W - 3 && x < 0; ++i)
      if (light(i, j) == 0 && height(i, j) == GROUND && !hasSlab(i, j)
          && !isStructure(topMat(i, j))) { x = i; y = j; }
  TEST_ASSERT_TRUE(x > 0);
  TEST_ASSERT_EQUAL_UINT8(0, light(x, y));

  TEST_ASSERT_TRUE(place(x, y, B_TORCH));
  TEST_ASSERT_EQUAL_UINT8(LIGHT_MAX, light(x, y));
  TEST_ASSERT_TRUE(light(x + 2, y) > 0);
  TEST_ASSERT_TRUE(light(x + 2, y) < light(x, y));
  TEST_ASSERT_EQUAL_UINT8(0, light(x + LIGHT_MAX + 1, y));

  // Mine it back out and the light has to go with it.
  uint8_t m = 0, b = 0;
  int guard = 0;
  while (!mineTop(x, y, 64, m, b)) TEST_ASSERT_TRUE(++guard < 500);
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
  uint8_t m = 0, b = 0;
  TEST_ASSERT_FALSE(mineTop(x, y, 1000000, m, b));
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
  uint8_t m = 0, b = 0;
  TEST_ASSERT_FALSE(mineTop(-1, -1, 100, m, b));
  TEST_ASSERT_FALSE(place(-1, -1, B_PLANK));
  TEST_ASSERT_EQUAL_INT(0, explode(-50, -50, 2));
  TEST_ASSERT_EQUAL_UINT8(0, damage(-1, -1));
  TEST_ASSERT_FALSE(fits(GROUND, -3.0f, -3.0f, 0.3f));
}


// The bug this locks down: matAt measured soil depth from the column's CURRENT
// height, and mining is what changes that height — so the layer under the one
// you just took off was always "one below the top", which is the dirt band.
// Digging down produced dirt forever and never reached stone, coal or iron.
// Cliff faces rendered the bands correctly, so you could see ore, mine it, and
// get dirt. Ore was obtainable only where the generator wrote it onto a
// surface, which made going underground pointless.
static void test_digging_down_reaches_stone_and_ore(void) {
  world::generate(88);

  bool sawStone = false;
  int oreTotal = 0;
  // A sample of columns, because whether any single one carries ore is a
  // function of the position hash.
  for (int y = 20; y < 76 && !(sawStone && oreTotal); ++y) {
    for (int x = 20; x < 76; x += 3) {
      if (world::isBorder(x, y)) continue;
      for (int i = 0; i < 40 && world::height(x, y) > 1; ++i) {
        uint8_t m = 0, b = 0;
        int guard = 0;
        while (!world::mineTop(x, y, 4096, m, b) && ++guard < 200) {}
        if (guard >= 200) break;
        if (m == world::B_STONE) sawStone = true;
        // Ore is an item now rather than a separate currency, so what says a
        // metal came off is the material, and how much of it is its own yield.
        if (m == world::B_COAL || m == world::B_IRON) oreTotal += b;
      }
    }
  }
  TEST_ASSERT_TRUE(sawStone);
  TEST_ASSERT_TRUE(oreTotal > 0);
}

// The metals are the slowest things on the map to dig and the only reason to go
// down. Their old ore yields moved onto dropBlocks when the currency went, and
// a lump apiece for two thousand effort would make the mine not worth entering.
static void test_the_metals_yield_more_than_a_single_lump(void) {
  world::generate(88);
  TEST_ASSERT_TRUE(world::info(world::B_COAL).dropBlocks >= 2);
  TEST_ASSERT_TRUE(world::info(world::B_IRON).dropBlocks >= 3);
  TEST_ASSERT_TRUE(world::info(world::B_IRON).dropBlocks
                   > world::info(world::B_STONE).dropBlocks);
}

// The other half of the same anchor: building a column up and mining it back
// down must not turn the blocks you placed into the soil profile.
static void test_a_built_column_mines_back_as_what_was_built(void) {
  world::generate(88);
  const int x = world::W / 2 + 5, y = world::H / 2;
  const uint8_t natural = world::height(x, y);
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(world::place(x, y, world::B_BRICK));

  for (int i = 0; i < 4; ++i) {
    uint8_t m = 0, b = 0;
    int guard = 0;
    while (!world::mineTop(x, y, 4096, m, b) && ++guard < 400) {}
    TEST_ASSERT_EQUAL_UINT8(world::B_BRICK, m);
  }
  TEST_ASSERT_EQUAL_UINT8(natural, world::height(x, y));
}


// ---- multi-run columns ------------------------------------------------------

// The verb a heightmap could not express: take a block out of the middle of a
// column and the part above it goes on standing. That is a tunnel through a
// hillside, and it is what "different levels" means for mining.
static void test_mining_mid_column_splits_it(void) {
  generate(88);
  const int x = W / 2 + 6, y = H / 2;
  while (height(x, y) < 10) TEST_ASSERT_TRUE(place(x, y, B_DIRT));
  TEST_ASSERT_EQUAL_INT(0, runsAt(x, y, (RunView*)nullptr, 0));

  uint8_t m, b;
  MineResult r = MINE_PROGRESS;
  for (int i = 0; i < 400 && r != MINE_BROKE; ++i) r = mine(x, y, 4, 4096, m, b);
  TEST_ASSERT_EQUAL_UINT8(MINE_BROKE, r);

  TEST_ASSERT_EQUAL_UINT8(4, height(x, y));      // the column keeps what is below
  RunView rv[8];
  TEST_ASSERT_EQUAL_INT(1, runsAt(x, y, rv, 8));    // and the rest stands overhead
  TEST_ASSERT_EQUAL_UINT8(5,  rv[0].base);
  TEST_ASSERT_EQUAL_UINT8(10, rv[0].top);

  TEST_ASSERT_TRUE(solidAt(x, y, 3));
  TEST_ASSERT_FALSE(solidAt(x, y, 4));           // the hole
  TEST_ASSERT_TRUE(solidAt(x, y, 5));
}

// A run cut out of terrain has to remember the surface its materials were
// measured from, or the piece left hanging over a tunnel turns into one flat
// colour instead of the banded rock you just dug through.
static void test_a_split_run_keeps_its_soil_profile(void) {
  generate(88);
  const int x = W / 2 + 6, y = H / 2;
  while (height(x, y) < 12) TEST_ASSERT_TRUE(place(x, y, B_DIRT));

  // What the untouched column reads at the heights that will end up in the run.
  const uint8_t was5 = matAt(x, y, 5), was6 = matAt(x, y, 6);

  uint8_t m, b;
  MineResult r = MINE_PROGRESS;
  for (int i = 0; i < 400 && r != MINE_BROKE; ++i) r = mine(x, y, 4, 4096, m, b);
  TEST_ASSERT_EQUAL_UINT8(MINE_BROKE, r);

  TEST_ASSERT_EQUAL_UINT8(was5, blockAt(x, y, 5));
  TEST_ASSERT_EQUAL_UINT8(was6, blockAt(x, y, 6));
}

// Building out into the air is the other half of the same feature: a floor, a
// roof, a bridge deck. A heightmap could not hold one at all.
static void test_a_block_can_be_placed_in_mid_air(void) {
  generate(88);
  const int x = W / 2 + 8, y = H / 2;
  const int z = (int)height(x, y) + 5;
  TEST_ASSERT_FALSE(solidAt(x, y, z));
  TEST_ASSERT_EQUAL_UINT8(PLACE_OK, place(x, y, z, B_PLANK));
  TEST_ASSERT_TRUE(solidAt(x, y, z));
  TEST_ASSERT_FALSE(solidAt(x, y, z - 1));       // still air underneath it
  TEST_ASSERT_EQUAL_UINT8(B_PLANK, blockAt(x, y, z));
}

// There must be exactly one way to describe a given solid, or every later edit
// has to guess which of two objects it meant. Filling a tunnel back in has to
// put the column back the way it was.
static void test_filling_a_tunnel_merges_the_run_back_in(void) {
  generate(88);
  const int x = W / 2 + 6, y = H / 2;
  while (height(x, y) < 10) TEST_ASSERT_TRUE(place(x, y, B_DIRT));

  uint8_t m, b;
  MineResult r = MINE_PROGRESS;
  for (int i = 0; i < 400 && r != MINE_BROKE; ++i) r = mine(x, y, 4, 4096, m, b);
  TEST_ASSERT_EQUAL_UINT8(MINE_BROKE, r);
  RunView rv[8];
  TEST_ASSERT_EQUAL_INT(1, runsAt(x, y, rv, 8));

  TEST_ASSERT_EQUAL_UINT8(PLACE_OK, place(x, y, 4, B_DIRT));
  TEST_ASSERT_EQUAL_INT(0, runsAt(x, y, rv, 8));    // one object again
  TEST_ASSERT_EQUAL_UINT8(10, height(x, y));
}

// A column is a bitmask, so there is no limit on how many holes it can have.
//
// This test used to assert the opposite: a cell could describe three runs and
// the fourth split was refused with MINE_NO_ROOM, before any effort was banked.
// That refusal was a real rule the player met -- there were hillsides you were
// simply not allowed to tunnel through -- and removing it is the point of the
// change. What is checked now is that the refusal is gone and that every hole
// is real.
static void test_a_column_can_be_split_without_limit(void) {
  generate(88);
  const int x = W / 2 + 10, y = H / 2;
  while (height(x, y) < 20) TEST_ASSERT_TRUE(place(x, y, B_STONE));

  uint8_t m, b;
  int splits = 0;
  for (int z = 2; z <= 16; z += 2) {
    MineResult r = MINE_PROGRESS;
    for (int i = 0; i < 900 && r != MINE_BROKE; ++i) r = mine(x, y, z, 4096, m, b);
    TEST_ASSERT_EQUAL_UINT8(MINE_BROKE, r);      // never refused
    ++splits;
  }
  TEST_ASSERT_EQUAL_INT(8, splits);              // eight holes in one column

  // Every hole is really there, and so is every block between them.
  for (int z = 2; z <= 16; z += 2) {
    TEST_ASSERT_FALSE(solidAt(x, y, z));
    TEST_ASSERT_TRUE(solidAt(x, y, z + 1));
  }

  // ...and the runs the walker will find are sorted and disjoint.
  RunView rv[8];
  const int n = runsAt(x, y, rv, 8);
  TEST_ASSERT_EQUAL_INT(8, n);
  for (int i = 1; i < n; ++i) {
    TEST_ASSERT_TRUE(rv[i].base >= rv[i - 1].top);
    TEST_ASSERT_TRUE(rv[i].base < rv[i].top);
  }
}

// Effort is still discarded when the player looks away, which is what makes a
// tough block a commitment rather than something you chip at from three angles.
//
// This replaces a test that checked a refused mine banked no effort. There is
// no refusal to bank against now.
static void test_looking_away_discards_banked_effort(void) {
  generate(88);
  const int x = W / 2 + 10, y = H / 2;
  while (height(x, y) < 20) TEST_ASSERT_TRUE(place(x, y, B_STONE));

  uint8_t m, b;
  TEST_ASSERT_EQUAL_UINT8(MINE_PROGRESS, mine(x, y, 8, 16, m, b));
  TEST_ASSERT_TRUE(damage(x, y, 8) > 0);

  // One swing at a different block, and the first one's progress is gone.
  TEST_ASSERT_EQUAL_UINT8(MINE_PROGRESS, mine(x, y, 10, 16, m, b));
  TEST_ASSERT_EQUAL_UINT8(0, damage(x, y, 8));
}

// The marker pool is finite and generate() runs many times over a session. A
// marker that leaks is a canopy that stops appearing several worlds later.
static void test_the_marker_pool_does_not_leak_across_generates(void) {
  generate(3);
  const int first = marksFree();
  for (int i = 0; i < 20; ++i) generate(3);
  TEST_ASSERT_EQUAL_INT(first, marksFree());
  // And a different world hands everything back too.
  generate(9);
  generate(3);
  TEST_ASSERT_EQUAL_INT(first, marksFree());
}

// Mining must not draw from the marker pool at all.
//
// This is the property that makes the pool safe to have: taking a block out
// cannot change what anything is made of, so the edit the player performs
// thousands of times a session allocates nothing. Digging a column to pieces
// should hand markers BACK, never take them.
static void test_mining_never_consumes_markers(void) {
  generate(88);
  const int x = W / 2 + 10, y = H / 2;
  while (height(x, y) < 24) TEST_ASSERT_TRUE(place(x, y, B_STONE));
  const int before = marksFree();

  uint8_t m, b;
  for (int z = 2; z <= 20; z += 2) {
    MineResult r = MINE_PROGRESS;
    for (int i = 0; i < 900 && r != MINE_BROKE; ++i) r = mine(x, y, z, 4096, m, b);
    TEST_ASSERT_EQUAL_UINT8(MINE_BROKE, r);
    TEST_ASSERT_TRUE(marksFree() >= before);
  }
}


// ---- standing on runs -------------------------------------------------------

// The point of runs, from the player's side: a floor you build is somewhere you
// can stand, not scenery you walk through the middle of.
static void test_you_can_stand_on_a_floor_you_built(void) {
  generate(4242);
  const int x = W / 2, y = H / 2;
  const int deck = (int)height(x, y) + 3;
  TEST_ASSERT_EQUAL_UINT8(PLACE_OK, place(x, y, deck, B_PLANK));

  // A body already at deck height rests on top of it.
  TEST_ASSERT_EQUAL_UINT8(deck + 1, surfaceUnder(x, y, deck));
  // ...but one on the ground stays on the ground. Three blocks up is not a
  // step, and being able to walk onto it from below would be a teleport.
  TEST_ASSERT_EQUAL_UINT8(height(x, y), surfaceUnder(x, y, (int)height(x, y)));
}

// The core loop of the game is walling yourself in at night, and it rests
// entirely on this: STEP_UP is one, so two is unclimbable.
static void test_a_two_high_wall_is_still_unclimbable(void) {
  generate(4242);
  const int x = W / 2 + 2, y = H / 2;
  const int g = (int)height(x, y);
  TEST_ASSERT_TRUE(place(x, y, B_STONE));
  TEST_ASSERT_TRUE(canEnter(g, x, y));         // one block is a step
  TEST_ASSERT_TRUE(place(x, y, B_STONE));
  TEST_ASSERT_FALSE(canEnter(g, x, y));        // two is a wall
}

// And the other half of the same rule: a staircase is climbable, which is what
// makes building one worth doing.
static void test_a_staircase_can_be_climbed(void) {
  generate(4242);
  const int x0 = W / 2, y = H / 2;
  const int g = (int)height(x0, y);
  for (int i = 1; i <= 4; ++i)
    for (int k = 0; k < i; ++k) TEST_ASSERT_TRUE(place(x0 + i, y, B_PLANK));

  int z = g;
  for (int i = 1; i <= 4; ++i) {
    const uint8_t sfc = surfaceUnder(x0 + i, y, z);
    TEST_ASSERT_NOT_EQUAL(NO_SURFACE, sfc);
    TEST_ASSERT_EQUAL_UINT8(g + i, sfc);
    z = sfc;
  }
}

// A body with something resting on its head has nowhere to stand, however
// solid the floor under it looks.
static void test_no_surface_where_there_is_no_headroom(void) {
  generate(4242);
  const int x = W / 2 + 4, y = H / 2;
  const int g = (int)height(x, y);
  TEST_ASSERT_EQUAL_UINT8(g, surfaceUnder(x, y, g));
  devSlab(x, y, g + 1, g + 2, B_STONE);        // a ceiling one block up
  TEST_ASSERT_EQUAL_UINT8(NO_SURFACE, surfaceUnder(x, y, g));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_generate_is_deterministic);
  RUN_TEST(test_border_is_full_height_bedrock);
  RUN_TEST(test_spawn_pad_is_clear);
  RUN_TEST(test_terrain_never_starts_below_ground);
  RUN_TEST(test_tree_is_wood_under_leaves);
  RUN_TEST(test_chopping_a_tree_actually_yields_wood);
  RUN_TEST(test_a_tree_canopy_hangs_over_walkable_ground);
  RUN_TEST(test_felling_a_tree_leaves_its_canopy_standing);
  RUN_TEST(test_a_house_has_posts_walls_and_a_roof);
  RUN_TEST(test_a_house_door_admits_and_its_windows_do_not);
  RUN_TEST(test_ore_is_below_the_dirt_band);
  RUN_TEST(test_the_generator_places_no_light);
  RUN_TEST(test_diamond_is_deep_and_rarer_than_iron);
  RUN_TEST(test_diamond_cannot_reach_the_surface_field);
  RUN_TEST(test_above_column_is_base_plane);
  RUN_TEST(test_mine_takes_exactly_one_block);
  RUN_TEST(test_mining_reveals_what_is_underneath);
  RUN_TEST(test_digging_down_reaches_stone_and_ore);
  RUN_TEST(test_the_metals_yield_more_than_a_single_lump);
  RUN_TEST(test_a_built_column_mines_back_as_what_was_built);
  RUN_TEST(test_mining_effort_resets_on_new_target);
  RUN_TEST(test_can_dig_below_ground_level);
  RUN_TEST(test_border_is_unbreakable);
  RUN_TEST(test_place_stacks_and_caps);
  RUN_TEST(test_step_up_limit);
  RUN_TEST(test_fits_checks_every_overlapped_cell);
  RUN_TEST(test_mining_mid_column_splits_it);
  RUN_TEST(test_a_split_run_keeps_its_soil_profile);
  RUN_TEST(test_a_block_can_be_placed_in_mid_air);
  RUN_TEST(test_filling_a_tunnel_merges_the_run_back_in);
  RUN_TEST(test_a_column_can_be_split_without_limit);
  RUN_TEST(test_looking_away_discards_banked_effort);
  RUN_TEST(test_the_marker_pool_does_not_leak_across_generates);
  RUN_TEST(test_mining_never_consumes_markers);
  RUN_TEST(test_explode_is_deepest_at_the_centre);
  RUN_TEST(test_all_biomes_appear);
  RUN_TEST(test_biome_sets_the_surface_material);
  RUN_TEST(test_light_follows_its_source);
  RUN_TEST(test_lava_glows_and_is_unbreakable);
  RUN_TEST(test_cannot_build_into_a_slab);
  RUN_TEST(test_standable_reports_headroom);
  RUN_TEST(test_you_can_stand_on_a_floor_you_built);
  RUN_TEST(test_a_two_high_wall_is_still_unclimbable);
  RUN_TEST(test_a_staircase_can_be_climbed);
  RUN_TEST(test_no_surface_where_there_is_no_headroom);
  RUN_TEST(test_degenerate_input);
  return UNITY_END();
}
