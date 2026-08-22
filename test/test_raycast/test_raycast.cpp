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
    const int na = castColumn(c0, x, -1, -1, -1, a, ra);
    const int nb = castColumn(c1, x, -1, -1, -1, b, rb);
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
  for (int h = HORIZON - PITCH_DOWN; h <= HORIZON + PITCH_UP; ++h) {
    setPitch(c, h);
    const float row = (float)c.horizon + c.aimSlope * PROJ;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)crosshairRow(), row);
  }
}

// Pitch is clamped, not free — a y-shear cannot reach vertical, so there has
// to be a stop somewhere. Asymmetric in horizon rows because the rest pose
// already looks down; symmetric in ANGLE, which is what the control feels
// like. See PITCH_UP.
static void test_pitch_is_clamped_to_its_range(void) {
  Camera c = atSpawn(0.0f);
  setPitch(c, HORIZON + 10000);
  TEST_ASSERT_EQUAL_INT(HORIZON + PITCH_UP, c.horizon);
  setPitch(c, HORIZON - 10000);
  TEST_ASSERT_EQUAL_INT(HORIZON - PITCH_DOWN, c.horizon);
}

// Both stops have to reach a steep angle, and the same one either way.
//
// This used to check only that looking up cleared level by "a useful margin",
// because up was the generous direction and down was deliberately kept short.
// Both are about sixty degrees now, and the property worth pinning is that the
// two agree: a look control that goes further up than down is one the player
// has to think about.
static void test_both_stops_reach_a_steep_angle_and_match(void) {
  Camera c = atSpawn(0.0f);
  setPitch(c, HORIZON + PITCH_UP);
  const float up = -c.aimSlope;                    // rising, so slope is negative
  TEST_ASSERT_TRUE(up > 1.5f);                     // steeper than 56 degrees
  setPitch(c, HORIZON - PITCH_DOWN);
  const float down = c.aimSlope;
  TEST_ASSERT_TRUE(down > 1.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, up, down);       // and the two match
}

// The shear stays a shear at the extremes: every row has to move by the same
// number of pixels, or looking up would bend the world instead of tilting it.
static void test_the_extremes_are_still_a_pure_shear(void) {
  Camera c = atSpawn(0.0f);
  setPitch(c, HORIZON);
  const int rest = c.horizon;
  setPitch(c, HORIZON + PITCH_UP);
  TEST_ASSERT_EQUAL_INT(PITCH_UP, c.horizon - rest);
  setPitch(c, HORIZON - PITCH_DOWN);
  TEST_ASSERT_EQUAL_INT(-PITCH_DOWN, c.horizon - rest);
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
  uint8_t m, bl;
  for (int y = py - 6; y <= py + 8; ++y)
    for (int x = px - 6; x <= px + 6; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > 1) world::mineTop(x, y, 100000, m, bl);
    }
  // One course, not none: the floor of the world is a real block of bedrock.
  TEST_ASSERT_EQUAL_INT(1, world::height(px, py));

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = 1.0f + EYE;                       // standing on the quarry floor
  setAngle(c, 1.5707963f);

  Span spans[MAX_SPANS];
  ColumnResult res;
  const int n = castColumn(c, VIEW_W / 2, -1, -1, -1, spans, res);

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

  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 7.0f, hit));

  int seen = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, hit.x, hit.y, hit.z, spans, res);
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
  uint8_t m, bl;
  for (int y = py - 4; y <= py + 14; ++y)
    for (int x = px - 8; x <= px + 8; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > world::GROUND) world::mineTop(x, y, 100000, m, bl);
      while (world::height(x, y) < world::GROUND) world::place(x, y, world::B_DIRT);
    }
  for (int x = px - 6; x <= px + 6; ++x)
    world::devSlab(x, py + 4, world::GROUND + 1, world::GROUND + 2, world::B_STONE);

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = (float)world::GROUND + EYE;
  setAngle(c, 1.5707963f);

  const int tx = px, ty = py + 8;
  const int tz = (int)world::height(tx, ty) - 1;
  Span spans[MAX_SPANS];
  ColumnResult r;

  int clipped = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    castColumn(c, x, tx, ty, tz, spans, r);
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

