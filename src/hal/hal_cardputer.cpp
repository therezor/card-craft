// =============================================================================
//  hal_cardputer.cpp — M5Stack Cardputer and Cardputer ADV
//
//  Both boards carry the same StampS3 module and run the same binary: M5GFX
//  autodetects the panel and M5Cardputer picks the keyboard controller (the
//  original's GPIO matrix, the ADV's TCA8418) at runtime.
//
//  The keyboard gives held state directly — isKeyPressed() scans the list of
//  keys currently down — which is what a game needs. Edge detection is done
//  here rather than in the game so that a board wired to real buttons can
//  produce the same four edges without the game knowing the difference.
// =============================================================================
#if defined(BOARD_CARDPUTER)

#include "hal.h"

#include <M5Cardputer.h>

namespace hal {

static Buttons s_btn;
static Buttons s_prev;
// Enter and the pause key are not in Buttons — they are only ever edges — so
// their previous state is tracked here rather than inferred from s_prev.
static bool s_prevEnter = false;
static bool s_prevPause = false;
static bool s_prevCycle = false;
static bool s_prevCraft = false;
// One bit per number key, so a held '3' selects slot three once rather than
// every frame — the same edge rule the rest of this file applies.
static uint16_t s_prevSlot = 0;

// Two-handed: the arrow cluster (; . , /) sits under the right hand and does
// all the moving and menu navigation, while the left hand rests on a four-key
// square and does all the acting.
//
//     [W][E]     W mines, A builds -- the two verbs that touch the world, on
//     [A][S]     the left of the square. E and S look up and down, on the
//                right of it, so the camera pair is one finger apart and up
//                is above down under the hand as well as on the panel.
//
// D falls just off the square and cycles the hotbar; SPACE, under the thumb,
// jumps. No key means two things, which is the rule this layout is built on --
// and it is why ENTER alone confirms now. Confirm used to be aliased onto the
// act key, which was harmless while act was E and is not harmless at all now
// that act is W: every card would be confirmed by the mine key.
static const Caps s_caps = {
  /*kTurn */ "\x1B\x1A",   // left/right arrows, drawn from the HUD font
  /*kMove */ "\x18\x19",   // up/down
  /*kAct  */ "W",
  /*kBuild*/ "A",
  /*kConfirm*/ "ENTER",
  // ESC rather than ` -- the HUD font has no backtick, and the Cardputer has
  // no key engraved ESC either, but the one under the player's left thumb at
  // the top-left corner is where ESC lives on every keyboard they have ever
  // used, and that is what a hint is for. TAB is no longer an alias for it:
  // TAB opens the crafting card now.
  /*kBack*/ "ESC",
  /*kCycle*/ "D",
  /*kCraft*/ "TAB",
  /*kDrop*/  "Q",
  // E sits directly above S on this keyboard, so up is up and down is down
  // under the fingers as well as on the panel.
  /*kLook */ "E S",
  /*kJump */ "SPACE",
};

void begin() {
  auto cfg = M5.config();
  cfg.output_power = false;      // nothing hangs off Port.A; skip the 5 V rail
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(160);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(140);
}

LGFX_Device& display() { return M5Cardputer.Display; }

void update() {
  M5Cardputer.update();
  auto& kb = M5Cardputer.Keyboard;
  s_prev = s_btn;

  const auto st = kb.keysState();
  Buttons b;
  // Right hand: the arrow cluster moves the player and the menu cursor.
  b.left  = kb.isKeyPressed(',');
  b.right = kb.isKeyPressed('/');
  b.fwd   = kb.isKeyPressed(';');
  b.back  = kb.isKeyPressed('.');
  // Left hand, left column of the square: W swings, A builds.
  b.act   = kb.isKeyPressed('w');
  b.build = kb.isKeyPressed('a');
  // Q throws the selected item on the floor. Minecraft's key, and the one free
  // letter next to the movement hand -- held rather than edged, because the
  // simulation paces the repeat that empties a stack.
  b.drop  = kb.isKeyPressed('q');
  // Right column of the square: E over S, one row apart, so the pair reads as
  // up and down without a hint. Both together recentres the view; the game
  // handles that, not the HAL.
  b.lookUp   = kb.isKeyPressed('e');
  b.lookDown = kb.isKeyPressed('s');
  // Held, not edged. The frame builds one Input and hands it to as many as four
  // catch-up ticks, so an edge computed here would take off four times; the
  // rising edge is latched in the simulation instead, where it is seen once.
  b.jump  = kb.isKeyPressed(' ');

  // ` alone. TAB used to be an alias for it and now opens the craft card, which
  // is the one place a second key for "back" would have been actively wrong.
  const bool pause = kb.isKeyPressed('`');
  // D falls just off the square and steps the hotbar; TAB opens crafting.
  const bool cyc = kb.isKeyPressed('d');
  const bool crf = st.tab;

  b.leftEdge  = b.left  && !s_prev.left;
  b.rightEdge = b.right && !s_prev.right;
  b.fwdEdge   = b.fwd   && !s_prev.fwd;
  b.backEdge  = b.back  && !s_prev.back;
  b.actEdge   = b.act   && !s_prev.act;
  b.buildEdge = b.build && !s_prev.build;
  // ENTER, and only ENTER. The act key used to be aliased onto confirm because
  // act was E and a hand on E would try it first; act is W now, and leaving the
  // alias would mean the mine key confirms every card in the game. One meaning,
  // one key -- which is what the hints have always named anyway.
  b.confirmEdge = st.enter && !s_prevEnter;
  b.cancelEdge  = pause && !s_prevPause;

  // One cluster, two vocabularies. Up the panel is up the list.
  b.navUp    = b.fwdEdge;
  b.navDown  = b.backEdge;
  b.navLeft  = b.leftEdge;
  b.navRight = b.rightEdge;
  b.cycleEdge = cyc   && !s_prevCycle;
  b.craftEdge = crf   && !s_prevCraft;

  // The number row picks a hotbar slot directly. Lowest key wins if two are
  // down, which only matters to someone rolling a finger across the row.
  uint16_t slotNow = 0;
  for (int k = 0; k < 9; ++k)
    if (kb.isKeyPressed((char)('1' + k))) slotNow |= (uint16_t)(1u << k);
  for (int k = 0; k < 9; ++k) {
    const uint16_t bit = (uint16_t)(1u << k);
    if ((slotNow & bit) && !(s_prevSlot & bit)) { b.slotPick = (uint8_t)(k + 1); break; }
  }
  s_prevSlot = slotNow;

  s_prevEnter = st.enter;
  s_prevPause = pause;
  s_prevCycle = cyc;
  s_prevCraft = crf;
  s_btn = b;
}

const Buttons& buttons() { return s_btn; }
const Caps&    caps()    { return s_caps; }

const char* boardName() {
  return (M5.getBoard() == m5::board_t::board_M5CardputerADV) ? "CARDPUTER ADV"
                                                              : "CARDPUTER";
}

void beep(uint16_t freqHz, uint16_t ms) {
  M5Cardputer.Speaker.tone((float)freqHz, ms);
}

// Single-cycle wavetables, unsigned and centred on 128, which is the format
// Speaker_Class::tone takes for its raw_data overload. Signed data is playRaw's
// format, and mixing the two up is a silent distortion bug rather than an
// error, so the types are spelled out here once and never converted.
//
// The Cardputer's speaker is an I2S amplifier, not a buzzer (M5Unified brings it
// up on BCK 41 / WS 43 / DATA 42), so it mixes eight virtual channels properly
// and reproduces a waveform rather than a click. All of this was available from
// the start; the game just never asked for it.
static const uint8_t kSquare[16] = { 255,255,255,255,255,255,255,255,
                                       0,  0,  0,  0,  0,  0,  0,  0 };
static const uint8_t kSaw[16]    = {   0, 17, 34, 51, 68, 85,102,119,
                                     136,153,170,187,204,221,238,255 };
static const uint8_t kTri[16]    = {   0, 32, 64, 96,128,160,192,224,
                                     255,224,192,160,128, 96, 64, 32 };
// Not random at runtime: a fixed cycle keeps a "noise" burst reproducible and
// costs nothing. At the pitches these are played at it reads as noise.
static const uint8_t kNoise[16]  = {  17,203, 61,250,  8,144,231, 92,
                                     178, 35,255,119,  4,167, 76,212 };

static const uint8_t* const kWave[4] = { kSquare, kSaw, kTri, kNoise };

void voice(uint8_t channel, uint16_t freqHz, uint16_t ms, uint8_t wave, uint8_t vol) {
  if (channel > 7) channel = 7;
  if (wave > 3) wave = 3;
  M5Cardputer.Speaker.setChannelVolume(channel, vol);
  // stop_current_sound is false: the point of naming a channel is that the
  // other channels keep sounding.
  M5Cardputer.Speaker.tone((float)freqHz, ms, (int)channel, false,
                           kWave[wave], sizeof(kSquare), false);
}

bool sample(uint8_t channel, const int8_t* pcm, size_t n, uint32_t rateHz,
            uint8_t vol) {
  if (channel > 7) channel = 7;
  M5Cardputer.Speaker.setChannelVolume(channel, vol);
  // stop_current_sound is true here where voice() passes false, and the
  // difference matters: the per-channel request queue is two slots deep, and
  // these are whole sounds rather than 20 ms steps. Queued, a cue that outranks
  // the one playing would wait out most of a second behind an explosion and
  // arrive after the thing it was announcing. sfx.cpp has already decided this
  // cue wins the channel; the speaker should not second-guess it.
  return M5Cardputer.Speaker.playRaw(pcm, n, rateHz, false, 1, (int)channel, true);
}

void silence() { M5Cardputer.Speaker.stop(); }

}  // namespace hal

#endif  // BOARD_CARDPUTER
