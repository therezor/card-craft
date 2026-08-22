// =============================================================================
//  ui.cpp — HUD and cards
//
//  Palette note: every colour is a compile-time render::pack(), so the whole
//  table folds into constants and the HUD costs nothing but the rectangles it
//  actually draws.
// =============================================================================
#include "ui.h"

#include <stdio.h>

#include "hal/hal.h"
#include "raycast.h"
#include "render.h"
#include "sprites.h"
#include "world.h"

namespace ui {

using render::pack;

constexpr uint16_t C_HUD_BG   = pack( 12,  14,  20);   // #0C0E14
constexpr uint16_t C_HUD_LINE = pack( 34,  40,  52);   // #222834

// Hearts, in Minecraft's four tones: an almost-black outline, the red, a paler
// shine in the upper left, and a hollow socket for the ones you have lost.
constexpr uint16_t C_HEART_ED = pack( 14,  10,  12);   // #0E0A0C  outline
constexpr uint16_t C_HEART    = pack(216,  40,  40);   // #D82828
constexpr uint16_t C_HEART_HL = pack(255, 128, 128);   // #FF8080
constexpr uint16_t C_HEART_MT = pack( 46,  34,  38);   // #2E2226  empty socket

// Hotbar furniture. Minecraft's bar is a pale grey frame around dark cells with
// a white selection box that overhangs its slot on every side; this is that,
// dimmed to sit on a panel that is mostly night.
constexpr uint16_t C_SLOT_BG  = pack( 26,  28,  34);   // #1A1C22  cell ground
constexpr uint16_t C_SLOT_ED  = pack( 92,  96, 106);   // #5C606A  cell frame
constexpr uint16_t C_SLOT_SEL = pack(248, 250, 255);   // #F8FAFF  selection box
constexpr uint16_t C_TEXT     = pack(226, 230, 238);   // #E2E6EE
constexpr uint16_t C_DIM      = pack(128, 136, 152);   // #808898
constexpr uint16_t C_ACCENT   = pack( 96, 200, 240);   // #60C8F0
constexpr uint16_t C_CARD     = pack( 14,  17,  25);   // #0E1119  card ground
constexpr uint16_t C_CARD_ED  = pack( 60,  70,  92);   // #3C465C  card edge
constexpr uint16_t C_SEL      = pack( 38,  74, 110);   // #264A6E  selected option
constexpr uint16_t C_GOOD     = pack(110, 214, 120);   // #6ED678  affordable
constexpr uint16_t C_CROSS    = pack(236, 240, 248);   // #ECF0F8
constexpr uint16_t C_PROG     = pack(250, 214,  96);   // #FAD660  mining progress
constexpr uint16_t C_TITLE_BG = pack( 10,  14,  24);   // #0A0E18
constexpr uint16_t C_DEAD     = pack(226,  74,  74);   // #E24A4A
constexpr uint16_t C_FAINT    = pack( 58,  64,  80);   // #3A4050

constexpr int W = render::W;
constexpr int H = render::H;

// ---- HUD --------------------------------------------------------------------

// A heart, 9x9, as Minecraft draws it: two lobes over a point, outlined so it
// reads against grass as well as against a night sky. 0 is transparent, 1 the
// outline, 2 the body, 3 the shine.
//
// Spelled out as a picture rather than generated from a curve, because at nine
// pixels a heart is a specific arrangement of about forty of them and there is
// no formula that gets the notch right. Squares were what this used to draw,
// and a red square next to a red square is a bar, not a life.
constexpr int HEART_PX = 9;
static const uint8_t kHeart[HEART_PX][HEART_PX] = {
  { 0,1,1,0,0,0,1,1,0 },
  { 1,3,3,1,0,1,2,2,1 },
  { 1,3,3,3,1,2,2,2,1 },
  { 1,3,3,2,2,2,2,2,1 },
  { 1,3,2,2,2,2,2,2,1 },
  { 0,1,2,2,2,2,2,1,0 },
  { 0,0,1,2,2,2,1,0,0 },
  { 0,0,0,1,2,1,0,0,0 },
  { 0,0,0,0,1,0,0,0,0 },
};

static void heart(int x, int y, bool full) {
  const uint16_t body = full ? C_HEART    : C_HEART_MT;
  const uint16_t hi   = full ? C_HEART_HL : C_HEART_MT;
  for (int r = 0; r < HEART_PX; ++r)
    for (int c = 0; c < HEART_PX; ++c) {
      const uint8_t k = kHeart[r][c];
      if (!k) continue;
      render::rect(x + c, y + r, 1, 1,
                   k == 1 ? C_HEART_ED : (k == 2 ? body : hi));
    }
}

// A block as an item: the flat swatch plus a lit top edge and a shaded right
// edge. Two extra rectangles, and they are the difference between a coloured
// square and something that reads as a cube you could pick up.
// mul is a 0..256 fixed-point scale on the whole swatch. Greying an icon by
// darkening it rather than by drawing it in one flat grey is what keeps an
// unaffordable recipe still *readable* as the material it wants -- the player
// needs to know they are short of stone, not merely that they are short.
static void blockIcon(int x, int y, int n, uint8_t mat, int mul = 256) {
  const world::BlockInfo& bi = world::info(mat);
  const int lift = 60, drop = 55;
  auto sc = [mul](int v) {
    const int q = v * mul / 256;
    return (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
  };
  render::rect(x, y, n, n, pack(sc(bi.r), sc(bi.g), sc(bi.b)));
  render::rect(x, y, n, 3, pack(sc(bi.r + lift), sc(bi.g + lift), sc(bi.b + lift)));
  render::rect(x + n - 3, y + 3, 3, n - 3,
               pack(sc(bi.r - drop), sc(bi.g - drop), sc(bi.b - drop)));
}

// A tool, from the same art the first-person view uses, taken every other
// texel: the art is 32x32 and the icon is 16x16. The art is chunky enough to
// survive it, and a second hand-drawn icon would be a second thing to keep in
// step with the first.
//
// The tier is a whole palette rather than four swapped indices, so this picks
// one and reads it straight — the hand-kept kIconMetal twin of render.cpp's
// table is gone with it.
static void toolIcon(int x, int y, uint8_t kind, uint8_t tier, int mul = 256) {
  const bool sword = (kind == game::TK_SWORD);
  const uint8_t (*art)[sprites::PICK_W] =
      sword ? sprites::kSword[0] : sprites::kPick[0];
  const uint8_t (*srcPal)[3] = sword ? sprites::kSwordTierPal[tier]
                                     : sprites::kPickTierPal[tier];
  auto sc = [mul](int v) {
    const int q = v * mul / 256;
    return (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
  };

  for (int r = 0; r < 16; ++r)
    for (int c = 0; c < 16; ++c) {
      const uint8_t k = art[r * 2][c * 2];
      if (!k) continue;
      const uint8_t* rgb = srcPal[k];
      render::rect(x + c, y + r, 1, 1, pack(sc(rgb[0]), sc(rgb[1]), sc(rgb[2])));
    }
}

// Any recipe result -- material, tool, or the heal -- at 16x16. The one place
// that knows a result byte can be any of the three, so the book and the card do
// not each have to.
//
// The heal is why this takes ITEM_NONE rather than just refusing it. PATCH
// produces no item at all, and drawing nothing for it left one row of the book
// as an arrow pointing at an empty slot -- which reads as a bug, not as "this
// one gives you hearts back".
static void itemIcon(int x, int y, uint8_t item, int mul = 256) {
  if (item == game::ITEM_NONE) {
    heart(x + 4, y + 4, mul >= 200);
    return;
  }
  if (game::isTool(item))
    toolIcon(x, y, game::toolKind(item), game::toolTier(item), mul);
  else if (item < world::B_COUNT)
    blockIcon(x, y, 16, item, mul);
}

// What a recipe makes, as a count: items for most, hearts for the heal. Zero
// where there is nothing worth labelling.
static uint8_t resultQty(const game::RecipeInfo& r) {
  return (r.outItem == game::ITEM_NONE) ? r.heal : r.outQty;
}

// How much of a tool is left, as a rule across the foot of its slot. Green to
// red rather than a shrinking green bar alone: on a sixteen-pixel cell the
// length is only a few pixels of difference, and colour is what carries at a
// glance. Drawn only when the tool is actually worn, so a fresh one is not
// wearing a status bar it has nothing to say with.
static void durabilityBar(int x, int y, int w, uint16_t left, uint16_t full) {
  if (!full || left >= full) return;
  const int lit = (int)((uint32_t)left * w / full);
  render::rect(x, y, w, 1, pack(40, 40, 46));
  if (lit > 0) {
    const int t = (int)((uint32_t)left * 255u / full);       // 0..255 remaining
    render::rect(x, y, lit, 1,
                 pack((uint8_t)(t < 128 ? 230 : 2 * (255 - t)),
                      (uint8_t)(t < 128 ? 2 * t : 220), 48));
  }
}

void hud(const game::State& s) {
  // Hearts on top, hotbar under them, both centred on the same 160-pixel run so
  // the two read as one panel rather than as two widgets that happen to share
  // an edge.
  constexpr int SLOT = 16, GAP = 2;
  constexpr int BARW = game::SLOT_N * SLOT + (game::SLOT_N - 1) * GAP;
  constexpr int BARX = (W - BARW) / 2;
  const int barY = H - SLOT - 3;

  // -- hearts
  const int hearts = s.maxHp > 10 ? 10 : s.maxHp;
  for (int i = 0; i < hearts; ++i)
    heart(BARX + i * (HEART_PX + 1), barY - HEART_PX - 3, i < s.hp);

  // -- the night, small and out of the way. Not a clock — the sky is the clock
  // — but the score is built on it and a run wants to know how far it got.
  char buf[16];
  snprintf(buf, sizeof(buf), "N%u", (unsigned)s.night);
  render::text(W - 3 - render::textWidth(buf), 3, buf, C_FAINT, 1);

  // -- hotbar
  for (int i = 0; i < game::SLOT_N; ++i) {
    const int sx = BARX + i * (SLOT + GAP);
    const uint8_t it = s.slot[i];

    render::rect(sx, barY, SLOT, SLOT, C_SLOT_BG);
    render::frameRect(sx, barY, SLOT, SLOT, 1, C_SLOT_ED);

    if (game::isTool(it)) {
      toolIcon(sx, barY, game::toolKind(it), game::toolTier(it));
      durabilityBar(sx + 2, barY + SLOT - 3, SLOT - 4, s.dur[i],
                    game::toolInfo(game::toolKind(it),
                                   game::toolTier(it)).durability);
    } else if (it < world::B_COUNT) {
      blockIcon(sx + 2, barY + 2, SLOT - 4, it);
      // The count, bottom right, and only past one — a stack of one is a thing
      // you can see, and a "1" on every slot is noise on a bar this small.
      if (s.inv[it] > 1) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)s.inv[it]);
        const int tw = render::textWidth(buf);
        render::text(sx + SLOT - 1 - tw, barY + SLOT - 7, buf, C_TEXT, 1);
      }
    }

    // The selection box overhangs its slot on every side, which is what makes
    // it read as a thing sitting on top of the bar rather than as one cell
    // painted a different colour.
    if (i == s.sel)
      render::frameRect(sx - 1, barY - 1, SLOT + 2, SLOT + 2, 1, C_SLOT_SEL);
  }
}

