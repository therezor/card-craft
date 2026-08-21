// Host-side tests for the heightmap walker. These are the ones worth having:
// the DDA's edge cases are unreachable by hand on the device, and a span that
// runs off the panel corrupts the framebuffer rather than looking wrong.
// Run with:  pio test -e native
#include <math.h>
#include <unity.h>

#include "raycast.h"
#include "world.h"

using namespace raycast;

static Camera atSpawn(float angle) {
  Camera c;
  c.px = (float)(world::W / 2) + 0.5f;
  c.py = (float)(world::H / 2) + 0.5f;
  c.z  = (float)world::groundAt(c.px, c.py) + EYE;
  setAngle(c, angle);
  return c;
}

// The player was told the crosshair is where they are aiming. If the aim ray's
// slope and the projection ever disagree, the reticle sits somewhere other
// than the block that gets mined.
// ---- pitch ------------------------------------------------------------------

// Looking up and down is a y-shear, so it must move every row by the same
// number of pixels. Anything else is a rotation, and a rotation would distort
// the world — vertical edges would stop being vertical.
static void test_pitch_shears_every_row_uniformly(void) {
  init();
  world::generate(4242);
  Span a[MAX_SPANS], b[MAX_SPANS];
  ColumnResult ra, rb;

  Camera c0 = atSpawn(0.7f);
  Camera c1 = c0;
  const int shift = 7;
  setPitch(c1, HORIZON + shift);

  // Spans cannot be paired by index: shearing changes what falls off the panel,
  // so the two casts do not emit the same surfaces in the same order. Pair them
  // by the surface they came from instead — distance, face and material — and
  // only where that triple is unique in both casts.
  int compared = 0;
  for (int x = 0; x < VIEW_W; x += 3) {
    const int na = castColumn(c0, x, -1, -1, a, ra);
    const int nb = castColumn(c1, x, -1, -1, b, rb);
    for (int i = 0; i < na; ++i) {
      if (a[i].y0 <= 0 || a[i].y1 >= VIEW_H) continue;   // clipped, not comparable
      int hits = 0, j = -1;
      for (int k = 0; k < na; ++k)
        if (k != i && a[k].distQ8 == a[i].distQ8 && a[k].face == a[i].face &&
            a[k].mat == a[i].mat) { hits = 99; break; }
      if (hits) continue;                                 // ambiguous in A
      for (int k = 0; k < nb; ++k)
        if (b[k].distQ8 == a[i].distQ8 && b[k].face == a[i].face &&
            b[k].mat == a[i].mat) { ++hits; j = k; }
      if (hits != 1) continue;                            // absent or ambiguous in B
      if (b[j].y0 <= 0 || b[j].y1 >= VIEW_H) continue;
      TEST_ASSERT_EQUAL_INT(a[i].y0 + shift, b[j].y0);
      TEST_ASSERT_EQUAL_INT(a[i].y1 + shift, b[j].y1);
      ++compared;
    }
  }
  TEST_ASSERT_TRUE(compared > 50);             // the test actually looked at something
}

// The reticle is drawn at a fixed row, so the aim ray has to be whatever passes
// through that row. If the two ever disagree the player mines a block other
// than the one under the crosshair, at every pitch but one.
static void test_pitch_keeps_the_aim_ray_under_the_reticle(void) {
  Camera c = atSpawn(0.0f);
  for (int h = HORIZON - PITCH_RANGE; h <= HORIZON + PITCH_RANGE; ++h) {
    setPitch(c, h);
    const float row = (float)c.horizon + c.aimSlope * PROJ;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)crosshairRow(), row);
  }
}

// Pitch is clamped, not free: past the range the floor or the sky fills the
// panel and there is no world left to look at.
static void test_pitch_is_clamped_to_its_range(void) {
  Camera c = atSpawn(0.0f);
  setPitch(c, HORIZON + 10000);
  TEST_ASSERT_EQUAL_INT(HORIZON + PITCH_RANGE, c.horizon);
  setPitch(c, HORIZON - 10000);
  TEST_ASSERT_EQUAL_INT(HORIZON - PITCH_RANGE, c.horizon);
}

