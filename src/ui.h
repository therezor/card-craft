// =============================================================================
//  ui.h — HUD and the full-screen cards
//
//  Everything here draws through render's primitives into the same framebuffer
//  as the 3D pass, so a frame is still one DMA transfer. Nothing in this file
//  touches the panel directly.
//
//  The HUD is hearts over a nine-slot hotbar, floating at the bottom of the
//  panel rather than filling a strip across it. On 240x135 every row it covers
//  is a row of world you cannot see, so anything the picture can carry — how
//  late it is, that you are hurt — is left to the picture.
// =============================================================================
#pragma once

#include <stdint.h>

#include "game.h"

namespace ui {

// Hearts (9 px) over the hotbar (18 px) with a little air. Nothing renders
// around this — the world is drawn full-frame and the HUD is painted over it —
// so it is an occlusion budget, not a viewport.
constexpr int HUD_H = 30;

// Hearts and the hotbar. There is no clock in it and no objective line: the sky
// says how late it is, and the dusk and dawn cues say when it turned. A meter
// counting down to nightfall is a thing this game was reading instead of
// looking at the world, which is the opposite of what it wants to be.
void hud(const game::State& s);

void crosshair(const game::State& s);

// The 2x2 crafting card: the grid, what it currently spells, and the cursor.
// Draws only — the cursor and the cells live in game::State, so the host tests
// can lay out a recipe and commit it with no renderer in the picture.
//
// footer is the control hint, built by the caller from hal::caps() the same way
// every other card's is. It is not spelled here because the key that does a job
// is a property of the board, not of the card.
void craftCard(const game::State& s, const char* footer);

// The recipe book: every recipe drawn as its own ingredients, not described in
// words. A cost line like "2 STONE + 1 PLANK" asks the player to hold three
// facts in their head and then find those materials on a bar that shows them as
// pictures; this shows the same pictures, in the order they go into the grid.
//
// sel is the highlighted row; the window scrolls to keep it visible.
void recipeBook(const game::State& s, int sel, const char* footer);

// How many rows of the book fit on the card. The caller owns the cursor, so it
// needs the same number to clamp against.
constexpr int BOOK_ROWS = 4;

// The title card. `sel` is which of its two rows the cursor is on; the caller
// owns that cursor, the same way it owns the book's.
void title(const char* board, uint32_t best, int sel);

// Picks a new caption for the title. Called when the title is entered rather
// than every frame -- see kSplash in ui.cpp for why. Never repeats the caption
// currently showing, so `seed` only has to decide WHICH other one; it does
// still have to vary, or every boot opens on the same joke.
void rerollSplash(uint32_t seed);

// The caption showing, and a way to put one back. Together these let the boot
// remember what the last boot ended on, so the first title of a new session
// avoids repeating it too. -1 is "nothing shown yet".
int  splashIndex();
void setSplashIndex(int i);

// How many stops the title menu has. The caller wraps against this.
constexpr int TITLE_ROWS = 2;

// Every binding the board has, drawn from hal::Caps. Reachable from the title
// and from the pause card, which is why it is a card of its own rather than
// something either of them draws inline.
void controlsCard(const char* footer);

void deathCard(const game::State& s, uint32_t best, bool isRecord);

// ---- menus ------------------------------------------------------------------

struct MenuItem {
  const char* label   = nullptr;
  const char* detail  = nullptr;   // second line, optional
  bool        enabled = true;      // false greys the row
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
  // How many rows fit on the card. See Menu::draw for where 4 comes from; a
  // list longer than this scrolls a window of this size over itself.
  static constexpr int MAX_ROWS = 4;

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