void crosshair(const game::State& s) {
  // Not the middle of the panel: the aim ray is steeper than the view centre
  // so that it meets flat ground a few cells ahead, and the reticle has to sit
  // where that ray actually projects or the two disagree about what you hit.
  const int cx = W / 2, cy = raycast::crosshairRow();

  // A gapped cross rather than a dot: a dot disappears against pale stone, and
  // the gap keeps the centre pixel showing the block underneath it.
  render::rect(cx - 4, cy, 3, 1, C_CROSS);
  render::rect(cx + 2, cy, 3, 1, C_CROSS);
  render::rect(cx, cy - 4, 1, 3, C_CROSS);
  render::rect(cx, cy + 2, 1, 3, C_CROSS);

  if (s.aimDamage) {
    // Mining progress sits under the crosshair, where the eye already is.
    const int bw = 22;
    const int fx = (int)s.aimDamage * bw / 255;
    render::rect(cx - bw / 2, cy + 9, bw, 3, C_HUD_BG);
    render::rect(cx - bw / 2, cy + 9, fx, 3, C_PROG);
  }
}

// ---- cards ------------------------------------------------------------------

static void cardGround(int x, int y, int w, int h) {
  render::rect(x, y, w, h, C_CARD);
  render::frameRect(x, y, w, h, 1, C_CARD_ED);
}