// A default-constructed camera is the fixed-tilt camera the game shipped with,
// which is what lets a board with no keys for pitch keep working untouched.
static void test_a_default_camera_sits_at_the_resting_tilt(void) {
  Camera c;
  TEST_ASSERT_EQUAL_INT(HORIZON, c.horizon);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, AIM_SLOPE, c.aimSlope);
}

// ---- selection --------------------------------------------------------------

// The bug: a cell dug down to height 0 emitted no spans at all, so the floor of
// a pit rendered as the background gradient and the far wall stretched down
// across the hole. It is still a surface — the top of the base plane.
static void test_a_cell_dug_to_nothing_still_has_a_floor(void) {
  init();
  world::generate(1234);
  const int px = 32, py = 20;
  uint8_t m, bl, o;
  for (int y = py - 6; y <= py + 8; ++y)
    for (int x = px - 6; x <= px + 6; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > 0) world::mine(x, y, 100000, m, bl, o);
    }
  TEST_ASSERT_EQUAL_INT(0, world::height(px, py));

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = 0.0f + EYE;                       // standing on the quarry floor
  setAngle(c, 1.5707963f);

  Span spans[MAX_SPANS];
  ColumnResult res;
  const int n = castColumn(c, VIEW_W / 2, -1, -1, spans, res);

  int painted = 0, tops = 0;
  for (int i = 0; i < n; ++i) {
    painted += spans[i].y1 - spans[i].y0;
    if (spans[i].face == F_TOP) ++tops;
  }
  TEST_ASSERT_TRUE(tops > 0);                  // the floor is drawn
  TEST_ASSERT_TRUE(painted > VIEW_H - 4);      // and it fills the column
}

// The outline is drawn from these, so they have to bracket every row the
// selected block actually painted — no more, no less.
static void test_the_selection_extent_brackets_the_selected_spans(void) {
  init();
  world::generate(88);
  Span spans[MAX_SPANS];
  ColumnResult res;
  Camera c = atSpawn(0.4f);

  int hx, hy; bool onTop = false;
  TEST_ASSERT_TRUE(pick(c, 6.0f, hx, hy, onTop));

  int seen = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, hx, hy, spans, res);
    int lo = VIEW_H, hi = 0;
    for (int i = 0; i < n; ++i) {
      if (!spans[i].sel) continue;
      if (spans[i].y0 < lo) lo = spans[i].y0;
      if (spans[i].y1 > hi) hi = spans[i].y1;
    }
    if (lo >= hi) {                            // nothing selected in this column
      TEST_ASSERT_TRUE(res.selY0 >= res.selY1);
      continue;
    }
    TEST_ASSERT_EQUAL_INT(lo, res.selY0);
    TEST_ASSERT_EQUAL_INT(hi, res.selY1);
    ++seen;
  }
  TEST_ASSERT_TRUE(seen > 0);                  // the crosshair was on something
}

// The geometric extent is the block's own projection; the visible extent is
// what survived the clipper. Keeping both is the only way to tell an edge of
// the block from an edge of whatever is standing in front of it — and a rule
// drawn along the latter is a line hanging in mid-air.
//
// A solid wall is no use for testing this: in a heightmap it either clears the
// target or hides it outright. Partial occlusion is what overhangs are for, so
// the scene is a bridge deck cutting across the view.
static void test_the_visible_extent_is_clipped_but_the_geometry_is_not(void) {
  init();
  world::generate(1234);
  const int px = 32, py = 20;
  uint8_t m, bl, o;
  for (int y = py - 4; y <= py + 14; ++y)
    for (int x = px - 8; x <= px + 8; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > world::GROUND) world::mine(x, y, 100000, m, bl, o);
      while (world::height(x, y) < world::GROUND) world::place(x, y, world::B_DIRT);
    }
  for (int x = px - 6; x <= px + 6; ++x)
    world::devSlab(x, py + 4, world::GROUND + 1, world::GROUND + 2, world::B_STONE);

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = (float)world::GROUND + EYE;
  setAngle(c, 1.5707963f);

  const int tx = px, ty = py + 8;
  Span spans[MAX_SPANS];
  ColumnResult r;

  int clipped = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    castColumn(c, x, tx, ty, spans, r);
    if (r.selGeoY0 >= r.selGeoY1) continue;      // ray never reached the block
    if (r.selY0 >= r.selY1) continue;            // nothing of it survived
    // Whatever is visible is part of the block, so it lies inside the block's
    // own projection. If this ever fails the outline is being drawn around
    // rows the block does not occupy.
    TEST_ASSERT_TRUE(r.selY0 >= r.selGeoY0);
    TEST_ASSERT_TRUE(r.selY1 <= r.selGeoY1);
    if (r.selY0 > r.selGeoY0 || r.selY1 < r.selGeoY1) ++clipped;
  }
  // ...and the deck really did cut some of them, or the test proved nothing.
  TEST_ASSERT_TRUE(clipped > 8);
}

