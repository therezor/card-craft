// =============================================================================
//  render.cpp — the frame
//
//  Order of a frame, and why:
//
//    1. background as full rows. Sky and ground are a per-row gradient, so a
//       row is one repeated colour and can go out two pixels per 32-bit store.
//       Filling everything and then overdrawing the walls costs fewer stores
//       than filling sky/wall/floor per column, and it is far less code.
//    2. wall spans. Strided 16-bit stores, but only over pixels walls occupy.
//    3. billboards, back to front, z-tested per column against the wall array.
//    4. HUD, drawn by ui.cpp through the primitives here.
//
//  With no textures, the shade tables in step 1 and 2 are the entire art
//  direction: a flat colour that neither darkens with distance nor changes
//  with the face it is on reads as a solid blob, not a world.
// =============================================================================
#include "render.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include "font5x7.h"
#include "sprites.h"
#include "textures.h"
#include "world.h"

namespace render {

// Distance bands; band = distQ8 >> 8, i.e. one band per cell. This is the fog:
// a block at BANDS cells is entirely fog colour, and raycast::MAX_DIST is set
// one cell past it so the walker stops exactly where the haze finishes.
//
// Ten rather than sixteen. See raycast::MAX_DIST for what that measured; the
// short version is that the frame time only responds once the fog closes inside
// where a column typically finishes painting, and sixteen was nowhere near it.
//
// It pays twice regardless: RUNGS sizes s_shade, s_edge, s_bevel and
// s_shadeSel, so the shorter ramp hands back about six kilobytes of internal
// RAM, which on this board is margin against the two framebuffers rather than a
// rounding error.
constexpr int BANDS   = 10;

// Rungs below band 0, where a block is brighter than the ambient light of the
// hour rather than merely un-fogged.
//
// Torch light is applied by pulling a block toward the near end of the shade
// ramp — one subtraction, no second table. The trouble was where that ramp
// started: its near end is the block at ambient, and at midnight ambient is
// almost black. A torch could clear the haze and nothing more, so standing
// beside one looked very nearly like standing in the open, and the whole point
// of carrying torches went with it.
//
// Extending the ramp downward past ambient, toward fully lit, fixes it without
// touching the hot loop: same subtraction, one added constant, and now the
// light has somewhere brighter to pull toward.
constexpr int LIT     = world::LIGHT_MAX;
constexpr int RUNGS   = LIT + BANDS;

// Where the sky/ground gradient turns over. Not raycast::HORIZON: the geometric
// horizon is where distant terrain converges, while this is where the haze
// finishes handing over to the ground colour, and putting it at the middle of
// the panel is what keeps the band of fog wide enough to read. It shears with
// the camera by the same number of pixels the geometric horizon does, so the
// sky stays put in the world when you look up.
constexpr int BG_HORIZON = H / 2;

static LGFX_Device*    s_disp = nullptr;
static lgfx::swap565_t* s_buf[2] = { nullptr, nullptr };
static int             s_cur = 0;
static bool            s_held = false;   // SPI transaction kept open, see present()

// [material][face][distance band]. Three faces, because with no textures the
// only thing giving a cube its edges is that its top, its north face and its
// east face are three different brightnesses.
// [material][face][rung][grain]. The last axis used to be a single bit of
// per-block wobble; it is four levels now, and that is what carries surface
// detail without a texture fetch. A vertical face is hit at one horizontal
// position per screen column — see raycast::Span::u — so a face needs no
// interpolation across a span, only an index down it, and the whole cost of
// giving stone grain is one byte loaded from a shared noise tile per pixel.
//
// Amplitude is per material, so snow stays smooth and coal reads as stone shot
// through with black. Flat faces (tops, undersides) use levels 0 and 2 for the
// old per-block wobble.
constexpr int TEXELS = textures::TEXELS;

// The texel tiles, copied out of flash into internal RAM at startup.
//
// They are `static const` in the generated header, which puts them in .rodata
// — and on this chip .rodata is external flash reached through the instruction
// cache. The wall loop reads them at a position that jumps with the material
// and the ray's angle, which is close to the worst access pattern a cache can
// be given: measured, leaving them in flash cost 4.9 ms a frame, nearly
// doubling the walker. Four kilobytes of SRAM buys all of it back.
//
// The shared 8x8 grain tile this replaced never showed the problem because 64
// bytes stay resident whatever you do to them.
static uint8_t s_tex[world::B_COUNT][textures::TEX_N][textures::TEX_N];

// Top faces, for the materials whose top is not their side, and a per-material
// pointer that resolves which of the two a floor span should sample.
//
// Grass is why. Its tile is dirt with a green lip — right for a wall, and wrong
// from above, because drawFloorSpan indexes the tile by world XY and so drew
// that lip as a green stripe every sixteen texels across open ground. The
// pointer table means the floor loop still does one load and no branch: which
// tile a material uses is decided once, at startup, not per span.
static uint8_t s_texTop[textures::TOP_N][textures::TEX_N][textures::TEX_N];
static const uint8_t* s_topFor[world::B_COUNT];
static bool    s_texReady = false;

// 1 / (row - horizon), for every row offset a floor can be drawn at.
//
// This is the whole reason a textured floor is affordable. A top face is a flat
// surface at a known height, so the distance to the point under a given screen
// row is dz * PROJ / (row - horizon) — a divide, per pixel, over the largest
// area of the panel. Build did it with a table and so does this: the divides
// are done once at startup and the loop is a load and two multiplies.
//
// Note this is NOT the floor-span merging that was tried and rejected here
// before (see raycast.h). That replaced a flat fill with a per-row shade
// computation across merged spans; this keeps one span per cell and one shade
// per span, and only changes what colour each pixel reads.
// Covers every (row - horizon) the pitch allows: the horizon reaches -213
// looking fully down, and a row can be VIEW_H - 1, so the largest offset is
// 347. Rounded up. Undersized, this clamps, and a clamped reciprocal is a
// floor whose texture stops converging at the far end.
constexpr int INV_ROWS = 384;
static float s_invRow[INV_ROWS];

static void buildTextures() {
  if (s_texReady) return;
  s_texReady = true;
  s_invRow[0] = 0.0f;
  for (int i = 1; i < INV_ROWS; ++i) s_invRow[i] = 1.0f / (float)i;
  for (int b = 0; b < world::B_COUNT; ++b)
    for (int u = 0; u < textures::TEX_N; ++u)
      for (int v = 0; v < textures::TEX_N; ++v)
        s_tex[b][u][v] = textures::kTexel[b][u][v];
  for (int t = 0; t < textures::TOP_N; ++t)
    for (int u = 0; u < textures::TEX_N; ++u)
      for (int v = 0; v < textures::TEX_N; ++v)
        s_texTop[t][u][v] = textures::kTopTexel[t][u][v];
  for (int b = 0; b < world::B_COUNT; ++b) {
    const uint8_t t = textures::kTopOf[b];
    s_topFor[b] = (t == textures::TOP_NONE) ? &s_tex[b][0][0]
                                            : &s_texTop[t][0][0];
  }
}
static uint16_t s_shade[world::B_COUNT][raycast::F_COUNT][RUNGS][TEXELS];
static uint16_t s_edge[world::B_COUNT][raycast::F_COUNT][RUNGS];      // contact shadow
static uint16_t s_bevel[world::B_COUNT][raycast::F_COUNT][RUNGS];     // lit top edge

// The hour the material tables were last built for, quantised. See shadeFor.
static int  s_shadeQuantum = -1;
static bool s_shadeBuilt   = false;

// Which of the four grain levels each texel of a face takes. One tile shared by
// every material: the material decides how far apart the four levels are, not
// what the pattern is, so this costs 64 bytes of flash for all of them.
// Stored column-major — eight runs of eight — because a span reads one column
// of it all the way down. Row-major meant a stride-8 index per pixel in the
// hottest loop in the program, for nothing.
static const uint8_t kGrain[64] = {
  1,0,2,1,0,2,1,3,   // u = 0
  2,1,0,3,1,1,0,2,   // u = 1
  0,3,1,2,1,0,2,1,   // u = 2
  1,2,1,0,3,1,2,0,   // u = 3
  3,0,2,1,2,1,3,1,   // u = 4
  1,2,3,1,0,2,1,2,   // u = 5
  0,1,1,2,1,3,0,1,   // u = 6
  2,1,0,1,2,0,1,2,   // u = 7
};

// The same table lifted toward white, for the block under the crosshair.
// Precomputed rather than blended per pixel: selection changes once a tick,
// the pixels it covers are redrawn sixty times a second.
//
// It carries the tint dimension for the same reason s_shade does. It used to be
// a plain [band] table read from its own branch in the span loop, which meant a
// highlighted block lost both the per-block wobble and the fog dither and sat
// there visibly flatter and more banded than the blocks around it. Same shape,
// same loop, and the highlight becomes a table swap instead of a special case.
// The highlight table, for the one block the crosshair is on.
//
// It used to mirror s_shade exactly — every material, every face, every rung —
// which is 21 KB describing fifteen materials when at most one of them is
// selected at a time. On a board whose two framebuffers leave about eight
// kilobytes of internal RAM spare, that was most of the remaining headroom
// spent on a table that is 14/15 unread. It is built for the selected material
// instead, which is 1.4 KB and rebuilt only when the selection or the hour
// changes.
static uint16_t s_shadeSel[raycast::F_COUNT][RUNGS][TEXELS];
static int      s_selShadeMat = -1;
static int      s_selQuantum = -2;

// The outline colour. One value for every material and every hour: the box is
// the same box wherever it lands, and Minecraft draws it black.
//
// It used to be picked per material by luminance — near-black on snow, white on
// wood or coal — so mining across a snow line changed the colour of the
// highlight halfway through and two blocks side by side got two different
// boxes. Reading the outline as "this is the block" is worth more than reading
// it against every possible background; at night it is faint, which is also
// true of the game this is imitating.
constexpr uint16_t C_SEL_EDGE = pack(8, 8, 10);

// Topmost painted row per column, sampled at ZBUCKETS distances. drawWorld
// fills it; drawMobs clips billboards against it. 1.9 KB to let a mob be
// correctly hidden by the hill in front of it.
static uint8_t s_limit[raycast::VIEW_W][raycast::ZBUCKETS];
// The slab band per column and bucket: rows a nearer overhang already owns.
// The selected block, per column: what reached the screen, what the block's own
// projection was, and what it is made of. Filled by drawColumns; consumed by
// drawSelBox once both cores have finished, which is the only moment a column
// may look at its neighbours.
static int16_t s_selVis0[raycast::VIEW_W], s_selVis1[raycast::VIEW_W];
static int16_t s_selGeo0[raycast::VIEW_W], s_selGeo1[raycast::VIEW_W];

static uint8_t s_slabHi[raycast::VIEW_W][raycast::ZBUCKETS];
static uint8_t s_slabLo[raycast::VIEW_W][raycast::ZBUCKETS];
static uint16_t s_bg[H];
static float    s_amb  = 1.0f;      // ambient light, for mobs (blocks use the LUT)
static uint8_t  s_fogR, s_fogG, s_fogB;

bool reserve() {
  for (int i = 0; i < 2; ++i) {
    s_buf[i] = (lgfx::swap565_t*)heap_caps_malloc(
        (size_t)W * H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf[i]) return false;
    memset(s_buf[i], 0, (size_t)W * H * sizeof(uint16_t));
  }
  return true;
}

void attach(LGFX_Device& disp) { s_disp = &disp; buildTextures(); }

static inline uint16_t* raw() { return (uint16_t*)s_buf[s_cur]; }

// ---- primitives -------------------------------------------------------------

void fill(uint16_t c) {
  const uint32_t v = (uint32_t)c | ((uint32_t)c << 16);
  uint32_t* p = (uint32_t*)raw();
  for (int i = 0; i < W * H / 2; ++i) p[i] = v;
}

void rect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t* p = raw() + y * W + x;
  for (int j = 0; j < h; ++j, p += W)
    for (int i = 0; i < w; ++i) p[i] = c;
}