void title(const char* board, uint32_t best) {
  render::fill(C_TITLE_BG);

  // A strip of the actual block palette as the logo: it says what the game is
  // made of before a single frame of world has been drawn.
  static const uint8_t kStrip[6] = { world::B_GRASS, world::B_DIRT, world::B_STONE,
                                     world::B_WOOD,  world::B_COAL, world::B_IRON };
  for (int i = 0; i < 6; ++i) {
    const world::BlockInfo& bi = world::info(kStrip[i]);
    render::rect(48 + i * 24, 16, 22, 14, pack(bi.r, bi.g, bi.b));
  }

  render::textCentred(W / 2, 40, "CARD CRAFT", C_TEXT, 2);
  render::textCentred(W / 2, 58, "SURVIVE THE NIGHT", C_ACCENT, 1);

  char buf[40];
  snprintf(buf, sizeof(buf), "BEST %u", (unsigned)best);
  render::textCentred(W / 2, 72, buf, C_DIM, 1);

  // Right hand on the arrows, left hand on E and D — the hints are laid out
  // the way the hands are, so the split reads at a glance.
  const hal::Caps& c = hal::caps();
  snprintf(buf, sizeof(buf), "%s MOVE  %s TURN", c.kMove, c.kTurn);
  render::textCentred(W / 2, 88, buf, C_DIM, 1);
  if (c.kLook)
    snprintf(buf, sizeof(buf), "%s MINE  %s BUILD  %s LOOK", c.kAct, c.kBuild, c.kLook);
  else
    snprintf(buf, sizeof(buf), "%s MINE  %s BUILD", c.kAct, c.kBuild);
  render::textCentred(W / 2, 98, buf, C_DIM, 1);
  snprintf(buf, sizeof(buf), "%s CRAFT  %s SLOT  %s MENU", c.kCraft, c.kCycle, c.kBack);
  render::textCentred(W / 2, 107, buf, C_DIM, 1);
  if (c.kDrop)
    snprintf(buf, sizeof(buf), "1-9 PICK ITEM  %s DROP", c.kDrop);
  else
    snprintf(buf, sizeof(buf), "1-9 PICK ITEM");
  render::textCentred(W / 2, 116, buf, C_DIM, 1);

  snprintf(buf, sizeof(buf), "PRESS %s", c.kConfirm);
  render::textCentred(W / 2, 126, buf, C_GOOD, 1);
  render::text(3, 3, board, C_FAINT, 1);
}

