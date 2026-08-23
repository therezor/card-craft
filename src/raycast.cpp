// =============================================================================
//  raycast.cpp — the heightmap walker
//
//  Per column, the loop below steps outward one grid cell at a time and paints
//  bottom-up, tracking yLimit: the topmost row already covered. Anything a
//  farther cell would draw below yLimit is hidden by something nearer, so it
//  is clipped for free, and once yLimit reaches the top of the screen the walk
//  stops — no fixed view distance, no depth sort, no overdraw.
//
//  Cost control: one divide per cell (PROJ/d, reused for every height at that
//  distance) rather than one per span, and the per-column camera offset is a
//  table so the direction maths has no divide in it at all.
// =============================================================================
#include "raycast.h"

#include <math.h>

#include "textures.h"
#include "world.h"

namespace raycast {

static float s_camX[VIEW_W];   // 2*x/VIEW_W - 1, constant for the panel

// A cheap deterministic wobble per block. Without it a six-high stone column
// is one flat grey slab covering a quarter of the panel, which is the single
// most artificial thing about a world with no textures.
static inline uint8_t tintOf(int x, int y, int z) {
  uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ (uint32_t)(z * 83492791);
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  return (uint8_t)(h & 1u);
}

void init() {
  for (int x = 0; x < VIEW_W; ++x)
    s_camX[x] = 2.0f * (float)x / (float)VIEW_W - 1.0f;
}

void setPitch(Camera& cam, int horizon) {
  const int lo = HORIZON - PITCH_DOWN, hi = HORIZON + PITCH_UP;
  if (horizon < lo) horizon = lo;
  if (horizon > hi) horizon = hi;
  cam.horizon  = (int16_t)horizon;
  cam.aimSlope = slopeFor(horizon);
}

void setAngle(Camera& cam, float angle) {
  cam.dx = cosf(angle);
  cam.dy = sinf(angle);
  cam.planeX = -cam.dy * PLANE_LEN;   // rotate dir by +90 degrees, then scale
  cam.planeY =  cam.dx * PLANE_LEN;
}

// A ray exactly parallel to an axis has an infinite step along the other one.
// 1e30 is finite (so it survives -ffast-math, which is entitled to assume no
// infinities) and still larger than any distance a 64-cell map can produce.
static constexpr float FAR_STEP = 1e30f;

// Rows are computed as floats and clamped before the cast, because a camera
// flush against a block produces a row well outside int range and the cast
// would be undefined.
static inline int clampRow(float y) {
  if (y < -30000.0f) return -30000;
  if (y >  30000.0f) return  30000;
  return (int)y;
}


// The clipper. Everything the walker wants to draw goes through here: the
// candidate row range is intersected with the still-open ranges, each piece
// that survives becomes a span, and those rows stop being open.
//
// This is the whole reason overhangs work. With one "topmost painted row" a
// column's painted region has to be contiguous from the bottom, which is
// exactly the assumption a bridge breaks.
namespace {

struct Painter {
  OpenSpan* open;
  int*      nOpen;
  Span*     out;
  int*      n;

  // Where the face being painted begins on screen BEFORE clipping, and how
  // much of the texture one screen row covers. Set once per block rather than
  // passed in: paint() is called tens of thousands of times a frame, and two
  // more arguments on that signature is two more words of call overhead every
  // time — measurably more than the per-pixel work they pay for.
  //
  // Together they give every emitted piece its own start into the texture, so a
  // face half hidden behind something nearer still shows the right half of
  // itself instead of restarting the pattern at the cut.
  float    fyTop   = 0.0f;
  uint16_t vStepQ12 = 0;
  uint8_t  zTop    = 0;

