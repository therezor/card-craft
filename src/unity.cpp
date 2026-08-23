// =============================================================================
//  unity.cpp — the four files in the frame's path, compiled as one translation
//  unit so the compiler can inline across them.
//
//  ui, sfx and screenshot are cold and stay separate: a unity build costs
//  incremental rebuild time and there is no reason to pay it for code that is
//  not measured. The native test env compiles the testable units directly, so a
//  host test failure points at a file.
// =============================================================================

#include "world.cpp"     // NOLINT(bugprone-suspicious-include)
#include "raycast.cpp"   // NOLINT(bugprone-suspicious-include)
#include "game.cpp"      // NOLINT(bugprone-suspicious-include)
#include "render.cpp"    // NOLINT(bugprone-suspicious-include)
