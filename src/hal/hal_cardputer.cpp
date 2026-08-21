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

// Two-handed: the arrow cluster (; . , /) sits under the right hand and does
// all the moving and menu navigation, while E and D sit under the left and do
// all the acting. They never share a key, which is why WASD is not also bound
// to movement — D has to mean build, and a key cannot mean two things.
//
// SPACE and ENTER stay bound as aliases for act and confirm: they are what
// anyone tries first, and they cost nothing to keep.
static const Caps s_caps = {
  /*kTurn */ "\x1B\x1A",   // left/right arrows, drawn from the HUD font
  /*kMove */ "\x18\x19",   // up/down
  /*kAct  */ "E",
  /*kBuild*/ "D",
  /*kStart*/ "E",
  // Named TAB rather than ` : both are bound, but the HUD font has no
  // backtick and a hint the player cannot read is worse than the longer name.
  /*kPause*/ "TAB",
  /*kCycle*/ "S",
  /*kCraft*/ "W",
  // R sits directly above F on this keyboard, so up is up and down is down
  // under the fingers as well as on the panel.
  /*kLook */ "RF",
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
  // Left hand: E swings, D builds.
  b.act   = kb.isKeyPressed('e') || kb.isKeyPressed(' ');
  b.build = kb.isKeyPressed('d');
  // R over F, one row apart, so the pair reads as up and down without a hint.
  // Both together recentres the view; the game handles that, not the HAL.
  b.lookUp   = kb.isKeyPressed('r');
  b.lookDown = kb.isKeyPressed('f');

  const bool pause = kb.isKeyPressed('`') || st.tab;
  // W and S finish the left-hand cluster: craft and mine on the top row,
  // cycle and build on the bottom, all under one hand.
  const bool cyc = kb.isKeyPressed('s');
  const bool crf = kb.isKeyPressed('w');

  b.leftEdge  = b.left  && !s_prev.left;
  b.rightEdge = b.right && !s_prev.right;
  b.fwdEdge   = b.fwd   && !s_prev.fwd;
  b.backEdge  = b.back  && !s_prev.back;
  b.actEdge   = b.act   && !s_prev.act;
  // Enter confirms but never acts, so dismissing a card cannot also swing.
  b.startEdge = (b.act || st.enter) && !(s_prev.act || s_prevEnter);
  b.pauseEdge = pause && !s_prevPause;
  b.cycleEdge = cyc   && !s_prevCycle;
  b.craftEdge = crf   && !s_prevCraft;

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