  // Where in the tile this piece starts, wrapped.
  //
  // Through int32 rather than straight to uint16, because the product is texels
  // times 4096 over the whole face and a tall close wall is hundreds of texels
  // -- far outside a uint16, and converting an out-of-range float to an
  // unsigned type is undefined, not modular. int32 holds it, and the mask does
  // the wrap the texture wants anyway. Two's complement makes that right for a
  // slightly negative rows-from-top as well, which float slack can produce.
  static uint16_t startQ12(float rows, uint16_t stepQ12) {
    return (uint16_t)((int32_t)(rows * (float)stepQ12) & 0xFFFF);
  }

  bool paint(int a, int b, uint16_t distQ8, uint8_t mat, uint8_t face,
             uint8_t sel, uint8_t tint, uint8_t cap, uint8_t lit, uint8_t u,
             int& topRow) {
    if (a < 0) a = 0;
    if (b > VIEW_H) b = VIEW_H;
    const int no = *nOpen;
    if (a >= b || no == 0) return false;
    // Cheap reject before touching the list at all: most candidates on open
    // terrain are entirely behind what is already painted.
    if (b <= open[0].y0 || a >= open[no - 1].y1) return false;

    // Fast path, and it is the path almost every candidate takes. Until
    // something paints a *hole* in a column, the open list is always exactly
    // one range anchored at the top of the panel — [0, paint front] — and
    // geometry painted front to back always reaches that front. That is the
    // old single-limit walker, and taking it here rather than through the
    // general clipper is the difference between 27 fps and comfortably over
    // target: the general case costs the same per call but runs 30,000 times
    // a frame, and only a handful of those are actually under an overhang.
    if (no == 1 && open[0].y0 == 0 && b >= open[0].y1) {
      const int lim = open[0].y1;
      if (b > lim) b = lim;
      if (a >= b) return false;
      if (*n < MAX_SPANS) {
        Span& s = out[(*n)++];
        s.y0 = (int16_t)a; s.y1 = (int16_t)b;
        s.distQ8 = distQ8; s.mat = mat; s.face = face;
        s.sel = sel; s.tint = tint; s.cap = cap; s.lit = lit; s.u = u;
        s.vStepQ12 = vStepQ12;
        s.vStartQ12 = startQ12((float)a - fyTop, vStepQ12);
        s.zTop     = zTop;
      }
      open[0].y1 = (int16_t)a;
      if (a <= 0) *nOpen = 0;
      if (a < topRow) topRow = a;
      return true;
    }

    bool any = false;
    for (int i = 0; i < *nOpen; ) {
      const int o0 = open[i].y0, o1 = open[i].y1;
      if (o0 >= b) break;              // list is sorted; nothing further overlaps
      const int c0 = a > o0 ? a : o0;
      const int c1 = b < o1 ? b : o1;
      if (c0 >= c1) { ++i; continue; }

      if (*n < MAX_SPANS) {
        Span& s = out[(*n)++];
        s.y0 = (int16_t)c0; s.y1 = (int16_t)c1;
        s.distQ8 = distQ8; s.mat = mat; s.face = face;
        s.sel = sel; s.tint = tint; s.cap = cap; s.lit = lit; s.u = u;
        s.vStepQ12 = vStepQ12;
        s.vStartQ12 = startQ12((float)c0 - fyTop, vStepQ12);
        s.zTop     = zTop;
        any = true;
        if (c0 < topRow) topRow = c0;
      }

      // Remove [c0, c1) from this open range, splitting it if the paint
      // landed in the middle — which is what a bridge deck does to the sky
      // above the gap it leaves.
      if (c0 > o0 && c1 < o1) {
        if (*nOpen < MAX_OPEN) {
          for (int j = *nOpen; j > i + 1; --j) open[j] = open[j - 1];
          open[i]     = { (int16_t)o0, (int16_t)c0 };
          open[i + 1] = { (int16_t)c1, (int16_t)o1 };
          ++*nOpen;
          i += 2;
        } else {
          // Out of room. Keeping the larger fragment loses a sliver of sky
          // rather than dropping geometry or corrupting the list.
          if (c0 - o0 >= o1 - c1) open[i] = { (int16_t)o0, (int16_t)c0 };
          else                    open[i] = { (int16_t)c1, (int16_t)o1 };
          ++i;
        }
      } else if (c0 > o0) {
        open[i] = { (int16_t)o0, (int16_t)c0 };
        ++i;
      } else if (c1 < o1) {
        open[i] = { (int16_t)c1, (int16_t)o1 };
        ++i;
      } else {
        for (int j = i; j < *nOpen - 1; ++j) open[j] = open[j + 1];
        --*nOpen;
      }
    }
    return any;
  }
};

}  // namespace

// One solid run in a column — the ground stack, or something floating over it.
// Both draw the same way, which is why they share this.
struct Run { int base, top; bool ground; };

// The next run of set bits at or above `from`. Both ends come out of one bit
// scan each; on Xtensa NSAU makes those single instructions.
//
// This replaces copying a cell's run list into a fixed array. The array had to
// be fixed because the world had a per-cell run cap, and that cap was the
// reason mining and building could be refused. A mask has no cap, so neither
// does this: a column can be as full of holes as it likes and the walker just
// finds one more run.
static inline bool nextRun(uint32_t m, int from, int& base, int& top) {
  const uint32_t rest = (from >= 32) ? 0u : (m & (~0u << from));
  if (!rest) return false;
  base = __builtin_ctz(rest);
  const uint32_t gaps = ~m >> base;
  top = gaps ? base + __builtin_ctz(gaps) : 32;
  return true;
}

int castColumn(const Camera& cam, int x, int selX, int selY, int selZ,
               Span* out, ColumnResult& res) {
  const float camX = s_camX[x];
  const float rdx = cam.dx + cam.planeX * camX;
  const float rdy = cam.dy + cam.planeY * camX;

  int mapX = (int)floorf(cam.px);
  int mapY = (int)floorf(cam.py);

  const float deltaX = (rdx == 0.0f) ? FAR_STEP : fabsf(1.0f / rdx);
  const float deltaY = (rdy == 0.0f) ? FAR_STEP : fabsf(1.0f / rdy);

  int stepX, stepY;
  float sideX, sideY;
  if (rdx < 0) { stepX = -1; sideX = (cam.px - (float)mapX) * deltaX; }
  else         { stepX =  1; sideX = ((float)mapX + 1.0f - cam.px) * deltaX; }
  if (rdy < 0) { stepY = -1; sideY = (cam.py - (float)mapY) * deltaY; }
  else         { stepY =  1; sideY = ((float)mapY + 1.0f - cam.py) * deltaY; }

  // Converted once. It is a variable now rather than a compile-time constant,
  // and it appears in nine projections inside the per-cell loop — leaving the
  // int-to-float conversion in there put it in the innermost loop of the frame.
  const float horizon = (float)cam.horizon;

  int n = 0;
  res.nOpen = 1;
  res.open[0] = { 0, (int16_t)VIEW_H };
  Painter paint{ res.open, &res.nOpen, out, &n };

  int groundTop = VIEW_H;       // topmost row painted by ground geometry
  int slabHi = VIEW_H, slabLo = 0;   // merged extent of slab spans, empty while hi > lo
  // The selected block's rows as geometry, before anything clipped them.
  int selGeo0 = VIEW_H, selGeo1 = 0;
  int bucket = 0;
  float dNear = 0.0f;
  uint8_t enterFace = F_NS;
  float invNear = PROJ / 0.02f;

  for (int stepN = 0; stepN < MAX_STEPS; ++stepN) {
    while (bucket < ZBUCKETS && (float)bucket * ZBUCKET_SPAN <= dNear) {
      res.limit[bucket]  = (uint8_t)(groundTop < 0 ? 0 : groundTop);
      res.slabHi[bucket] = (uint8_t)(slabHi < 0 ? 0 : slabHi);
      res.slabLo[bucket] = (uint8_t)(slabLo < 0 ? 0 : slabLo);
      ++bucket;
    }

    if (res.nOpen == 0 || dNear >= MAX_DIST || n >= MAX_SPANS - 12) break;

    const float dFar = (sideX < sideY) ? sideX : sideY;
    const bool  selCell = (mapX == selX && mapY == selY);
    const world::Cell cell = world::cellAt(mapX, mapY);
    const uint8_t lit = cell.light;

    // Where this column's ray crosses the face it entered through. One
    // multiply-add per grid step, and it is what lets a wall carry surface
    // detail without any per-pixel interpolation.
    float wall = (enterFace == F_NS) ? (cam.py + dNear * rdy) : (cam.px + dNear * rdx);
    wall -= floorf(wall);
    const uint8_t wallU = (uint8_t)(wall * 255.0f);

    const float df = dFar < 0.02f ? 0.02f : dFar;
    const float invFar = PROJ / df;
    const uint16_t qn = (uint16_t)(dNear * 256.0f > 65535.0f ? 65535.0f : dNear * 256.0f);
    const uint16_t qf = (uint16_t)(df * 256.0f > 65535.0f ? 65535.0f : df * 256.0f);

    // The overall open extent, for rejecting whole runs without touching them.
    const int openLo = res.open[0].y0;
    const int openHi = res.open[res.nOpen - 1].y1;

    // The ground column first, then every run floating above it, ascending.
    // This loop was written against a generic Run from the start; what changed
    // is only where the runs come from — a bit scan of the column instead of a
    // copied list with a cap on it.
    //
    // The ground run is walked even when it is empty. A column dug down to
    // nothing is not an absence of run: the base plane's top face is still
    // there at z = 0, and it is the floor of the pit that was just dug.
    Run run = { 0, (int)cell.h, true };
    for (;;) {
      // A ground column dug down to nothing is not an empty run: the base
      // plane's top face is still there at z = 0, and it is the floor of the
      // pit you just dug. Skipping the run outright left the hole with no
      // bottom — the far wall's side face stretched down across the opening
      // and you saw straight through where the floor should have been.
      if (run.top < run.base || (!run.ground && run.top == run.base)) goto nextrun;
      {
      int* topTracker = run.ground ? &groundTop : nullptr;
      int scratch = VIEW_H;
      int& track = topTracker ? *topTracker : scratch;
      const int slabMark = n;      // spans emitted from here on belong to a slab

      // Everything this run could draw lives between these two rows. On open
      // ground that is entirely below what is already painted for almost every
      // cell the ray crosses, and skipping the whole run there — rather than
      // testing each of its blocks in turn — is most of the walker's speed.
      const float dzTop = cam.z - (float)run.top;
      const float dzBot = cam.z - (float)run.base;
      int lo = clampRow(horizon + dzTop * invNear);
      int hi = lo;
      {
        const int r2 = clampRow(horizon + dzBot * invNear);
        const int r3 = clampRow(horizon + dzTop * invFar);
        const int r4 = clampRow(horizon + dzBot * invFar);
        if (r2 < lo) lo = r2; if (r2 > hi) hi = r2;
        if (r3 < lo) lo = r3; if (r3 > hi) hi = r3;
        if (r4 < lo) lo = r4; if (r4 > hi) hi = r4;
      }
      if (hi <= openLo || lo >= openHi) goto nextrun;

      // -- underside, seen from below. A slab's floor is the ceiling of
      // whatever you are standing under; the ground run has none, since
      // nothing can get beneath the base plane.
      if (!run.ground && cam.z < (float)run.base) {
        paint.vStepQ12 = 0;
        // The plane this face lies in, so the renderer can texture it the way
        // it textures a floor. It used to be left at zero, which is not a
        // height at all — the underside took the flat-colour path and a bridge
        // deck seen from below was a single shade of grey while every other
        // face of the same block carried its material's grain.
        paint.zTop = (uint8_t)run.base;
        const float dz = cam.z - (float)run.base;
        const int ya = clampRow(horizon + dz * invFar);
        const int yb = clampRow(horizon + dz * invNear);
        // An underside belongs to the block directly above it, exactly as a top
        // face belongs to the block below. Without this the selection box could
        // not be drawn from underneath: standing under a deck and pointing up
        // put the crosshair on a block the walker never flagged, so there was
        // nothing on screen to outline.
        const bool selHere = (selCell && run.base == selZ);
        if (selHere) {
          const int a = ya < yb ? ya : yb, b = ya < yb ? yb : ya;
          if (a < selGeo0) selGeo0 = a;
          if (b > selGeo1) selGeo1 = b;
        }
        paint.paint(ya < yb ? ya : yb, ya < yb ? yb : ya, qf,
                    world::matAt(mapX, mapY, run.base), F_BOT, selHere ? 1 : 0,
                    tintOf(mapX, mapY, run.base), 0, lit, wallU, track);
      }

      // -- the side, one span per block.
      // Consecutive blocks are exactly invNear rows apart, so the loop steps
      // by subtraction rather than reprojecting each one.
      // Texels per screen row, 8.8. A block is one world unit tall and pitch is
      // a shear rather than a rotation, so one unit is always invNear pixels —
      // which makes this one multiply off a reciprocal the cell already has,
      // and no divide per span.
      // TEX_N << 12 is 65536, so this is one whole tile per unit of 1/invNear.
      // Bounded rather than trusted: dNear never reaches MAX_DIST, and at
      // MAX_DIST this is 16 * 9 * 4096 / 160 = 3686, so the uint16 has better
      // than sixteen times the headroom it needs. The assert is what says so if
      // MAX_DIST is ever raised past 160 cells.
      static_assert((double)(textures::TEX_N << 12) * MAX_DIST / PROJ < 65536.0,
                    "vStepQ12 must fit a uint16 at the far clip");
      const uint16_t vStepQ12 =
          (uint16_t)((float)(textures::TEX_N << 12) / invNear);

      if (dNear > 0.001f) {
        // Only the blocks that can land somewhere still unpainted. A column is
        // up to MAX_H blocks tall and every one of them used to cost a loop
        // turn, two clamps and a rejected paint() call — and close to a tall
        // cliff almost all of them project off the top of the panel, so the
        // walker spent most of its time discarding geometry it had just
        // computed. Measured on a 32-block world: 41.7 ms worst-case column
        // walk before, and the frame rate floor went with it.
        //
        // Row of the underside of block k is horizon + (cam.z - k) * invNear,
        // which falls as k rises, so the two bounds inverting is what "no
        // block of this run is visible" looks like. Inverted by a block either
        // way for float slack — one extra iteration is far cheaper than a
        // dropped span.
        int kLo = run.base, kHi = run.top;
        {
          // The selected column reports its full projected extent, clipped
          // only to the panel: the crosshair has to stay inside the outline
          // even where something nearer covers the rows it lands on.
          const int bandLo = selCell ? 0 : openLo;
          const int bandHi = selCell ? VIEW_H : openHi;
          const float invStep = 1.0f / invNear;
          const int lo2 = (int)(cam.z - 1.0f - (float)(bandHi - horizon) * invStep);
          const int hi2 = (int)(cam.z - (float)(bandLo - horizon) * invStep) + 2;
          if (lo2 > kLo) kLo = lo2;
          if (hi2 < kHi) kHi = hi2;
          if (kLo < run.base) kLo = run.base;
        }
        // One span per RUN of like blocks, not one per block.
        //
        // This is where the walker's worst case was. Every block of every
        // column used to be its own candidate: a loop turn, two clamps and a
        // full trip through the clipper, ~30,000 times a frame — which is why
        // paint() carries a hand-written fast path at all. A six-high stone
        // wall is one span now, and the pixels it produces are identical,
        // because a face's texture coordinate is a function of world height:
        // v simply keeps accumulating and wraps every TEX_N texels, which is
        // once a block, exactly as it did when each block emitted its own.
        //
        // The run breaks where the material does — and below the natural
        // surface that is often every block, because the ore lattice is seeded
        // per position. That is not a defect: the scene that costs the frame is
        // a close face of uniform stone or something a player built, and both
        // of those merge whole.
        //
        // It also breaks at the selected block, which has to stay its own span:
        // `sel` picks a different shade table for the whole span, and the
        // outline is a property of one block.
        float fyb = horizon + (cam.z - (float)kLo) * invNear;
        int k = kLo;
        while (k < kHi) {
          const uint8_t mat = world::matAt(mapX, mapY, k);
          const bool selHere = (selCell && k == selZ);

          // How far this run of like blocks goes. Stops at a material change,
          // at the selected block, and at the end of the run.
          int kEnd = k + 1;
          if (!selHere)
            while (kEnd < kHi
                   && !(selCell && kEnd == selZ)
                   && world::matAt(mapX, mapY, kEnd) == mat) ++kEnd;

          const float fyt = fyb - invNear * (float)(kEnd - k);
          const int yb = clampRow(fyb);
          const int yt = clampRow(fyt);
          fyb = fyt;
          if (yt < yb) {
            paint.fyTop = fyt;
            paint.vStepQ12 = vStepQ12;
            paint.zTop = 0;
            if (selHere) {
              if (yt < selGeo0) selGeo0 = yt;
              if (yb > selGeo1) selGeo1 = yb;
            }
            // `cap` is 2 rather than 1 where the span is more than one block
            // tall, so the renderer knows to rule a bevel at every block
            // boundary inside it instead of only at its ends.
            paint.paint(yt, yb, qn, mat, enterFace, selHere ? 1 : 0,
                        tintOf(mapX, mapY, k), (kEnd - k) > 1 ? 2 : 1,
                        lit, wallU, track);
          }
          k = kEnd;
        }
      }

      // -- the top, seen from above
      if ((float)run.top < cam.z) {
        paint.vStepQ12 = 0;
        paint.zTop = (uint8_t)run.top;
        const uint8_t mat = world::matAt(mapX, mapY, run.top - 1);
        const float dz = cam.z - (float)run.top;
        const int yb = clampRow(horizon + dz * invNear);
        const int yt = clampRow(horizon + dz * invFar);
        // The top face belongs to the block under it.
        const bool selHere = (selCell && run.top - 1 == selZ);
        if (selHere) {
          if (yt < selGeo0) selGeo0 = yt;
          if (yb > selGeo1) selGeo1 = yb;
        }
        paint.paint(yt, yb, qf, mat, F_TOP, selHere ? 1 : 0,
                    tintOf(mapX, mapY, run.top), 0, lit, wallU, track);
      }

      // Everything this run just emitted was overhead, so fold it into the
      // band a farther billboard has to clip against.
      if (!run.ground) {
        for (int i = slabMark; i < n; ++i) {
          if (out[i].y0 < slabHi) slabHi = out[i].y0;
          if (out[i].y1 > slabLo) slabLo = out[i].y1;
        }
      }
      }
nextrun:
      {
        int nb, nt;
        if (!nextRun(cell.solid, run.top, nb, nt)) break;
        run = { nb, nt, false };
      }
    }

    if (sideX < sideY) { sideX += deltaX; mapX += stepX; enterFace = F_NS; }
    else               { sideY += deltaY; mapY += stepY; enterFace = F_EW; }
    dNear = dFar;
    invNear = invFar;
  }

  while (bucket < ZBUCKETS) {
    res.limit[bucket]  = (uint8_t)(groundTop < 0 ? 0 : groundTop);
    res.slabHi[bucket] = (uint8_t)(slabHi < 0 ? 0 : slabHi);
    res.slabLo[bucket] = (uint8_t)(slabLo < 0 ? 0 : slabLo);
    ++bucket;
  }

  // Merge every face of the selected block into one extent. Done here on the
  // painted rows rather than on the candidate rows so a block half-hidden
  // behind a nearer hill outlines the part of it you can actually see.
  if (selGeo0 < 0) selGeo0 = 0;
  if (selGeo1 > VIEW_H) selGeo1 = VIEW_H;
  res.selGeoY0 = (int16_t)selGeo0;
  res.selGeoY1 = (int16_t)selGeo1;
  res.selY0 = (int16_t)VIEW_H;
  res.selY1 = 0;
  if (selX >= 0) {
    for (int i = 0; i < n; ++i) {
      if (!out[i].sel) continue;
      if (out[i].y0 < res.selY0) res.selY0 = out[i].y0;
      if (out[i].y1 > res.selY1) res.selY1 = out[i].y1;
    }
  }
  return n;
}

bool pick(const Camera& cam, float maxDist, Hit& hit) {
  const float rdx = cam.dx, rdy = cam.dy;
  int mapX = (int)floorf(cam.px);
  int mapY = (int)floorf(cam.py);

  // maxDist is a reach: a distance from the eye to the thing being touched, in
  // world units. The stepper below measures horizontal distance, because it is
  // a 2-D DDA with the height carried alongside as a slope — so the reach has
  // to be converted into that measure once, here, rather than compared against
  // a number that means something else.
  //
  // The aim ray is (dx, dy, -aimSlope) with (dx, dy) already unit length, so a
  // horizontal distance d covers d * sqrt(1 + aimSlope^2) of actual ray. At the
  // resting tilt that factor is 1.028, which is small enough that the old
  // horizontal comparison looked right and wrong enough that reach grew as you
  // looked down — the one direction where a shorter reach is what you want.
  const float ray = sqrtf(1.0f + cam.aimSlope * cam.aimSlope);
  const float hMax = maxDist / ray;

  const float deltaX = (rdx == 0.0f) ? FAR_STEP : fabsf(1.0f / rdx);
  const float deltaY = (rdy == 0.0f) ? FAR_STEP : fabsf(1.0f / rdy);

  int stepX, stepY;
  float sideX, sideY;
  if (rdx < 0) { stepX = -1; sideX = (cam.px - (float)mapX) * deltaX; }
  else         { stepX =  1; sideX = ((float)mapX + 1.0f - cam.px) * deltaX; }
  if (rdy < 0) { stepY = -1; sideY = (cam.py - (float)mapY) * deltaY; }
  else         { stepY =  1; sideY = ((float)mapY + 1.0f - cam.py) * deltaY; }

  // Which way the ray entered the cell being examined, so a side hit knows
  // which face it landed on. The camera's own cell is never a side hit, so the
  // initial value is only ever read after the first step has set it.
  uint8_t enterFace = F_NS;
  int     enterStepX = stepX, enterStepY = stepY;

  float dNear = 0.0f;
  for (int stepN = 0; stepN < MAX_STEPS && dNear < hMax; ++stepN) {
    const float dFar = (sideX < sideY) ? sideX : sideY;

    // The height the ray has at each end of its crossing of this cell. At the
    // resting tilt zFar is the lower of the two; with the pitch keys the slope
    // goes negative and the ray climbs, so neither end can be assumed.
    const float zNear = cam.z - cam.aimSlope * dNear;
    const float zFar  = cam.z - cam.aimSlope * dFar;
    const bool  down  = zFar <= zNear;

    // One 32-bit load answers the whole column, and it is the whole column that
    // has to be answered.
    //
    // This used to be two questions — "how tall is the ground here" and "is
    // there a slab in the way" — and it got both wrong in the same way. The
    // ground column was the only thing that could be a target, so a block
    // hanging in mid-air could not be mined at all: a bridge deck, a roof, a
    // tree crown, anything the player had built out over nothing. And a slab
    // was treated as an opaque blocker that returned NO hit, so aiming at one
    // aimed at nothing. It also only ever consulted the LOWEST floating run, so
    // a second deck above the first was not even in the conversation.
    //
    // A block is a bit at a z. Scanning the mask in ray order finds whichever
    // one the ray meets first, and a ground column is just the low bits of the
    // same word — so the two branches this replaces are one, and it can no
    // longer matter what is holding the block up.
    const uint32_t solid = world::cellAt(mapX, mapY).solid;

    int   bz = -2;          // the block struck, or -2 for none in this cell
    float zPlane = 0.0f;    // the horizontal face crossed, when it is one
    bool  side = false;

    if (solid) {
      const float zLo = down ? zFar : zNear;
      const float zHi = down ? zNear : zFar;
      int lo = (int)floorf(zLo);
      int hi = (int)floorf(zHi);
      if (lo < 0) lo = 0;
      if (hi > world::MAX_H - 1) hi = world::MAX_H - 1;

      for (int z = down ? hi : lo; down ? (z >= lo) : (z <= hi); z += down ? -1 : 1) {
        if (!((solid >> z) & 1u)) continue;
        // The block the eye is standing inside, if the camera has clipped into
        // geometry. Skipping it keeps the old rule that the cell you are in
        // cannot give you a side hit, without hiding the block over your head
        // or under your feet — both of which are horizontal faces, and both of
        // which are reachable when the view is pitched hard.
        const bool inside = zNear >= (float)z && zNear < (float)(z + 1);
        if (inside && dNear <= 0.001f) continue;
        bz   = z;
        side = inside;
        // Descending, the ray came down onto the block's top; climbing, it came
        // up onto its underside.
        zPlane = down ? (float)(z + 1) : (float)z;
        break;
      }
    }

    // Nothing in the column, and the ray has dropped out of the bottom of it:
    // the base plane. Bedrock, so the world refuses to mine it, but it is a
    // surface a block can be built on and the ray has to stop somewhere.
    if (bz == -2 && down && zFar < 0.0f) { bz = -1; zPlane = 0.0f; side = false; }

    if (bz != -2) {
      float dHit = dNear;
      if (!side) {
        // Where the ray actually crosses the face, not the far edge of the cell
        // the crossing happens in. dFar overshoots by up to a whole cell, which
        // was invisible while the reach was loose and stopped being invisible
        // the moment it was tightened to something the player can feel: flat
        // ground at the resting tilt is met 5.05 cells out and was measuring
        // nearly 6.
        dHit = dFar;
        const float sl = cam.aimSlope;
        if (sl > 1e-4f || sl < -1e-4f) {
          dHit = (cam.z - zPlane) / sl;
          if (dHit < dNear) dHit = dNear;
          if (dHit > dFar)  dHit = dFar;
        }
      }
      hit.x = (int16_t)mapX; hit.y = (int16_t)mapY; hit.z = (int16_t)bz;
      if (side) {
        hit.face = enterFace;
        hit.nx = (enterFace == F_NS) ? (int8_t)-enterStepX : (int8_t)0;
        hit.ny = (enterFace == F_EW) ? (int8_t)-enterStepY : (int8_t)0;
        hit.nz = 0;
      } else {
        // A face pointing up or a face pointing down. The underside is new:
        // world::place() has always taken an exact z, so naming the bottom face
        // is the whole of what it takes to build onto the belly of a bridge.
        hit.face = down ? F_TOP : F_BOT;
        hit.nx = 0; hit.ny = 0; hit.nz = down ? (int8_t)1 : (int8_t)-1;
      }
      hit.dist = dHit * ray;
      return dHit <= hMax;
    }

    if (sideX < sideY) {
      sideX += deltaX; mapX += stepX;
      enterFace = F_NS; enterStepX = stepX;
    } else {
      sideY += deltaY; mapY += stepY;
      enterFace = F_EW; enterStepY = stepY;
    }
    dNear = dFar;
  }
  return false;
}

}  // namespace raycast
