// =============================================================================
//  facing.h — which drawn view of a mob the camera is looking at
//
//  Pure arithmetic, no Arduino and no renderer state, so the host tests can ask
//  the same question drawMobs asks. It lives in its own header rather than
//  inside render.cpp precisely because it was wrong once and there was no way
//  to prove it from the host: the mobs simply all faced front and the only
//  instrument was a photograph.
// =============================================================================
#pragma once

#include <stdint.h>

namespace facing {

// The three authored views, in the order the frames are laid out.
enum View : uint8_t { V_FRONT = 0, V_SIDE = 1, V_BACK = 2 };

// (hx, hy) is the mob's heading and (spx, spy) runs from the camera to the mob,
// so the direction back to the eye is the negation of the latter. `f` is how
// much the mob is facing us and `c` says which side of it we are on.
//
// Both scale by |h| * |v|, so the comparison is scale-free and the heading
// never has to be a true unit vector -- which is what lets it be a pair of
// signed bytes on the mob.
//
// An even quadrant split: 90 degrees of front, 90 of back, 90 of each side.
// With three views anything else stretches one of them over an angle it was not
// drawn for, and a mob walking straight at you wants to sit dead centre of
// FRONT rather than near a boundary, where it would flicker.
//
// A mob that has never moved has no heading and is drawn front-on.
inline View pickView(int hx, int hy, float spx, float spy, bool& mirror) {
  mirror = false;
  if (hx == 0 && hy == 0) return V_FRONT;
  const float f = -((float)hx * spx + (float)hy * spy);
  const float c = -((float)hx * spy - (float)hy * spx);
  const float ac = c < 0.0f ? -c : c;
  if (f >=  ac) return V_FRONT;
  if (f <= -ac) return V_BACK;
  mirror = (c < 0.0f);
  return V_SIDE;
}

}  // namespace facing
