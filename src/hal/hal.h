// =============================================================================
//  hal.h — board abstraction for Card Craft
//
//  Everything device-specific lives behind this header: display and input.
//  Nothing in src/ outside hal/ includes a board library.
//
//  Adding a board: write src/hal/hal_<board>.cpp guarded by its own BOARD_*
//  define, implement the functions below, and add a matching [env:...] in
//  platformio.ini with -D BOARD_<X>. No game code changes.
//
//  The input contract is nine held flags and nine edges, and the game is
//  playable with only four of them — see Buttons. That ceiling is deliberate:
//  it is what lets this run on a board with three buttons and no keyboard.
//  Everything past the core degrades to less game, never to a broken one: no
//  pitch keys means the fixed downward tilt the game shipped with, no jump key
//  means a player who walks everywhere, and no speaker means silence, not a
//  crash.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <M5GFX.h>

namespace hal {

struct Buttons {
  // Held state, sampled every frame. Movement and mining read these.
  bool left  = false, right = false;
  bool fwd   = false, back  = false;
  bool act   = false, build = false;

  // Looking up and down. Outside the four-button core on purpose: a board with
  // no keys to spare leaves these false and plays at the fixed downward tilt,
  // which is exactly how the game shipped. Nothing depends on them.
  bool lookUp = false, lookDown = false;
  // Held, and passed straight through to game::Input. The repeat that empties a
  // stack is paced by the simulation, not here. A board with no key to spare
  // leaves it false and simply cannot throw anything away.
  bool drop = false;
  // Held, and latched into a single take-off inside the simulation rather than
  // edged here. The frame builds one Input and feeds it to up to four catch-up
  // ticks, so an edge in this struct would launch the player four times.
  bool jump = false;

  // Down-edges, true for exactly one frame. Menus read these, so a held key
  // cannot rip through a three-item card in one press.
  bool leftEdge = false, rightEdge = false;
  bool fwdEdge  = false, backEdge  = false;
  bool actEdge  = false;
  // The two keys every card is driven by, alongside the four arrows. Named for
  // what they mean, and meaning the same thing on every screen: one meaning,
  // one key.
  bool confirmEdge = false;   // ENTER
  bool cancelEdge  = false;   // ESC -- one level up, whatever level that is

  // The same four arrow keys as the movement edges above, under the names a
  // card reads them by. Menus navigate; the world moves. They are the same
  // switches because the Cardputer has one arrow cluster, but they are not the
  // same idea, and a card asking for "forward" to mean "up the list" is how
  // the old screens ended up each inventing their own answer.
  bool navUp = false, navDown = false, navLeft = false, navRight = false;
  // The build key's own edge. Held state alone was enough while BUILD only ever
  // placed blocks, which is a thing you hold down; the craft card needs it as a
  // discrete "commit", and a held key would craft once a frame. A board that
  // does not have a build key leaves it false and simply cannot commit a craft
  // from the grid -- the recipe book still crafts on its own key.
  bool buildEdge = false;
  // Beyond the four-button core. A board without these keeps one block type
  // and no crafting bench, which is a smaller game but still a whole one.
  bool cycleEdge = false, craftEdge = false;

  // A hotbar slot named outright: 0 for none, 1..9 for the number key pressed
  // this frame. Cycling with one key works and is what a board with no number
  // row is left with, but nine presses to reach the slot you want is nine
  // presses, and this is the row Minecraft trained everyone to reach for.
  uint8_t slotPick = 0;
};

// Names of the physical controls, so on-screen hints match the hardware
// instead of hardcoding a Cardputer keycap into a shared screen.
struct Caps {
  const char* kTurn;    // e.g. "< >"
  const char* kMove;    // e.g. "^ v"
  const char* kAct;     // e.g. "W"
  const char* kBuild;   // e.g. "A"
  const char* kConfirm; // e.g. "ENTER"
  const char* kBack;    // e.g. "ESC"
  const char* kCycle;   // e.g. "D"
  const char* kCraft;   // e.g. "TAB"
  const char* kDrop;    // e.g. "Q", or nullptr where the board has no drop key
  const char* kLook;    // e.g. "E S", or nullptr where the board has no pitch
  const char* kJump;    // e.g. "SPACE", or nullptr where the board has no jump
};

void          begin();
LGFX_Device&  display();
void          update();        // pump board input, once per frame
const Buttons& buttons();
const Caps&   caps();
const char*   boardName();

// Short beep. Boards with no speaker implement this as a no-op rather than
// making every caller test for one.
void beep(uint16_t freqHz, uint16_t ms);

// One note on one mixing channel, with a timbre and a volume.
//
// beep() cannot layer: it names no channel, and the underlying tone() cuts
// whatever is already sounding. This can, which is what lets a frame play every
// event that fired in it rather than picking one.
//
// A board whose speaker is a passive buzzer implements this by ignoring channel
// and wave and calling beep(). It loses the layering and the timbre; it does
// not lose the sound. See sfx.h for what is built on top.
void voice(uint8_t channel, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol);

// Plays a block of 8-bit signed PCM on a mixing channel, cutting whatever that
// channel was doing. Returns false where the board has no PCM path at all, so
// the caller can fall back to voice() rather than go silent.
//
// This is what the game's sounds actually are: voice() can only hold a pitch at
// a volume, and an envelope, a noise burst and a pitch sweep are all outside
// what that can express. The waveforms are rendered offline instead — see
// tools/make-sfx.py — and this hands one to the speaker.
//
// The data is not copied. It has to outlive the sound, which is why the bank
// lives in flash as const arrays.
bool sample(uint8_t channel, const int8_t* pcm, size_t n, uint32_t rateHz, uint8_t vol);

// Stops every channel now. The sound toggle uses this, so switching sound off
// during a fuse does not leave the fuse running.
void silence();

}  // namespace hal