void frameRect(int x, int y, int w, int h, int t, uint16_t c) {
  rect(x, y, w, t, c);
  rect(x, y + h - t, w, t, c);
  rect(x, y, t, h, c);
  rect(x + w - t, y, t, h, c);
}

void text(int x, int y, const char* s, uint16_t c, int scale) {
  for (; *s; ++s) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);        // the font is uppercase only
    if (ch >= FONT_FIRST && ch <= FONT_LAST) {
      const uint8_t* g = kFont5x7[(int)(ch - FONT_FIRST)];
      for (int cx = 0; cx < FONT_W; ++cx) {
        const uint8_t bits = g[cx];
        if (!bits) continue;
        for (int cy = 0; cy < FONT_H; ++cy)
          if (bits & (1u << cy))
            rect(x + cx * scale, y + cy * scale, scale, scale, c);
      }
    }
    x += (FONT_W + 1) * scale;
  }
}

int textWidth(const char* s, int scale) {
  int n = 0;
  for (const char* p = s; *p; ++p) ++n;
  return n ? (n * (FONT_W + 1) - 1) * scale : 0;
}

void textCentred(int cx, int y, const char* s, uint16_t c, int scale) {
  text(cx - textWidth(s, scale) / 2, y, s, c, scale);
}

// ---- shading ----------------------------------------------------------------

// The selection lift. A share of the headroom left above the colour, not a
// multiplier: scaling by a constant clipped sand and snow to flat white and
// threw away the material the player was being shown. This lifts a dark block
// a long way in absolute terms, a pale one only a little, and never clips —
// which is what lets the highlight brighten a block without erasing it.
static inline uint8_t lift(uint8_t c) {
  return (uint8_t)((int)c + ((255 - (int)c) * 30) / 100);
}

