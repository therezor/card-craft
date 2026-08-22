// =============================================================================
//  sfx.cpp — the cue table and the sequencer that walks it
//
//  Everything here is data plus about forty lines of scheduler. The scheduler
//  never blocks and never waits on the speaker: it starts a sound and notes when
//  the channel comes free again.
//
//  A cue is a rendered waveform from sfxdata.h. It was not always: the sounds
//  used to be spelled out as the step tables further down, which the sequencer
//  walked one constant-pitch tone at a time. That is the format the beeps came
//  from — it has no envelope, so nothing decays; its noise is a sixteen-sample
//  cycle played at a pitch, which is a buzz; and it cannot sweep, so an
//  explosion is five tones in a row rather than one long fall. The tables are
//  still here as the fallback for a board with no PCM path, which is the same
//  bargain hal.h makes everywhere else: less sound, never none.
// =============================================================================
#include "sfx.h"

#include "hal/hal.h"
#include "sfxdata.h"

namespace sfx {

// ---- the fallback step tables -----------------------------------------------
//
// Only reached where hal::sample() says the board cannot play a waveform. On the
// Cardputer nothing below is ever heard.
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

// A skeleton loosing an arrow, and the arrow arriving. Both used to be silent.
static const Step kArrowFireS[] = {{ 330, 30, W_SAW, 120 }, { 900, 26, W_NOISE, 60 }};
static const Step kArrowHitS[]  = {{ 700, 22, W_NOISE, 170 }, { 160, 40, W_TRI, 130 }};

static const Step kMenuMoveS[]  = {{ 660, 18, W_SQUARE, 90 }};
static const Step kBuyS[]       = {{ 523, 70, W_TRI, 180 }, { 784, 90, W_TRI, 170 }};
static const Step kCraftS[]     = {{ 440, 50, W_TRI, 160 }, { 587, 50, W_TRI, 160 },
                                   { 784, 80, W_TRI, 150 }};
static const Step kCraftFailS[] = {{ 200, 50, W_SQUARE, 130 }, { 150, 70, W_SQUARE, 110 }};

// Binds a rendered waveform to its role in the mix. The step table rides along
// as the fallback; pcmMs is derived here so play() never divides.
#define CUE(name, arr, pcm, ch, prio, vol, varies)                            \
  const Cue name = { sfxdata::pcm, (uint16_t)sizeof(sfxdata::pcm),            \
                     (uint16_t)((uint32_t)sizeof(sfxdata::pcm) * 1000u        \
                                / sfxdata::RATE),                             \
                     vol, varies,                                             \
                     arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), ch, prio }

//   cue          fallback      waveform       channel    prio  vol  varies
CUE(kMineTick,  kMineTickS,  kMineTick,   CH_TOOL,   1,   200, true);
CUE(kSwing,     kSwingS,     kSwing,      CH_TOOL,   2,   190, true);
CUE(kWhiff,     kWhiffS,     kWhiff,      CH_TOOL,   2,   180, false);
CUE(kBlockBroke,kBlockBrokeS,kBlockBroke, CH_IMPACT, 4,   230, true);
CUE(kPlace,     kPlaceS,     kPlace,      CH_TOOL,   3,   200, false);
CUE(kNoBlocks,  kNoBlocksS,  kNoBlocks,   CH_UI,     3,   190, false);
CUE(kMobHit,    kMobHitS,    kMobHit,     CH_IMPACT, 5,   240, true);
CUE(kMobDied,   kMobDiedS,   kMobDied,    CH_VOICE,  5,   225, false);
CUE(kHurt,      kHurtS,      kHurt,       CH_IMPACT, 8,   255, false);
CUE(kDied,      kDiedS,      kDied,       CH_IMPACT, 9,   255, false);
CUE(kTelegraph, kTelegraphS, kTelegraph,  CH_VOICE,  3,   210, false);
CUE(kHiss,      kHissS,      kHiss,       CH_VOICE,  6,   240, false);
CUE(kExplode,   kExplodeS,   kExplode,    CH_IMPACT, 9,   255, false);
// The bow sits on the mob channel, not the tool channel: it is a skeleton's
// telegraph, and on CH_TOOL it would lose to the player's own swings — which is
// exactly the moment an incoming arrow has to be heard.
CUE(kArrowFire, kArrowFireS, kArrowFire,  CH_VOICE,  3,   215, true);
CUE(kArrowHit,  kArrowHitS,  kArrowHit,   CH_IMPACT, 4,   230, true);
CUE(kDusk,      kDuskS,      kDusk,       CH_UI,     6,   215, false);
CUE(kDawn,      kDawnS,      kDawn,       CH_UI,     6,   215, false);
CUE(kMenuMove,  kMenuMoveS,  kMenuMove,   CH_UI,     2,   170, true);
CUE(kBuy,       kBuyS,       kBuy,        CH_UI,     5,   215, false);
CUE(kCraft,     kCraftS,     kCraft,      CH_UI,     5,   215, false);
CUE(kCraftFail, kCraftFailS, kCraftFail,  CH_UI,     5,   200, false);

