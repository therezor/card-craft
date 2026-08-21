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
//  The input contract is eight held flags and nine edges, and the game is
//  playable with only four of them — see Buttons. That ceiling is deliberate:
//  it is what lets this run on a board with three buttons and no keyboard.
//  Everything past the core degrades to less game, never to a broken one: no
//  pitch keys means the fixed downward tilt the game shipped with, and no
//  speaker means silence, not a crash.
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

  // Down-edges, true for exactly one frame. Menus read these, so a held key
  // cannot rip through a three-item card in one press.
  bool leftEdge = false, rightEdge = false;
  bool fwdEdge  = false, backEdge  = false;
  bool actEdge  = false, startEdge = false;
  bool pauseEdge = false;
  // Beyond the four-button core. A board without these keeps one block type
  // and no crafting bench, which is a smaller game but still a whole one.
  bool cycleEdge = false, craftEdge = false;
};

// Names of the physical controls, so on-screen hints match the hardware
// instead of hardcoding a Cardputer keycap into a shared screen.
struct Caps {
  const char* kTurn;    // e.g. "< >"
  const char* kMove;    // e.g. "^ v"
  const char* kAct;     // e.g. "E"
  const char* kBuild;   // e.g. "D"
  const char* kStart;   // e.g. "E"
  const char* kPause;   // e.g. "TAB"
  const char* kCycle;   // e.g. "S"
  const char* kCraft;   // e.g. "W"
  const char* kLook;    // e.g. "R F", or nullptr where the board has no pitch
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
// beep() cannot layer: it names no channel, and the underlying tone() defaults
// to cutting whatever is already sounding. That is why the game used to pick a
// single event per frame and throw away the rest — mining while something bit
// you was silent about one of the two.
//
// A board whose speaker is a passive buzzer implements this by ignoring channel
// and wave and calling beep(). It loses the layering and the timbre; it does
// not lose the sound. See sfx.h for what is built on top.
void voice(uint8_t channel, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol);

}  // namespace hal
