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
  const int lo = HORIZON - PITCH_RANGE, hi = HORIZON + PITCH_RANGE;
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

// One solid run in a column — the ground stack, or a slab floating over it.
// Both draw the same way, which is why they share this.
struct Run { int base, top; uint8_t mat; bool ground; };

int castColumn(const Camera& cam, int x, int selX, int selY,
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
    const bool  sel  = (mapX == selX && mapY == selY);
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

    const int h     = (int)cell.h;
    const int sBase = (int)cell.slabBase;
    const int sTop  = (int)cell.slabTop;

    Run runs[2];
    int nRuns = 0;
    runs[nRuns++] = { 0, h, 0, true };
    if (sTop) runs[nRuns++] = { sBase, sTop, cell.slabMat, false };

    // The overall open extent, for rejecting whole runs without touching them.
    const int openLo = res.open[0].y0;
    const int openHi = res.open[res.nOpen - 1].y1;

    for (int r = 0; r < nRuns; ++r) {
      const Run& run = runs[r];
      // A ground column dug down to nothing is not an empty run: the base
      // plane's top face is still there at z = 0, and it is the floor of the
      // pit you just dug. Skipping the run outright left the hole with no
      // bottom — the far wall's side face stretched down across the opening
      // and you saw straight through where the floor should have been.
      if (run.top < run.base || (!run.ground && run.top == run.base)) continue;
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
      if (hi <= openLo || lo >= openHi) continue;

      // -- underside, seen from below. A slab's floor is the ceiling of
      // whatever you are standing under; the ground run has none, since
      // nothing can get beneath the base plane.
      if (!run.ground && cam.z < (float)run.base) {
        const float dz = cam.z - (float)run.base;
        const int ya = clampRow(horizon + dz * invFar);
        const int yb = clampRow(horizon + dz * invNear);
        paint.paint(ya < yb ? ya : yb, ya < yb ? yb : ya, qf,
                    run.mat, F_BOT, 0,
                    tintOf(mapX, mapY, run.base), 0, lit, wallU, track);
      }

      // -- the side, one span per block.
      // Consecutive blocks are exactly invNear rows apart, so the loop steps
      // by subtraction rather than reprojecting each one.
      if (dNear > 0.001f) {
        float fyb = horizon + (cam.z - (float)run.base) * invNear;
        for (int k = run.base; k < run.top; ++k) {
          const float fyt = fyb - invNear;
          const int yb = clampRow(fyb);
          const int yt = clampRow(fyt);
          fyb = fyt;
          if (yt >= yb) continue;
          const uint8_t mat = run.ground ? world::matAt(mapX, mapY, k) : run.mat;
          const bool selHere = (sel && run.ground);
          if (selHere) {
            if (yt < selGeo0) selGeo0 = yt;
            if (yb > selGeo1) selGeo1 = yb;
          }
          paint.paint(yt, yb, qn, mat, enterFace,
                      selHere ? (k == run.top - 1 ? 2 : 1) : 0,
                      tintOf(mapX, mapY, k), 1, lit, wallU, track);
        }
      }

      // -- the top, seen from above
      if ((float)run.top < cam.z) {
        const uint8_t mat = run.ground ? cell.top : run.mat;
        const float dz = cam.z - (float)run.top;
        const int yb = clampRow(horizon + dz * invNear);
        const int yt = clampRow(horizon + dz * invFar);
        // run.ground, not just sel: pick() aims at a cell's ground column, and
        // a slab cannot be mined at all. Lighting a bridge deck because the
        // crosshair is on the cell underneath it points at the wrong block.
        const bool selHere = (sel && run.ground);
        if (selHere) {
          if (yt < selGeo0) selGeo0 = yt;
          if (yb > selGeo1) selGeo1 = yb;
        }
        paint.paint(yt, yb, qf, mat, F_TOP, selHere ? 2 : 0,
                    tintOf(mapX, mapY, run.top), 0, lit, wallU, track);
      }

      // Everything this run just emitted was slab, so fold it into the band a
      // farther billboard has to clip against.
      if (!run.ground) {
        for (int i = slabMark; i < n; ++i) {
          if (out[i].y0 < slabHi) slabHi = out[i].y0;
          if (out[i].y1 > slabLo) slabLo = out[i].y1;
        }
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
  res.selMat = 0;
  if (selX >= 0) {
    for (int i = 0; i < n; ++i) {
      if (!out[i].sel) continue;
      if (out[i].y0 < res.selY0) res.selY0 = out[i].y0;
      if (out[i].y1 > res.selY1) res.selY1 = out[i].y1;
      res.selMat = out[i].mat;
    }
  }
  return n;
}

bool pick(const Camera& cam, float maxDist, int& hitX, int& hitY, bool& onTop) {
  const float rdx = cam.dx, rdy = cam.dy;
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

  float dNear = 0.0f;
  for (int stepN = 0; stepN < MAX_STEPS && dNear < maxDist; ++stepN) {
    const float dFar = (sideX < sideY) ? sideX : sideY;
    const int   h    = (int)world::height(mapX, mapY);
    const float zNear = cam.z - cam.aimSlope * dNear;
    const float zFar  = cam.z - cam.aimSlope * dFar;

    // Running into the side of a column. Skipped for the cell the camera is
    // standing in, where the ray starts above its own floor by definition.
    if (h > 0 && dNear > 0.001f && zNear <= (float)h) {
      hitX = mapX; hitY = mapY; onTop = false;
      return dNear <= maxDist;
    }
    // Coming down onto the top of a column, or onto open ground.
    if (zFar <= (float)h) {
      hitX = mapX; hitY = mapY; onTop = true;
      return dFar <= maxDist;
    }

    if (sideX < sideY) { sideX += deltaX; mapX += stepX; }
    else               { sideY += deltaY; mapY += stepY; }
    dNear = dFar;
  }
  return false;
}

}  // namespace raycast
