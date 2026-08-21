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
  const Step* step;
  uint8_t     n;
  uint8_t     ch;
  uint8_t     prio;   // a louder cue on the same channel interrupts a quieter one
};

// The cues the game plays. Named rather than numbered so main.cpp reads as a
// list of what happened, not a table of frequencies.
extern const Cue kMineTick, kBlockBroke, kPlace, kNoBlocks;
extern const Cue kSwing, kWhiff, kMobHit, kMobDied;
extern const Cue kHurt, kDied, kTelegraph, kHiss, kExplode;
extern const Cue kDusk, kDawn, kMenuMove, kBuy, kCraft, kCraftFail;

// Starts a cue. Cheap enough to call unconditionally: a cue that loses to a
// higher-priority one still playing is dropped here rather than at the speaker.
void play(const Cue& c);

// Advances every channel. Call once a frame with a millisecond clock.
void update(uint32_t nowMs);

}  // namespace sfx
