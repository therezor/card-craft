// =============================================================================
//  sfx.cpp — the cue table and the sequencer that walks it
//
//  Everything here is data plus about forty lines of scheduler. The scheduler
//  never blocks and never waits on the speaker: it notes when the current step
//  should end and hands the next one over on the frame after that.
// =============================================================================
#include "sfx.h"

#include "hal/hal.h"

namespace sfx {

// ---- the cues ---------------------------------------------------------------
//
// Pitches are chosen against each other rather than in isolation. The player
// hears these overlapping all night, so the rule is that anything urgent (hurt,
// fuse, explosion) lives low and anything routine (mining, placing) lives high,
// and the two never compete for the same part of the range.

// Mining: a dull tick, not a chirp. Fired every eighth tick by main.cpp.
static const Step kMineTickS[] = {{ 210, 26, W_NOISE, 90 }, { 150, 18, W_SQUARE, 70 }};
// The block gives way: the crack, then the piece landing.
static const Step kBlockBrokeS[] = {
  { 640, 34, W_NOISE, 190 }, { 320, 46, W_SQUARE, 150 }, { 180, 60, W_TRI, 110 },
};
static const Step kPlaceS[]    = {{ 380, 28, W_SQUARE, 130 }, { 520, 34, W_TRI, 110 }};
static const Step kNoBlocksS[] = {{ 150, 40, W_SQUARE, 110 }, {   0, 30, W_SQUARE,   0 },
                                  { 130, 46, W_SQUARE,  90 }};

// The swing itself, and the swing that hits nothing. A whiff is air: quiet,
// wide, and gone. Before this a miss made no sound at all, so pressing the
// attack button into empty space did nothing you could hear or see.
static const Step kSwingS[] = {{ 900, 22, W_NOISE, 70 }, { 520, 26, W_NOISE, 45 }};
static const Step kWhiffS[] = {{1400, 26, W_NOISE, 55 }, { 700, 40, W_NOISE, 35 },
                               { 400, 34, W_NOISE, 20 }};
// A landed hit is meat and bone, so: a low square with noise on the front.
static const Step kMobHitS[] = {{ 520, 20, W_NOISE, 200 }, { 260, 40, W_SQUARE, 180 },
                                { 200, 30, W_SQUARE, 120 }};
static const Step kMobDiedS[] = {{ 440, 40, W_SAW, 190 }, { 330, 46, W_SAW, 160 },
                                 { 220, 60, W_SAW, 130 }, { 150, 80, W_TRI, 90 }};

// Being hurt has to cut through whatever else is happening, so it is the lowest
// and the loudest thing in the mix.
static const Step kHurtS[] = {{ 200, 30, W_NOISE, 230 }, { 140, 70, W_SQUARE, 210 },
                              { 110, 60, W_SQUARE, 150 }};
static const Step kDiedS[] = {
  { 300, 90, W_SAW, 220 }, { 240,100, W_SAW, 200 }, { 180,120, W_SAW, 180 },
  { 140,150, W_SAW, 160 }, { 100,240, W_TRI, 140 },
};

// A creeper announces itself twice: once when it commits, and then a fuse that
// climbs. The climb is the whole tell — it says "now" without a word of UI.
static const Step kTelegraphS[] = {{ 150, 40, W_SQUARE, 140 }, { 120, 50, W_SQUARE, 110 }};
static const Step kHissS[] = {
  { 900, 60, W_NOISE, 150 }, {1100, 60, W_NOISE, 170 },
  {1400, 60, W_NOISE, 190 }, {1800, 80, W_NOISE, 210 },
};
static const Step kExplodeS[] = {
  { 300, 50, W_NOISE, 255 }, { 190, 70, W_NOISE, 240 }, { 120, 90, W_SQUARE, 220 },
  {  80,120, W_SQUARE, 190 }, {  60,150, W_TRI, 150 },
};

// The clock. Two notes, not one: a single beep says "something happened" and a
// falling or rising pair says which.
static const Step kDuskS[] = {{ 330,140, W_TRI, 170 }, { 247,200, W_TRI, 150 }};
static const Step kDawnS[] = {{ 392,120, W_TRI, 170 }, { 523,120, W_TRI, 170 },
                              { 659,200, W_TRI, 160 }};

static const Step kMenuMoveS[]  = {{ 660, 18, W_SQUARE, 90 }};
static const Step kBuyS[]       = {{ 523, 70, W_TRI, 180 }, { 784, 90, W_TRI, 170 }};
static const Step kCraftS[]     = {{ 440, 50, W_TRI, 160 }, { 587, 50, W_TRI, 160 },
                                   { 784, 80, W_TRI, 150 }};
static const Step kCraftFailS[] = {{ 200, 50, W_SQUARE, 130 }, { 150, 70, W_SQUARE, 110 }};

#define CUE(name, arr, ch, prio) \
  const Cue name = { arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), ch, prio }

