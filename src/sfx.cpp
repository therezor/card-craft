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
// Index 1 is the creeper, and it is the zombie's moan because no creeper idle
// cue was ever recorded. Nothing reaches it any more -- game.cpp's voice loop
// skips creepers outright, deliberately -- and it is left pointing somewhere
// harmless rather than removed, because the array is indexed by MobKind and a
// hole in it would be a crash waiting for the day a creeper does get a voice.
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


// ---- background music -------------------------------------------------------
//
// A calm loop under the daylight, in the register a speaker this size can
// actually reproduce. Two voices: a sparse melody and a two-note harmony pulse
// under it, both triangle, both quiet enough to sit beneath the effects rather
// than compete with them.
//
// This is ORIGINAL music written for the game. It is in the manner of the genre
// -- slow, sparse, major sevenths, long rests, no strong hook -- and it copies
// no actual tune.
//
// Why notes and not a waveform: every other sound here is PCM rendered offline
// by tools/make-sfx.py, and that is right for a 300 ms crack of stone. At 48
// seconds it would be several megabytes and would not fit the flash the whole
// game lives in. Notes are a few hundred bytes.

// MIDI note to Hz, equal temperament, A4 = 440. MIDI 48 (C3) to 96 (C7), which
// covers both voices with room either side. Rounded to whole Hz: the largest
// error in here is a fifth of a cent and the speaker is a great deal further
// out of tune than that.
static constexpr uint16_t kMidiHz[] = {
   131,  139,  147,  156,  165,  175,  185,  196,
   208,  220,  233,  247,  262,  277,  294,  311,
   330,  349,  370,  392,  415,  440,  466,  494,
   523,  554,  587,  622,  659,  698,  740,  784,
   831,  880,  932,  988, 1047, 1109, 1175, 1245,
  1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
  2093,
};
constexpr uint8_t MIDI_LO = 48;
constexpr uint8_t MIDI_HI = 96;

struct MusNote {
  uint8_t  midi;   // 0 for a rest
  uint16_t ms;
};

// The piece: twenty bars of 2000 ms, looping. The two tracks must total the
// same number of milliseconds or they drift a little further apart on every lap
// until the harmony is landing under the wrong bar -- see the static_assert
// below, which is what stops that being found by ear six months from now.
//
// TWO WRONG VERSIONS CAME BEFORE THIS ONE, and the second was wrong in the
// opposite direction from the first, which is worth writing down.
//
// The first put a note on every beat of every bar. Sixteen bars of
// root-rest-fifth-rest is a metronome, and hal::voice has no envelope, so each
// note is a flat tone starting and stopping at full volume. It became a tick
// you could not stop hearing.
//
// The fix for that was more silence -- 55% of the loop, one note every two
// seconds -- and it was the wrong lever. Measuring a calm track from the genre
// settles it: 1.40 notes a second, almost no silence at all, and it is still
// calm. Density was never the problem.
//
// Three things were, and all three are here:
//
//   REGISTER. The reference sits at 97-311 Hz, entirely below middle C. This
//   used to run to 988 Hz, which is the part of the spectrum a small speaker is
//   harshest in and the ear is least willing to ignore. 174-440 Hz now -- the
//   bottom is raised off the reference's because a speaker this size does not
//   really reproduce 97 Hz, so going as low again would trade shrill for
//   inaudible.
//
//   TWO NOTES AT ONCE. Nearly half the reference's attacks are dyads, mostly
//   major thirds. This had a melody and a separate accompanying pulse and never
//   struck two notes together at all, which is most of why it sounded thin. The
//   two voices land together on the downbeats now and the upper one moves alone
//   after -- that is what the second channel is for.
//
//   SHORT NOTES. The reference is built from 500 and 1000 ms with a few 1500s.
//   This was built from 1500s and 3000s, and a three-second flat tone with no
//   envelope is a drone rather than a note.
//
// The melody is original. What was taken from the measurement is proportion --
// how low, how dense, how often two notes sound together, how long a note runs
// -- and none of that is anybody's tune.

// The upper voice. Lands with the lower one on a downbeat, then moves alone.
static constexpr MusNote kMelody[] = {
  //  1
  {60, 1000}, {62,  500}, {60,  500},
  //  2
  {57, 1000}, {55, 1000},
  //  3
  {58, 1000}, {57,  500}, {55,  500},
  //  4
  {53, 1500}, { 0,  500},
  //  5
  {64, 1000}, {62,  500}, {60,  500},
  //  6
  {62, 1000}, {60, 1000},
  //  7
  {65, 1000}, {64,  500}, {62,  500},
  //  8
  {60, 1500}, { 0,  500},
  //  9
  {67, 1000}, {65,  500}, {64,  500},
  // 10
  {69, 1000}, {67, 1000},
  // 11
  {65, 1000}, {64,  500}, {62,  500},
  // 12
  {60, 1500}, { 0,  500},
  // 13
  {62, 1000}, {60,  500}, {58,  500},
  // 14
  {57, 1000}, {55, 1000},
  // 15
  {60, 1000}, {58,  500}, {57,  500},
  // 16
  {53, 1500}, { 0,  500},
  // 17
  {57, 1500}, { 0,  500},
  // 18
  {55, 1000}, {53, 1000},
  // 19
  { 0, 2000},
  // 20
  { 0, 2000},
};

