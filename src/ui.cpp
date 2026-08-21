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
#include "world.h"

namespace ui {

using render::pack;

constexpr uint16_t C_HUD_BG   = pack( 12,  14,  20);   // #0C0E14  strip
constexpr uint16_t C_HUD_LINE = pack( 34,  40,  52);   // #222834  strip top edge
constexpr uint16_t C_HEART    = pack(226,  58,  58);   // #E23A3A
constexpr uint16_t C_HEART_HL = pack(255, 128, 128);   // #FF8080  highlight pip
constexpr uint16_t C_HEART_MT = pack( 46,  30,  34);   // #2E1E22  empty socket
constexpr uint16_t C_BLOCK    = pack(176, 140,  88);   // #B08C58  matches B_PLANK
constexpr uint16_t C_ORE      = pack(198, 168, 132);   // #C6A884  matches B_IRON
constexpr uint16_t C_TEXT     = pack(226, 230, 238);   // #E2E6EE
constexpr uint16_t C_DIM      = pack(128, 136, 152);   // #808898
constexpr uint16_t C_ACCENT   = pack( 96, 200, 240);   // #60C8F0
constexpr uint16_t C_DAY      = pack(248, 196,  92);   // #F8C45C  phase bar, day
constexpr uint16_t C_NIGHT    = pack( 92, 108, 200);   // #5C6CC8  phase bar, night
constexpr uint16_t C_CARD     = pack( 14,  17,  25);   // #0E1119  card ground
constexpr uint16_t C_CARD_ED  = pack( 60,  70,  92);   // #3C465C  card edge
constexpr uint16_t C_SEL      = pack( 38,  74, 110);   // #264A6E  selected option
constexpr uint16_t C_GOOD     = pack(110, 214, 120);   // #6ED678  affordable
constexpr uint16_t C_BAD      = pack(180,  76,  76);   // #B44C4C  too expensive
constexpr uint16_t C_CROSS    = pack(236, 240, 248);   // #ECF0F8
constexpr uint16_t C_PROG     = pack(250, 214,  96);   // #FAD660  mining progress
constexpr uint16_t C_TITLE_BG = pack( 10,  14,  24);   // #0A0E18
constexpr uint16_t C_DEAD     = pack(226,  74,  74);   // #E24A4A
constexpr uint16_t C_FAINT    = pack( 58,  64,  80);   // #3A4050

constexpr int W = render::W;
constexpr int H = render::H;

// ---- HUD --------------------------------------------------------------------

void phaseBar(const game::State& s) {
  const uint32_t total = (s.phase == game::PH_DAY) ? (uint32_t)game::DAY_TICKS
                                                   : (uint32_t)game::NIGHT_TICKS;
  const uint32_t done = s.phaseTick > total ? total : s.phaseTick;
  int px = (int)((uint64_t)(total - done) * (uint32_t)W / total);
  if (px < 0) px = 0;
  if (px > W) px = W;
  render::rect(0, 0, px, 2, (s.phase == game::PH_DAY) ? C_DAY : C_NIGHT);
}

void objective(const game::State& s) {
  const uint32_t total = (s.phase == game::PH_DAY) ? (uint32_t)game::DAY_TICKS
                                                   : (uint32_t)game::NIGHT_TICKS;
  const uint32_t done = s.phaseTick > total ? total : s.phaseTick;
  const unsigned left = (unsigned)((total - done) / game::TICK_HZ) + 1u;

  char buf[40];
  if (s.phase == game::PH_DAY) {
    // The prompt follows what the player is short of, so it stays useful past
    // the first day instead of becoming a fixed banner nobody reads.
    const char* what = (game::totalBlocks(s) < 12) ? "MINE" : "DIG FOR ORE";
    snprintf(buf, sizeof(buf), "%s   DUSK IN %us", what, left);
  } else {
    snprintf(buf, sizeof(buf), "SURVIVE   DAWN IN %us", left);
  }
  render::textCentred(W / 2, 4, buf,
                      (s.phase == game::PH_DAY) ? C_DAY : C_NIGHT, 1);
}

void hud(const game::State& s) {
  const int y0 = H - HUD_H;
  render::rect(0, y0, W, HUD_H, C_HUD_BG);
  render::rect(0, y0, W, 1, C_HUD_LINE);

  // Hearts get a lighter pip in the corner, because a plain red square at this
  // size is hard to tell from the ore swatch a few pixels to its right.
  int x = 3;
  const int hearts = s.maxHp > 22 ? 22 : s.maxHp;
  for (int i = 0; i < hearts; ++i) {
    const bool full = (i < s.hp);
    render::rect(x, y0 + 4, 4, 5, full ? C_HEART : C_HEART_MT);
    if (full) render::rect(x, y0 + 4, 2, 2, C_HEART_HL);
    x += 5;
  }

  char buf[16];
  // Laid out right to left, so a three-digit count never shoves the night
  // number off the edge of the panel.
  int rx = W - 3;

  snprintf(buf, sizeof(buf), "N%u", (unsigned)s.night);
  rx -= render::textWidth(buf);
  render::text(rx, y0 + 3, buf, C_ACCENT);
  rx -= 8;

  snprintf(buf, sizeof(buf), "%u", (unsigned)s.ore);
  rx -= render::textWidth(buf);
  render::text(rx, y0 + 3, buf, C_TEXT);
  rx -= 7;
  render::rect(rx, y0 + 4, 5, 5, C_ORE);
  rx -= 9;

  // The held block, in its own colour. A named count would not fit and would
  // not read as fast: the swatch is the same colour as the thing that appears
  // in the world when the build key is pressed.
  const uint8_t held = game::heldBlock(s);
  const world::BlockInfo& hb = world::info(held);
  snprintf(buf, sizeof(buf), "%u", (unsigned)s.inv[held]);
  rx -= render::textWidth(buf);
  render::text(rx, y0 + 3, buf, s.inv[held] ? C_TEXT : C_DIM);
  rx -= 9;
  render::rect(rx - 1, y0 + 3, 7, 7, C_HUD_LINE);
  render::rect(rx, y0 + 4, 5, 5, pack(hb.r, hb.g, hb.b));
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
  snprintf(buf, sizeof(buf), "%s CRAFT  %s BLOCK  %s PAUSE", c.kCraft, c.kCycle, c.kPause);
  render::textCentred(W / 2, 107, buf, C_DIM, 1);

  snprintf(buf, sizeof(buf), "PRESS %s", c.kStart);
  render::textCentred(W / 2, 121, buf, C_GOOD, 1);
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
  const int rowH = 22, gap = 3;
  const int bodyH = count_ * rowH + (count_ - 1) * gap;
  const int cardH = 16 + bodyH + (footer ? 14 : 6);
  const int top = (H - cardH) / 2;
  const int bx = 12, bw = W - 24;

  cardGround(bx - 6, top, bw + 12, cardH);
  if (title_) render::textCentred(W / 2, top + 6, title_, C_ACCENT, 1);

  char buf[16];
  for (int i = 0; i < count_; ++i) {
    const MenuItem& it = items_[i];
    const int by = top + 16 + i * (rowH + gap);
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

    if (it.cost) {
      snprintf(buf, sizeof(buf), "%u", (unsigned)it.cost);
      const int cw = render::textWidth(buf);
      render::rect(bx + bw - cw - 15, by + rowH / 2 - 3, 5, 5, C_ORE);
      render::text(bx + bw - cw - 7, by + rowH / 2 - 4, buf,
                   it.enabled ? C_GOOD : C_BAD, 1);
    }
  }

  if (footer) render::textCentred(W / 2, top + cardH - 11, footer, C_DIM, 1);
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
  snprintf(buf, sizeof(buf), "%s AGAIN", c.kStart);
  render::textCentred(W / 2, H - 30, buf, C_GOOD, 1);
}

}  // namespace ui
