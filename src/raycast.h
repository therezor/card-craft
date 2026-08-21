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
//  "topmost row painted so far". It has to be: the moment a world can have an
//  overhang, you can see the ground, then a gap of sky under a bridge, then
//  the bridge deck, and the painted region of a column is no longer one
//  contiguous run. Every candidate span is clipped against the open list, the
//  pieces that land are emitted, and those rows are removed. The walk stops
//  when the list is empty, which is the occlusion test and the early-out at
//  once, and because the list does the ordering nothing needs a depth sort.
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

// Pixels per world unit at distance 1. Set to match the horizontal scale
// (VIEW_W/2 / PLANE_LEN = 160) so a cube is square on screen rather than
// stretched.
constexpr float PROJ = 160.0f;

// The camera's resting downward tilt, as the horizon's offset from the middle
// of the panel, in pixels. A board with only four buttons never leaves this
// value; boards with keys to spare can shear away from it (see Camera::horizon)
// and look up at an overhang or down into a pit.
//
// TILT and EYE are the two numbers that decide how far ahead the crosshair
// meets flat ground at rest: EYE * PROJ / TILT, about 5 cells as set. Steeper
// puts the target closer but eats the sky a six-block tower has to show
// against; shallower pushes it out of reach.
constexpr int TILT = 38;
constexpr int HORIZON = VIEW_H / 2 - TILT;

// How far the horizon may shear from its resting place, in pixels. Bounded
// rather than free: past this the floor or the sky fills the panel and the
// world stops being legible, and the walker's cost grows with how much wall
// is on screen.
constexpr int PITCH_RANGE = 46;

// Eye height above whatever surface the body is standing on. A little over one
// block, so a one-block step is waist height and a two-block wall is over your
// head — which is the whole reason to build one.
constexpr float EYE = 1.2f;

// The aim ray is the ray through the middle of the panel, so the crosshair
// sits dead centre. Any other slope would put the reticle somewhere other than
// where the player is looking — which is also why the slope has to be derived
// from the horizon rather than fixed: shear the horizon and the ray through
// the panel's centre tilts with it, keeping reticle and aim in agreement.
constexpr float AIM_SLOPE = (float)TILT / PROJ;
constexpr float slopeFor(int horizon) {
  return (float)(VIEW_H / 2 - horizon) / PROJ;
}

// The shade table saturates at 16 cells (one distance band per cell), so a
// span drawn beyond that is exactly the fog colour the background already
// holds. Walking further is work with nothing to show for it.
constexpr float MAX_DIST  = 17.0f;
constexpr int   MAX_STEPS = 72;
constexpr int   MAX_SPANS = 64;      // a column is 135 rows; this is never reached

// Merging consecutive floor cells into one span, Doom-style, was tried here
// and measured as a clear loss: A/B on a fixed scene put the walker at 10.5 ms
// merged against 4.3 ms unmerged. Doom's floor casting pays for itself because
// it replaces per-column ray work; this replaced an already-cheap flat fill
// over the largest area of the screen with a per-row shade computation, and
// the pixels cost far more than the spans saved.

// Distance buckets recorded per column for mob occlusion. See castColumn.
constexpr int   ZBUCKETS     = 8;
constexpr float ZBUCKET_SPAN = 4.0f;

enum Face : uint8_t { F_NS = 0, F_EW = 1, F_TOP = 2, F_BOT = 3, F_COUNT = 4 };

// Still-unpainted rows, kept sorted and disjoint. Six is well past what the
// terrain generator can actually produce along one ray — a column would need
// three separate overhangs stacked over it — and the clipper degrades by
// keeping the larger fragment rather than by corrupting the list.
constexpr int MAX_OPEN = 6;
struct OpenSpan { int16_t y0, y1; };

