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

#include "world.h"

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

// Mining, per material family: a dull tick, not a chirp. On a board with no PCM
// path the three families are three pitches of the same gesture — less
// difference than the waveforms carry, but still a difference.
static const Step kDigSoftS[]  = {{ 150, 26, W_NOISE, 80 }, { 110, 16, W_SQUARE, 60 }};
static const Step kDigStoneS[] = {{ 320, 22, W_NOISE, 95 }, { 190, 16, W_SQUARE, 70 }};
static const Step kDigWoodS[]  = {{ 420, 30, W_TRI,   95 }, { 240, 18, W_NOISE, 55 }};
// The block gives way: the crack, then the piece landing.
static const Step kBreakSoftS[] = {
  { 420, 34, W_NOISE, 170 }, { 240, 46, W_NOISE, 130 }, { 160, 60, W_TRI, 100 },
};
static const Step kBreakStoneS[] = {
  { 640, 34, W_NOISE, 190 }, { 320, 46, W_SQUARE, 150 }, { 180, 60, W_TRI, 110 },
};
static const Step kBreakWoodS[] = {
  { 520, 30, W_NOISE, 170 }, { 400, 50, W_TRI, 160 }, { 260, 70, W_TRI, 110 },
};
static const Step kPlaceSoftS[]  = {{ 220, 28, W_NOISE, 110 }, { 300, 34, W_TRI, 100 }};
static const Step kPlaceStoneS[] = {{ 380, 28, W_SQUARE, 130 }, { 520, 34, W_TRI, 110 }};
static const Step kPlaceWoodS[]  = {{ 480, 30, W_TRI, 130 }, { 560, 34, W_TRI, 100 }};
static const Step kNoBlocksS[] = {{ 150, 40, W_SQUARE, 110 }, {   0, 30, W_SQUARE,   0 },
                                  { 130, 46, W_SQUARE,  90 }};

// The swing itself, and the swing that hits nothing. A whiff is air: quiet,
// wide, and gone. Before this a miss made no sound at all, so pressing the
// attack button into empty space did nothing you could hear or see.
static const Step kSwingS[] = {{ 900, 22, W_NOISE, 70 }, { 520, 26, W_NOISE, 45 }};
static const Step kWhiffS[] = {{1400, 26, W_NOISE, 55 }, { 700, 40, W_NOISE, 35 },
                               { 400, 34, W_NOISE, 20 }};
// One voice per mob. A buzzer cannot do formants, so the fallback separates
// them by register and rhythm instead: the zombie low and slurred, the skeleton
// a run of short clicks, the creeper pure noise.
static const Step kZombieIdleS[] = {{ 120,180, W_SAW, 110 }, {  98,240, W_SAW, 90 }};
static const Step kZombieHurtS[] = {{ 190, 70, W_SAW, 190 }, { 140, 90, W_SAW, 150 }};
static const Step kZombieDieS[]  = {{ 165,120, W_SAW, 190 }, { 120,150, W_SAW, 160 },
                                    {  85,200, W_TRI, 120 }};
static const Step kSkeletonIdleS[] = {
  {2400, 14, W_NOISE, 120 }, { 0, 70, W_NOISE, 0 }, {2900, 14, W_NOISE, 100 },
  { 0, 50, W_NOISE, 0 }, {2100, 14, W_NOISE, 110 },
};
static const Step kSkeletonHurtS[] = {{2600, 16, W_NOISE, 200 },
                                      {1500, 26, W_NOISE, 150 }};
static const Step kSkeletonDieS[] = {
  {2500, 14, W_NOISE, 190 }, {2200, 14, W_NOISE, 165 }, {1900, 14, W_NOISE, 140 },
  {1600, 16, W_NOISE, 115 }, {1300, 20, W_NOISE,  90 },
};
static const Step kCreeperHurtS[] = {{2800, 30, W_NOISE, 190 },
                                     {1800, 40, W_NOISE, 130 }};
static const Step kCreeperDieS[]  = {{2400, 70, W_NOISE, 170 },
                                     {1400, 90, W_NOISE, 120 },
                                     { 700,120, W_NOISE,  80 }};

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
CUE(kSwing,     kSwingS,     kSwing,      CH_TOOL,   2,   190, true);
CUE(kWhiff,     kWhiffS,     kWhiff,      CH_TOOL,   2,   180, false);
CUE(kNoBlocks,  kNoBlocksS,  kNoBlocks,   CH_UI,     3,   190, false);