// ---- menus ------------------------------------------------------------------

void Menu::open(const char* title, const MenuItem* items, int count) {
  title_ = title;
  items_ = items;
  count_ = count;
  if (index_ >= count_) index_ = 0;
}

void Menu::move(int delta) {
  if (count_ <= 0) return;
  index_ = (index_ + delta % count_ + count_) % count_;
}

void Menu::draw(const char* footer) const {
  if (!items_ || count_ <= 0) return;

  // The card is sized to its contents rather than to the panel, so a two-item
  // pause menu does not leave a hole where a third row would have been.
  //
  // Four rows is the ceiling: at 22 px a row plus the gaps, the heading and the
  // footer, four comes to 127 of the 135 the panel has and a fifth does not
  // fit. Lists longer than that scroll a window over themselves rather than
  // shrinking the rows -- the font is 7 px tall and there is nothing to give.
  const int rowH = 22, gap = 3;
  const int shown = count_ < MAX_ROWS ? count_ : MAX_ROWS;
  const int bodyH = shown * rowH + (shown - 1) * gap;
  const int cardH = 16 + bodyH + (footer ? 14 : 6);
  const int top = (H - cardH) / 2;
  const int bx = 12, bw = W - 24;

  // The window follows the cursor and then stops at either end, so the last
  // page does not scroll past the final row into empty space.
  int first = index_ - shown / 2;
  if (first > count_ - shown) first = count_ - shown;
  if (first < 0) first = 0;

  cardGround(bx - 6, top, bw + 12, cardH);
  if (title_) render::textCentred(W / 2, top + 6, title_, C_ACCENT, 1);

  for (int k = 0; k < shown; ++k) {
    const int i = first + k;
    const MenuItem& it = items_[i];
    const int by = top + 16 + k * (rowH + gap);
    const bool sel = (i == index_);

    render::rect(bx, by, bw, rowH, sel ? C_SEL : C_CARD);
    if (sel) {
      render::frameRect(bx, by, bw, rowH, 1, C_ACCENT);
      // A caret as well as the highlight: on a panel this small the fill alone
      // is easy to lose against a bright world showing through behind the card.
      render::text(bx + 3, by + (it.detail ? 3 : 8), "\x1a", C_ACCENT, 1);
    }

    const uint16_t fg = it.enabled ? C_TEXT : C_DIM;
    render::text(bx + 11, by + (it.detail ? 3 : 8), it.label, fg, 1);
    if (it.detail) render::text(bx + 11, by + 12, it.detail, C_DIM, 1);
  }

  // That there is more in either direction, said in the right margin. Without
  // it a windowed list looks exactly like a complete one that happens to be
  // four long, and the player has no reason to keep pressing.
  if (first > 0)
    render::text(bx + bw - 7, top + 17, "\x18", C_ACCENT, 1);
  if (first + shown < count_)
    render::text(bx + bw - 7, top + 16 + bodyH - 8, "\x19", C_ACCENT, 1);

  if (footer) render::textCentred(W / 2, top + cardH - 11, footer, C_DIM, 1);
}

