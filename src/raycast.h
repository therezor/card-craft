// =============================================================================
//  raycast.h — camera -> screen spans
//
//  Free of Arduino/M5GFX so the walker can be checked on the host (see
//  test/test_raycast) at camera positions no play session would reach.
//
//  A ray does not stop at the first solid cell: it keeps stepping outward, and
//  each cell it crosses contributes the side of its ground column (split into
//  one span per block so a stack reads as stacked cubes), the top of that
//  column, and — where the cell carries a slab — the slab's underside, side
//  and top as well.
//
//  Occlusion is a list of still-unpainted row ranges rather than a single
//  "topmost row painted so far". It has to be: with an overhang you can see the
//  ground, then a gap of sky under a bridge, then the bridge deck, so a
//  column's painted region is not one contiguous run. Every candidate span is
//  clipped against the open list, the pieces that land are emitted, and those
//  rows are removed. The walk stops when the list is empty, which is the
//  occlusion test and the early-out at once, and because the list does the
//  ordering nothing needs a depth sort.
// =============================================================================
#pragma once

#include <stdint.h>

namespace raycast {

// Panel geometry. A different panel is a two-line change here; nothing else in
// the codebase names a pixel count.
constexpr int VIEW_W = 240;
constexpr int VIEW_H = 135;

// Half-width of the camera plane: tan(halfFov), so 0.75 is a ~73 degree
// horizontal field of view — wide, because on a 240x135 panel peripheral
// vision is the only warning you get that something is coming for you.
constexpr float PLANE_LEN = 0.75f;

// Pixels per world unit at distance 1. Matches the horizontal scale
// (VIEW_W/2 / PLANE_LEN = 160) so a cube is square on screen.
constexpr float PROJ = 160.0f;

// The camera's resting downward tilt, as the horizon's offset from the middle
// of the panel, in pixels. A board with only four buttons never leaves this
// value; boards with keys to spare shear away from it (see Camera::horizon).
//
// TILT and EYE decide how far ahead the crosshair meets flat ground at rest:
// EYE * PROJ / TILT, about 6.4 cells as set. Steeper puts the target closer but
// eats the sky a six-block tower has to show against; shallower pushes it out
// of reach.
constexpr int TILT = 30;
constexpr int HORIZON = VIEW_H / 2 - TILT;

// How far the horizon may shear from its resting place, in pixels.
//
// Pitch is a y-shear, so "straight up" is not a thing this projection can
// reach: the slope through a screen row is (VIEW_H/2 - horizon) / PROJ, and
// vertical is that slope going to infinity. So the limit is an angle — about 60
// degrees either side of level, slope 1.75, which is 280 pixels of shear at
// PROJ 160.
//
// Two limits and not one, because the rest pose is not level: HORIZON already
// carries TILT pixels of downward tilt, so these are symmetric about level
// rather than about the rest pose, which is what the control has to feel like.
// As horizon rows, with VIEW_H 135 and HORIZON 37: level is 67, fully up 347,
// fully down -213.
constexpr int PITCH_UP   = 310;   // level (67) + 280, less HORIZON
constexpr int PITCH_DOWN = 250;   // HORIZON, less (level (67) - 280)

// Eye height above whatever surface the body is standing on. A little over one
// block, so a one-block step is waist height and a two-block wall is over your
// head — which is the whole reason to build one.
constexpr float EYE = 1.2f;

// The aim ray is the ray through the middle of the panel, so the crosshair sits
// dead centre. The slope is derived from the horizon rather than fixed: shear
// the horizon and the ray through the panel's centre tilts with it, keeping
// reticle and aim in agreement.
constexpr float AIM_SLOPE = (float)TILT / PROJ;
constexpr float slopeFor(int horizon) {
  return (float)(VIEW_H / 2 - horizon) / PROJ;
}

// One cell past where the fog shuts. render::fogAt is solid from 8 cells, and a
// span drawn past that is exactly the fog colour the background already holds.
// Shorter than this and the walker stops before the fog has finished closing,
// which reads as a cut-off edge rather than as distance.
constexpr float MAX_DIST  = 9.0f;
constexpr int   MAX_STEPS = 72;
// A column is 135 rows, so this cannot be reached by geometry that tiles the
// panel — but a stack thirty-two blocks tall seen edge-on emits one span per
// block, and several such stacks along one ray can. The walker stops early at
// MAX_SPANS - 12 and the clipper drops the rest, so overflow costs far detail
// rather than correctness; this is set well clear of it instead.
constexpr int   MAX_SPANS = 112;

// Distance buckets recorded per column for sprite occlusion. See castColumn.
// One bucket per cell — the finest resolution that means anything in a world of
// unit cubes, affordable because MAX_DIST is short enough that twelve of them
// cover the whole draw distance. A true per-pixel depth buffer would be 31 KB
// and this board does not have it.
constexpr int   ZBUCKETS     = 12;
constexpr float ZBUCKET_SPAN = 1.0f;

enum Face : uint8_t { F_NS = 0, F_EW = 1, F_TOP = 2, F_BOT = 3, F_COUNT = 4 };

// Still-unpainted rows, kept sorted and disjoint. Six is well past what the
// terrain generator can produce along one ray — a column would need three
// separate overhangs stacked over it — and the clipper degrades by keeping the
// larger fragment rather than by corrupting the list.
constexpr int MAX_OPEN = 6;
struct OpenSpan { int16_t y0, y1; };

struct Span {
  int16_t  y0, y1;    // screen rows [y0, y1), already clipped to the panel
  uint16_t distQ8;    // 8.8 fixed distance, for the shade band
  uint8_t  mat;       // world::Block
  uint8_t  face;      // Face — drives which of the three shade tables is used
  uint8_t  sel;       // 1 = this span belongs to the block the crosshair is on
  uint8_t  tint;      // 0 or 1: a per-block shade wobble, so faces are not flat
  // 0 = a flat face (top, underside): no texture, no rules.
  // 1 = a vertical face exactly one block tall: rule its two ends.
  // 2 = a vertical face covering SEVERAL blocks, merged into one span. The
  //     renderer has to find the block boundaries inside it, which it does off
  //     the texture coordinate — v wraps once a block by construction.
  uint8_t  cap;
  uint8_t  lit;       // torch light on this column, 0..world::LIGHT_MAX