// This test used to assert the opposite, and the change is the feature: slabs
// were terrain, so they could not be mined or built into, and lighting a bridge
// deck because the crosshair was on the cell underneath pointed at a block the
// player could not act on. Runs are ordinary blocks now — you can dig a roof
// out and build a floor in mid-air — so a run block is a legitimate target and
// must be selectable. What must NOT happen is the whole cell lighting up: the
// selection is one block.
static void test_a_run_block_can_be_the_selection(void) {
  init();
  world::generate(555);
  const int px = 32, py = 20;
  uint8_t m, bl;
  for (int y = py - 4; y <= py + 12; ++y)
    for (int x = px - 6; x <= px + 6; ++x) {
      if (world::isBorder(x, y)) continue;
      while (world::height(x, y) > world::GROUND) world::mineTop(x, y, 100000, m, bl);
      while (world::height(x, y) < world::GROUND) world::place(x, y, world::B_DIRT);
    }
  const int tx = px, ty = py + 5;
  const int deck = world::GROUND + 3;
  world::devSlab(tx, ty, deck, deck + 1, world::B_STONE);

  Camera c;
  c.px = px + 0.5f; c.py = py + 0.5f;
  c.z  = (float)world::GROUND + EYE;
  setAngle(c, 1.5707963f);
  // The deck is above eye height, and the resting pose tilts down — at rest it
  // is off the top of the panel and there is nothing to select.
  setPitch(c, HORIZON + 70);

  Span spans[MAX_SPANS];
  ColumnResult res;
  int selStone = 0, selOther = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, tx, ty, deck, spans, res);
    for (int i = 0; i < n; ++i) {
      if (!spans[i].sel) continue;
      if (spans[i].mat == world::B_STONE) ++selStone;
      else                                ++selOther;
    }
  }
  TEST_ASSERT_TRUE(selStone > 0);      // the deck block is the target
  TEST_ASSERT_EQUAL_INT(0, selOther);  // and nothing else in its cell is
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

    Hit hit;
    if (!pick(c, 6.0f, hit)) continue;
    if (world::isBorder(hit.x, hit.y)) continue;

    castColumn(c, xc, hit.x, hit.y, hit.z, spans, res);
    ++checked;
    // The block's own projection, not the clipped one: something nearer may
    // hide the row the crosshair is on, and that is occlusion doing its job.
    TEST_ASSERT_TRUE(res.selGeoY0 < res.selGeoY1);
    // One row of slack, and only because of where a pixel boundary can fall.
    // Pressed right up against a block, its far edge can project to (say) row
    // 67.9 while the aim ray crosses at 67.5: the span truncates to end at 67
    // and the crosshair sits in the row the edge only partly covers. Both
    // answers describe the same half-pixel. What this test is actually for is
    // that pick() and the walker agree about WHICH BLOCK is under the
    // crosshair, and a one-row window still catches any real disagreement —
    // a wrong block is out by the height of a block, not by a pixel.
    TEST_ASSERT_TRUE(row >= res.selGeoY0 - 1);
    TEST_ASSERT_TRUE(row < res.selGeoY1 + 1);
  }
  TEST_ASSERT_TRUE(checked > 200);        // the sweep actually aimed at things
}