static inline uint8_t mix(uint8_t a, uint8_t b, float t) {
  const float v = (float)a + ((float)b - (float)a) * t;
  return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

void shadeFor(float dl, int horizon) {
#ifdef DEV_SERIAL
  const uint32_t tShade = micros();
  struct ShadeTimer { uint32_t t; ~ShadeTimer() { g_usShade = micros() - t; } } st{tShade};
#endif
  if (dl < 0.0f) dl = 0.0f;
  if (dl > 1.0f) dl = 1.0f;

  // Sunset is a separate colour rather than a point on the night-day line,
  // because the orange has to peak in the middle and vanish at both ends —
  // it is the game's warning that the day is nearly over.
  float set = 1.0f - fabsf(dl - 0.42f) / 0.42f;
  if (set < 0.0f) set = 0.0f;
  set *= set * 0.65f;

  // Night is a colour, not an absence of one. The old night sky and ground both
  // sat within a few levels of black, so the fog they average to was black too
  // and every ramp in the table ran from almost-black to almost-black.
  const uint8_t skyR = mix(mix(16, 128, dl), 226, set);
  const uint8_t skyG = mix(mix(22, 178, dl), 116, set);
  const uint8_t skyB = mix(mix(48, 238, dl),  62, set);

  const uint8_t grdR = mix(18, 62, dl);
  const uint8_t grdG = mix(24, 96, dl);
  const uint8_t grdB = mix(34, 48, dl);

  // Everything distant converges here. Halfway between sky and ground, so the
  // horizon reads as haze in both halves of the screen.
  s_fogR = mix(skyR, grdR, 0.42f);
  s_fogG = mix(skyG, grdG, 0.42f);
  s_fogB = mix(skyB, grdB, 0.42f);

  const int bgH = BG_HORIZON + (horizon - raycast::HORIZON);
  for (int y = 0; y < H; ++y) {
    // Measured from the turnover rather than from the panel edges, so shearing
    // the horizon slides the whole gradient instead of stretching it.
    const int d = y - bgH;
    if (d < 0) {
      float t = (float)(-d) / (float)BG_HORIZON;         // 0 horizon .. 1 zenith
      if (t > 1.0f) t = 1.0f;
      s_bg[y] = pack(mix(s_fogR, (uint8_t)(skyR * 0.62f), t),
                     mix(s_fogG, (uint8_t)(skyG * 0.62f), t),
                     mix(s_fogB, (uint8_t)(skyB * 0.62f), t));
    } else {
      float t = (float)d / (float)(H - BG_HORIZON);
      if (t > 1.0f) t = 1.0f;
      s_bg[y] = pack(mix(s_fogR, grdR, t),
                     mix(s_fogG, grdG, t),
                     mix(s_fogB, grdB, t));
    }
  }

  // Night is dark but never unplayable: below about a third ambient the flat
  // colours stop being distinguishable from each other and the game turns into
  // guesswork rather than tension. The floor is a little higher than that now,
  // because the measured result of 0.34 against a near-black fog was a panel
  // you genuinely could not read.
  s_amb = 0.42f + 0.58f * dl;

  // Moonlight is blue, and a flat dim version of the daylight palette is not
  // what night looks like. Pulling every base colour a little way toward a cool
  // grey-blue before the ambient multiplier is what separates "dark" from
  // "unlit", and it costs one lerp per material per frame.
  const float night = (1.0f - dl) * 0.30f;

  // Everything above is per-frame: it is a handful of lerps and a 135-entry
  // gradient, and the gradient has to be rebuilt anyway because it shears with
  // the pitch. Everything below is the material tables, and those are a very
  // different proposition — 14 materials x 4 faces x 22 rungs x 4 grain levels,
  // measured at 6.4 ms a frame, which was half the frame budget.
  //
  // They depend on one number: the hour. And the hour takes a hundred seconds
  // to cross its whole range. Rebuilding them sixty times a second was spending
  // half of every frame recomputing a value that had not changed. Quantising
  // the day into 512 steps rebuilds them about ten times a second, which is far
  // finer than the eye can follow a sunset, and gives the frame back.
  const int quantum = (int)(dl * 512.0f);
  if (quantum == s_shadeQuantum && s_shadeBuilt) return;
  s_shadeQuantum = quantum;
  s_selShadeMat = -1;         // the highlight is derived from what follows
  s_shadeBuilt = true;

  // Tops catch the sky, the two side orientations fall away from it, and an
  // underside catches nothing at all. The gaps have to be wide: at 20 pixels
  // across, a subtle difference reads as noise rather than as an edge. The
  // very dark underside is what sells a bridge as something with air beneath
  // it rather than a stripe painted on a hill.
  static const float kFace[raycast::F_COUNT] = { 0.78f, 0.58f, 1.00f, 0.34f };

  for (int b = 0; b < world::B_COUNT; ++b) {
    // The material's own colour is not read here any more: every colour this
    // block is made of comes from its texture palette, whose entry 0 is that
    // colour by construction (tools/make-textures.py checks it).
    for (int side = 0; side < raycast::F_COUNT; ++side) {
      // A block that makes its own light does not dim at night: a torch that
      // goes grey after dusk is worse than no torch at all.
      const bool emits = world::emission((uint8_t)b) != 0;
      const float amb = emits ? 1.0f : s_amb;
      const float face = kFace[side];
      // Emissive blocks keep their own colour: a torch that turns moon-blue
      // after dusk is a worse torch than one that does not dim at all.
      // A quarter of the amplitude per level, so the four levels span it.
      for (int d = 0; d < RUNGS; ++d) {
        // Below LIT the block is lifted from the hour's ambient toward fully
        // lit; at LIT it is exactly the ambient near colour the table used to
        // start at; past that it fades into fog as before.
        const float ambD = (d < LIT)
            ? amb + (1.0f - amb) * (float)(LIT - d) / (float)LIT
            : amb;
        float t = (d < LIT) ? 0.0f : (float)(d - LIT) / (float)(BANDS - 1);
        t = t * t * (3.0f - 2.0f * t);                  // crisp near, saturating far

        // Every colour the material's texture is made of, run through exactly
        // the shading the single face colour used to get. That is what makes a
        // texture free here: the per-pixel work is still one byte indexing this
        // table, and the table is the same size it was when the last axis meant
        // "one of four brightness steps" instead of "one of eight colours".
        //
        // Entry 0 is the material's own colour, so anything that reads
        // BlockInfo directly — particles, the HUD swatch, distant fog — still
        // agrees with the texture.
        uint8_t cr0 = 0, cg0 = 0, cb0 = 0;
        for (int k = 0; k < TEXELS; ++k) {
          const uint8_t* pal = textures::kTexPal[b][k];
          const float pr = emits ? pal[0] : mix(pal[0], 90, night);
          const float pg = emits ? pal[1] : mix(pal[1], 110, night);
          const float pb = emits ? pal[2] : mix(pal[2], 150, night);
          const uint8_t cr = mix((uint8_t)(pr * ambD * face), s_fogR, t);
          const uint8_t cg = mix((uint8_t)(pg * ambD * face), s_fogG, t);
          const uint8_t cb = mix((uint8_t)(pb * ambD * face), s_fogB, t);
          s_shade[b][side][d][k] = pack(cr, cg, cb);
          if (k == 0) { cr0 = cr; cg0 = cg; cb0 = cb; }
        }
        const uint8_t cr = cr0, cg = cg0, cb = cb0;
        // A block's bottom edge is a contact shadow and its top edge catches the
        // light. One dark rule alone read as a black grid ruled over the world;
        // a pair reads as a cube with a lit edge, which is the whole difference
        // between a shaded polygon and a block. Both fade into the fog with the
        // face, or distant terrain turns back into a grid.
        s_edge[b][side][d]  = pack(mix(cr, 0, 0.34f),
                                   mix(cg, 0, 0.34f),
                                   mix(cb, 0, 0.34f));
        s_bevel[b][side][d] = pack(mix(cr, 255, 0.22f),
                                   mix(cg, 255, 0.22f),
                                   mix(cb, 255, 0.22f));
      }
    }
  }
}

// Mobs are not in the block table, so they shade at runtime. A handful of
// these per frame — cheaper than another LUT to keep in step.
// lit is the torch light on the cell the mob is standing in. Without it a lit
// clearing would show bright ground with a still-black zombie standing on it,
// which looks like the mob failed to draw rather than like night.
static uint16_t shadeMob(uint8_t r, uint8_t g, uint8_t b, int band, int lit) {
  float amb = s_amb;
  if (lit > 0) {
    const float k = (lit > world::LIGHT_MAX ? world::LIGHT_MAX : lit)
                    / (float)world::LIGHT_MAX;
    amb += (1.0f - amb) * k;
  }
  float t = (float)band / (float)(BANDS - 1);
  t = t * t * (3.0f - 2.0f * t);
  return pack(mix((uint8_t)(r * amb), s_fogR, t),
              mix((uint8_t)(g * amb), s_fogG, t),
              mix((uint8_t)(b * amb), s_fogB, t));
}

// Lifts one material's shading into the highlight table. A few hundred packs,
// and only when the player looks at something new.
static void selectMaterial(int mat) {
  if (mat < 0 || mat >= world::B_COUNT) { s_selShadeMat = -1; return; }
  if (mat == s_selShadeMat && s_shadeQuantum == s_selQuantum) return;
  s_selShadeMat = mat;
  s_selQuantum = s_shadeQuantum;
  for (int side = 0; side < raycast::F_COUNT; ++side)
    for (int d = 0; d < RUNGS; ++d)
      for (int k = 0; k < TEXELS; ++k) {
        const uint16_t c = s_shade[mat][side][d][k];
        // Unpack the pre-swapped RGB565 the table holds, lift it, pack it back.
        const uint16_t v = (uint16_t)((c >> 8) | (c << 8));
        const uint8_t r = (uint8_t)((v >> 8) & 0xF8);
        const uint8_t g = (uint8_t)((v >> 3) & 0xFC);
        const uint8_t b = (uint8_t)((v << 3) & 0xF8);
        s_shadeSel[side][d][k] = pack(lift(r), lift(g), lift(b));
      }
}

// ---- world ------------------------------------------------------------------

// One stripe of columns. Split out from drawWorld so the two cores can each
// take half: measurement put 8.5 ms of a frame in the walker and only 2.4 ms
// in the pixel writes, so halving the walker is the whole optimisation.
// One textured floor span.
//
// Deliberately its own function and deliberately not inlined. The span loop
// this used to sit inside already keeps a dozen values live — the span, the
// shade rows, the dither parity, the framebuffer pointer — and the floor path
// adds a dozen more: two texture coordinates, two steps, four span constants,
// the tile, two palette rows. Xtensa has sixteen visible registers, so inlined
// it spilled the inner loop to the stack and every floor pixel paid for it.
//
// That was the entire cost, and it took four wrong theories to find. A probe
// that textured every pixel and a probe that stored a constant measured the
// same 12 ms walker, which is the shape of a bottleneck that is not in the
// arithmetic at all. A call gets a fresh register window; the same arithmetic
// then costs what it looks like it should.
static void __attribute__((noinline))
drawFloorSpan(uint16_t* p, const raycast::Span& s,
              const uint16_t (*tbl)[TEXELS], int band, int frac, int thX,
              int horizon, const raycast::Camera& cam, float rdx, float rdy) {
        // A floor, textured. Every pixel needs where on the surface it is
        // looking, which is a function of the distance to that row — and that
        // distance is dz * PROJ / (row - horizon), a divide the table above has
        // already done.
        //
        // Perspective-correct, not interpolated: the reciprocal is exact for
        // every row, so a floor cell right under the player maps as truthfully
        // as one at the far clip. Stepping u and v linearly down the span would
        // have been cheaper still and visibly wrong up close, where a single
        // cell can fill half the panel.
        // A ceiling is a floor above your head. The same plane, the same
        // arithmetic, and the only difference is that dz and (row - horizon)
        // are both negative — so their ratio, which is the only thing that
        // matters, comes out identical. Undersides used to take the flat-colour
        // path instead, and the underside of a bridge was one grey rectangle.
        const bool ceiling = (s.face == raycast::F_BOT);

        const float dz = cam.z - (float)s.zTop;
        const float kx = rdx * dz * raycast::PROJ * (float)textures::TEX_N;
        const float ky = rdy * dz * raycast::PROJ * (float)textures::TEX_N;
        const float bx = cam.px * (float)textures::TEX_N;
        const float by = cam.py * (float)textures::TEX_N;
        const uint8_t* tex = s_topFor[s.mat];
        const uint16_t* rowA = tbl[band];
        const uint16_t* rowB = tbl[band + (frac ? 1 : 0)];

        // The signed reciprocal of the row's distance from the horizon. The
        // table only holds positives, so a ceiling reads it by magnitude and
        // negates; the clamp at 1 keeps the row ON the horizon, where 1/0 is
        // not a number, out of the arithmetic.
        auto invAt = [](int row) -> float {
          const int a = row < 0 ? -row : row;
          const int i = a < 1 ? 1 : (a < INV_ROWS ? a : INV_ROWS - 1);
          return row < 0 ? -s_invRow[i] : s_invRow[i];
        };

        // Rows on the wrong side of the horizon are not part of this face.
        // Clipped once here rather than tested on every pixel.
        int y = s.y0, yEnd = s.y1, r = s.y0 - horizon;
        if (ceiling) {
          if (yEnd > horizon) yEnd = horizon;
          if (y >= yEnd) return;
        } else {
          if (r < 1) { const int k = 1 - r; y += k; p += k * W; r = 1; }
        }

        // Two paths, and the split is the whole reason a textured floor is
        // affordable.
        //
        // The walker emits one span per cell, so a frame is a few thousand
        // floor spans and most of them are two or three pixels tall — a distant
        // cell seen edge-on. The per-pixel work was never the problem: a probe
        // that textured every floor pixel and one that stored a constant
        // measured the same, 12.0 ms of walker either way. What cost 4.2 ms a
        // frame was the SETUP, paid once per span whatever its height, and the
        // two integer divides in it above all.
        //
        // So: a short span takes one sample and fills, which is what it would
        // have looked like anyway at two pixels tall. A tall one — the handful
        // that cover most of the panel — steps affine in 16.16 between exact
        // fixes eight rows apart, and eight is a shift rather than a divide.
        //
        // Magnitudes are safe because dz cancels: kx * inv works out as
        // rdx * distance * TEX_N, and distance is bounded by MAX_DIST.
        constexpr int SEG = 8;

        #define FLOOR_SAMPLE_AT(row) \
            ({ const float iv = invAt(row); \
               const int su = (int)(bx + kx * iv) & 15; \
               const int sv = (int)(by + ky * iv) & 15; \
               tex[(su << 4) | sv]; })

        if (yEnd - y <= 4) {
          // One texel for the whole span, taken from its middle.
          const uint8_t t = FLOOR_SAMPLE_AT(r + ((yEnd - y) >> 1));
          if (frac == 0) {
            const uint16_t c = rowA[t];
            for (; y < yEnd; ++y, p += W) *p = c;
          } else {
            const uint16_t cA = rowA[t], cB = rowB[t];
            int par = y & 1;
            for (; y < yEnd; ++y, p += W) {
              *p = (frac > thX + (par ? 4 : 0)) ? cB : cA;
              par ^= 1;
            }
          }
        } else {
          float inv = invAt(r);
          int32_t u = (int32_t)((bx + kx * inv) * 65536.0f);
          int32_t v = (int32_t)((by + ky * inv) * 65536.0f);
          int par = y & 1;
          while (yEnd - y >= SEG) {
            const int r2 = r + SEG;
            inv = invAt(r2);
            const int32_t u2 = (int32_t)((bx + kx * inv) * 65536.0f);
            const int32_t v2 = (int32_t)((by + ky * inv) * 65536.0f);
            const int32_t du = (u2 - u) >> 3;      // SEG is 8, so this is /SEG
            const int32_t dv = (v2 - v) >> 3;
            if (frac == 0) {
              for (int i = 0; i < SEG; ++i, ++y, p += W, u += du, v += dv)
                *p = rowA[tex[(((u >> 16) & 15) << 4) | ((v >> 16) & 15)]];
            } else {
              for (int i = 0; i < SEG; ++i, ++y, p += W, u += du, v += dv) {
                const uint8_t t = tex[(((u >> 16) & 15) << 4) | ((v >> 16) & 15)];
                *p = (frac > thX + (par ? 4 : 0)) ? rowB[t] : rowA[t];
                par ^= 1;
              }
            }
            u = u2; v = v2; r = r2;
          }
          // Tail: fewer than SEG rows left, so one sample rather than another
          // fix-up pair.
          if (y < yEnd) {
            const uint8_t t = FLOOR_SAMPLE_AT(r + ((yEnd - y) >> 1));
            const uint16_t cA = rowA[t], cB = rowB[t];
            for (; y < yEnd; ++y, p += W) {
              *p = (frac && frac > thX + (par ? 4 : 0)) ? cB : cA;
              par ^= 1;
            }
          }
        }
        #undef FLOOR_SAMPLE_AT
}

static void drawColumns(const raycast::Camera& cam, int selX, int selY, int selZ,
                        int xStart, int stride) {
  uint16_t* base = raw();

  raycast::Span spans[raycast::MAX_SPANS];
  raycast::ColumnResult res;
  const int horizon = (int)cam.horizon;
#ifdef DEV_SERIAL
  uint32_t fSpans = 0, fTall = 0, fPix = 0, fSeg = 0;
#endif
  for (int x = xStart; x < W; x += stride) {
    const int n = raycast::castColumn(cam, x, selX, selY, selZ, spans, res);

    // This column's ray, rebuilt the same way the walker builds it. A textured
    // floor needs to know which way it is looking, and recomputing it here is
    // two multiplies a column rather than eight more bytes on every span.
    const float camOff = 2.0f * (float)x / (float)W - 1.0f;
    const float rdx = cam.dx + cam.planeX * camOff;
    const float rdy = cam.dy + cam.planeY * camOff;

    // Sky goes exactly where nothing was painted. That is a list now, not a
    // single run from the top: the gap you see under a bridge is sky with
    // terrain both above and below it. Filling the whole panel first and
    // overdrawing would be ~90% waste — the walker covers most of the screen.
    for (int i = 0; i < res.nOpen; ++i) {
      uint16_t* q = base + (int)res.open[i].y0 * W + x;
      for (int y = res.open[i].y0; y < res.open[i].y1; ++y, q += W) *q = s_bg[y];
    }
    // Own-column writes only, so the two cores never touch the same byte.
    s_selVis0[x] = res.selY0;
    s_selVis1[x] = res.selY1;
    s_selGeo0[x] = res.selGeoY0;
    s_selGeo1[x] = res.selGeoY1;

    for (int k = 0; k < raycast::ZBUCKETS; ++k) {
      s_limit[x][k]  = res.limit[k];
      s_slabHi[x][k] = res.slabHi[k];
      s_slabLo[x][k] = res.slabLo[k];
    }

    for (int i = 0; i < n; ++i) {
      const raycast::Span& s = spans[i];
      // Torch light pulls a block toward the near end of the fog ramp. The
      // shade table already runs from "fully lit at your feet" to "gone in the
      // haze", so lighting something is exactly the same operation as bringing
      // it closer — no extra table, and it costs one subtraction.
      //
      // Kept in sixteenths of a band rather than whole bands, because sixteen
      // fog steps across a ground plane read as visible arcs. The fraction
      // drives a 2x2 ordered dither between the two neighbouring bands below,
      // which costs one compare per pixel and removes the banding entirely.
      // Distance in sixteenths of a band, pulled toward the lit end by any torch
      // light on the cell, and offset by LIT so the pull has brighter-than-
      // ambient rungs to reach. Plus a small glow the player carries: the two
      // cells at your feet are readable at any hour, which is the difference
      // between a dark game and a blind one.
      int bandQ = (int)(s.distQ8 >> 4) - ((int)s.lit << 4) + (LIT << 4);
      const int glow = (int)(s.distQ8 >> 4) - 40;
      if (glow < 0) bandQ += glow;                       // -40..0, strongest underfoot
      if (bandQ < 0) bandQ = 0;
      int band = bandQ >> 4;
      int frac = bandQ & 15;
      if (band >= RUNGS - 1) { band = RUNGS - 1; frac = 0; }
      // One table lookup for selected and unselected alike: the highlight is a
      // different table, not a different code path. That is what keeps the
      // dither and the texture on the block the player is aiming at, and it
      // takes a branch out of the hottest loop in the program.
      const uint16_t (*tbl)[TEXELS] = s.sel ? s_shadeSel[s.face]
                                            : s_shade[s.mat][s.face];
      const int thX = (x & 1) ? 8 : 0;
      uint16_t* p = base + (int)s.y0 * W + x;

      // Texture goes on vertical faces only, and only where there is room for
      // it to read as surface. On the two-pixel slivers a distant block turns
      // into it is noise, and those slivers are most of the spans in a frame.
      if (s.cap && s.y1 - s.y0 > 2) {
        // A vertical face, so it has a real position on the block in both
        // directions: u across, fixed for the whole span because a vertical
        // face seen from one screen column is hit at one horizontal position,
        // and v down, stepped in 8.8 by the walker.
        //
        // One tile across the face, not the eight the shared noise tile used.
        // That was the right call for noise indexed by screen row — at one tile
        // per face a texel was ten pixels wide against one tall, which draws as
        // brushed metal — but with a real v the texels are square by
        // construction, and one tile per block is what makes brick courses line
        // up and a grass lip sit on the top edge instead of eight of them.
#ifdef NO_TEXTURES
        // A/B control: same loop shape, same two loads, but the index comes
        // from the old shared 8x8 grain tile by screen row instead of from a
        // wall-space texture coordinate.
        const uint8_t* col = kGrain + ((((int)s.u >> 2) & 7) << 3);
        uint32_t v = 0;
        const uint32_t vStep = 0;
#else
        const uint8_t* col = s_tex[s.mat][((int)s.u * textures::TEX_N) >> 8];
        uint32_t v = s.vStartQ8;
        const uint32_t vStep = s.vStepQ8;
#endif
        const uint16_t* rowA = tbl[band];
        if (frac == 0) {
          for (int y = s.y0; y < s.y1; ++y, p += W, v += vStep)
            *p = rowA[col[(v >> 8) & (textures::TEX_N - 1)]];
        } else {
          const uint16_t* rowB = tbl[band + 1];
          int par = (int)s.y0 & 1;
          for (int y = s.y0; y < s.y1; ++y, p += W, v += vStep) {
            const uint8_t t = col[(v >> 8) & (textures::TEX_N - 1)];
            *p = (frac > thX + (par ? 4 : 0)) ? rowB[t] : rowA[t];
            par ^= 1;
          }
        }
      } else if ((s.face == raycast::F_TOP || s.face == raycast::F_BOT)
                 && s.y1 - s.y0 > 1) {
#ifdef DEV_SERIAL
        // The rows drawFloorSpan will actually walk: it skips everything at or
        // above the horizon, where 1/(row - horizon) is not a number.
        {
          const int fy0 = s.y0 > horizon + 1 ? s.y0 : horizon + 1;
          const int fh  = s.y1 - fy0;
          if (fh > 0) {
            ++fSpans; fPix += (uint32_t)fh;
            if (fh > 4) { ++fTall; fSeg += (uint32_t)(fh >> 3); }
          }
        }
#endif
        drawFloorSpan(p, s, tbl, band, frac, thX, horizon, cam, rdx, rdy);
      } else if (frac == 0) {
        const uint16_t cA = tbl[band][s.tint];
        for (int y = s.y0; y < s.y1; ++y, p += W) *p = cA;
      } else {
        // Two-by-two Bayer: the column contributes 8, the row 4, so a span
        // is a stipple of the two neighbouring fog steps in the proportion
        // the true distance falls between them.
        const uint16_t cA = tbl[band][s.tint];
        const uint16_t cB = tbl[band + 1][s.tint];
        int par = (int)s.y0 & 1;
        for (int y = s.y0; y < s.y1; ++y, p += W) {
          *p = (frac > thX + (par ? 4 : 0)) ? cB : cA;
          par ^= 1;
        }
      }

      // Bevel. A lit rule along the top of a block and a contact shadow along
      // its bottom, so a stack of six reads as six cubes rather than as one
      // tall face with lines ruled across it.
      if (s.cap && s.y1 - s.y0 > 3) {
        base[(int)s.y0 * W + x]       = s_bevel[s.mat][s.face][band];
        base[((int)s.y1 - 1) * W + x] = s_edge[s.mat][s.face][band];

        // A span covers a whole run of like blocks now, so the boundaries
        // between them fall INSIDE it and need ruling too.
        //
        // Found by arithmetic, out here, rather than by watching the texture
        // coordinate wrap inside the pixel loop. That was the obvious way to do
        // it — v wraps exactly once a block, so a wrap is a boundary — and it
        // was measured at +726 us a frame against the 183 us the merging saves.
        // A compare and an unpredictable branch on every wall pixel is simply
        // dearer than the clipper calls it was meant to pay for. There are two
        // stores per boundary here and nothing at all per pixel.
        if (s.cap > 1) {
          const float step  = (float)(textures::TEX_N << 8) / (float)s.vStepQ8;
          const uint16_t off = (uint16_t)(s.vStartQ8 & ((textures::TEX_N << 8) - 1));
          float yb = (float)s.y0
                   + (float)((textures::TEX_N << 8) - off) / (float)s.vStepQ8;
          const uint16_t bevel = s_bevel[s.mat][s.face][band];
          const uint16_t edge  = s_edge[s.mat][s.face][band];
          for (; yb < (float)s.y1; yb += step) {
            const int yy = (int)yb;
            if (yy <= s.y0 || yy >= s.y1) continue;
            base[yy * W + x]       = bevel;
            base[(yy - 1) * W + x] = edge;
          }
        }
      }
    }
  }
#ifdef DEV_SERIAL
  __atomic_fetch_add(&g_floorSpans, fSpans, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_floorTall,  fTall,  __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_floorPix,   fPix,   __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_floorSeg,   fSeg,   __ATOMIC_RELAXED);
#endif
}

// ---- the second core --------------------------------------------------------
//
// Columns are independent: each one walks the world read-only and writes only
// its own pixels and its own entry in the occlusion table. Two cores can take
// half each with no locking at all — the only shared state is the framebuffer,
// and they never touch the same address.
//
// The handshake is a pair of task notifications, which cost a few microseconds
// against the ~4 ms of walker time this moves off the critical path.
static TaskHandle_t s_worker = nullptr;
static TaskHandle_t s_main   = nullptr;
static const raycast::Camera* s_jobCam = nullptr;
static int s_jobSelZ = -1;
static int s_jobSelX = -1, s_jobSelY = -1;

static void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    drawColumns(*s_jobCam, s_jobSelX, s_jobSelY, s_jobSelZ, 0, 2);
    xTaskNotifyGive(s_main);
  }
}