// The lower voice. Not a bass line and not a pulse: it exists to be the second
// note of a dyad, so it sounds only where one is wanted and rests out the rest
// of the bar.
static constexpr MusNote kHarmony[] = {
  //  1
  {53, 1000}, { 0, 1000},
  //  2
  {53, 1000}, { 0, 1000},
  //  3
  {55, 1000}, { 0, 1000},
  //  4
  { 0, 2000},
  //  5
  {60, 1000}, { 0, 1000},
  //  6
  {58, 1000}, { 0, 1000},
  //  7
  {60, 1000}, { 0, 1000},
  //  8
  {53, 1500}, { 0,  500},
  //  9
  {60, 1000}, { 0, 1000},
  // 10
  {65, 1000}, { 0, 1000},
  // 11
  {62, 1000}, { 0, 1000},
  // 12
  {57, 1500}, { 0,  500},
  // 13
  {55, 1000}, { 0, 1000},
  // 14
  {53, 1000}, { 0, 1000},
  // 15
  {53, 1000}, { 0, 1000},
  // 16
  { 0, 2000},
  // 17
  {53, 1500}, { 0,  500},
  // 18
  { 0, 2000},
  // 19
  { 0, 2000},
  // 20
  { 0, 2000},
};

// Compile-time proof that the two tracks are the same length.
constexpr uint32_t totalMs(const MusNote* n, size_t count) {
  uint32_t t = 0;
  for (size_t i = 0; i < count; ++i) t += n[i].ms;
  return t;
}
static_assert(totalMs(kMelody, sizeof(kMelody) / sizeof(kMelody[0])) ==
              totalMs(kHarmony, sizeof(kHarmony) / sizeof(kHarmony[0])),
              "music tracks must be the same length or they drift apart");

struct MusTrack {
  const MusNote* notes;
  uint16_t       n;
  uint8_t        ch;
  uint8_t        vol;
  uint16_t       i = 0;
  uint32_t       nextAt = 0;
};

// Above CH_COUNT by construction, so music never takes a channel a cue wants.
// hal::voice accepts eight and the cues use four.
static MusTrack s_mus[2] = {
  { kMelody,  (uint16_t)(sizeof(kMelody) / sizeof(kMelody[0])),
    (uint8_t)(CH_COUNT + 0), 38 },
  { kHarmony, (uint16_t)(sizeof(kHarmony) / sizeof(kHarmony[0])),
    (uint8_t)(CH_COUNT + 1), 30 },
};
static bool s_musOn = false;


// Advances both voices. Called from update(), which has already established
// that sound is switched on at all.
static void musicUpdate(uint32_t nowMs) {
  if (!s_musOn) return;
  for (MusTrack& t : s_mus) {
    if ((int32_t)(nowMs - t.nextAt) < 0) continue;
    const MusNote& note = t.notes[t.i];
    if (note.midi >= MIDI_LO && note.midi <= MIDI_HI)
      hal::voice(t.ch, kMidiHz[note.midi - MIDI_LO], note.ms, W_TRI, t.vol);
    // Advance the clock by the note rather than from now, so rounding does not
    // accumulate into a tempo. If a frame hitch has already put us more than a
    // whole note behind, that is a stall rather than drift and chasing it would
    // rattle off the backlog at once -- resync instead.
    t.nextAt = ((int32_t)(nowMs - t.nextAt) > (int32_t)note.ms)
                   ? nowMs + note.ms
                   : t.nextAt + note.ms;
    if (++t.i >= t.n) t.i = 0;
  }
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
  // ...and the music, which schedules straight to the HAL and has no Voice slot
  // to clear. Left running it would keep feeding notes to a silenced speaker and
  // come back mid-phrase the moment sound was switched on again.
  s_musOn = false;
}

bool enabled() { return s_on; }

void musicSet(bool playing) {
  if (playing == s_musOn) return;
  s_musOn = playing;
  // Starting picks up where it left off rather than rewinding. Three days into
  // a run, restarting from bar one every dawn would make the piece feel shorter
  // than it is; carrying the position means each morning opens somewhere new.
  // The clock is what has to be reset -- it has been standing still since dusk.
  if (playing)
    for (MusTrack& t : s_mus) t.nextAt = s_nowMs;
}

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

  musicUpdate(nowMs);

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
