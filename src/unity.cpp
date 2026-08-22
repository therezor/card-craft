// =============================================================================
//  unity.cpp — the hot translation units, compiled as one
//
//  This exists because -flto does not work with this toolchain, for a reason
//  that has nothing to do with this code: the plugin has to be on the link line
//  to resolve src/'s objects at all, which collects the Arduino core, and there
//  it drops app_main() as unreferenced because the only caller lives in a
//  precompiled libfreertos.a it cannot see (see platformio.ini).
//
//  The inlining LTO would have bought is available anyway, and without touching
//  the link line, by handing the compiler one translation unit instead of four.
//  world::cellAt() was the hand-rolled version of this — one batched call
//  standing in for five cross-module ones the compiler could not inline — and
//  it is exactly the kind of workaround that stops being necessary here.
//
//  Only the four files in the frame's path. ui, sfx and screenshot are cold and
//  stay separate, because a unity build costs incremental rebuild time and
//  there is no reason to pay it for code that is not measured.
//
//  There is no name-collision hazard to manage: every static in these files
//  lives inside its own namespace already, so merging them changes no linkage.
//  The native test env deliberately does NOT use this file — it compiles the
//  three testable units directly, so a host test failure points at a file.
// =============================================================================

#include "world.cpp"     // NOLINT(bugprone-suspicious-include)
#include "raycast.cpp"   // NOLINT(bugprone-suspicious-include)
#include "game.cpp"      // NOLINT(bugprone-suspicious-include)
#include "render.cpp"    // NOLINT(bugprone-suspicious-include)