void startWorker() {
  s_main = xTaskGetCurrentTaskHandle();
  // Core 0 is otherwise idle: the radios are never brought up, and Arduino
  // runs loop() on core 1. Stack has to hold a Span array and a ColumnResult.
  if (xTaskCreatePinnedToCore(workerTask, "raycols", 6144, nullptr, 2,
                              &s_worker, 0) != pdPASS) {
    s_worker = nullptr;      // fall back to doing both halves inline
  }
}

#ifdef DEV_SERIAL
uint32_t g_usWorld = 0, g_usMobs = 0, g_usSky = 0, g_usSel = 0, g_usShade = 0;

// What the floor pass actually did, per frame: how many top-face spans reached
// drawFloorSpan, how many of those were tall enough to take its stepped path
// rather than the one-sample fill, how many pixels they covered in total, and
// how many eight-row segments those pixels were walked in.
//
// Counted at the call site rather than inside drawFloorSpan, which is
// deliberate: that function is noinline precisely because it is one register
// short of spilling its inner loop (see the comment above it), and four
// counters live across that loop would put it back over the edge. Everything
// here is a function of the span alone, so the caller can work it out for free.
//
// Accumulated per column-stripe and folded in once, because both cores run
// drawColumns and a plain += from two of them loses counts.
uint32_t g_floorSpans = 0, g_floorTall = 0, g_floorPix = 0, g_floorSeg = 0;
#endif

// The selection box, drawn once both cores have finished their columns.
//
// It has to be a post-pass. A vertical edge is a place where one column has the
// block and its neighbour does not, and inside drawColumns a column cannot see
// its neighbours — they belong to the other core and may not have been walked
// yet. Afterwards the extents are complete and one left-to-right sweep can see
// the whole silhouette.
//
// Two extents per column, not one. The visible extent is what survived the
// clipper; the geometric extent is where the block's own edges are. A boundary
// is ruled only where the two agree — anywhere they differ the boundary belongs
// to whatever is standing in front of the block, and ruling it would hang a
// line in mid-air.
//
// Everything here is proportional to the *outline*, never to the area inside
// it. The first version tested every row of every selected column and hashed
// every pixel for the crack stipple; measured on the bench that cost 1.5 ms
// average and 5.7 ms on a frame where the target filled the panel — for an
// outline. Walking the two uncovered row ranges per side instead, and drawing
// cracks as a few short segments, is the same picture for a fortieth of it.