struct Span {
  int16_t  y0, y1;    // screen rows [y0, y1), already clipped to the panel
  uint16_t distQ8;    // 8.8 fixed distance, for the shade band
  uint8_t  mat;       // world::Block
  uint8_t  face;      // Face — drives which of the three shade tables is used
  // 0 = not the target. 1 = part of the column the crosshair is on. 2 = the
  // top block of it, which is the one a swing actually takes off.
  //
  // Two levels, not one, because in a heightmap those are different things.
  // Mining always removes the topmost block, so the block that comes off is not
  // the block the crosshair is pointing at unless you happen to be aiming at
  // the top of the stack. Outlining only the top block put the box somewhere
  // the player was not looking — and, standing next to a six-high wall, off the
  // top of the panel entirely. The outline follows the whole column, so the
  // crosshair is always inside it; the lift stays on the top block, so what is
  // about to come off is still the thing that is glowing.
  uint8_t  sel;
  uint8_t  tint;      // 0 or 1: a per-block shade wobble, so faces are not flat
  uint8_t  cap;       // 1 = draw a darker rule along the top, separating blocks
  uint8_t  lit;       // torch light on this column, 0..world::LIGHT_MAX

  // Where across the face this column landed, 0..255. Constant for a whole
  // span, because a vertical face seen from a fixed camera column is hit at one
  // horizontal position — which is exactly what makes surface texture cheap
  // here: the renderer needs no interpolation across the span, only down it.
  uint8_t  u;
};

struct Camera {
  float px, py;             // position, in grid cells
  float z;                  // eye height, absolute world units
  float dx, dy;             // unit direction
  float planeX, planeY;     // camera plane, perpendicular to dir, length PLANE_LEN

  // Looking up and down is a y-shear, not a rotation: every row simply
  // projects further from a horizon that has moved. That is what makes it
  // nearly free — no per-pixel work changes, and the walker's arithmetic is
  // identical — and it is why the world does not distort when you look up.
  //
  // Default member initialisers, so a Camera built without naming these is the
  // fixed-tilt camera the game shipped with. A board with no keys to spare for
  // pitch leaves them alone and behaves exactly as before.
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

// Walks screen column x and writes its spans front to back. Returns the count.
//
// What a walked column leaves behind besides its spans.
struct ColumnResult {
  // Rows nothing was painted into. The renderer fills these with sky; there
  // may be more than one, which is precisely what an overhang looks like.
  OpenSpan open[MAX_OPEN];
  int      nOpen;

  // limit[k] is the topmost row painted by *ground* geometry once everything
  // nearer than k * ZBUCKET_SPAN cells has been drawn — what a billboard
  // clips against. Slabs are excluded from it deliberately: a mob stands on
  // the floor, so letting a bridge deck overhead into this number would clip
  // mobs that are plainly visible underneath it.
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
  // One extent per column rather than a mark per span, because the selection
  // outline is a property of the *block*, not of the spans it happens to be cut
  // into. A selected block emits a side span per stacked block plus a top face;
  // outlining each of those separately draws rules through the middle of the
  // thing it is supposed to be framing. Merging first means the renderer can
  // walk the extents left to right and draw the silhouette, which is also the
  // only way to get vertical edges: a column cannot see its neighbours, and its
  // neighbours are being walked by the other core.
  //
  // Empty is encoded as selY0 >= selY1.
  int16_t  selY0, selY1;
  uint8_t  selMat;

  // The same block's extent *before* the clipper saw it. The visible extent
  // alone cannot tell an edge of the block from an edge of whatever is standing
  // in front of it, and drawing a rule along the latter puts a line in mid-air.
  // A boundary is a real block edge only where the two extents agree.
  int16_t  selGeoY0, selGeoY1;
};

// (selX, selY) is the column the crosshair is on; spans belonging to that
// column's topmost block — the one a swing would take off — come back with
// sel set, so the player can see what they are about to mine. Pass (-1, -1)
// for no selection.
int castColumn(const Camera& cam, int x, int selX, int selY,
               Span* out, ColumnResult& res);

// The crosshair ray. Returns the column it lands on and whether it came down
// on the top of that column rather than running into its side.
bool pick(const Camera& cam, float maxDist, int& hitX, int& hitY, bool& onTop);

}  // namespace raycast