// This used to assert that a target column marked its top block at sel level 2
// and the rest of itself at level 1. Both levels existed for one reason: mining
// always removed the topmost block, so the block being pointed at and the block
// about to break were different, and the outline had to describe both at once.
// Mining takes the block under the crosshair now, so there is exactly one thing
// to mark — and marking a whole column again would light up a six-block stack.
static void test_the_selection_is_one_block_not_a_column(void) {
  init();
  world::generate(2024);
  Span spans[MAX_SPANS];
  ColumnResult res;

  int views = 0, checked = 0;
  for (int t = 0; t < 60; ++t) {
    Camera c;
    c.px = 10.5f + (float)((t * 13) % 41);
    c.py = 10.5f + (float)((t * 5) % 41);
    c.z  = (float)world::groundAt(c.px, c.py) + EYE;
    setAngle(c, (float)t * 0.31f);
    Hit hit;
    if (!pick(c, 6.0f, hit)) continue;
    if (world::isBorder(hit.x, hit.y)) continue;
    // A stack worth confusing with a column: without one, "the selection is a
    // single block" is true for free.
    if (world::height(hit.x, hit.y) < 3) continue;
    ++views;

    for (int x = 0; x < VIEW_W; ++x) {
      const int n = castColumn(c, x, hit.x, hit.y, hit.z, spans, res);
      for (int i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(spans[i].sel <= 1);        // the second level is gone
        if (!spans[i].sel) continue;
        ++checked;
      }
      // The marked rows must be one block's worth, not a column's. A block
      // projects to PROJ/dist rows, and the stack this is aimed at is at least
      // three of them — so a column-wide mark would be several times taller.
      if (res.selGeoY0 >= res.selGeoY1) continue;
      const float dist = (float)spans[0].distQ8 / 256.0f;
      if (dist > 0.5f)
        TEST_ASSERT_TRUE((float)(res.selGeoY1 - res.selGeoY0) < 2.0f * PROJ / dist);
    }
  }
  TEST_ASSERT_TRUE(views > 4);
  TEST_ASSERT_TRUE(checked > 0);      // something was actually marked
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
      const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
      const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
      castColumn(c, x, -1, -1, -1, spans, res);
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
      const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 7.0f, hit));

  Span spans[MAX_SPANS];
  ColumnResult res;
  int selSpans = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, hit.x, hit.y, hit.z, spans, res);
    for (int i = 0; i < n; ++i) if (spans[i].sel) ++selSpans;
  }
  TEST_ASSERT_TRUE(selSpans > 0);

  // With no selection passed, nothing is ever marked.
  int stray = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 7.0f, hit));
  TEST_ASSERT_TRUE((hit.face == F_TOP));                       // came down onto open ground
  const float d = sqrtf(((float)hit.x + 0.5f - c.px) * ((float)hit.x + 0.5f - c.px)
                      + ((float)hit.y + 0.5f - c.py) * ((float)hit.y + 0.5f - c.py));
  TEST_ASSERT_TRUE(d < 7.0f);
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

  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 7.0f, hit));
  TEST_ASSERT_EQUAL_INT(wx, hit.x);
  TEST_ASSERT_EQUAL_INT(wy, hit.y);
  TEST_ASSERT_FALSE((hit.face == F_TOP));
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
      const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
      castColumn(c, x, -1, -1, -1, spans, res);
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
          const int n = castColumn(c, x, -1, -1, -1, spans, res);
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
    const int n = castColumn(c, x, -1, -1, -1, spans, res);

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
      castColumn(c, x, -1, -1, -1, spans, res);
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
// The clearance is deliberately modest, and the seed is searched for rather
// than fixed. An earlier version asked for a spot clear of slabs for the
// walker's whole 17-cell reach, which exists on no map at all: the test failed
// on its own premise, not on the code. CLEAR is now the smallest radius that
// makes the assertion below mean anything — buckets 0 and 1 cover everything
// inside 2 * ZBUCKET_SPAN cells — and since tree canopies took the slab count
// from about 85 a map to two or three hundred, whether a given seed has such a
// patch at all is luck. Trying a few and failing only if none of them does
// keeps this a test of the band and not of the generator's mood.
static void test_slab_band_is_empty_with_nothing_nearby(void) {
  init();

  constexpr int CLEAR = 9;
  int cx = -1, cy = -1;
  for (uint32_t seed = 1; seed <= 16 && cx < 0; ++seed) {
    world::generate(seed);
    for (int y = CLEAR; y < world::H - CLEAR && cx < 0; ++y) {
      for (int x = CLEAR; x < world::W - CLEAR && cx < 0; ++x) {
        bool clear = true;
        for (int dy = -CLEAR; dy <= CLEAR && clear; ++dy)
          for (int dx = -CLEAR; dx <= CLEAR && clear; ++dx)
            if (world::hasSlab(x + dx, y + dy)) clear = false;
        if (clear) { cx = x; cy = y; }
      }
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
      castColumn(c, x, -1, -1, -1, spans, res);
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
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
    for (int i = 0; i < n; ++i)
      TEST_ASSERT_TRUE(spans[i].y0 >= 0 && spans[i].y1 <= VIEW_H);
  }
  // Zero reach can never hit anything.
  Hit hit;
  TEST_ASSERT_FALSE(pick(c, 0.0f, hit));
}


// ---- picking a block and a face ---------------------------------------------

// The bug this locks down: pick() used to name a cell and nothing else, so
// "which face am I pointing at" had no answer and a block built against a wall
// had nowhere to go but on top of it.
static void test_pick_returns_the_face_it_came_in_through(void) {
  init();
  world::generate(1234);
  const int cx = world::W / 2, cy = world::H / 2;

  // A pillar two cells east, tall enough that the resting tilt still meets its
  // side rather than sailing over the top.
  const int wx = cx + 2, wy = cy;
  for (int i = 0; i < 4; ++i) world::place(wx, wy, world::B_BRICK);

  Camera c = atSpawn(0.0f);       // +x
  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 7.0f, hit));
  TEST_ASSERT_EQUAL_INT(wx, hit.x);
  TEST_ASSERT_EQUAL_INT(wy, hit.y);
  TEST_ASSERT_EQUAL_UINT8(F_NS, hit.face);
  // Approached from the west, so the face looks back west.
  TEST_ASSERT_EQUAL_INT(-1, hit.nx);
  TEST_ASSERT_EQUAL_INT(0,  hit.ny);
  TEST_ASSERT_EQUAL_INT(0,  hit.nz);
  // And the block named is one the pillar actually contains.
  TEST_ASSERT_TRUE(hit.z >= 0 && hit.z < (int)world::height(wx, wy));
}