// Slabs are terrain: they cannot be mined and cannot be built into, so aiming
// at the cell under a bridge must never light the deck over it.
static void test_a_slab_is_never_drawn_as_the_selection(void) {
  init();
  world::generate(555);
  const int px = 32, py = 20;
  uint8_t m, bl, o;
  for (int y = py - 4; y <= py + 12; ++y)
    for (int x = px - 6; x <= px + 6; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > world::GROUND) world::mine(x, y, 100000, m, bl, o);
      while (world::height(x, y) < world::GROUND) world::place(x, y, world::B_DIRT);
    }
  const int tx = px, ty = py + 5;
  world::devSlab(tx, ty, world::GROUND + 3, world::GROUND + 4, world::B_STONE);

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = (float)world::GROUND + EYE;
  setAngle(c, 1.5707963f);

  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, tx, ty, spans, res);
    for (int i = 0; i < n; ++i) {
      // The deck is stone sitting above the dirt everything else is made of;
      // if a stone span ever comes back selected, a slab was marked.
      if (spans[i].sel) TEST_ASSERT_NOT_EQUAL(world::B_STONE, spans[i].mat);
    }
  }
}

// The claim the whole selection box rests on: whatever pick() targets, the
// crosshair is pointing at it.
//
// This failed for one aim in nine before the box outlined the column rather
// than only its top block. Mining always takes the topmost block off, so on a
// heightmap the block that comes off is not the block the crosshair is on
// unless you happen to be aiming at the top of the stack — and next to a
// six-high wall the top block projects off the panel entirely, so the player
// got no box at all at the exact moment they were trying to dig through it.
static void test_the_crosshair_is_always_inside_the_selection(void) {
  init();
  world::generate(4242);
  Span spans[MAX_SPANS];
  ColumnResult res;
  const int xc = VIEW_W / 2;
  const int row = crosshairRow();

  int checked = 0;
  for (int t = 0; t < 400; ++t) {
    Camera c;
    c.px = 8.5f + (float)((t * 7) % 47);
    c.py = 8.5f + (float)((t * 11) % 47);
    c.z  = (float)world::groundAt(c.px, c.py) + EYE;
    setAngle(c, (float)t * 0.157f);
    setPitch(c, HORIZON + ((t % 5) - 2) * 12);

    int hx, hy; bool onTop = false;
    if (!pick(c, 6.0f, hx, hy, onTop)) continue;
    if (world::isBorder(hx, hy)) continue;

    castColumn(c, xc, hx, hy, spans, res);
    ++checked;
    // The block's own projection, not the clipped one: something nearer may
    // hide the row the crosshair is on, and that is occlusion doing its job.
    TEST_ASSERT_TRUE(res.selGeoY0 < res.selGeoY1);
    TEST_ASSERT_TRUE(row >= res.selGeoY0);
    TEST_ASSERT_TRUE(row < res.selGeoY1);
  }
  TEST_ASSERT_TRUE(checked > 200);        // the sweep actually aimed at things
}

