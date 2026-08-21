// =============================================================================
//  ui.h — HUD and the full-screen cards
//
//  Everything here draws through render's primitives into the same framebuffer
//  as the 3D pass, so a frame is still one DMA transfer. Nothing in this file
//  touches the panel directly.
//
//  The whole HUD lives in a 13-pixel strip. On a 240x135 panel anything more is
//  taken out of the world you are trying to see, so state the picture can carry
//  — how late in the day it is, that you are hurt — is carried by the picture.
// =============================================================================
#pragma once

#include <stdint.h>

#include "game.h"

namespace ui {

constexpr int HUD_H = 13;

void hud(const game::State& s);

// A single line under the phase bar saying what to do and how long is left.
// The game has no tutorial and no manual: without this a new player spends
// their first day not knowing a night is coming.
void objective(const game::State& s);
void crosshair(const game::State& s);

// The thin band at the top of the screen: how much of the current phase is
// left. Not a widget so much as a horizon line — it reads without being read.
void phaseBar(const game::State& s);

void title(const char* board, uint32_t best);
void deathCard(const game::State& s, uint32_t best, bool isRecord);

// ---- menus ------------------------------------------------------------------

struct MenuItem {
  const char* label   = nullptr;
  const char* detail  = nullptr;   // second line, optional
  uint16_t    cost    = 0;         // 0 = free; otherwise drawn with an ore swatch
  bool        enabled = true;      // false greys it and marks the cost red
};

// A card with a title, a cursor and up to a handful of rows.
//
// A class rather than a draw function taking a cursor, because the cursor has
// invariants — it wraps, it skips nothing, and it has to stay inside a list
// whose length changes between the dawn card and the pause card. Keeping the
// index next to the list it indexes is the whole reason this is a type.
//
// It renders and it moves a cursor. It does not read input and it does not
// decide what a selection means: the screen that opened it does both.
class Menu {
 public:
  void open(const char* title, const MenuItem* items, int count);
  void close() { items_ = nullptr; }
  bool isOpen() const { return items_ != nullptr; }

  void move(int delta);            // wraps at both ends
  int  index() const { return index_; }

  // footer is the control hint along the bottom; pass nullptr for none.
  void draw(const char* footer) const;

 private:
  const char*     title_ = nullptr;
  const MenuItem* items_ = nullptr;
  int             count_ = 0;
  int             index_ = 0;
};

}  // namespace ui