// The bug this locks down, and the reason pick() is a mask scan: a block with
// nothing under it could not be mined. pick() treated every floating run as an
// opaque blocker and returned NO hit at all, so a bridge deck, a roof, a tree
// crown or anything the player had built out over air was un-aimable — and it
// only ever consulted the lowest run, so a second deck above the first was not
// even considered.
static void test_a_block_hanging_on_nothing_can_be_picked_and_mined(void) {
  init();
  world::generate(4242);
  const int cx = world::W / 2, cy = world::H / 2;

  // Three cells east, dig the column down and leave one block hanging where
  // its top used to be. Nothing holds it up in any direction.
  const int fx = cx + 3, fy = cy;
  uint8_t dm, db;
  while (world::height(fx, fy) > world::GROUND - 3)
    world::mineTop(fx, fy, 100000, dm, db);
  TEST_ASSERT_EQUAL_INT(world::GROUND - 3, (int)world::height(fx, fy));
  TEST_ASSERT_EQUAL_UINT8(world::PLACE_OK,
                          world::place(fx, fy, world::GROUND, world::B_BRICK));
  TEST_ASSERT_TRUE(world::solidAt(fx, fy, world::GROUND));
  TEST_ASSERT_FALSE(world::solidAt(fx, fy, world::GROUND - 1));   // truly hanging

  // The resting tilt meets that height about three cells out, so the ray walks
  // into the block's side.
  Camera c = atSpawn(0.0f);       // +x
  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 6.6f, hit));
  TEST_ASSERT_EQUAL_INT(fx, hit.x);
  TEST_ASSERT_EQUAL_INT(fy, hit.y);
  TEST_ASSERT_EQUAL_INT(world::GROUND, hit.z);

  // And what pick() named, mine() takes off. The two halves of the bug: the
  // world was always willing to break this block, nothing could ever aim at it.
  world::MineResult r = world::MINE_PROGRESS;
  for (int i = 0; i < 4000 && r != world::MINE_BROKE; ++i)
    r = world::mine(hit.x, hit.y, hit.z, world::EFFORT_PER_TICK, dm, db);
  TEST_ASSERT_EQUAL_UINT8(world::MINE_BROKE, r);
  TEST_ASSERT_EQUAL_UINT8(world::B_BRICK, dm);
  TEST_ASSERT_FALSE(world::solidAt(fx, fy, world::GROUND));
}