// ---- the crafting card ------------------------------------------------------

// Four cells, an arrow, the result, and a row to open the book. Not a Menu: a
// grid is not a row list, and bending one into the other would have cost more
// than the sixty lines below.
//
// Every one of those is a cursor stop, which is the whole point. The card used
// to need four keys -- arrows to move, E to fill a cell, D to commit, S for the
// book -- and a player had to be told which. Now the cursor goes to the thing
// and ENTER does whatever that thing does.
//
// The cells show the material rather than its name. A 22 px cell has room for
// about three characters of a 5x7 font, and "STONE" and "SNOW" both start "S";
// the block icons are what the hotbar has already taught the player to read.
void craftCard(const game::State& s, const char* footer) {
  // Sized and placed to clear the HUD rather than centred on the panel. The
  // hearts start at H - HUD_H + 4 and the card used to be drawn straight over
  // them, so crafting hid how close to dead you were -- and the bar underneath
  // it, which is the one thing you are looking at while you decide what to make.
  constexpr int CELL = 22, CGAP = 3;
  constexpr int cardW = 168, cardH = 100;
  const int left = (W - cardW) / 2, top = 2;

  cardGround(left, top, cardW, cardH);
  render::textCentred(W / 2, top + 4, "CRAFT", C_ACCENT, 1);

  const int gx = left + 14, gy = top + 14;
  for (int i = 0; i < game::GRID_N; ++i) {
    const int cx = gx + (i % 2) * (CELL + CGAP);
    const int cy = gy + (i / 2) * (CELL + CGAP);
    render::rect(cx, cy, CELL, CELL, C_SLOT_BG);
    render::frameRect(cx, cy, CELL, CELL, 1, C_SLOT_ED);
    if (s.grid[i] < world::B_COUNT) blockIcon(cx + 3, cy + 3, CELL - 6, s.grid[i]);
    if (i == s.gridSel)
      render::frameRect(cx - 1, cy - 1, CELL + 2, CELL + 2, 1, C_SLOT_SEL);
  }

  // The arrow, then the result. Dimmed rather than blank when the grid spells
  // something the player cannot pay for: "you have the idea but not the
  // materials" is a different answer from "this is not a recipe", and they
  // need telling apart without counting pockets.
  const int ax = gx + 2 * CELL + CGAP + 6;
  render::text(ax, gy + CELL - 6, "\x1a", C_DIM, 2);

  const int rx = ax + 22, ry = gy + 2;
  const uint8_t r = game::matchGrid(s.grid);
  const bool afford = (r != game::R_NONE) && game::canAffordGrid(s);

  render::rect(rx, ry, CELL, CELL, C_SLOT_BG);
  render::frameRect(rx, ry, CELL, CELL, 1, afford ? C_GOOD : C_SLOT_ED);
  if (r != game::R_NONE) {
    const game::RecipeInfo& ri = game::recipeInfo(r);
    itemIcon(rx + 3, ry + 3, ri.outItem, afford ? 256 : 110);
    const uint8_t rq = resultQty(ri);
    if (rq > 1) {
      char q[8];
      snprintf(q, sizeof(q), "X%u", (unsigned)rq);
      render::text(rx + CELL + 2, ry + CELL - 8, q, afford ? C_TEXT : C_DIM, 1);
    }
  }
  if (s.gridSel == game::GRID_FOCUS_OUT)
    render::frameRect(rx - 1, ry - 1, CELL + 2, CELL + 2, 1, C_SLOT_SEL);

  // What the grid currently spells, under it.
  const int ny = gy + 2 * CELL + CGAP + 3;
  if (r != game::R_NONE)
    render::textCentred(W / 2, ny, game::recipeInfo(r).name,
                        afford ? C_GOOD : C_DIM, 1);
  else
    render::textCentred(W / 2, ny, "NO RECIPE", C_FAINT, 1);

  // The book row, a cursor stop like everything else.
  const int by = ny + 10;
  const bool onBook = (s.gridSel == game::GRID_FOCUS_BOOK);
  render::rect(left + 10, by, cardW - 20, 12, onBook ? C_SEL : C_CARD);
  render::frameRect(left + 10, by, cardW - 20, 12, 1,
                    onBook ? C_ACCENT : C_CARD_ED);
  render::textCentred(W / 2, by + 3, "RECIPES", onBook ? C_TEXT : C_DIM, 1);

  if (footer) render::textCentred(W / 2, top + cardH - 9, footer, C_DIM, 1);
}

