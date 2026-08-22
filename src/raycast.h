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
// meets flat ground at rest: EYE * PROJ / TILT, about 6.4 cells as set. Steeper
// puts the target closer but eats the sky a six-block tower has to show
// against; shallower pushes it out of reach.
//
// It was 38 — 5 cells — and the view sat noticeably down at the floor. Every
// pixel of tilt removed pushes the ground contact further out, and REACH in
// game.cpp has to move with it or open ground stops being mineable at rest;
// the two numbers are a pair and neither can be retuned alone.
constexpr int TILT = 30;
constexpr int HORIZON = VIEW_H / 2 - TILT;

// How far the horizon may shear from its resting place, in pixels.
//
// Pitch is a y-shear, so "straight up" is not a thing this projection can
// reach: the slope through a screen row is (VIEW_H/2 - horizon) / PROJ, and
// vertical is that slope going to infinity. What "fully" can mean here is
// therefore an angle, and these are set to about 60 degrees either side of
// level — slope 1.75, which is 280 pixels of shear at PROJ 160.
//
// They used to be 111 and 39, roughly 25 degrees up and 12 down, and the
// reason given for keeping down short was that down is the direction that
// fills the screen with near geometry and that is where the frame time is.
// That reasoning does not survive contact with the walker: looking steeply
// down, every ray meets the floor within a cell or two and the DDA stops
// almost immediately. The expensive view is the one across a landscape, which
// is the RESTING tilt, not either extreme.
//
// Two limits and not one, because the rest pose is not level. HORIZON already
// carries TILT pixels of downward tilt, so a range symmetric around it is not
// symmetric around the direction the player is actually facing; these are
// symmetric about level, which is what the control has to feel like.
//
// As horizon rows, with VIEW_H 135 and HORIZON 37: level is 67, fully up is
// 347 and fully down is -213.
constexpr int PITCH_UP   = 310;   // level (67) + 280, less HORIZON
constexpr int PITCH_DOWN = 250;   // HORIZON, less (level (67) - 280)

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

// A span drawn past the point where the fog has fully closed is exactly the fog
// colour the background already holds. Walking further is work with nothing to
// show for it.
//
// The rule is ONE CELL PAST WHERE THE FOG SHUTS, and the second half of that
// moved. It used to be one past render::BANDS, because the old smoothstep only
// reached full fog on the very last band it had. render::fogAt saturates two
// bands early now, so the haze is solid from 8 cells and this comes in from 11
// to 9 — the cells given up were already painted the background colour, pixel
// for pixel. Shorter than this and the walker would stop before the fog has
// finished closing, which reads as a cut-off edge rather than as distance.
//
// Measured rather than reasoned about. The walker usually stops because every
// screen row is painted, not because it ran out of distance, so this only bites
// once it drops below where a column typically fills up — which is why 12 was
// worth nothing and 11 was worth something. On the fixed-seed benchmark:
//
//     17 cells  11.9 ms  34 fps        11 cells  15.3 ms CPU  33 fps worst
//     12 cells  11.7 ms  35 fps         9 cells  13.9 ms CPU  39 fps worst
//
// The right-hand pair is the current fog and the current bench; the left-hand
// pair is the older measurement that set the figure at eleven, kept because it
// is what rules 12 and 17 out. Worst-case frame rate is the number that matters
// and it went up by a fifth for no visible change at all.
constexpr float MAX_DIST  = 9.0f;
constexpr int   MAX_STEPS = 72;
// A column is 135 rows, so this cannot be reached by geometry that tiles the
// panel — but a stack thirty-two blocks tall seen edge-on emits one span per
// block, and several such stacks along one ray can. The walker stops early at
// MAX_SPANS - 12 and the clipper drops the rest, so overflow costs far detail
// rather than correctness; this is set well clear of it instead.
constexpr int   MAX_SPANS = 112;

// Merging consecutive floor cells into one span, Doom-style, was tried here
// and measured as a clear loss: A/B on a fixed scene put the walker at 10.5 ms
// merged against 4.3 ms unmerged. Doom's floor casting pays for itself because
// it replaces per-column ray work; this replaced an already-cheap flat fill
// over the largest area of the screen with a per-row shade computation, and
// the pixels cost far more than the spans saved.

// Distance buckets recorded per column for sprite occlusion. See castColumn.
//
// One bucket per cell, which is one bucket per block — the finest resolution
// that means anything in a world made of unit cubes, and the reason it can be
// afforded is MAX_DIST: twelve buckets cover the whole draw distance now,
// where at seventeen cells they would not have.
//
// It used to be eight buckets of four cells, and four cells is three blocks of
// slack. A mob standing anywhere in the same four-cell band as the wall in
// front of it was tested against occlusion the wall had not contributed yet,
// so it drew straight through the wall — which is exactly the resolution of
// the artefact. Sprites clip against a per-column, per-cell horizon now; a
// true per-pixel depth buffer would be 31 KB and this board does not have it.
constexpr int   ZBUCKETS     = 12;
constexpr float ZBUCKET_SPAN = 1.0f;

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
  // 1 = this span belongs to the block the crosshair is on.
  //
  // It used to carry two levels: the whole column at 1, its top block at 2.
  // That existed because mining always removed the topmost block, so the block
  // being pointed at and the block about to break were different things and the
  // outline had to describe both — and standing beside a six-high wall it ran
  // off the top of the panel. Mining takes the block you are aiming at now, so
  // there is one thing to draw.
  uint8_t  sel;
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
  // horizontal position — which is exactly what makes surface texture cheap
  // here: the renderer needs no interpolation across the span, only down it.
  uint8_t  u;

  // ...and how far down the face it starts, with how much of the texture each
  // screen row covers, both 8.8. This is the other half of a texture
  // coordinate, and the half the grain tile never had: it indexed by screen
  // row, so the pattern slid over a block as the camera moved instead of
  // sitting on it. Zero on faces that take the flat path.
  uint16_t vStartQ8;
  uint16_t vStepQ8;

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

  // The same block's extent *before* the clipper saw it. The visible extent
  // alone cannot tell an edge of the block from an edge of whatever is standing
  // in front of it, and drawing a rule along the latter puts a line in mid-air.
  // A boundary is a real block edge only where the two extents agree.
  int16_t  selGeoY0, selGeoY1;
};

// What the crosshair is on.
//
// It used to be two ints and a bool — the cell, and whether the ray came down
// on its top. That was everything a heightmap could act on, because both verbs
// worked on the column rather than on a block: mining popped the top whatever
// you aimed at, and building grew the same column. Placing a block *against a
// face* needs to know which face, and which block owns it, and neither of those
// is a property of a column.
struct Hit {
  int16_t x, y, z;      // the block the ray struck
  uint8_t face;         // which of its faces: F_NS, F_EW, F_TOP, F_BOT
  int8_t  nx, ny, nz;   // that face's outward normal, so a block placed against
                        // it goes at (x + nx, y + ny, z + nz)
  float   dist;         // distance from the eye along the ray, the same measure
                        // pick()'s maxDist is given in
};

// (selX, selY, selZ) is the block the crosshair is on; its spans come back with
// sel set, so the player can see what they are about to mine. Pass (-1, -1, -1)
// for no selection.
int castColumn(const Camera& cam, int x, int selX, int selY, int selZ,
               Span* out, ColumnResult& res);

// The crosshair ray. Returns the column it lands on and whether it came down
// on the top of that column rather than running into its side.
bool pick(const Camera& cam, float maxDist, Hit& hit);

}  // namespace raycast