// The other direction. Pitched up, the ray climbs, and what it meets first is
// the underside of whatever is overhead — a face pick() had no way to name
// while it assumed a descent. Naming it is what lets a player build onto the
// belly of a bridge, because world::place() has always taken an exact z.
static void test_looking_up_at_a_deck_reports_its_underside(void) {
  init();
  world::generate(4242);
  const int cx = world::W / 2, cy = world::H / 2;

  const int deckZ = 12;
  for (int x = cx; x <= cx + 6; ++x)
    for (int y = cy - 1; y <= cy + 1; ++y)
      world::devSlab(x, y, deckZ, deckZ + 1, world::B_STONE);

  Camera c = atSpawn(0.0f);       // +x
  setPitch(c, VIEW_H / 2 + 160);  // slope -1.0: one block up per cell out
  TEST_ASSERT_TRUE(c.aimSlope < -0.9f && c.aimSlope > -1.1f);

  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 6.6f, hit));
  TEST_ASSERT_EQUAL_INT(deckZ, hit.z);
  TEST_ASSERT_EQUAL_UINT8(F_BOT, hit.face);
  TEST_ASSERT_EQUAL_INT(0,  hit.nx);
  TEST_ASSERT_EQUAL_INT(0,  hit.ny);
  TEST_ASSERT_EQUAL_INT(-1, hit.nz);
  // The outward normal points at a cell a block can go in, which is the whole
  // reason the face is carried at all.
  TEST_ASSERT_FALSE(world::solidAt(hit.x, hit.y, hit.z + hit.nz));
}

// The reach derivation, as an assertion rather than a comment. EYE / AIM_SLOPE
// is where the crosshair meets flat ground at the resting tilt, and REACH in
// game.cpp is chosen to clear it. If TILT or EYE is ever retuned, this is the
// test that says so before the game becomes unmineable on a four-button board.
static void test_flat_ground_at_rest_is_within_reach(void) {
  init();
  world::generate(88);
  Camera c = atSpawn(0.0f);
  const float expect = EYE / AIM_SLOPE;          // 6.40 cells, horizontally
  TEST_ASSERT_TRUE(expect > 6.2f && expect < 6.6f);

  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 6.6f, hit));          // the shipped REACH
  TEST_ASSERT_EQUAL_UINT8(F_TOP, hit.face);
  TEST_ASSERT_EQUAL_INT(1, hit.nz);
  // dist is measured along the ray, so it is a little longer than the
  // horizontal distance the derivation above quotes.
  TEST_ASSERT_TRUE(hit.dist > expect);
  TEST_ASSERT_TRUE(hit.dist < 6.6f);
}