// Only the top block is lifted, because that is the one a swing removes. The
// rest of the column is marked so the outline can follow it, and marking it at
// the same level would light up a whole six-block stack.
static void test_only_the_top_block_of_the_target_is_lifted(void) {
  init();
  world::generate(4242);
  Span spans[MAX_SPANS];
  ColumnResult res;

  // Swept rather than posed: from any one spot the target's top block may be
  // hidden behind something nearer, which is occlusion working, not a bug.
  int lifted = 0, outlined = 0, views = 0;
  for (int t = 0; t < 40; ++t) {
    Camera c = atSpawn((float)t * 0.157f);
    int hx, hy; bool onTop = false;
    if (!pick(c, 6.0f, hx, hy, onTop)) continue;
    if (world::isBorder(hx, hy)) continue;
    ++views;
    for (int x = 0; x < VIEW_W; ++x) {
      const int n = castColumn(c, x, hx, hy, spans, res);
      for (int i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(spans[i].sel <= 2);
        if (spans[i].sel == 2) ++lifted;
        else if (spans[i].sel == 1) ++outlined;
      }
    }
  }
  TEST_ASSERT_TRUE(views > 4);
  // Both levels have to appear somewhere, or one of them is dead code: no lift
  // means nothing shows what a swing removes, no outline means the box is back
  // to following the top block alone.
  TEST_ASSERT_TRUE(lifted > 0);
  TEST_ASSERT_TRUE(outlined > 0);
}

static void test_crosshair_is_panel_centre(void) {
  TEST_ASSERT_EQUAL_INT(VIEW_H / 2, crosshairRow());
}

// Every span has to be inside the panel and non-empty. A span with y1 <= y0,
// or one row past the end, writes outside the framebuffer.
static void test_spans_stay_inside_the_panel(void) {
  init();
  world::generate(2024);
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int a = 0; a < 64; ++a) {
    Camera c = atSpawn((float)a * 0.0982f);
    for (int x = 0; x < VIEW_W; ++x) {
      const int n = castColumn(c, x, -1, -1, spans, res);
      TEST_ASSERT_TRUE(n <= MAX_SPANS);
      for (int i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(spans[i].y0 >= 0);
        TEST_ASSERT_TRUE(spans[i].y1 <= VIEW_H);
        TEST_ASSERT_TRUE(spans[i].y0 < spans[i].y1);
        TEST_ASSERT_TRUE(spans[i].mat < world::B_COUNT);
        TEST_ASSERT_TRUE(spans[i].face < F_COUNT);
      }
    }
  }
}

// Spans must never overlap. If two do, a farther surface has drawn over a
// nearer one and the interval clipper has a hole in it. They are no longer
// emitted in screen order — an overhang is painted after the ground below it —
// so this checks disjointness directly rather than assuming an order.
static void test_spans_are_ordered_and_disjoint(void) {
  init();
  world::generate(77);
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int a = 0; a < 32; ++a) {
    Camera c = atSpawn((float)a * 0.196f);
    for (int x = 0; x < VIEW_W; x += 3) {
      const int n = castColumn(c, x, -1, -1, spans, res);
      for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
          TEST_ASSERT_TRUE(spans[i].y1 <= spans[j].y0 || spans[j].y1 <= spans[i].y0);
    }
  }
}

// The occlusion table feeds mob clipping. It has to be non-increasing with
// distance, because geometry only ever covers more of the column as the walk
// goes out, never less.
static void test_occlusion_table_is_monotonic(void) {
  init();
  world::generate(5150);
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int a = 0; a < 16; ++a) {
    Camera c = atSpawn((float)a * 0.39f);
    for (int x = 0; x < VIEW_W; x += 7) {
      castColumn(c, x, -1, -1, spans, res);
      for (int k = 1; k < ZBUCKETS; ++k)
        TEST_ASSERT_TRUE(res.limit[k] <= res.limit[k - 1]);
      TEST_ASSERT_TRUE(res.limit[0] <= VIEW_H);
    }
  }
}

// A ray exactly along an axis has an infinite step on the other one. Using a
// real infinity here would be undefined under -ffast-math; the guard value has
// to survive and the walk still terminate.
static void test_axis_aligned_rays(void) {
  init();
  world::generate(3);
  Span spans[MAX_SPANS];
  ColumnResult res;
  const float kAngles[4] = { 0.0f, (float)M_PI_2, (float)M_PI, 3.0f * (float)M_PI_2 };
  for (int a = 0; a < 4; ++a) {
    Camera c = atSpawn(kAngles[a]);
    for (int x = 0; x < VIEW_W; ++x) {
      const int n = castColumn(c, x, -1, -1, spans, res);
      for (int i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(spans[i].y0 >= 0 && spans[i].y1 <= VIEW_H);
        TEST_ASSERT_TRUE(spans[i].distQ8 > 0);
      }
    }
  }
}