  // Where across the face this column landed, 0..255. Constant for a whole
  // span, because a vertical face seen from a fixed camera column is hit at one
  // horizontal position — which is what makes surface texture cheap here: the
  // renderer needs no interpolation across the span, only down it.
  uint8_t  u;

  // ...and how far down the face it starts, with how much of the texture each
  // screen row covers. Zero on faces that take the flat path.
  //
  // Q12 — texels times 4096 — and the twelve is load-bearing twice over.
  //
  // It must not be 8.8. The step is texels per screen row, TEX_N * dNear /
  // PROJ, so it SHRINKS as the player approaches: a quarter-cell away it is
  // 6.4, which 8.8 rounds to 6. Neighbouring columns of one flat wall sit at
  // slightly different dNear, round to different integers, and disagree about
  // vertical scale by up to a sixth — which walks a brick course several rows
  // up or down between one column and the next. Measured as rows of stagger on
  // a wall 0.3 cells away and 35 degrees off square: 9.0 at 8.8, 1.34 here.
  // The remainder is not precision and cannot be removed; neighbouring columns
  // of an oblique wall genuinely are at different distances.
  //
  // And TEX_N << 12 is 65536 exactly, so a uint16 start wraps at precisely one
  // tile. The wrap IS the modulo; no masking, and no width to overflow.
  uint16_t vStartQ12;
  uint16_t vStepQ12;

  // For a top face: the world height of the surface being looked down on.
  // A floor's texture coordinates are a function of how far the ray has
  // travelled when it reaches each row, and that distance falls out of this
  // height and the row — see render.cpp. Zero on every other face.
  uint8_t  zTop;
};

struct Camera {
  float px, py;             // position, in grid cells
  float z;                  // eye height, absolute world units
  float dx, dy;             // unit direction
  float planeX, planeY;     // camera plane, perpendicular to dir, length PLANE_LEN