static void drawSelBox(int selX, int selY) {
  if (selX < 0) return;
  uint16_t* base = raw();

  int bx0 = W, bx1 = -1, by0 = H, by1 = -1;

  for (int x = 0; x < W; ++x) {
    const int v0 = s_selVis0[x], v1 = s_selVis1[x];
    if (v0 >= v1) continue;                       // nothing of it in this column
    constexpr uint16_t col = C_SEL_EDGE;
    if (x < bx0) bx0 = x;
    if (x > bx1) bx1 = x;
    if (v0 < by0) by0 = v0;
    if (v1 > by1) by1 = v1;

    // Horizontal rules: only where the visible boundary is the block's own.
    if (v0 == s_selGeo0[x]) base[v0 * W + x] = col;
    if (v1 == s_selGeo1[x]) base[(v1 - 1) * W + x] = col;

    // Vertical rules. A neighbour off the panel counts as empty, so the box
    // closes at the screen edge rather than running off it.
    const int lv0 = (x > 0)     ? s_selVis0[x - 1] : 1;
    const int lv1 = (x > 0)     ? s_selVis1[x - 1] : 0;
    const int rv0 = (x < W - 1) ? s_selVis0[x + 1] : 1;
    const int rv1 = (x < W - 1) ? s_selVis1[x + 1] : 0;

    // The rows a neighbour leaves uncovered are two ranges, not a scan: above
    // where it starts, and below where it ends.
    for (int side = 0; side < 2; ++side) {
      const int n0 = side ? rv0 : lv0;
      const int n1 = side ? rv1 : lv1;
      int a1 = n0 < v1 ? n0 : v1;                 // [v0, min(v1, n0))
      for (int y = v0; y < a1; ++y) base[y * W + x] = col;
      int b0 = n1 > v0 ? n1 : v0;                 // [max(v0, n1), v1)
      for (int y = b0; y < v1; ++y) base[y * W + x] = col;
    }
  }

  if (bx1 < bx0) return;                          // nothing selected on screen

  // Cracks, spreading from the centre of the face as the block gives way. Kept
  // inside the visible extent column by column so they cannot spill past the
  // silhouette onto whatever is in front of it.
  // Cracks. A stipple that spreads across the face as the block gives way,
  // reusing the same grain tile the walls do — a few lines radiating from the
  // centre, which is what this was first, reads as a sparkle drawn over the
  // block rather than as damage to it.
  //
  // Sampled every second pixel in both axes, so the cost is a quarter of the
  // face even when the player is standing close enough for it to fill the
  // panel, which is exactly when they are mining it.
  const int dmg = (int)world::damage(selX, selY);
  if (dmg < 40) return;
  for (int x = bx0; x <= bx1; x += 2) {
    const int v0 = s_selVis0[x], v1 = s_selVis1[x];
    if (v1 - v0 < 4) continue;
    const uint8_t* g = kGrain + ((((int)x >> 1) & 7) << 3);
    uint16_t* col = base + x;
    for (int y = v0 + 1; y < v1 - 1; y += 2)
      if (((int)g[((int)y >> 1) & 7] << 6) + 40 < dmg) col[y * W] = C_SEL_EDGE;
  }
}

void drawWorld(const raycast::Camera& cam, int selX, int selY, int selZ) {
  // Before either core starts: the highlight table covers one material, and
  // both of them read it.
  selectMaterial(selX < 0 ? -1 : (int)world::blockAt(selX, selY, selZ));
#ifdef DEV_SERIAL
  const uint32_t t0 = micros();
#endif
  if (s_worker) {
    s_jobCam = &cam; s_jobSelX = selX; s_jobSelY = selY; s_jobSelZ = selZ;
    // Interleaved, not left half / right half. A contiguous split leaves one
    // core staring at a cliff filling its whole stripe while the other looks
    // at empty sky, and the frame costs whatever the slower half costs.
    // Alternating columns gives both cores a near-identical sample of the view.
    xTaskNotifyGive(s_worker);          // core 0 takes the even columns
    drawColumns(cam, selX, selY, selZ, 1, 2); // core 1 takes the odd ones
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  } else {
    drawColumns(cam, selX, selY, selZ, 0, 1);
  }
#ifdef DEV_SERIAL
  const uint32_t tSel = micros();
#endif
  drawSelBox(selX, selY);
#ifdef DEV_SERIAL
  g_usSel = micros() - tSel;
  g_usWorld = micros() - t0;
#endif
}

// ---- sky --------------------------------------------------------------------

// Sky objects are world-locked, not screen-locked: they sit at a fixed compass
// bearing and slide across the panel as the player turns. A sun painted at a
// fixed screen position would turn with the camera and read as a smudge on the
// lens rather than as something out in the world.
//
// They are clipped against the final occlusion limit rather than drawn before
// the terrain, because the background is filled per column and there is no
// single "sky pass" to draw underneath.
static void skyDisc(const raycast::Camera& cam, float bearing, float elev,
                    int r, uint16_t col) {
  // Bearing into camera space, using the same transform the billboards use —
  // a direction is just a point one unit away, and reusing the proven formula
  // means the sun cannot drift out of step with everything else in the frame.
  const float det = cam.planeX * cam.dy - cam.dx * cam.planeY;
  if (fabsf(det) < 1e-6f) return;
  const float invDet = 1.0f / det;
  const float dirX = cosf(bearing), dirY = sinf(bearing);
  const float tx = invDet * (cam.dy * dirX - cam.dx * dirY);
  const float ty = invDet * (-cam.planeY * dirX + cam.planeX * dirY);
  if (ty < 0.20f) return;                           // behind the camera
  const int cx = (int)((float)W * 0.5f * (1.0f + tx / ty));

  // Elevation is mapped into the band of sky the fixed tilt actually leaves on
  // screen, which is only the rows above the horizon.
  const int cy = (int)cam.horizon - 4 - (int)(elev * 20.0f);

  const int r2 = r * r;
  for (int y = cy - r; y <= cy + r; ++y) {
    if (y < 0 || y >= H) continue;
    for (int x = cx - r; x <= cx + r; ++x) {
      if (x < 0 || x >= W) continue;
      const int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy > r2) continue;
      // Only where the world left sky. Against EVERYTHING, at the farthest
      // bucket, because a sky object is further away than anything that could
      // stand in front of it.
      //
      // limit[] is deliberately ground-only — a mob standing under a bridge is
      // plainly visible and must not be clipped by the deck over its head — so
      // clipping the sun against limit[] alone let it shine straight through
      // every bridge, roof and tree crown on the map. The slab band is the
      // other half of the column, and a sky object has to answer to both.
      if (y >= (int)s_limit[x][raycast::ZBUCKETS - 1]) continue;
      const int shi = (int)s_slabHi[x][raycast::ZBUCKETS - 1];
      const int slo = (int)s_slabLo[x][raycast::ZBUCKETS - 1];
      if (shi < slo && y >= shi && y < slo) continue;
      raw()[y * W + x] = col;
    }
  }
}

void drawSky(const raycast::Camera& cam, float dl) {
#ifdef DEV_SERIAL
  const uint32_t t0 = micros();
  struct Timer { uint32_t t; ~Timer() { g_usSky = micros() - t; } } timer{t0};
#endif
  // One arc for both: the sun climbs through the day, the moon takes the same
  // path at night. Bearing is fixed so they are a usable compass.
  constexpr float BEARING = 0.9f;
  const float arc = dl > 0.0f ? dl : 0.0f;
  const float elev = 0.20f + 0.55f * arc;

  if (dl > 0.05f) {
    skyDisc(cam, BEARING, elev, 7, pack(255, 244, 200));
  } else {
    // Stars first, so the moon sits in front of them. Bearings come from a
    // hash so the field is fixed to the world and turns with the player.
    for (int i = 0; i < 40; ++i) {
      uint32_t h = (uint32_t)(i * 2654435761u);
      h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
      const float b = (float)(h & 1023u) * (6.2832f / 1024.0f);
      const float e = 0.08f + (float)((h >> 10) & 255u) * (0.62f / 255.0f);
      const int bright = 150 + (int)((h >> 18) & 63u);
      skyDisc(cam, b, e, 0, pack(bright, bright, bright + 20 > 255 ? 255 : bright + 20));
    }
    skyDisc(cam, BEARING, 0.45f, 6, pack(226, 232, 244));
  }
}

// ---- mobs -------------------------------------------------------------------

// Mobs are drawn from authored pixel art — see tools/make-sprites.py and the
// generated src/sprites.h — rather than from stacks of coloured rectangles.
//
// The rectangles were the wrong tool twice over. They scaled continuously, so
// every edge crawled as a mob walked toward you and small ones dissolved into
// mush; and a body assembled from nine axis-aligned boxes has no shading, no
// outline that follows its silhouette, and no way to animate without adding
// more boxes. An indexed bitmap has all three for less work per pixel: the
// palette is shaded once per mob and the inner loop is an index and a store.
//
// Nearest-neighbour, with the source column worked out once per destination
// column and the source row stepped in fixed point down it. No filtering: the
// chunky pixels are the point.

constexpr float OUTLINE_H = 10.0f;   // below this, a blob reads better than art

// Where a committed blow stops being a wind-up and becomes the strike. The
// zombie's windup is 16 ticks, so this is the last six of them — a tenth of a
// second of arms-out, which is as long as a strike should ever hang.
constexpr uint16_t ATTACK_STRIKE = 6;

// Palette shaded for one mob at one instant: distance, torch light, and any
// flash it happens to be showing. Small enough to live on the stack, rebuilt
// per drawn mob rather than per band — a table indexed by band would be 768
// bytes rebuilt every frame to save a hundred and ninety lerps.
struct MobPal { uint16_t c[16]; };

static void buildMobPal(MobPal& out, const sprites::MobArt& art,
                        int band, int lit, float flash) {
  for (int i = 1; i < art.colours; ++i) {
    uint8_t r = art.pal[i][0], g = art.pal[i][1], b = art.pal[i][2];
    // A struck mob washes toward white for a few ticks, and a creeper does the
    // same in time with its fuse. Folding it into the palette costs nothing:
    // the blit does not know it happened.
    if (flash > 0.0f) {
      r = mix(r, 255, flash); g = mix(g, 255, flash); b = mix(b, 255, flash);
    }
    out.c[i] = shadeMob(r, g, b, band, lit);
  }
}

