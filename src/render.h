// =============================================================================
//  render.h — framebuffers, shading, and everything that touches a pixel
//
//  Two raw framebuffers rather than an M5Canvas, in the panel's own byte order
//  (lgfx::swap565_t). That combination is what buys the frame rate: the CPU
//  builds the next frame while the previous one is still going out over SPI,
//  and the DMA path never converts a pixel on the way.
//
//  The cost of owning the buffer is that M5GFX's drawing calls cannot be used
//  on it — hence the small primitive set below and the font in font5x7.h.
// =============================================================================
#pragma once

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include "game.h"
#include "raycast.h"

namespace render {

constexpr int W = raycast::VIEW_W;
constexpr int H = raycast::VIEW_H;

// Allocates the two framebuffers. Deliberately separate from attach(), and
// called first thing in setup(): 130 KB of DMA-capable internal RAM in two
// contiguous blocks is the tightest allocation in the program, and asking for
// it before the display and keyboard libraries have fragmented the heap costs
// nothing. There is no graceful degradation if it fails.
bool reserve();

// Binds the panel. Call after the HAL has brought the display up.
void attach(LGFX_Device& disp);

// Packs to the panel's byte order. Every colour in the program goes through
// here, so nothing downstream has to think about endianness.
//
// Native RGB565 is rrrrrggg gggbbbbb; the ST7789 wants those two bytes the
// other way round. Swapping at the point colours are made means the DMA path
// hands the buffer straight to the bus (lgfx::swap565_t -> no_convert) and
// nothing in a hot loop ever swaps a byte.
constexpr uint16_t pack(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}

// ---- primitives, all writing to the buffer being built ----------------------

void fill(uint16_t c);
void rect(int x, int y, int w, int h, uint16_t c);
void frameRect(int x, int y, int w, int h, int t, uint16_t c);
void text(int x, int y, const char* s, uint16_t c, int scale = 1);
int  textWidth(const char* s, int scale = 1);
void textCentred(int cx, int y, const char* s, uint16_t c, int scale = 1);

// ---- block art outside the 3D pass ------------------------------------------
//
// The same 16x16 material art the walls and floors are drawn from, available to
// the UI. It already lives in RAM for the renderer's sake, so an icon drawn
// from it costs no data and no flash -- only a different way of reading what is
// there. `top` picks the top-face tile where a material has one (grass), `mul`
// is the 0..256 fixed-point darkening the UI already uses for things you cannot
// afford, and both write straight into the framebuffer rather than going
// through rect() per texel, which is what makes a full-screen tiling affordable.

// One tile stretched to fill w x h. This is what a block preview is.
void stretchTex(int x, int y, int w, int h, uint8_t mat, bool top, int mul = 256);

// The tile repeated at `zoom` pixels per texel. This is what a background is.
void tileTex(int x, int y, int w, int h, uint8_t mat, bool top, int zoom,
             int mul = 256);

// ---- the 3D pass ------------------------------------------------------------

// Rebuilds the sky, ground and block shade tables for this instant of the
// day/night cycle. Must be called before drawWorld.
//
// horizon is the camera's, so the sky/ground gradient shears with the pitch
// rather than staying nailed to the panel while the world slides under it.
void shadeFor(float daylight, int horizon = raycast::HORIZON);

// Walks every screen column and paints it. Also fills the per-column
// occlusion table that drawMobs needs, so drawMobs must follow it.
//
// (selX, selY) is the column under the crosshair; its top block is drawn lit
// and outlined. Pass (-1, -1) for none.
void drawWorld(const raycast::Camera& cam, int selX, int selY, int selZ);
// Sun, moon and stars, drawn into whatever the terrain left as sky. Must run
// after drawWorld, which is what fills the occlusion table it clips against.
void drawSky(const raycast::Camera& cam, float daylight);

// The camera is passed in rather than read from the state: the frame may be
// drawn from a shaken copy, and mobs have to shake with the world they are
// standing in.
void drawMobs(const game::State& s, const raycast::Camera& cam);

// Items lying on the floor. Clipped against the same per-column tables mobs and
// particles use, so a drop behind a wall or under a bridge deck is hidden by
// it. Must follow drawWorld for the same reason drawMobs must.
void drawDrops(const game::State& s, const raycast::Camera& cam);

// ---- particles --------------------------------------------------------------
//
// Split in two on purpose. stepParticles advances the simulation and is called
// only from the play screen; drawParticles paints and is called from every
// screen that shows the world, including the frozen backdrop behind the pause
// and upgrade cards. One call would leave a smoke plume drifting while the game
// is paused, which is exactly the kind of thing that reads as unfinished.
void emit(const game::Spark& sp);
void stepParticles();
void drawParticles(const raycast::Camera& cam);
// The pickaxe in the corner of the frame. Its head takes its colour from the
// mining upgrade level, so a bought upgrade is visible in the player's hand
// rather than only on a menu they already closed.
void drawTool(const game::State& s);

void drawHurt(const game::State& s);

// Hands the finished buffer to the panel and flips. Deliberately does not wait
// for the transfer: with two buffers, the one we return to has always had a
// full transfer complete after it, so the CPU can start the next frame at once.
void present();

// Blocks until the panel is idle. Only needed before tearing things down or
// handing the display to another drawing path.
void flush();

// The buffer currently being built, in the panel's byte order. Only the
// screenshot responder uses this, and only between the last draw call and
// present().
const uint16_t* buildBuffer();

#ifdef DEV_SERIAL
// Per-stage frame cost, so tuning is driven by measurement rather than by
// guesswork about which stage is expensive.
extern uint32_t g_floorSpans, g_floorTall, g_floorPix, g_floorSeg;
extern uint32_t g_usWorld, g_usMobs, g_usSky, g_usSel, g_usShade;
#endif

// Brings up the second core's column worker. Call once, after reserve() and
// attach(), from the task that will be calling drawWorld.
void startWorker();

}  // namespace render