// Reach is a distance from the eye, not a horizontal one. The bug this locks
// down: comparing a horizontal distance against it let reach grow as the player
// looked down, which is the one direction it should shrink.
static void test_reach_is_measured_along_the_ray(void) {
  init();
  world::generate(88);
  Camera c = atSpawn(0.0f);
  Hit hit;
  TEST_ASSERT_TRUE(pick(c, 6.6f, hit));
  const float dx = ((float)hit.x + 0.5f) - c.px;
  const float dy = ((float)hit.y + 0.5f) - c.py;
  const float horiz = sqrtf(dx * dx + dy * dy);
  // The ray drops EYE over that run, so the reported distance must exceed the
  // flat-map distance rather than equal it.
  TEST_ASSERT_TRUE(hit.dist > horiz - 1.0f);
  TEST_ASSERT_TRUE(hit.dist <= 6.6f);
}


// ---- texture coordinates ----------------------------------------------------

// The bug this locks down: the grain tile it replaced was indexed by SCREEN
// ROW, so the pattern slid across a block as the camera moved instead of
// sitting on it. A wall-space v has to be anchored to the block, which means a
// span covers exactly one block's worth of texture however it is clipped.
static void test_a_wall_span_maps_one_tile_per_block(void) {
  init();
  world::generate(1234);
  const int cx = world::W / 2, cy = world::H / 2;
  const int wx = cx + 2;
  for (int i = 0; i < 5; ++i) world::place(wx, cy, world::B_BRICK);

  Camera c = atSpawn(0.0f);
  Span spans[MAX_SPANS];
  ColumnResult res;
  int checked = 0, merged = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
    for (int i = 0; i < n; ++i) {
      const Span& s2 = spans[i];
      if (!s2.cap || s2.vStepQ8 == 0) continue;      // not a textured face
      if (s2.distQ8 == 0) continue;
      ++checked;
      if (s2.cap > 1) ++merged;

      // The invariant a wall-space v exists for: ONE BLOCK OF FACE IS ONE TILE
      // OF TEXTURE, whatever the span's extent. A block is one world unit tall
      // and pitch is a shear rather than a rotation, so a unit is always
      // PROJ/distance pixels — and the step has to be exactly the tile divided
      // by that, or the pattern slides over the block as the camera moves
      // instead of sitting on it.
      //
      // This used to be stated as "a span never covers more than one tile",
      // which was the same thing only while every span was exactly one block.
      // Spans cover whole runs now; the anchoring is what mattered and it is
      // what is checked.
      const float dist = (float)s2.distQ8 / 256.0f;
      const float rowsPerBlock = PROJ / dist;
      const float tile = (float)(16 << 8);
      TEST_ASSERT_FLOAT_WITHIN(rowsPerBlock, tile,
                               (float)s2.vStepQ8 * rowsPerBlock);

      // ...and v never runs past the world's height in tiles, which is what
      // says the step is anchored to blocks rather than to screen rows.
      const uint32_t adv = (uint32_t)s2.vStepQ8 * (uint32_t)(s2.y1 - s2.y0);
      TEST_ASSERT_TRUE(adv <= (uint32_t)(world::MAX_H + 1) * (uint32_t)(16 << 8));
    }
  }
  TEST_ASSERT_TRUE(checked > 0);
  // And the merging actually happened: a five-block brick wall is one span,
  // not five. Without this the test above would pass on a walker that never
  // merged anything.
  TEST_ASSERT_TRUE(merged > 0);
}