void drawMobs(const game::State& s, const raycast::Camera& cam) {
#ifdef DEV_SERIAL
  const uint32_t t0 = micros();
  struct Timer { uint32_t t; ~Timer() { g_usMobs = micros() - t; } } timer{t0};
#endif
  static_assert((int)game::MOB_COUNT == 3, "kMobArt is indexed by MobKind");

  const float det = cam.planeX * cam.dy - cam.dx * cam.planeY;
  if (fabsf(det) < 1e-6f) return;
  const float invDet = 1.0f / det;

  struct Draw {
    float depth, sx, standZ, flash, walk;
    uint8_t kind, lit, hurt;
    uint16_t windup;   // ticks left in a committed blow; picks the attack pose
  };
  Draw list[game::MAX_MOBS];
  int n = 0;

  for (int i = 0; i < game::MAX_MOBS; ++i) {
    const game::Mob& m = s.mobs[i];
    if (!m.alive) continue;
    const float spx = m.x - cam.px, spy = m.y - cam.py;
    const float tx = invDet * (cam.dy * spx - cam.dx * spy);
    const float ty = invDet * (-cam.planeY * spx + cam.planeX * spy);
    if (ty < 0.15f) continue;                       // behind, or on the lens
    // Past the fog there is nothing to see: a mob out there is already banded
    // to the fog colour and drawing it is a sprite's worth of work to paint the
    // background over itself. The cull only became worth writing when the fog
    // came in — at seventeen cells almost nothing was ever beyond it.
    if (ty > raycast::MAX_DIST) continue;
    Draw& d = list[n++];
    d.depth  = ty;
    d.sx     = (float)W * 0.5f * (1.0f + tx / ty);
    d.standZ = (float)m.z;      // whatever it is standing on, terrain or not
    d.kind   = m.kind;
    d.lit    = world::light((int)m.x, (int)m.y);
    d.walk   = m.walk;
    // A mob committing to a blow stands still, so its stride freezes and the
    // walk cycle says nothing about what it is doing. The windup is the only
    // thing that knows, and until now the renderer never saw it: a zombie
    // winding up to hit you looked exactly like a zombie standing there.
    d.windup = m.windup;
    // A struck mob flashes white and jolts. It is the only feedback a hit gives
    // at range, and without it combat is two numbers changing somewhere the
    // player cannot see them.
    // Clamped, not scaled: hitFlash is a tick count and nothing stops it being
    // set higher than the nominal five, so the strength has to saturate rather
    // than run past white and wrap the mix.
    d.hurt   = m.hitFlash > 5 ? 5 : m.hitFlash;
    d.flash  = d.hurt ? 0.30f + 0.55f * (float)d.hurt / 5.0f : 0.0f;
    // Creepers strobe while their fuse burns, faster as it runs out.
    if (m.kind == game::MOB_CREEPER && m.timer && d.flash == 0.0f)
      d.flash = ((m.timer / 5u) & 1u) ? 0.75f : 0.0f;
  }

  // Back to front, so a near mob covers a far one. Insertion sort: n is at
  // most MAX_MOBS and usually a handful.
  for (int i = 1; i < n; ++i) {
    const Draw key = list[i];
    int j = i - 1;
    while (j >= 0 && list[j].depth < key.depth) { list[j + 1] = list[j]; --j; }
    list[j + 1] = key;
  }

  uint16_t* base = raw();
  static int16_t srcX[W];        // destination column -> source column

  for (int i = 0; i < n; ++i) {
    const Draw& d = list[i];
    const sprites::MobArt& art = sprites::kMobArt[d.kind];
    const float invD = (float)raycast::PROJ / d.depth;

    int band = (int)d.depth;
    if (band >= BANDS) band = BANDS - 1;
    if (band < 0) band = 0;

    // Which occlusion sample applies at this distance. The table is filled
    // front to back, so the bucket at or below the mob is the conservative one.
    int bucket = (int)(d.depth / raycast::ZBUCKET_SPAN);
    if (bucket >= raycast::ZBUCKETS) bucket = raycast::ZBUCKETS - 1;
    if (bucket < 0) bucket = 0;

    // Feet on the terrain, not on the horizon: a mob on top of a wall has to
    // draw up there or the whole height system stops being legible.
    float feetRow = (float)cam.horizon + (cam.z - d.standZ) * invD;
    float fullH   = art.worldH * invD;

    // The jolt. A struck mob is knocked up and squashed for a few ticks, which
    // is what a hit looks like; the white flash alone reads as a lighting
    // glitch. Both decay together, so the recoil is over before the next swing.
    if (d.hurt) {
      const float k = (float)d.hurt / 5.0f;
      feetRow -= fullH * 0.10f * k;
      fullH   *= 1.0f - 0.14f * k;
    }
    const float fullW = art.worldH * invD * (float)art.w / (float)art.h;
    if (fullH < 2.0f || fullW < 2.0f) continue;

    const int y0 = (int)(feetRow - fullH), y1 = (int)feetRow;
    const int x0 = (int)(d.sx - fullW * 0.5f), x1 = (int)(d.sx + fullW * 0.5f);
    if (x1 <= x0 || y1 <= y0) continue;

    const int cx0 = x0 < 0 ? 0 : x0, cx1 = x1 > W ? W : x1;
    const int cy0 = y0 < 0 ? 0 : y0, cy1 = y1 > H ? H : y1;
    if (cx0 >= cx1 || cy0 >= cy1) continue;

    MobPal pal;
    buildMobPal(pal, art, band, (int)d.lit, d.flash);

    // Too small for the art to survive being resampled into it: two or three
    // pixels of a face is noise, and noise on a moving object reads as a fault.
    // A solid silhouette in the body colour is honest at that size and is what
    // the eye is using anyway.
    if (fullH < OUTLINE_H) {
      const uint16_t body = pal.c[3];
      for (int x = cx0; x < cx1; ++x) {
        const int lim = (int)s_limit[x][bucket];
        const int cut = cy1 > lim ? lim : cy1;
        for (int y = cy0; y < cut; ++y) base[y * W + x] = body;
      }
      continue;
    }

    // Legs swap every half-block walked, so the stride matches the speed
    // instead of a timer, and two mobs side by side never march in step.
    //
    // Modulo art.walk, not art.frames: the frames past the walk cycle are
    // attack poses, and dealing one out as part of a stride would make a
    // zombie throw a punch every other step.
    int frame = ((int)(d.walk * 2.0f)) % art.walk;
    if (d.windup && art.frames > art.walk) {
      // Wind up, then strike. The blow lands on the tick windup reaches
      // zero, so the arms are out for the tenth of a second before it and
      // drop on the tick it arrives — which is the tick the screen kicks and
      // the hurt flash covers anyway.
      const bool strike = d.windup <= ATTACK_STRIKE;
      frame = art.walk + ((strike && art.frames > art.walk + 1) ? 1 : 0);
    }
    const uint8_t* pix = art.pix + (size_t)frame * art.h * art.w;

    // One divide per mob, not one per pixel.
    const int32_t stepX = (int32_t)(((int32_t)art.w << 16) / (x1 - x0));
    const int32_t stepY = (int32_t)(((int32_t)art.h << 16) / (y1 - y0));

    for (int x = cx0; x < cx1; ++x) {
      int32_t sx = (int32_t)(x - x0) * stepX >> 16;
      if (sx < 0) sx = 0;
      if (sx >= art.w) sx = art.w - 1;
      srcX[x] = (int16_t)sx;
    }

    for (int x = cx0; x < cx1; ++x) {
      // Everything nearer than this mob painted rows from s_limit downward,
      // so the mob can only show above that line. One test per column is
      // exactly the resolution the walker gives us.
      const int lim = (int)s_limit[x][bucket];
      const int cut = cy1 > lim ? lim : cy1;
      if (cy0 >= cut) continue;

      // A nearer overhang owns [hi, lo). Those rows are in front of this mob,
      // so what is left of it is at most the piece above the band and the piece
      // below it — which is exactly what you see of something standing on the
      // far side of a bridge.
      const int hi = (int)s_slabHi[x][bucket];
      const int lo = (int)s_slabLo[x][bucket];
      int a0 = cy0, a1 = cut, b0 = 0, b1 = 0;
      if (hi < lo && hi < cut && lo > cy0) {
        a1 = hi < cy0 ? cy0 : hi;
        b0 = lo > cut ? cut : lo;
        b1 = cut;
      }

      const uint8_t* col = pix + srcX[x];
      for (int part = 0; part < 2; ++part) {
        const int ya = part ? b0 : a0, yb = part ? b1 : a1;
        if (ya >= yb) continue;
        int32_t sy = (int32_t)(ya - y0) * stepY;
        uint16_t* p = base + ya * W + x;
        for (int y = ya; y < yb; ++y, p += W, sy += stepY) {
          int row = sy >> 16;
          if (row >= art.h) row = art.h - 1;
          const uint8_t v = col[row * art.w];
          if (v) *p = pal.c[v];
        }
      }
    }
  }
}

// ---- particles --------------------------------------------------------------
//
// World-space points with a little physics, projected exactly the way mobs and
// the sun are, and clipped against the same per-column occlusion table. They
// are what makes a hit land and a block come apart rather than simply change.
//
// The pool has its own random number generator. It must not touch State::rng:
// that seed is what makes a run reproducible on the host, and letting cosmetics
// draw from it would mean the number of sparks on screen changed the simulation.

struct Particle {
  float    x, y, z;
  float    vx, vy, vz;
  uint16_t life, born;
  uint8_t  r, g, b;      // unshaded: the hour and the fog are applied when drawn
  uint8_t  size;
  uint8_t  gravity;      // 0 = drifts (smoke), 1 = falls (debris)
};

constexpr int MAX_PARTICLES = 96;
static Particle s_part[MAX_PARTICLES];
static uint32_t s_prng = 0x9e3779b9u;

static inline float prand(float lo, float hi) {
  s_prng ^= s_prng << 13; s_prng ^= s_prng >> 17; s_prng ^= s_prng << 5;
  return lo + (hi - lo) * (float)(s_prng & 0xFFFFu) * (1.0f / 65535.0f);
}

static Particle* freeParticle() {
  for (int i = 0; i < MAX_PARTICLES; ++i)
    if (!s_part[i].life) return &s_part[i];
  return nullptr;      // full: the burst is simply smaller, never queued
}

void emit(const game::Spark& sp) {
  int count = 0;
  float spread = 0.0f, rise = 0.0f, ttl = 0.0f;
  uint8_t size = 1, grav = 1;
  uint8_t r = 220, g = 220, b = 220;

  switch (sp.kind) {
    case game::SP_BREAK: {
      // Shards the colour of the block that broke, so taking coal out of a
      // stone face looks different from taking the face.
      const world::BlockInfo& bi = world::info(sp.mat);
      r = bi.r; g = bi.g; b = bi.b;
      count = 12; spread = 2.6f; rise = 2.4f; ttl = 34.0f; size = 2;
      break;
    }
    case game::SP_HIT:
      r = 255; g = 236; b = 180;
      count = 7; spread = 2.2f; rise = 2.0f; ttl = 14.0f; size = 2;
      break;
    case game::SP_DEATH:
      r = 210; g = 220; b = 230;
      count = 14; spread = 1.6f; rise = 2.6f; ttl = 40.0f; size = 2; grav = 0;
      break;
    case game::SP_BLAST:
      r = 255; g = 168; b = 60;
      count = 22; spread = 4.2f; rise = 4.0f; ttl = 46.0f; size = 3;
      break;
    case game::SP_ARROW:
      r = 200; g = 190; b = 170;
      count = 5; spread = 1.6f; rise = 1.2f; ttl = 16.0f; size = 1;
      break;
    default: return;
  }
  if (sp.mag < 255) count = count * (int)sp.mag / 255;

  for (int i = 0; i < count; ++i) {
    Particle* p = freeParticle();
    if (!p) return;
    p->x = sp.x + prand(-0.12f, 0.12f);
    p->y = sp.y + prand(-0.12f, 0.12f);
    p->z = sp.z + prand(-0.10f, 0.10f);
    p->vx = prand(-spread, spread) / 60.0f;
    p->vy = prand(-spread, spread) / 60.0f;
    p->vz = prand(rise * 0.25f, rise) / 60.0f;
    p->life = p->born = (uint16_t)(ttl + prand(0.0f, ttl * 0.5f));
    p->size = size;
    p->gravity = grav;
    // Each one a little different, or a burst reads as one flat blob.
    const float k = prand(0.72f, 1.15f);
    const int cr = (int)(r * k), cg = (int)(g * k), cb = (int)(b * k);
    p->r = (uint8_t)(cr > 255 ? 255 : cr);
    p->g = (uint8_t)(cg > 255 ? 255 : cg);
    p->b = (uint8_t)(cb > 255 ? 255 : cb);
  }
}