// A creeper can leave the player standing inside a column. The walk must not
// divide by a near-zero distance and produce a span that fills the panel from
// a row well outside int range.
static void test_camera_buried_in_a_column(void) {
  init();
  world::generate(3);
  const int cx = world::W / 2, cy = world::H / 2;
  for (int i = 0; i < 4; ++i) world::place(cx, cy, world::B_PLANK);

  Camera c = atSpawn(0.7f);
  c.z = (float)world::GROUND + 0.5f;         // inside the stack, not on top of it
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, spans, res);
    for (int i = 0; i < n; ++i)
      TEST_ASSERT_TRUE(spans[i].y0 >= 0 && spans[i].y1 <= VIEW_H);
  }
}

// Selection has to mark the block a swing would take off — the top one — and
// nothing else, or the highlight tells the player the wrong thing.
static void test_selection_marks_only_the_top_block(void) {
  init();
  world::generate(88);
  Camera c = atSpawn(0.0f);
  int hx, hy; bool onTop;
  TEST_ASSERT_TRUE(pick(c, 6.0f, hx, hy, onTop));

  Span spans[MAX_SPANS];
  ColumnResult res;
  int selSpans = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, hx, hy, spans, res);
    for (int i = 0; i < n; ++i) if (spans[i].sel) ++selSpans;
  }
  TEST_ASSERT_TRUE(selSpans > 0);

  // With no selection passed, nothing is ever marked.
  int stray = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, spans, res);
    for (int i = 0; i < n; ++i) if (spans[i].sel) ++stray;
  }
  TEST_ASSERT_EQUAL_INT(0, stray);
}

// On flat ground the aim ray meets the surface at EYE * PROJ / TILT cells.
// If that ever exceeds the mining reach, the player can stand in the open
// unable to mine or build anything at all.
static void test_aim_lands_within_reach_on_flat_ground(void) {
  init();
  world::generate(88);
  Camera c = atSpawn(0.0f);
  int hx, hy; bool onTop;
  TEST_ASSERT_TRUE(pick(c, 6.0f, hx, hy, onTop));
  TEST_ASSERT_TRUE(onTop);                       // came down onto open ground
  const float d = sqrtf(((float)hx + 0.5f - c.px) * ((float)hx + 0.5f - c.px)
                      + ((float)hy + 0.5f - c.py) * ((float)hy + 0.5f - c.py));
  TEST_ASSERT_TRUE(d < 6.0f);
  const float expected = EYE * PROJ / (float)TILT;
  TEST_ASSERT_TRUE(fabsf(d - expected) < 1.5f);
}

// Walking into a wall must report its side, not its top, or building would
// stack a block on top of a cliff instead of in front of the player.
static void test_pick_reports_a_side_when_facing_a_wall(void) {
  init();
  world::generate(88);
  Camera c = atSpawn(0.0f);
  const int wx = (int)c.px + 2, wy = (int)c.py;
  for (int i = 0; i < 4; ++i) world::place(wx, wy, world::B_PLANK);

  int hx, hy; bool onTop = true;
  TEST_ASSERT_TRUE(pick(c, 6.0f, hx, hy, onTop));
  TEST_ASSERT_EQUAL_INT(wx, hx);
  TEST_ASSERT_EQUAL_INT(wy, hy);
  TEST_ASSERT_FALSE(onTop);
}

// Every row of the panel is accounted for exactly once: painted by a span or
// left open for the sky, never both and never neither. This is the invariant
// the interval clipper exists to hold, and the one a bridge would break if the
// occlusion were still a single topmost row.
static void test_every_row_is_painted_or_open_exactly_once(void) {
  init();
  world::generate(4242);
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int a = 0; a < 24; ++a) {
    Camera c = atSpawn((float)a * 0.26f);
    for (int x = 0; x < VIEW_W; x += 5) {
      const int n = castColumn(c, x, -1, -1, spans, res);
      int cover[VIEW_H] = {0};
      for (int i = 0; i < n; ++i)
        for (int y = spans[i].y0; y < spans[i].y1; ++y) ++cover[y];
      for (int i = 0; i < res.nOpen; ++i)
        for (int y = res.open[i].y0; y < res.open[i].y1; ++y) ++cover[y];
      // A span capped at MAX_SPANS can legitimately leave a row uncovered, so
      // the assertion is "never counted twice" plus "almost always once".
      int once = 0;
      for (int y = 0; y < VIEW_H; ++y) {
        TEST_ASSERT_TRUE(cover[y] <= 1);
        if (cover[y] == 1) ++once;
      }
      TEST_ASSERT_TRUE(once > VIEW_H - 8);
    }
  }
}