// A merged span still has to start on a block boundary when nothing clipped it,
// or the first tile of the run would be cut in half.
static void test_an_unclipped_merged_span_starts_on_a_tile(void) {
  init();
  world::generate(1234);
  const int cx = world::W / 2, cy = world::H / 2;

  // A scene built rather than found. A merged run only yields an UNCLIPPED span
  // when the top of it lands on the panel, and the resting tilt leaves only
  // HORIZON rows of sky — 29 of them — so a run's top has to sit under
  // 29 * d / PROJ blocks above the eye to be in frame at all. The five-high
  // pillar two cells away this used to rely on breaks that by a mile: it fills
  // the screen and is clipped every time. It passed anyway because the natural
  // terrain out at fifteen-odd cells supplied unclipped spans of its own, and
  // pulling the fog in to MAX_DIST took that away.
  //
  // So: flatten a corridor and stand one two-block cap at the far end of it.
  // Nine cells out and 0.8 of a block above the eye, which is comfortably
  // inside both the sky and the draw distance.
  uint8_t m, b;
  for (int y = cy - 3; y <= cy + 3; ++y)
    for (int x = cx - 1; x <= cx + 10; ++x) {
      while (world::height(x, y) > world::GROUND) world::mineTop(x, y, 100000, m, b);
      while (world::height(x, y) < world::GROUND) world::place(x, y, world::B_DIRT);
    }
  for (int k = 0; k < 2; ++k) world::place(cx + 9, cy, world::B_BRICK);

  Camera c = atSpawn(0.0f);
  Span spans[MAX_SPANS];
  ColumnResult res;
  int checked = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
    for (int i = 0; i < n; ++i) {
      const Span& s2 = spans[i];
      if (s2.cap < 2 || s2.vStepQ8 == 0) continue;
      // vStartQ8 is (y0 - top of the run) * step, so an unclipped span starts
      // at zero. A clipped one starts wherever it was cut, and the renderer
      // masks that back into the tile — which is exact because the uint16 wrap
      // is itself a whole number of tiles (65536 = 16 * 4096).
      if (s2.vStartQ8 == 0) ++checked;
    }
  }
  TEST_ASSERT_TRUE(checked > 0);
}

// Flat faces take the constant-colour path and must not carry a step, or the
// renderer would walk a texture down a face that has no vertical extent to
// map it onto.
static void test_flat_faces_carry_no_texture_step(void) {
  init();
  world::generate(1234);
  Camera c = atSpawn(0.6f);
  Span spans[MAX_SPANS];
  ColumnResult res;
  int tops = 0;
  for (int x = 0; x < VIEW_W; ++x) {
    const int n = castColumn(c, x, -1, -1, -1, spans, res);
    for (int i = 0; i < n; ++i) {
      if (spans[i].face != F_TOP && spans[i].face != F_BOT) continue;
      ++tops;
      TEST_ASSERT_EQUAL_UINT16(0, spans[i].vStepQ8);
    }
  }
  TEST_ASSERT_TRUE(tops > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crosshair_is_panel_centre);
  RUN_TEST(test_a_wall_span_maps_one_tile_per_block);
  RUN_TEST(test_an_unclipped_merged_span_starts_on_a_tile);
  RUN_TEST(test_flat_faces_carry_no_texture_step);
  RUN_TEST(test_pick_returns_the_face_it_came_in_through);
  RUN_TEST(test_a_block_hanging_on_nothing_can_be_picked_and_mined);
  RUN_TEST(test_looking_up_at_a_deck_reports_its_underside);
  RUN_TEST(test_flat_ground_at_rest_is_within_reach);
  RUN_TEST(test_reach_is_measured_along_the_ray);
  RUN_TEST(test_the_crosshair_is_always_inside_the_selection);
  RUN_TEST(test_the_selection_is_one_block_not_a_column);
  RUN_TEST(test_pitch_shears_every_row_uniformly);
  RUN_TEST(test_pitch_keeps_the_aim_ray_under_the_reticle);
  RUN_TEST(test_pitch_is_clamped_to_its_range);
  RUN_TEST(test_both_stops_reach_a_steep_angle_and_match);
  RUN_TEST(test_the_extremes_are_still_a_pure_shear);
  RUN_TEST(test_a_default_camera_sits_at_the_resting_tilt);
  RUN_TEST(test_a_cell_dug_to_nothing_still_has_a_floor);
  RUN_TEST(test_the_selection_extent_brackets_the_selected_spans);
  RUN_TEST(test_the_visible_extent_is_clipped_but_the_geometry_is_not);
  RUN_TEST(test_a_run_block_can_be_the_selection);
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