CUE(kMineTick,  kMineTickS,  CH_TOOL,   1);
CUE(kSwing,     kSwingS,     CH_TOOL,   2);
CUE(kWhiff,     kWhiffS,     CH_TOOL,   2);
CUE(kBlockBroke,kBlockBrokeS,CH_IMPACT, 4);
CUE(kPlace,     kPlaceS,     CH_TOOL,   3);
CUE(kNoBlocks,  kNoBlocksS,  CH_UI,     3);
CUE(kMobHit,    kMobHitS,    CH_IMPACT, 5);
CUE(kMobDied,   kMobDiedS,   CH_VOICE,  5);
CUE(kHurt,      kHurtS,      CH_IMPACT, 8);
CUE(kDied,      kDiedS,      CH_IMPACT, 9);
CUE(kTelegraph, kTelegraphS, CH_VOICE,  3);
CUE(kHiss,      kHissS,      CH_VOICE,  6);
CUE(kExplode,   kExplodeS,   CH_IMPACT, 9);
CUE(kDusk,      kDuskS,      CH_UI,     6);
CUE(kDawn,      kDawnS,      CH_UI,     6);
CUE(kMenuMove,  kMenuMoveS,  CH_UI,     2);
CUE(kBuy,       kBuyS,       CH_UI,     5);
CUE(kCraft,     kCraftS,     CH_UI,     5);
CUE(kCraftFail, kCraftFailS, CH_UI,     5);

#undef CUE

// ---- the sequencer ----------------------------------------------------------

namespace {
struct Voice {
  const Step* step = nullptr;
  uint8_t     left = 0;      // steps remaining, including the one playing
  uint8_t     prio = 0;
  uint32_t    nextAt = 0;    // when the current step is due to end
};
Voice s_voice[CH_COUNT];
}  // namespace

void play(const Cue& c) {
  if (c.ch >= CH_COUNT || c.n == 0) return;
  Voice& v = s_voice[c.ch];
  // A cue in progress is only interrupted by something at least as important.
  // Without this the fuse whine would restart every frame and never be heard.
  if (v.left && c.prio < v.prio) return;
  v.step   = c.step;
  v.left   = c.n;
  v.prio   = c.prio;
  v.nextAt = 0;              // due immediately; update() starts it this frame
}

void update(uint32_t nowMs) {
  for (uint8_t ch = 0; ch < CH_COUNT; ++ch) {
    Voice& v = s_voice[ch];
    if (!v.left) continue;
    if (v.nextAt && (int32_t)(nowMs - v.nextAt) < 0) continue;

    const Step& st = *v.step;
    if (st.freq) hal::voice(ch, st.freq, st.ms, st.wave, st.vol);
    v.nextAt = nowMs + st.ms;
    ++v.step;
    if (--v.left == 0) v.prio = 0;
  }
}

}  // namespace sfx
