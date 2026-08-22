// =============================================================================
//  sfx.h — a small non-blocking sound sequencer
//
//  Sits above the HAL, so every board with a speaker gets it for free: a cue is
//  a short table of steps, and update() walks the active ones once a frame.
//
//  It exists because a game event is rarely one tone. A block breaking is a
//  thud and a crack; an explosion is a noise burst falling into a boom; a swing
//  that misses is air. Spelling those out as data rather than as a single
//  beep() is the whole difference between a sound and a beep.
//
//  Channels are fixed by role rather than allocated, so a cue can never cut a
//  more important one. main.cpp used to pick exactly one event per frame and
//  drop the other twelve, because a single voice was all there was — mining
//  while something bit you was silent about one of the two.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sfx {

// Timbres. Which one a sound uses carries as much as its pitch does: a mining
// thunk on a sine is a doorbell, and on a square with noise under it is a pick
// hitting rock.
enum Wave : uint8_t { W_SQUARE, W_SAW, W_TRI, W_NOISE, W_COUNT };

// Roles, not indices. Two cues on the same channel take turns; cues on
// different channels sound together.
enum Channel : uint8_t {
  CH_IMPACT,   // hits, breaks, explosions — the loudest thing in the frame
  CH_VOICE,    // mobs: hiss, telegraph, death
  CH_TOOL,     // the pickaxe: mining ticks and swings
  CH_UI,       // menus, pickups, the clock
  CH_COUNT
};

struct Step {
  uint16_t freq;   // Hz, 0 for a rest
  uint16_t ms;
  uint8_t  wave;
  uint8_t  vol;    // 0..255
};

struct Cue {
  // What the sound actually is: a waveform rendered offline by
  // tools/make-sfx.py and played back whole. See sfxdata.h.
  const int8_t* pcm;
  uint16_t      pcmLen;
  uint16_t      pcmMs;   // precomputed, so play() can tell a busy channel from
                         // a free one without asking the speaker
  uint8_t       vol;
  // Jitters the playback rate a few percent per play. Only worth it on the
  // sounds that repeat quickly: eight identical mining ticks a second is a
  // machine gun, and the ear notices the sameness long before the sound.
  bool          varies;

  // The fallback, for a board whose speaker cannot take PCM. Less sound, not no
  // sound — the same bargain hal.h makes for every other optional capability.
  const Step*   step;
  uint8_t       n;

  uint8_t       ch;
  uint8_t       prio;   // a louder cue on the same channel interrupts a quieter one
};

// The cues the game plays. Named rather than numbered so main.cpp reads as a
// list of what happened, not a table of frequencies.
// Digging, breaking and placing come in three material families rather than one
// cue apiece. A pickaxe in dirt and a pickaxe in iron used to be the same sound,
// which is most of the reason mining read as a UI event rather than as work.
enum MatClass : uint8_t { MC_SOFT, MC_STONE, MC_WOOD, MC_COUNT };

// Which family a block belongs to. Soft is anything you would use a shovel on,
// wood is anything grown or milled, stone is the rest.
MatClass matClass(uint8_t block);

extern const Cue kDig[MC_COUNT], kBreak[MC_COUNT], kPlace[MC_COUNT];
extern const Cue kNoBlocks;
extern const Cue kSwing, kWhiff;

// A voice per mob, because a night you can hear is a night you can read: a
// groan behind you and a bone rattle to your left are two different problems,
// and on a panel this small the ear does more of that work than the eye.
// Indexed by game::MobKind.
extern const Cue kMobIdle[3], kMobHurt[3], kMobDied[3];

extern const Cue kHurt, kDied, kTelegraph, kHiss, kExplode;
extern const Cue kArrowFire, kArrowHit;
extern const Cue kDusk, kDawn, kMenuMove, kBuy, kCraft, kCraftFail;

// ---- background music -------------------------------------------------------
//
// Daylight has a tune under it. It is two voices of note data rather than a
// recording -- a rendered piece of this length would be megabytes of PCM, and
// the whole sound bank is 10,000 lines for a couple of seconds of effects.
// Notes are a few hundred bytes and loop for as long as the sun is up.
//
// It plays on channels ABOVE CH_COUNT, so it occupies none of the four the cues
// share out and can never be the reason a creeper hiss is not heard.
//
// Switched on and off by the caller rather than by the clock in here, because
// what counts as "day" is the simulation's business: see the call in main.cpp.
// Turning it off stops scheduling notes and lets the last one ring out, which
// at under a second is a fade rather than a cut. Sound being switched off kills
// it outright, like everything else.
void musicSet(bool playing);

// Starts a cue. Cheap enough to call unconditionally: a cue that loses to a
// higher-priority one still playing is dropped here rather than at the speaker.
void play(const Cue& c);

// Advances every channel. Call once a frame with a millisecond clock.
void update(uint32_t nowMs);

// The sound setting, off the pause card. This is the one gate — every sound the
// game makes goes through play(), so there is nowhere else for one to leak out.
// Switching off also stops whatever is in flight, so a fuse does not outlive
// the decision to silence it.
void setEnabled(bool on);
bool enabled();

}  // namespace sfx