  // Looking up and down is a y-shear, not a rotation: every row simply projects
  // further from a horizon that has moved. That is what makes it nearly free,
  // and why the world does not distort when you look up.
  //
  // Default member initialisers, so a Camera built without naming these is the
  // fixed-tilt camera a board with no keys to spare for pitch will use.
  int16_t horizon = HORIZON;
  float   aimSlope = AIM_SLOPE;
};

// Points the camera at a horizon row, keeping the aim ray through the centre
// of the panel. The two always move together; nothing should set either alone.
void setPitch(Camera& cam, int horizon);

// Screen row the aim ray projects to. Constant at any pitch: the aim slope is
// defined as whatever puts the ray through the middle of the panel, so the
// reticle never moves and the world moves under it.
constexpr int crosshairRow() { return HORIZON + (int)(AIM_SLOPE * PROJ); }

void setAngle(Camera& cam, float angle);
void init();

// What a walked column leaves behind besides its spans.
struct ColumnResult {
  // Rows nothing was painted into. The renderer fills these with sky; there
  // may be more than one, which is precisely what an overhang looks like.
  OpenSpan open[MAX_OPEN];
  int      nOpen;

  // limit[k] is the topmost row painted by *ground* geometry once everything
  // nearer than k * ZBUCKET_SPAN cells has been drawn — what a billboard clips
  // against. Slabs are excluded deliberately: a mob stands on the floor, so
  // letting a bridge deck overhead into this number would clip mobs that are
  // plainly visible underneath it.
  uint8_t  limit[ZBUCKETS];

  // ...but a deck between the camera and a mob does hide the part of it behind
  // the deck, and a single topmost row cannot say that. So slabs nearer than
  // each bucket contribute their own band: rows [slabHi, slabLo). A mob row
  // inside that band is behind a slab and must not be drawn. Empty is encoded
  // as hi > lo, which makes the test fail for every row without a special case.
  uint8_t  slabHi[ZBUCKETS];
  uint8_t  slabLo[ZBUCKETS];

  // The rows this column actually painted of the selected block, merged across
  // every face of it, and the material they were made of.
  //
  // One extent per column rather than a mark per span: a selected block emits a
  // side span per stacked block plus a top face, and outlining each separately
  // draws rules through the middle of the thing it is meant to be framing.
  // Merging first also gets the vertical edges, which a span cannot — a column
  // cannot see its neighbours, and its neighbours are on the other core.
  //
  // Empty is encoded as selY0 >= selY1.
  int16_t  selY0, selY1;

  // The same block's extent *before* the clipper saw it. The visible extent
  // alone cannot tell an edge of the block from an edge of whatever is standing
  // in front of it, and drawing a rule along the latter puts a line in mid-air.
  // A boundary is a real block edge only where the two extents agree.
  int16_t  selGeoY0, selGeoY1;
};

// What the crosshair is on. Placing a block *against a face* needs to know
// which face and which block owns it, so this names a block rather than a
// column.
struct Hit {
  int16_t x, y, z;      // the block the ray struck
  uint8_t face;         // which of its faces: F_NS, F_EW, F_TOP, F_BOT
  int8_t  nx, ny, nz;   // that face's outward normal, so a block placed against
                        // it goes at (x + nx, y + ny, z + nz)
  float   dist;         // distance from the eye along the ray, the same measure
                        // pick()'s maxDist is given in
};

// Walks screen column x and writes its spans front to back. Returns the count.
// (selX, selY, selZ) is the block the crosshair is on; its spans come back with
// sel set, so the player can see what they are about to mine. Pass (-1, -1, -1)
// for no selection.
int castColumn(const Camera& cam, int x, int selX, int selY, int selZ,
               Span* out, ColumnResult& res);

// The crosshair ray. Returns the column it lands on and whether it came down
// on the top of that column rather than running into its side.
bool pick(const Camera& cam, float maxDist, Hit& hit);

}  // namespace raycast