// The three material families, in MatClass order so a block's class indexes
// straight into them.
CUE(kDigSoft,   kDigSoftS,   kDigSoft,    CH_TOOL,   1,   200, true);
CUE(kDigStone,  kDigStoneS,  kDigStone,   CH_TOOL,   1,   200, true);
CUE(kDigWood,   kDigWoodS,   kDigWood,    CH_TOOL,   1,   200, true);
CUE(kBreakSoft, kBreakSoftS, kBreakSoft,  CH_IMPACT, 4,   225, true);
CUE(kBreakStone,kBreakStoneS,kBreakStone, CH_IMPACT, 4,   230, true);
CUE(kBreakWood, kBreakWoodS, kBreakWood,  CH_IMPACT, 4,   228, true);
CUE(kPlaceSoft, kPlaceSoftS, kPlaceSoft,  CH_TOOL,   3,   195, false);
CUE(kPlaceStone,kPlaceStoneS,kPlaceStone, CH_TOOL,   3,   200, false);
CUE(kPlaceWood, kPlaceWoodS, kPlaceWood,  CH_TOOL,   3,   198, false);

// Mob voices, in game::MobKind order for the same reason.
//
// An idle sits on CH_VOICE at low priority: it is the least important thing in
// the mix and must never step on a fuse or a death.
CUE(kZombieIdle,  kZombieIdleS,  kZombieIdle,   CH_VOICE, 1, 175, true);
CUE(kZombieHurt,  kZombieHurtS,  kZombieHurt,   CH_IMPACT,5, 240, true);
CUE(kZombieDie,   kZombieDieS,   kZombieDie,    CH_VOICE, 5, 225, false);
CUE(kSkeletonIdle,kSkeletonIdleS,kSkeletonIdle, CH_VOICE, 1, 175, true);
CUE(kSkeletonHurt,kSkeletonHurtS,kSkeletonHurt, CH_IMPACT,5, 240, true);
CUE(kSkeletonDie, kSkeletonDieS, kSkeletonDie,  CH_VOICE, 5, 225, false);
CUE(kCreeperHurt, kCreeperHurtS, kCreeperHurt,  CH_IMPACT,5, 240, true);
CUE(kCreeperDie,  kCreeperDieS,  kCreeperDie,   CH_VOICE, 5, 225, false);
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

// The tables the game actually indexes. Built from the cues above rather than
// being the cues themselves, because a Cue carries a reference to its waveform
// and an array of them would either duplicate that or need an extra indirection
// at every call site.
const Cue kDig[MC_COUNT]   = { kDigSoft,   kDigStone,   kDigWood };
const Cue kBreak[MC_COUNT] = { kBreakSoft, kBreakStone, kBreakWood };
const Cue kPlace[MC_COUNT] = { kPlaceSoft, kPlaceStone, kPlaceWood };

// MobKind order: zombie, creeper, skeleton. A creeper has no idle — the thing
// that announces one is its fuse, and giving it an ambient noise would spend
// the only warning the player gets on a mob that is not yet coming.
const Cue kMobIdle[3] = { kZombieIdle, kZombieIdle, kSkeletonIdle };
const Cue kMobHurt[3] = { kZombieHurt, kCreeperHurt, kSkeletonHurt };
const Cue kMobDied[3] = { kZombieDie,  kCreeperDie,  kSkeletonDie };

MatClass matClass(uint8_t block) {
  switch (block) {
    case world::B_GRASS: case world::B_DIRT: case world::B_SAND:
    case world::B_SNOW:  case world::B_LEAVES:
      return MC_SOFT;
    case world::B_WOOD:  case world::B_PLANK: case world::B_TORCH:
      return MC_WOOD;
    // Ore in a stone matrix sounds like the matrix, not like the ore. Spelled
    // out rather than left to the default so that adding a material and
    // forgetting this switch is a compile-time-visible omission here.
    case world::B_DIAMOND:
      return MC_STONE;
    default:
      return MC_STONE;
  }
}

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