#undef CUE

// ---- the sequencer ----------------------------------------------------------

namespace {

struct Voice {
  // Set for a step-table cue: how many steps are left and where the next one is.
  const Step* step = nullptr;
  uint8_t     left = 0;
  // Set for either: how important what is playing is, and when the channel
  // comes free. A waveform has no steps to count down, so the end of it has to
  // be a time rather than a count.
  uint8_t     prio = 0;
  uint32_t    until = 0;
  uint32_t    nextAt = 0;    // step cues only: when the current step ends
};
Voice s_voice[CH_COUNT];

bool     s_on    = false;
uint32_t s_nowMs = 0;

// Deliberately not esp_random(): the pitch jitter has to be reproducible for
// the same reason the "noise" wavetable was a fixed cycle. Numbers from Numerical
// Recipes' LCG; only the top bits are used, because the low ones barely move.
uint32_t s_rand = 0x1234567u;
uint32_t lcg() {
  s_rand = s_rand * 1664525u + 1013904223u;
  return s_rand >> 16;
}

}  // namespace

void setEnabled(bool on) {
  s_on = on;
  if (on) return;
  // Stopping is not enough on its own: a step cue mid-table would keep handing
  // notes to a speaker that had just been silenced, so the channels are cleared
  // as well.
  hal::silence();
  for (uint8_t ch = 0; ch < CH_COUNT; ++ch) s_voice[ch] = Voice{};
}

bool enabled() { return s_on; }

void play(const Cue& c) {
  if (!s_on || c.ch >= CH_COUNT) return;
  Voice& v = s_voice[c.ch];

  // A cue in progress is only interrupted by something at least as important.
  // Without this a creeper's fuse would be cut by every routine thing sharing
  // its channel — a mob dying, another one winding up — and the fuse is the
  // only warning the player gets.
  //
  // A waveform is busy until its window closes; a step table is busy while it
  // still has steps. Both are needed: the two kinds of cue share a channel.
  const bool busy = v.left || (int32_t)(s_nowMs - v.until) < 0;
  if (busy && c.prio < v.prio) return;

  if (c.pcm) {
    uint32_t rate = sfxdata::RATE;
    // +/- 8%. Small enough that nothing changes character, wide enough that two
    // plays in a row are audibly not the same recording.
    if (c.varies) rate = rate * (92u + lcg() % 17u) / 100u;
    if (hal::sample(c.ch, c.pcm, c.pcmLen, rate, c.vol)) {
      v.step  = nullptr;
      v.left  = 0;
      v.prio  = c.prio;
      v.until = s_nowMs + c.pcmMs;
      return;
    }
  }

  if (c.n == 0) return;
  v.step   = c.step;
  v.left   = c.n;
  v.prio   = c.prio;
  v.nextAt = 0;              // due immediately; update() starts it this frame
  // A step cue owns the channel through v.left. Clearing the window stops a
  // stale one left over from an earlier waveform from keeping the channel busy
  // after the steps have run out.
  v.until  = 0;
}

void update(uint32_t nowMs) {
  // Cached for play(), which is called from the frame before this one runs and
  // so reads a clock at most one frame old. That is tens of milliseconds against
  // sounds hundreds of milliseconds long, and it only ever errs towards letting
  // a cue through.
  s_nowMs = nowMs;
  if (!s_on) return;

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