void stepParticles() {
  for (int i = 0; i < MAX_PARTICLES; ++i) {
    Particle& p = s_part[i];
    if (!p.life) continue;
    --p.life;
    p.x += p.vx; p.y += p.vy; p.z += p.vz;
    if (p.gravity) {
      p.vz -= 0.0055f;
      // Settles on whatever it lands on rather than falling through the world.
      const float floorZ = (float)world::height((int)p.x, (int)p.y);
      if (p.z <= floorZ) { p.z = floorZ; p.vz = 0.0f; p.vx *= 0.6f; p.vy *= 0.6f; }
    } else {
      p.vz *= 0.97f;                       // smoke slows and hangs
      p.vx *= 0.97f; p.vy *= 0.97f;
    }
  }
}

void drawParticles(const raycast::Camera& cam) {
  const float det = cam.planeX * cam.dy - cam.dx * cam.planeY;
  if (fabsf(det) < 1e-6f) return;
  const float invDet = 1.0f / det;
  uint16_t* base = raw();

  for (int i = 0; i < MAX_PARTICLES; ++i) {
    const Particle& p = s_part[i];
    if (!p.life) continue;

    const float spx = p.x - cam.px, spy = p.y - cam.py;
    const float tx = invDet * (cam.dy * spx - cam.dx * spy);
    const float ty = invDet * (-cam.planeY * spx + cam.planeX * spy);
    if (ty < 0.20f) continue;
    const float invD = (float)raycast::PROJ / ty;

    const int cx = (int)((float)W * 0.5f * (1.0f + tx / ty));
    const int cy = (int)((float)cam.horizon + (cam.z - p.z) * invD);

    int bucket = (int)(ty / raycast::ZBUCKET_SPAN);
    if (bucket >= raycast::ZBUCKETS) bucket = raycast::ZBUCKETS - 1;
    if (bucket < 0) bucket = 0;

    int sz = (int)((float)p.size * invD * 0.014f);
    if (sz < 1) sz = 1;
    if (sz > 4) sz = 4;

    // Shaded like everything else in the world. Emitting a packed colour meant
    // a shower of debris stayed at full daylight brightness after dark and read
    // as sparks of light rather than as pieces of the block it came off.
    int band = (int)ty;
    if (band >= BANDS) band = BANDS - 1;
    if (band < 0) band = 0;
    const uint16_t col = shadeMob(p.r, p.g, p.b, band,
                                  (int)world::light((int)p.x, (int)p.y));

    const int x0 = cx - sz / 2, x1 = x0 + sz;
    const int y0 = cy - sz / 2, y1 = y0 + sz;
    for (int x = x0 < 0 ? 0 : x0; x < (x1 > W ? W : x1); ++x) {
      // The same clip mobs use: the ground cut, then the slab band. Not the
      // one skyDisc uses — that ignores slabs, and particles would show through
      // a bridge deck.
      const int lim = (int)s_limit[x][bucket];
      const int cut = y1 > lim ? lim : y1;
      const int hi = (int)s_slabHi[x][bucket];
      const int lo = (int)s_slabLo[x][bucket];
      for (int y = y0 < 0 ? 0 : y0; y < cut; ++y) {
        if (y >= H) break;
        if (hi < lo && y >= hi && y < lo) continue;
        base[y * W + x] = col;
      }
    }
  }
}

// Metal for the tool head, darkest shade first, one ramp per tier.
//
// There were six of these once, one per mining upgrade, so buying one was
// visible in the player's hand rather than only on a card the player had
// already closed. The upgrades went away and this collapsed to a single iron
// ramp; tiers are back, and they are back in the same place, because a tier
// you cannot see in your own hand is a number in a menu.
//
// Wood is deliberately not the haft's brown: the haft is already 'e'..'g' in
// the art, and a wooden head in the same brown would erase the silhouette that
// says which end does the work. It is a paler, greyer wood instead.
static const uint8_t kMetal[game::TT_COUNT][4][3] = {
  { {  84,  60,  38 }, { 124,  92,  58 }, { 162, 126,  84 }, { 198, 166, 124 } },  // wood
  { {  62,  64,  70 }, {  96, 100, 108 }, { 134, 138, 146 }, { 176, 180, 188 } },  // stone
  { {  74,  78,  86 }, { 126, 131, 140 }, { 176, 181, 190 }, { 222, 226, 232 } },  // iron
  { {  28, 118, 122 }, {  58, 176, 178 }, { 110, 226, 224 }, { 196, 250, 248 } },  // diamond
};

// ---- dropped items ----------------------------------------------------------

// A small cube, billboarded. Not a sprite: at the size these are drawn -- a few
// pixels a side past four or five cells -- the art would be one colour anyway,
// and taking the block's own colour means a new material needs nothing here.
//
// It bobs. A stationary flat square on ground of a similar colour is genuinely
// hard to find, and the bob is what separates "an item is lying there" from a
// texture artefact, for about six instructions a frame.
void drawDrops(const game::State& s, const raycast::Camera& cam) {
  const float det = cam.planeX * cam.dy - cam.dx * cam.planeY;
  if (fabsf(det) < 1e-6f) return;
  const float invDet = 1.0f / det;
  uint16_t* base = raw();

  for (int i = 0; i < game::MAX_DROPS; ++i) {
    const game::Drop& d = s.drops[i];
    if (!d.alive) continue;

    // Bobbing is driven by the item's own remaining life, so two drops side by
    // side are out of phase instead of pulsing as one block.
    const float bob = d.rest
        ? 0.055f * sinf((float)d.life * 0.06f + (float)i)
        : 0.0f;

    const float spx = d.x - cam.px, spy = d.y - cam.py;
    const float tx = invDet * (cam.dy * spx - cam.dx * spy);
    const float ty = invDet * (-cam.planeY * spx + cam.planeX * spy);
    if (ty < 0.20f) continue;
    const float invD = (float)raycast::PROJ / ty;

    const int cx = (int)((float)W * 0.5f * (1.0f + tx / ty));
    const int cy = (int)((float)cam.horizon
                         + (cam.z - (d.z + 0.22f + bob)) * invD);

    int bucket = (int)(ty / raycast::ZBUCKET_SPAN);
    if (bucket >= raycast::ZBUCKETS) bucket = raycast::ZBUCKETS - 1;
    if (bucket < 0) bucket = 0;

    int sz = (int)(0.32f * invD);
    if (sz < 2) sz = 2;
    if (sz > 26) sz = 26;

    int band = (int)ty;
    if (band >= BANDS) band = BANDS - 1;
    if (band < 0) band = 0;
    const int lit = (int)world::light((int)d.x, (int)d.y);

    // A tool takes its tier's mid metal; a block takes its own colour. Both go
    // through the world's shading, or an item would sit at full daylight
    // brightness after dark and read as a lamp.
    uint8_t r, g, b;
    if (game::isTool(d.item)) {
      const uint8_t* m = kMetal[game::toolTier(d.item)][2];
      r = m[0]; g = m[1]; b = m[2];
    } else {
      const world::BlockInfo& bi = world::info(d.item);
      r = bi.r; g = bi.g; b = bi.b;
    }
    const uint16_t body = shadeMob(r, g, b, band, lit);
    // A lid two shades up, so the thing reads as a cube with a top rather than
    // as a flat chip standing on the ground.
    auto up = [](uint8_t v) { const int q = v + 46; return (uint8_t)(q > 255 ? 255 : q); };
    const uint16_t lid = shadeMob(up(r), up(g), up(b), band, lit);
    const int lidH = sz / 4;

    const int x0 = cx - sz / 2, x1 = x0 + sz;
    const int y0 = cy - sz / 2, y1 = y0 + sz;
    for (int x = x0 < 0 ? 0 : x0; x < (x1 > W ? W : x1); ++x) {
      // The clip mobs and particles use: the ground cut, then the slab band.
      const int lim = (int)s_limit[x][bucket];
      const int cut = y1 > lim ? lim : y1;
      const int hi = (int)s_slabHi[x][bucket];
      const int lo = (int)s_slabLo[x][bucket];
      for (int y = y0 < 0 ? 0 : y0; y < cut; ++y) {
        if (y >= H) break;
        if (hi < lo && y >= hi && y < lo) continue;
        base[y * W + x] = (y < y0 + lidH) ? lid : body;
      }
    }
  }
}

// ---- the tool ---------------------------------------------------------------
//
// The pickaxe is authored pixel art now (tools/make-sprites.py), not a shape
// assembled at runtime from rotated squares. The old one had a correct
// silhouette and nothing inside it: one flat colour for the head, one for the
// haft, and a single lit edge. Art with an outline and four steps of metal
// reads as a tool made of something.
//
// Rotation happens in *texel* space, not pixel space. Each source texel becomes
// a solid SCALE x SCALE block on the panel, so the chunky pixels stay chunky
// through the whole swing instead of dissolving into a resampled smear —
// rotating the pixels would throw away exactly what makes it pixel art.

// Offsets only. The tool is never rotated — see drawTool for why — so the third
// column is what the arc *would* have leaned by, kept because it is what shapes
// the dx/dy curve and reads as the swing's phase.
struct SwingFrame { int8_t dx, dy, ang; };

// Hand-authored rather than generated from a curve: a swing is not symmetric.
// It winds up slowly, strikes in three frames, and recovers unhurried, and that
// asymmetry is the whole reason it reads as effort.
static const SwingFrame kSwing[game::TOOL_ANIM] = {
  {  0,   0,   0 }, {  2,  -3,   6 }, {  5,  -6,  13 }, {  7,  -9,  20 },
  {  8, -11,  26 }, {  4,  -4,   4 }, { -6,   8, -22 }, { -13, 16, -38 },
  { -11, 14, -32 }, { -8,  10, -22 }, { -5,   6, -14 }, { -3,   4,  -8 },
  { -2,   2,  -4 }, { -1,   1,  -2 }, {  0,   1,  -1 }, {  0,   0,   0 },
};


// Both tools go through the same blit: same size, same palette layout, and the
// art files agree that 'a'..'d' are the four metal steps. Only the texel array
// differs, so the caller picks that and everything below is shared.
// The shared blit below indexes both families through the pickaxe's constants,
// which is only sound while the two agree. Say so here rather than discovering
// it as a torn sprite: make-sprites.py is free to resize a family, and this is
// the line that stops it doing so silently.
static_assert(sprites::SWORD_W == sprites::PICK_W &&
              sprites::SWORD_H == sprites::PICK_H &&
              sprites::SWORD_COLOURS == sprites::PICK_COLOURS,
              "sword and pick art must share dimensions and palette size");