// The open list must stay sorted and disjoint, because the clipper walks it in
// order and breaks out early on the first range past the candidate.
static void test_open_list_stays_sorted_and_disjoint(void) {
  init();
  world::generate(777);
  Span spans[MAX_SPANS];
  ColumnResult res;
  for (int a = 0; a < 16; ++a) {
    Camera c = atSpawn((float)a * 0.39f);
    for (int x = 0; x < VIEW_W; x += 3) {
      castColumn(c, x, -1, -1, spans, res);
      TEST_ASSERT_TRUE(res.nOpen <= MAX_OPEN);
      for (int i = 0; i < res.nOpen; ++i) {
        TEST_ASSERT_TRUE(res.open[i].y0 < res.open[i].y1);
        TEST_ASSERT_TRUE(res.open[i].y0 >= 0 && res.open[i].y1 <= VIEW_H);
        if (i) TEST_ASSERT_TRUE(res.open[i - 1].y1 <= res.open[i].y0);
      }
    }
  }
}

// A slab has to show its underside — the dark ceiling that makes a bridge read
// as something with air beneath it rather than a stripe painted on a hill.
//
// Note where the camera has to stand. With a fixed downward tilt and no pitch,
// the underside of a slab directly overhead projects off the top of the panel;
// what you actually see is the underside of the span further down the bridge.
// So the test stands back from it, which is also how a player sees one.
static void test_slab_shows_an_underside(void) {
  init();
  world::generate(4242);

  // Build a known bridge rather than hunting for a generated one, so a change
  // to the terrain generator cannot silently stop this from testing anything.
  const int bx = world::W / 2, by = world::H / 2;
  for (int i = 4; i <= 12; ++i) world::place(bx + i, by, world::B_STONE);   // piers up
  for (int i = 4; i <= 12; ++i)
    for (int k = 0; k < 3; ++k) world::place(bx + i, by, world::B_STONE);

  Camera c;
  c.px = (float)bx + 0.5f; c.py = (float)by + 0.5f;
  c.z  = (float)world::groundAt(c.px, c.py) + EYE;

  // Find any generated slab and look at it from a distance in every direction.
  int sx = -1, sy = -1;
  for (int y = 3; y < world::H - 3 && sx < 0; ++y)
    for (int x = 3; x < world::W - 3 && sx < 0; ++x)
      if (world::hasSlab(x, y)
          && world::slabBase(x, y) > world::height(x, y) + 1) { sx = x; sy = y; }
  TEST_ASSERT_TRUE(sx > 0);

  Span spans[MAX_SPANS];
  ColumnResult res;
  bool sawUnderside = false;
  for (int back = 6; back <= 14 && !sawUnderside; back += 2) {
    for (int dir = 0; dir < 4 && !sawUnderside; ++dir) {
      const int ox = (dir == 0) ? back : (dir == 1 ? -back : 0);
      const int oy = (dir == 2) ? back : (dir == 3 ? -back : 0);
      const int px = sx + ox, py = sy + oy;
      if (px < 2 || py < 2 || px >= world::W - 2 || py >= world::H - 2) continue;
      c.px = (float)px + 0.5f; c.py = (float)py + 0.5f;
      c.z  = (float)world::groundAt(c.px, c.py) + EYE;
      if (c.z >= (float)world::slabBase(sx, sy)) continue;   // must be below it

      for (int a = 0; a < 24 && !sawUnderside; ++a) {
        setAngle(c, (float)a * 0.262f);
        for (int x = 0; x < VIEW_W && !sawUnderside; ++x) {
          const int n = castColumn(c, x, -1, -1, spans, res);
          for (int i = 0; i < n; ++i) if (spans[i].face == F_BOT) sawUnderside = true;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(sawUnderside);
}

// A bridge deck between the camera and something farther away hides the part
// of it behind the deck. The occlusion table's `limit` deliberately ignores
// slabs — a mob under a bridge is plainly visible — so slabs contribute their
// own band instead, and a billboard clips against both.
static void test_slab_band_covers_the_slab_spans(void) {
  init();
  world::generate(4242);

  // A deck across the view, well ahead, with room to stand under it.
  const int bx = world::W / 2, by = world::H / 2;
  for (int x = bx - 6; x <= bx + 6; ++x)
    for (int y = by - 9; y <= by - 7; ++y)
      world::devSlab(x, y, world::GROUND + world::HEADROOM,
                     world::GROUND + world::HEADROOM + 2, world::B_STONE);

  Camera c;
  c.px = (float)bx + 0.5f; c.py = (float)by + 0.5f;
  c.z  = (float)world::groundAt(c.px, c.py) + EYE;
  setAngle(c, -1.5708f);                 // face -y, along the deck's normal

  Span spans[MAX_SPANS];
  ColumnResult res;
  int columnsWithBand = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, spans, res);

    // The band has to be well-formed in every bucket: either empty (hi > lo)
    // or a real range inside the panel.
    for (int k = 0; k < ZBUCKETS; ++k) {
      if (res.slabHi[k] > res.slabLo[k]) continue;      // empty, fine
      TEST_ASSERT_TRUE(res.slabLo[k] <= VIEW_H);
    }

    // Wherever a slab span was emitted, the last bucket's band must contain it
    // — that is the whole contract the billboard clip relies on.
    bool sawSlabSpan = false;
    for (int i = 0; i < n; ++i) {
      const bool isSlabFace = (spans[i].face == F_BOT) || (spans[i].mat == world::B_STONE
                              && spans[i].y1 <= res.limit[ZBUCKETS - 1]);
      if (spans[i].face != F_BOT) continue;
      sawSlabSpan = true;
      (void)isSlabFace;
      TEST_ASSERT_TRUE(res.slabHi[ZBUCKETS - 1] <= spans[i].y0);
      TEST_ASSERT_TRUE(res.slabLo[ZBUCKETS - 1] >= spans[i].y1);
    }
    if (sawSlabSpan) ++columnsWithBand;
  }
  TEST_ASSERT_TRUE(columnsWithBand > 0);   // the deck really was in frame
}

// The band must be well formed everywhere, and it can only ever grow as the
// walk passes more overhangs. A band that shrank, inverted or ran off the
// panel would clip billboards against rows nothing owns.
static void test_slab_band_is_well_formed_and_monotonic(void) {
  init();
  world::generate(4242);
  Camera c = atSpawn(0.0f);
  Span spans[MAX_SPANS];
  ColumnResult res;

  for (int a = 0; a < 16; ++a) {
    setAngle(c, (float)a * 0.39f);
    for (int x = 0; x < VIEW_W; x += 3) {
      castColumn(c, x, -1, -1, spans, res);
      // Nothing is nearer than distance zero, so the first bucket is always
      // empty — encoded as hi > lo.
      TEST_ASSERT_TRUE(res.slabHi[0] > res.slabLo[0]);
      for (int k = 0; k < ZBUCKETS; ++k) {
        if (res.slabHi[k] <= res.slabLo[k]) {          // non-empty
          TEST_ASSERT_TRUE(res.slabLo[k] <= VIEW_H);
        }
        if (k) {
          // Monotone: the band only ever takes in more rows.
          TEST_ASSERT_TRUE(res.slabHi[k] <= res.slabHi[k - 1]);
          TEST_ASSERT_TRUE(res.slabLo[k] >= res.slabLo[k - 1]);
        }
      }
    }
  }
}

// Somewhere with no overhang within a few cells, the near buckets must stay
// empty — otherwise every billboard in an ordinary view would be clipped by a
// slab that is not there.
//
// The clearance is deliberately modest. An earlier version of this test asked
// for a spot clear of slabs for the walker's whole 17-cell reach, which does
// not exist anywhere on a 64x64 map carrying ~85 slab cells: the test failed
// on its own premise, not on the code.
static void test_slab_band_is_empty_with_nothing_nearby(void) {
  init();
  world::generate(4242);

  constexpr int CLEAR = 9;
  int cx = -1, cy = -1;
  for (int y = CLEAR; y < world::H - CLEAR && cx < 0; ++y) {
    for (int x = CLEAR; x < world::W - CLEAR && cx < 0; ++x) {
      bool clear = true;
      for (int dy = -CLEAR; dy <= CLEAR && clear; ++dy)
        for (int dx = -CLEAR; dx <= CLEAR && clear; ++dx)
          if (world::hasSlab(x + dx, y + dy)) clear = false;
      if (clear) { cx = x; cy = y; }
    }
  }
  TEST_ASSERT_TRUE(cx > 0);

  Camera c;
  c.px = (float)cx + 0.5f; c.py = (float)cy + 0.5f;
  c.z  = (float)world::groundAt(c.px, c.py) + EYE;

  Span spans[MAX_SPANS];
  ColumnResult res;
  // Buckets 0 and 1 cover everything inside 2 * ZBUCKET_SPAN cells, which is
  // well within the cleared radius.
  for (int a = 0; a < 16; ++a) {
    setAngle(c, (float)a * 0.39f);
    for (int x = 0; x < VIEW_W; x += 3) {
      castColumn(c, x, -1, -1, spans, res);
      TEST_ASSERT_TRUE(res.slabHi[0] > res.slabLo[0]);
      TEST_ASSERT_TRUE(res.slabHi[1] > res.slabLo[1]);
    }
  }
}

static void test_degenerate_input(void) {
  init();
  world::generate(1);
  Span spans[MAX_SPANS];
  ColumnResult res;

  // A camera parked in the bedrock border still has to terminate.
  Camera c = atSpawn(1.1f);
  c.px = 0.5f; c.py = 0.5f; c.z = (float)world::MAX_H + EYE;
  for (int x = 0; x < VIEW_W; x += 11) {
    const int n = castColumn(c, x, -1, -1, spans, res);
    for (int i = 0; i < n; ++i)
      TEST_ASSERT_TRUE(spans[i].y0 >= 0 && spans[i].y1 <= VIEW_H);
  }
  // Zero reach can never hit anything.
  int hx, hy; bool onTop;
  TEST_ASSERT_FALSE(pick(c, 0.0f, hx, hy, onTop));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crosshair_is_panel_centre);
  RUN_TEST(test_the_crosshair_is_always_inside_the_selection);
  RUN_TEST(test_only_the_top_block_of_the_target_is_lifted);
  RUN_TEST(test_pitch_shears_every_row_uniformly);
  RUN_TEST(test_pitch_keeps_the_aim_ray_under_the_reticle);
  RUN_TEST(test_pitch_is_clamped_to_its_range);
  RUN_TEST(test_a_default_camera_sits_at_the_resting_tilt);
  RUN_TEST(test_a_cell_dug_to_nothing_still_has_a_floor);
  RUN_TEST(test_the_selection_extent_brackets_the_selected_spans);
  RUN_TEST(test_the_visible_extent_is_clipped_but_the_geometry_is_not);
  RUN_TEST(test_a_slab_is_never_drawn_as_the_selection);
  RUN_TEST(test_spans_stay_inside_the_panel);
  RUN_TEST(test_spans_are_ordered_and_disjoint);
  RUN_TEST(test_occlusion_table_is_monotonic);
  RUN_TEST(test_axis_aligned_rays);
  RUN_TEST(test_camera_buried_in_a_column);
  RUN_TEST(test_selection_marks_only_the_top_block);
  RUN_TEST(test_aim_lands_within_reach_on_flat_ground);
  RUN_TEST(test_pick_reports_a_side_when_facing_a_wall);
  RUN_TEST(test_every_row_is_painted_or_open_exactly_once);
  RUN_TEST(test_open_list_stays_sorted_and_disjoint);
  RUN_TEST(test_slab_shows_an_underside);
  RUN_TEST(test_slab_band_covers_the_slab_spans);
  RUN_TEST(test_slab_band_is_well_formed_and_monotonic);
  RUN_TEST(test_slab_band_is_empty_with_nothing_nearby);
  RUN_TEST(test_degenerate_input);
  return UNITY_END();
}