// ---- the recipe book --------------------------------------------------------

// One row per recipe, drawn as the thing it is: the ingredients that go into
// the grid, an arrow, and what comes out. The old book said "2 STONE + 1
// PLANK" in a font with no lowercase, which asked the player to translate
// words back into the pictures on their own hotbar. This is the pictures.
//
// Rows the player cannot afford are darkened rather than replaced with grey, so
// a recipe still reads as the materials it wants while saying you are short.
void recipeBook(const game::State& s, int sel, const char* footer) {
  // Same rule as the craft card: four rows of art and a heading have to fit
  // above the hearts, not on top of them.
  constexpr int ROWH = 19, ICON = 16;
  const int cardH = 14 + BOOK_ROWS * ROWH + 11;
  const int top = 2;
  const int bx = 8, bw = W - 16;

  cardGround(bx - 4, top, bw + 8, cardH);

  char head[24];
  snprintf(head, sizeof(head), "RECIPES %d/%d", sel + 1, (int)game::R_COUNT);
  render::textCentred(W / 2, top + 3, head, C_ACCENT, 1);

  // The window follows the cursor and stops at either end, so the last page
  // does not scroll past the final row into empty space.
  int first = sel - BOOK_ROWS / 2;
  if (first > (int)game::R_COUNT - BOOK_ROWS) first = (int)game::R_COUNT - BOOK_ROWS;
  if (first < 0) first = 0;

  for (int k = 0; k < BOOK_ROWS; ++k) {
    const int i = first + k;
    if (i >= (int)game::R_COUNT) break;
    const game::RecipeInfo& ri = game::recipeInfo((uint8_t)i);
    const bool can = game::canCraft(s, (uint8_t)i);
    const int mul = can ? 256 : 105;
    const int y = top + 13 + k * ROWH;
    const bool cur = (i == sel);

    render::rect(bx, y, bw, ROWH - 1, cur ? C_SEL : C_CARD);
    if (cur) render::frameRect(bx, y, bw, ROWH - 1, 1, C_ACCENT);

    // Ingredients, left to right, in the order they go into the grid.
    int ix = bx + 4;
    for (int c = 0; c < game::GRID_N; ++c) {
      if (ri.cells[c] == game::CELL_EMPTY) continue;
      blockIcon(ix, y + 1, ICON, ri.cells[c], mul);
      ix += ICON + 2;
    }

    render::text(ix + 2, y + 6, "\x1a", can ? C_TEXT : C_FAINT, 1);
    itemIcon(ix + 12, y + 1, ri.outItem, mul);

    int tx = ix + 12 + ICON + 2;
    const uint8_t rq = resultQty(ri);
    if (rq > 1) {
      char q[8];
      snprintf(q, sizeof(q), "X%u", (unsigned)rq);
      render::text(tx, y + 6, q, can ? C_TEXT : C_DIM, 1);
      tx += render::textWidth(q) + 4;
    }
    render::text(tx + 2, y + 6, ri.name, can ? C_TEXT : C_DIM, 1);
  }

  if (first > 0)
    render::text(bx + bw - 6, top + 14, "\x18", C_ACCENT, 1);
  if (first + BOOK_ROWS < (int)game::R_COUNT)
    render::text(bx + bw - 6, top + 13 + BOOK_ROWS * ROWH - 8, "\x19", C_ACCENT, 1);

  if (footer) render::textCentred(W / 2, top + cardH - 9, footer, C_DIM, 1);
}

void deathCard(const game::State& s, uint32_t best, bool isRecord) {
  cardGround(10, 14, W - 20, H - 28);

  render::textCentred(W / 2, 24, "YOU DIED", C_DEAD, 2);

  char buf[40];
  snprintf(buf, sizeof(buf), "FELL ON NIGHT %u", (unsigned)s.night);
  render::textCentred(W / 2, 48, buf, C_TEXT, 1);

  snprintf(buf, sizeof(buf), "%u", (unsigned)game::score(s));
  render::textCentred(W / 2, 62, buf, C_ACCENT, 2);

  if (isRecord) {
    render::textCentred(W / 2, 84, "NEW BEST!", C_GOOD, 1);
  } else {
    snprintf(buf, sizeof(buf), "BEST %u", (unsigned)best);
    render::textCentred(W / 2, 84, buf, C_DIM, 1);
  }

  const hal::Caps& c = hal::caps();
  snprintf(buf, sizeof(buf), "%s AGAIN", c.kConfirm);
  render::textCentred(W / 2, H - 30, buf, C_GOOD, 1);
}

}  // namespace ui