static void drawToolArt(const SwingFrame& f, uint8_t kind, uint8_t tier) {

  // Two panel pixels per texel, against three for the 24x24 art this replaced.
  // The art is 32x32 now, so the same three would have put a 96-pixel tool on a
  // 135-pixel panel and left the player looking at their own pickaxe. Two lands
  // it at 64, slightly smaller than the 72 it used to be, and the extra eight
  // texels a side carry the detail the bigger ones were standing in for.
  //
  // The swing offsets below are in panel pixels and do not scale, so the arc is
  // exactly the size it was.
  //
  // The anchor is up and left of the corner: the head sits in frame and the
  // haft runs off the bottom edge, which is where a handle should go — into the
  // hand holding it.
  constexpr int SCALE = 2;
  const int ax = 168 + (int)f.dx * 3 / 2;   // where the art's centre lands
  // Low on the panel. The head is in the top-left of the art, so the anchor
  // sits well below where the head ends up: at 108 the art's centre is past the
  // bottom edge and what is in frame is the head and the top of the haft, which
  // is all a first-person tool should show. Lower than this and the downswing
  // takes the head off the bottom of the screen entirely.
  const int ay = 108 + (int)f.dy * 3 / 2;

  // Palette for this frame: the art's own colours, with the four metal steps
  // swapped for the head's. Twelve entries, built once per frame.
  const bool sword = (kind == game::TK_SWORD);
  const uint8_t (*art)[32] = sword ? sprites::kSword[0] : sprites::kPick[0];
  const uint8_t (*srcPal)[3] = sword ? sprites::kSwordPal : sprites::kPickPal;

  uint16_t pal[sprites::PICK_COLOURS];
  for (int i = 1; i < sprites::PICK_COLOURS; ++i)
    pal[i] = pack(srcPal[i][0], srcPal[i][1], srcPal[i][2]);
  for (int k = 0; k < 4; ++k)                      // 'a'..'d' are the metal
    pal[2 + k] = pack(kMetal[tier][k][0], kMetal[tier][k][1], kMetal[tier][k][2]);

  // The swing leans as well as moves, which it did not used to.
  //
  // Three ways of leaning it were tried before and all three were worse than
  // not leaning it: rotating and walking the *source* drops destination pixels
  // at any angle off ninety degrees, so a one-texel outline tears open and the
  // tool comes apart mid-swing; walking the source with oversized blocks closes
  // the holes and smears the edges into lumps; and a per-row shear keeps the
  // texels square but slides the head sideways off its own handle.
  //
  // What follows is the fourth way, and the difference is the direction. It
  // walks the *destination* — every panel pixel the tool could cover — and asks
  // each one which source texel it came from, by rotating it backwards. A hole
  // is then impossible by construction: every pixel in the box is written or
  // deliberately skipped, because the loop is over the pixels being filled
  // rather than over the texels doing the filling. Nearest-neighbour, so no
  // colour is invented and the palette stays exactly the nine entries above.
  //
  // f.ang was already in the table and had never been read.
  const int hw = sprites::PICK_W / 2, hh = sprites::PICK_H / 2;

  // The backwards rotation, in 16.16, with the texel-to-pixel scale folded in
  // so the inner loop divides by nothing.
  const float rad = (float)f.ang * 0.017453292f;
  const int32_t cs = (int32_t)(cosf(rad) * (65536.0f / SCALE));
  const int32_t sn = (int32_t)(sinf(rad) * (65536.0f / SCALE));

  // How far the leaned sprite can reach from its centre: half its width times
  // |cos| + |sin|. Computed rather than fixed at the diagonal because most
  // frames are barely leaned and the whole box is scanned — an upright tool
  // should not pay for the slack a fully leaned one needs.
  const int32_t acs = cs < 0 ? -cs : cs, asn = sn < 0 ? -sn : sn;
  const int R = (int)(((int32_t)(sprites::PICK_W * SCALE / 2)
                       * (acs + asn) * SCALE) >> 16) + 2;

  const int x0 = ax - R < 0 ? 0 : ax - R, x1 = ax + R > W ? W : ax + R;
  const int y0 = ay - R < 0 ? 0 : ay - R, y1 = ay + R > H ? H : ay + R;

  uint16_t* base = raw();
  for (int py = y0; py < y1; ++py) {
    const int dy = py - ay;
    // Both source coordinates are linear in px, so the two multiplies happen
    // once a row and the inner loop is an add each.
    int32_t u = (int32_t)(x0 - ax) * cs + (int32_t)dy * sn;
    int32_t v = (int32_t)dy * cs - (int32_t)(x0 - ax) * sn;
    uint16_t* row = base + py * W;
    for (int px = x0; px < x1; ++px, u += cs, v -= sn) {
      // Unsigned compare catches negative and past-the-end in one test.
      const unsigned sx = (unsigned)((u >> 16) + hw);
      const unsigned sy = (unsigned)((v >> 16) + hh);
      if (sx >= (unsigned)sprites::PICK_W || sy >= (unsigned)sprites::PICK_H)
        continue;
      const uint8_t val = art[sy][sx];
      if (val) row[px] = pal[val];
    }
  }
}

// A block in the hand. Not art: a cube drawn from the material's own colour,
// three faces of it, on the same swing arc the pickaxe rides. Authoring thirteen
// held-block sprites to say "you are holding dirt" would be thirteen things to
// keep in step with the palette, and the palette is already the answer.
static void drawHeldBlock(const SwingFrame& f, uint8_t mat) {
  // The material's OWN texture, sampled the way a wall samples it, not a flat
  // fill of its average colour. A held block that is a plain orange square
  // while the wall in front of you is patterned brick reads as a different
  // material — and the whole job of the thing in your hand is to tell you what
  // you are about to place.
  const int m = mat < world::B_COUNT ? mat : (int)world::B_STONE;
  auto texel = [&](int u, int v, int shift) {
    const uint8_t k = textures::kTexel[m][u & (textures::TEX_N - 1)]
                                        [v & (textures::TEX_N - 1)];
    const uint8_t* c = textures::kTexPal[m][k];
    auto lit = [](int x, int d) {
      const int q = x + d;
      return (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
    };
    return pack(lit(c[0], shift), lit(c[1], shift), lit(c[2], shift));
  };

  // Low and to the right, the same corner the pickaxe's haft comes out of, so
  // switching slots does not move the hand across the screen.
  constexpr int N = 40, SKEW = 10;
  const int x0 = 176 + (int)f.dx * 3 / 2;
  const int y0 =  96 + (int)f.dy * 3 / 2;

  // Three faces, each sampling the texture on its own two axes so the grain
  // turns the corner instead of being painted flat across it. Face shading
  // follows shadeFor's own ordering: the lid catches the sky, the front falls
  // away from it, the right cheek falls further.
  uint16_t* base = raw();

  // The lid: a parallelogram sliding one pixel left per row, which is as much
  // isometry as a forty-pixel cube needs.
  for (int r = 0; r < SKEW; ++r) {
    const int off = SKEW - r;
    const int py = y0 + r;
    if (py < 0 || py >= H) continue;
    for (int c = 0; c < N; ++c) {
      const int px = x0 + off + c;
      if (px < 0 || px >= W) continue;
      base[py * W + px] = texel(c * textures::TEX_N / N,
                                r * textures::TEX_N / SKEW, 34);
    }
  }
  for (int r = 0; r < N; ++r) {
    const int py = y0 + SKEW + r;
    if (py < 0 || py >= H) continue;
    for (int c = 0; c < N; ++c) {
      const int px = x0 + c;
      if (px < 0 || px >= W) continue;
      base[py * W + px] = texel(c * textures::TEX_N / N,
                                r * textures::TEX_N / N, 0);
    }
  }
  // The right cheek. A parallelogram of constant width, sheared upward by the
  // same one pixel per column the lid slides by — the two share the edge from
  // (x0+N, y0+SKEW) up to (x0+N+SKEW, y0), so they have to rise together.
  //
  // Walked by COLUMN rather than by row, which is what fixes the shape: driving
  // it from the front face's rows and shortening each one turned the side into
  // a triangle tapering to nothing, so the cube had a lid and a face and then
  // just stopped.
  for (int c = 0; c < SKEW; ++c) {
    const int px = x0 + N + c;
    if (px < 0 || px >= W) continue;
    const int yTop = y0 + SKEW - 1 - c;
    for (int r = 0; r < N; ++r) {
      const int py = yTop + r;
      if (py < 0 || py >= H) continue;
      base[py * W + px] = texel(c * textures::TEX_N / SKEW,
                                r * textures::TEX_N / N, -44);
    }
  }

  // One dark rule where the lid meets the face. Without it the two planes read
  // as a gradient rather than as a corner.
  const world::BlockInfo& bi = world::info((uint8_t)m);
  auto dim = [](int v) { const int q = v - 80; return (uint8_t)(q < 0 ? 0 : q); };
  rect(x0, y0 + SKEW, N, 1, pack(dim(bi.r), dim(bi.g), dim(bi.b)));
}

// An empty hand. Also not art: a fist is a rounded block of skin with a darker
// rule down it, and at this size that is all of one that survives anyway.
static void drawHand(const SwingFrame& f) {
  const uint16_t skin = pack(226, 174, 132);
  const uint16_t shade = pack(178, 128,  92);
  const uint16_t cuff  = pack( 96, 132, 196);

  const int x0 = 184 + (int)f.dx * 3 / 2;
  const int y0 = 100 + (int)f.dy * 3 / 2;

  rect(x0 + 3, y0,      26, 4,  skin);        // knuckles, tucked in a little
  rect(x0,     y0 + 4,  32, 22, skin);
  rect(x0,     y0 + 26, 32, H - (y0 + 26), cuff);   // sleeve, off the bottom
  // The fingers, as three rules rather than three shapes.
  for (int k = 0; k < 3; ++k) rect(x0 + 7 + k * 8, y0 + 4, 1, 14, shade);
}

// What is in the selected slot is what is in the hand. This is the item system
// where the player actually reads it — the hotbar says what is selected, and
// this says it again in the place they are already looking.
void drawTool(const game::State& s) {
  const SwingFrame& f = kSwing[s.toolPhase % game::TOOL_ANIM];
  const uint8_t held = game::heldItem(s);
  if (game::isTool(held))         drawToolArt(f, game::toolKind(held),
                                              game::toolTier(held));
  else if (held < world::B_COUNT) drawHeldBlock(f, held);
  else                            drawHand(f);
}

void drawHurt(const game::State& s) {
  if (!s.hurtFlash) return;
  // A border rather than a full-screen blend: 32k blends would cost more than
  // the whole wall pass, and at this panel size the frame reads just as loudly.
  const int t = 1 + (int)s.hurtFlash / 4;
  const uint16_t c = pack(200, 40, 40);
  frameRect(0, 0, W, H, t, c);
}

// ---- present ----------------------------------------------------------------

void present() {
  // The transaction is held open across frames. startWrite/endWrite are
  // reference-counted in LovyanGFX, so the endWrite() inside pushImageDMA only
  // decrements the count instead of ending the transfer and waiting on it --
  // which is what lets the next frame's CPU work run while this frame is still
  // going out over SPI. Nothing else shares this bus, so holding CS is safe.
  if (!s_held) { s_disp->startWrite(); s_held = true; }
  s_disp->pushImageDMA(0, 0, W, H, (const lgfx::swap565_t*)s_buf[s_cur]);
  s_cur ^= 1;
}

void flush() {
  if (s_held) { s_disp->endWrite(); s_held = false; }
  s_disp->waitDMA();
}

const uint16_t* buildBuffer() { return raw(); }

}  // namespace render
