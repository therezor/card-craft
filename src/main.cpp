// =============================================================================
//  main.cpp — screens, the fixed timestep, and the frame
//
//  Simulation runs at a fixed 60 Hz and rendering runs as fast as the panel
//  will take it. They are separate on purpose: the SPI transfer is ~13 ms of
//  the frame no matter what, and letting the physics inherit that jitter would
//  make mining speed and mob speed depend on how much of the world is on
//  screen. tick() is handed a count, never a delta.
//
//  Frame shape:
//      hal::update()      poll the keyboard
//      catch-up ticks     game::tick() x N, N usually 1
//      shadeFor()         rebuild the day/night tables
//      cast()             240 columns of DDA
//      drawWorld/Mobs     into the back buffer
//      ui::*              HUD into the same buffer
//      present()          hand it to the DMA and flip
// =============================================================================
#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include "game.h"
#include "hal/hal.h"
#include "raycast.h"
#include "render.h"
#include "screenshot.h"
#include "sfx.h"
#include "ui.h"
#include "world.h"

namespace {

enum Screen : uint8_t { SCR_TITLE, SCR_PLAY, SCR_PAUSE, SCR_CRAFT, SCR_RECIPES,
                        SCR_CONTROLS, SCR_DEAD };

constexpr uint32_t TICK_US   = 1000000u / game::TICK_HZ;
constexpr int      MAX_CATCHUP = 4;    // never simulate more than this per frame
constexpr int      LOCKOUT_FRAMES = 8; // swallow the key that dismissed a card

Screen           s_scr = SCR_TITLE;
game::State      s_game;
Preferences      s_prefs;
uint32_t         s_best = 0;
bool             s_record = false;
bool             s_sound = false;                 // the pause-card setting, kept in NVS
ui::Menu         s_menu;
// The pause card is the only Menu left: the recipe book draws itself now, and
// draws pictures rather than rows. Five is what openPauseMenu() writes -- one
// more than fits at once, which is what Menu's scrolling window is for.
ui::MenuItem     s_items[6];
char             s_title[28];                     // menu heading, rebuilt per open
char             s_soundRow[16];                  // "SOUND: ON", rebuilt on toggle
char             s_fpsRow[16];                    // "FPS: ON", rebuilt on toggle
uint32_t         s_tickAccum = 0;
uint32_t         s_lastUs = 0;
int              s_lockout = 0;
uint16_t         s_mineChirp = 0;

// ---- audio ------------------------------------------------------------------

// Every event that fired this frame, each on the channel its role belongs to.
// The speaker is an I2S amplifier and M5Unified mixes eight channels through
// it, so mining while a creeper hisses at you is two sounds rather than a coin
// toss between them — the hiss is the only warning the player gets.
void playEvents(uint32_t ev) {
  // The three cues that need a subject read it off State. An event is a bit and
  // a bit cannot say WHICH — which material is under the pick, which mob was
  // hit — so the simulation leaves the answer next to the flag.
  const sfx::MatClass mc = sfx::matClass(s_game.sfxDigMat);
  const uint8_t hitKind  = s_game.sfxHitKind  % 3;
  const uint8_t diedKind = s_game.sfxDiedKind % 3;
  const uint8_t idleKind = s_game.sfxIdleKind % 3;

  if (ev & game::EV_DIED)        sfx::play(sfx::kDied);
  if (ev & game::EV_EXPLODE)     sfx::play(sfx::kExplode);
  if (ev & game::EV_HURT)        sfx::play(sfx::kHurt);
  if (ev & game::EV_MOB_DIED)    sfx::play(sfx::kMobDied[diedKind]);
  if (ev & game::EV_HISS)        sfx::play(sfx::kHiss);
  if (ev & game::EV_TELEGRAPH)   sfx::play(sfx::kTelegraph);
  if (ev & game::EV_DUSK)        sfx::play(sfx::kDusk);
  if (ev & game::EV_DAWN)        sfx::play(sfx::kDawn);
  if (ev & game::EV_BLOCK_BROKE) sfx::play(sfx::kBreak[mc]);
  // A tool going is the one thing here the player cannot see happen -- it is a
  // slot emptying at the bottom of the screen while they are looking at a wall.
  if (ev & game::EV_TOOL_BROKE)  sfx::play(sfx::kCraftFail);
  // A heal is a pickup with a consequence, so it takes the pickup's place in
  // the chain rather than sitting alongside it: one heart raises both bits, and
  // kBuy under kCraft is two UI cues stacked in a single frame.
  if (ev & game::EV_HEAL)        sfx::play(sfx::kCraft);
  else if (ev & game::EV_PICKUP) sfx::play(sfx::kBuy);
  else if (ev & game::EV_DROP)   sfx::play(sfx::kMenuMove);
  if (ev & game::EV_MOB_HIT)     sfx::play(sfx::kMobHurt[hitKind]);
  else if (ev & game::EV_SWING)  sfx::play(sfx::kSwing);
  else if (ev & game::EV_WHIFF)  sfx::play(sfx::kWhiff);
  if (ev & game::EV_ARROW_FIRE)  sfx::play(sfx::kArrowFire);
  if (ev & game::EV_ARROW_HIT)   sfx::play(sfx::kArrowHit);
  if (ev & game::EV_PLACE)       sfx::play(sfx::kPlace[mc]);
  if (ev & game::EV_NO_BLOCKS)   sfx::play(sfx::kNoBlocks);
  // Something out in the dark, talking to itself. Lowest priority on its
  // channel, so it never lands on top of a fuse.
  if (ev & game::EV_MOB_IDLE)    sfx::play(sfx::kMobIdle[idleKind]);
  if (ev & game::EV_MINE_STEP) {
    // Mining fires every tick; a sound per tick is a buzz. Every eighth is a
    // pickaxe rhythm instead.
    if (++s_mineChirp >= 8) { s_mineChirp = 0; sfx::play(sfx::kDig[mc]); }
  }
}

// ---- the frame counter a player can switch on -------------------------------
//
// Just the number, and off by default. A number burnt into the corner of a game
// is hard to stop reading once you have started, which is why it is opt-in
// rather than opt-out -- and it is the only thing drawn on the panel now: the
// dev build's frame-time figures go over the wire instead. See below.
//
// Sampled every frame whether or not it is shown, so switching it on reads the
// rate you are actually getting rather than counting up from nothing.
bool     s_showFps = false;
uint32_t s_fpsPrevUs = 0, s_fpsAccUs = 0;
int      s_fpsFrames = 0;
char     s_fpsText[12] = "FPS: --";

void fpsCount() {
  const uint32_t now = micros();
  if (s_fpsPrevUs) {
    s_fpsAccUs += now - s_fpsPrevUs;
    ++s_fpsFrames;
    // Twice a second. Any faster and the last digit is a flicker to be squinted
    // at rather than a reading.
    if (s_fpsAccUs >= 500000u) {
      const unsigned fps =
          (unsigned)((uint64_t)s_fpsFrames * 1000000ull / s_fpsAccUs);
      snprintf(s_fpsText, sizeof(s_fpsText), "FPS: %u", fps > 999 ? 999u : fps);
      s_fpsAccUs = 0;
      s_fpsFrames = 0;
    }
  }
  s_fpsPrevUs = now;
}

// ---- frame-time telemetry ---------------------------------------------------
//
// Frame period, average CPU cost and the worst one, reported over USB. Never
// drawn on the panel: the player-facing frame counter above is the only thing
// that belongs in the corner of a game.

#ifdef SHOW_FPS
uint32_t s_fpsSum = 0, s_cpuSum = 0, s_cpuMax = 0;
int      s_fpsN = 0;
char     s_fpsLine[24] = "";   // formatted for the serial line, not for the panel
uint32_t s_usSfx = 0;          // the mixer's slice, measured where it is called

void fpsSample(uint32_t frameUs, uint32_t cpuUs) {
  s_fpsSum += frameUs;
  s_cpuSum += cpuUs;
  if (cpuUs > s_cpuMax) s_cpuMax = cpuUs;
  if (++s_fpsN < 30) return;
  const uint32_t avgFrame = s_fpsSum / (uint32_t)s_fpsN;
  const uint32_t avgCpu   = s_cpuSum / (uint32_t)s_fpsN;
  const unsigned fps = avgFrame ? (unsigned)(1000000u / avgFrame) : 0;
  // "58F 3.4C 5.1P" — frames per second, average CPU ms, worst CPU ms.
  snprintf(s_fpsLine, sizeof(s_fpsLine), "%uF %u.%uC %u.%uP",
           fps,
           (unsigned)(avgCpu / 1000), (unsigned)((avgCpu % 1000) / 100),
           (unsigned)(s_cpuMax / 1000), (unsigned)((s_cpuMax % 1000) / 100));
#ifdef DEV_SERIAL
  int mobs = 0;
  for (int i = 0; i < game::MAX_MOBS; ++i) if (s_game.mobs[i].alive) ++mobs;
  Serial.printf("fps=%u avg_cpu_us=%u max_cpu_us=%u mobs=%d "
                "world_us=%u mobs_us=%u sky_us=%u sel_us=%u shade_us=%u "
                // What is left after the framebuffers are up. Reported rather
                // than reasoned about: the build's own figures are static bytes
                // against a total the framebuffers are not counted in, so the
                // two cannot be subtracted to get this. Every new static array
                // is spent against this number.
                "heap=%u heapmax=%u "
                "present_us=%u wait_us=%u sfx_us=%u cast_us=%u draw_us=%u "
                "fspans=%u ftall=%u fpix=%u fseg=%u\n",
                fps, (unsigned)avgCpu, (unsigned)s_cpuMax, mobs,
                (unsigned)render::g_usWorld, (unsigned)render::g_usMobs,
                (unsigned)render::g_usSky, (unsigned)render::g_usSel,
                (unsigned)render::g_usShade,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)render::g_usPresent, (unsigned)render::g_usWait,
                (unsigned)s_usSfx,
                // Cycles over 30 frames at 240 MHz, and both cores fold into the
                // same accumulator, so this is CPU-microseconds per frame across
                // the two of them -- not wall-clock, which is what world_us is.
                (unsigned)(render::g_cyCast / (240u * 30u)),
                (unsigned)((render::g_cyCols - render::g_cyCast) / (240u * 30u)),
                (unsigned)render::g_floorSpans, (unsigned)render::g_floorTall,
                (unsigned)render::g_floorPix, (unsigned)render::g_floorSeg);
  render::g_floorSpans = render::g_floorTall = 0;
  render::g_floorPix = render::g_floorSeg = 0;
  render::g_cyCast = render::g_cyCols = 0;
#endif
  s_fpsSum = s_cpuSum = s_cpuMax = 0;
  s_fpsN = 0;
}

// Top left.
#endif

#ifdef DEV_SERIAL
// Synthetic play, so the frame rate can be measured the same way twice. Held
// forward + turn + mine sweeps the camera over the whole island and keeps the
// mining path hot; left running it walks into dusk and meets a real wave.
// Any seed would do; what matters is that it never changes.
constexpr uint32_t BENCH_SEED = 0xCA2DC4A7u;
bool     s_bench = false;
uint32_t s_benchFrames = 0;

// Holds a look key down: -1 down, 0 neither, +1 up. Drives the real Input flags
// rather than writing the horizon, so what gets measured is the clamp and the
// drift as the game runs them, not a value poked past both.
int8_t   s_look = 0;
#endif

// ---- helpers ----------------------------------------------------------------

// The book's cursor. It is UI and only UI -- unlike the grid cursor, which
// lives in State because the host tests lay recipes out and commit them --
// so it stays here rather than being carried through the simulation.
int s_book = 0;

// The title card's cursor, and where the controls card has to go back to. The
// controls card is reachable from two places, and "one level up" has to mean
// the level it was actually opened from.
int    s_titleSel = 0;
Screen s_ctrlFrom = SCR_TITLE;

// True on the screens that are inside a run -- the ones with a world behind
// them. The title, the death card and the controls card reached from the title
// are the outside; everything else is somewhere the player has paused in the
// middle of a day, and the music should carry on across all of it.
bool inRun() {
  switch (s_scr) {
    case SCR_PLAY: case SCR_PAUSE: case SCR_CRAFT: case SCR_RECIPES:
      return true;
    case SCR_CONTROLS:
      return s_ctrlFrom != SCR_TITLE;
    default:
      return false;
  }
}

// The craft card's control hint, built from caps() rather than spelled out, so
// a board that binds these elsewhere gets its own keys named back to it.
const char* menuFoot() {
  static char foot[40];
  snprintf(foot, sizeof(foot), "%s MOVE  %s OK  %s BACK",
           hal::caps().kMove, hal::caps().kConfirm, hal::caps().kBack);
  return foot;
}

const char* cardFoot() {
  static char foot[44];
  // Short on purpose: the card is 168 px wide and a longer hint runs off both
  // ends and over the hotbar. The arrows need no hint -- the cursor is visible
  // and moving it is the first thing anyone tries.
  snprintf(foot, sizeof(foot), "%s OK   %s BACK",
           hal::caps().kConfirm, hal::caps().kBack);
  return foot;
}

// Four rows is what the card can SHOW at once: Menu::draw sizes itself to its
// contents and four comes to 127 px of the 135 the panel has. Five items is
// fine -- the window scrolls and puts a chevron in the margin, which is
// machinery Menu already has and nothing was using.
constexpr int PAUSE_ROWS = 6;
void openPauseMenu() {
  snprintf(s_soundRow, sizeof(s_soundRow), "SOUND: %s", s_sound ? "ON" : "OFF");
  snprintf(s_fpsRow,   sizeof(s_fpsRow),   "FPS: %s",   s_showFps ? "ON" : "OFF");
  s_items[0] = ui::MenuItem{ "RESUME",    nullptr,         true };
  s_items[1] = ui::MenuItem{ "CONTROLS",  "what does what", true };
  s_items[2] = ui::MenuItem{ s_soundRow,  nullptr,         true };
  s_items[3] = ui::MenuItem{ s_fpsRow,    "frame counter", true };
  s_items[4] = ui::MenuItem{ "NEW WORLD", "start over",    true };
  s_items[5] = ui::MenuItem{ "QUIT",      "back to title", true };
  snprintf(s_title, sizeof(s_title), "PAUSED  NIGHT %u", (unsigned)s_game.night);
  s_menu.open(s_title, s_items, PAUSE_ROWS);
}

void startRun() {
  // esp_random() is seeded from hardware noise, so two runs from a cold boot
  // do not produce the same island.
  game::begin(s_game, esp_random() ^ micros());
  s_scr = SCR_PLAY;
  s_record = false;
  s_lockout = LOCKOUT_FRAMES;
  s_tickAccum = 0;
  s_lastUs = micros();
}

// Back to play from a card. The tick accumulator has to be cleared as well as
// the clock reset: the wall time spent on the card would otherwise be handed to
// the simulation as a backlog and the player would come back to a burst of
// movement they did not ask for.
void resume() {
  s_scr = SCR_PLAY;
  s_menu.close();
  s_lockout = LOCKOUT_FRAMES;
  s_tickAccum = 0;
  s_lastUs = micros();
}

void endRun() {
  const uint32_t sc = game::score(s_game);
  if (sc > s_best) {
    s_best = sc;
    s_record = true;
    s_prefs.putUInt("best", s_best);
  }
  s_scr = SCR_DEAD;
  s_lockout = LOCKOUT_FRAMES;
}

// Renders the world exactly as it stands. Used by the play screen and, frozen,
// as the backdrop behind the upgrade and death cards.
void drawScene() {
  // The camera the frame is drawn from, which is not the camera the simulation
  // owns. Screen shake is a copy: shaking State::cam itself would drag the aim
  // ray along with it — the crosshair would stop agreeing with what a swing
  // hits — and it would make a run unreproducible on the host.
  raycast::Camera cam = s_game.cam;
  if (s_game.shake) {
    // Alternating rather than random, so it reads as a jolt rather than as
    // noise, and so it cannot wander off centre.
    const int k = (int)s_game.shake;
    const int sign = (k & 1) ? 1 : -1;
    raycast::setPitch(cam, s_game.cam.horizon + sign * (k >> 2));
  }

  render::shadeFor(game::daylight(s_game), cam.horizon);
  render::drawWorld(cam,
                    s_game.aimValid ? s_game.aimX : -1,
                    s_game.aimValid ? s_game.aimY : -1,
                    s_game.aimValid ? s_game.aimZ : -1);
  render::drawSky(cam, game::daylight(s_game));
  render::drawMobs(s_game, cam);
  render::drawDrops(s_game, cam);
  render::drawParticles(cam);
  render::drawTool(s_game);
  render::drawHurt(s_game);
}

}  // namespace

// -----------------------------------------------------------------------------

void setup() {
  // Before anything else: two contiguous 64.8 KB DMA blocks are the tightest
  // allocation in the program, and the display and keyboard libraries have not
  // had a chance to fragment the heap yet.
  const bool got = render::reserve();

  hal::begin();
  render::attach(hal::display());
  raycast::init();

  if (!got) {
    // Nothing else will work, and a blank screen would look like a dead board.
    hal::display().fillScreen(TFT_BLACK);
    hal::display().setTextColor(TFT_RED);
    hal::display().drawString("NO DMA MEMORY", 8, 8);
    for (;;) delay(1000);
  }

#ifdef DEV_SERIAL
  shot::begin();
#endif

  render::startWorker();

  // The namespace moved with the rename. A player's best night is the only
  // thing this game asks them to keep, so it is carried across rather than
  // quietly reset: read the old namespace once, write it into the new one, and
  // never look at it again.
  s_prefs.begin("cardcraft", false);
  s_best = s_prefs.getUInt("best", 0);
  if (s_best == 0) {
    Preferences old;
    if (old.begin("esp32craft", true)) {
      const uint32_t carried = old.getUInt("best", 0);
      old.end();
      if (carried) {
        s_best = carried;
        s_prefs.putUInt("best", s_best);
      }
    }
  }

  // Sound is ON out of the box. It shipped silent, which was the wrong default
  // for a game whose mobs announce themselves before they arrive -- a creeper
  // hisses and a zombie winds up, and a player who never opens the pause card
  // never finds out that either of those makes a noise.
  //
  // Flipping the default is all this needs, and it is worth saying why, because
  // the last change here had to rename the key to take effect. getBool only
  // returns the default when nothing is stored, and the key is written only by
  // the toggle. So a board that has never touched the setting picks up the new
  // default and starts making noise, while anyone who deliberately switched
  // sound off keeps their stored false and stays silent. Both are what you
  // want; there is nothing to retire this time.
  s_sound = s_prefs.getBool("snd2", true);
  sfx::setEnabled(s_sound);
  s_showFps = s_prefs.getBool("fps1", false);

  // A caption for the title we are about to show, and it has to be a DIFFERENT
  // one every time the board is switched on. micros() alone is not enough and
  // was the bug here: at this point in setup it is boot time, which lands
  // within a few hundred microseconds of the same value on every cold start,
  // so mixing it produced the same caption every reset.
  //
  // Two independent things fix it, because either alone can let you down.
  // esp_random() is the hardware generator and is the real source, but with the
  // RF subsystem down -- which it is, this game never brings up WiFi -- the
  // ESP-IDF docs are explicit that it is not guaranteed random and may repeat.
  // So the last caption is also remembered in NVS and handed back to the UI
  // before the roll, and rerollSplash draws from the other N-1: even if the
  // seed were a constant, the line would still have to change.
  ui::setSplashIndex(s_prefs.getInt("splash", -1));
  ui::rerollSplash(esp_random() ^ micros());
  s_prefs.putInt("splash", ui::splashIndex());

  s_lastUs = micros();
}

void loop() {
#ifdef SHOW_FPS
  const uint32_t frameStart = micros();
  static uint32_t s_prevFrame = 0;
#endif

  hal::update();
  const hal::Buttons& b = hal::buttons();
  if (s_lockout) --s_lockout;
  const bool confirm = (s_lockout == 0) && b.confirmEdge;
  const bool cancel  = (s_lockout == 0) && b.cancelEdge;

  switch (s_scr) {
    case SCR_TITLE:
      ui::title(hal::boardName(), s_best, s_titleSel);
      if (!s_lockout) {
        if (b.navUp || b.navDown) {
          s_titleSel = (s_titleSel + (b.navUp ? ui::TITLE_ROWS - 1 : 1)) % ui::TITLE_ROWS;
          sfx::play(sfx::kMenuMove);
        }
        if (confirm) {
          if (s_titleSel == 0) {
            startRun();
          } else {
            s_ctrlFrom = SCR_TITLE;
            s_scr = SCR_CONTROLS;
            s_lockout = LOCKOUT_FRAMES;
          }
        }
      }
      break;

    // Reachable from the title and from the pause card. It knows which, because
    // "back" has to mean the level it was opened from and not one fixed screen.
    case SCR_CONTROLS:
      if (s_ctrlFrom == SCR_PAUSE) { drawScene(); ui::hud(s_game); }
      else                         render::fill(0);
      ui::controlsCard(cardFoot());
      if (!s_lockout && (cancel || confirm)) {
        s_scr = s_ctrlFrom;
        s_lockout = LOCKOUT_FRAMES;
      }
      break;

    case SCR_PLAY: {
      if (cancel) {
        openPauseMenu();
        s_scr = SCR_PAUSE;
        s_lockout = LOCKOUT_FRAMES;
        drawScene();
        s_menu.draw(menuFoot());
        break;
      }
      if (b.craftEdge && !s_lockout) {
        s_scr = SCR_CRAFT;
        s_lockout = LOCKOUT_FRAMES;
        drawScene();
        ui::craftCard(s_game, cardFoot());
        break;
      }
      if (b.slotPick && !s_lockout) {
        game::selectSlot(s_game, (int)b.slotPick - 1);
        sfx::play(sfx::kMenuMove);
      }

      const uint32_t now = micros();
      s_tickAccum += now - s_lastUs;
      s_lastUs = now;

      game::Input in;
      in.left  = b.left;   in.right = b.right;
      in.fwd   = b.fwd;    in.back  = b.back;
      in.act   = b.act && !s_lockout;
      in.build = b.build && !s_lockout;
      in.drop  = b.drop  && !s_lockout;
      in.lookUp     = b.lookUp;
      in.lookDown   = b.lookDown;
      in.lookCentre = b.lookCentre;
      // Lockout-gated like the other acting keys, so the press that dismissed a
      // card cannot also launch the player the instant play resumes.
      in.jump  = b.jump  && !s_lockout;
#ifdef DEV_SERIAL
      if (s_bench) {
        in = game::Input{};
        in.fwd = true;
        in.act = true;
        // Reverse every few seconds, so it sweeps new ground instead of
        // wearing a circle into one corner of the map.
        in.right = ((s_benchFrames / 240) & 1) != 0;
        in.left  = !in.right;
      }
      // After the bench block, which clears the whole Input: a benchmark run at
      // full pitch is the measurement worth having.
      if (s_look > 0)      in.lookUp   = true;
      else if (s_look < 0) in.lookDown = true;
#endif

      int steps = 0;
      uint32_t ev = 0;
#ifdef DEV_SERIAL
      if (s_bench) {
        // Exactly one tick a frame, and the wall clock ignored. This is what
        // makes two runs comparable, which is the only thing the benchmark is
        // for: catch-up ticks are driven by elapsed time, so a slower build
        // takes more ticks per frame, walks further, and a few seconds in is
        // looking at a different scene. That feedback loop puts two runs of the
        // SAME build 12% apart on frame rate. Pinned at one tick per frame the
        // walk is frame-indexed and window N is the same simulated moment in
        // every build. The game runs slower than real time while benching,
        // which does not matter — nothing here measures how the game feels.
        s_tickAccum = 0;
        ev |= game::tick(s_game, in);
        steps = 1;
      } else
#endif
      while (s_tickAccum >= TICK_US && steps < MAX_CATCHUP) {
        s_tickAccum -= TICK_US;
        ev |= game::tick(s_game, in);
        ++steps;
      }
      // Falling this far behind means a stall, not a slow frame. Dropping the
      // backlog keeps a hitch from turning into a burst of free movement.
      if (s_tickAccum > TICK_US * MAX_CATCHUP) s_tickAccum = 0;

      // Positional effects the tick produced. Drained here rather than polled
      // from State, because the frame may have covered several ticks and each
      // of them may have thrown one.
      for (int i = 0; i < s_game.sparkN; ++i) render::emit(s_game.sparks[i]);
      s_game.sparkN = 0;
      render::stepParticles();

      playEvents(ev);
      drawScene();
      ui::crosshair(s_game);
      ui::hud(s_game);

      if (s_game.dead) endRun();
      break;
    }

    case SCR_CRAFT: {
      drawScene();
      ui::hud(s_game);
      ui::craftCard(s_game, cardFoot());
      if (!s_lockout) {
        // Four arrows move the cursor across every stop on the card -- the four
        // cells, the result, and the book row -- and ENTER does whatever the
        // cursor is on. There is no key here that means one particular thing.
        if (b.navLeft)  { game::gridMove(s_game, -1, 0); sfx::play(sfx::kMenuMove); }
        if (b.navRight) { game::gridMove(s_game,  1, 0); sfx::play(sfx::kMenuMove); }
        if (b.navUp)    { game::gridMove(s_game, 0, -1); sfx::play(sfx::kMenuMove); }
        if (b.navDown)  { game::gridMove(s_game, 0,  1); sfx::play(sfx::kMenuMove); }

        // The number row fills the focused cell outright. Laying out a shaped
        // recipe by cycling each cell through everything you carry is four laps
        // of a list; this is four keypresses, and it is the same gesture the
        // number row already means everywhere else.
        if (b.slotPick)
          sfx::play(game::gridSetFromSlot(s_game, b.slotPick) ? sfx::kMenuMove
                                                              : sfx::kCraftFail);
        if (cancel || b.craftEdge) {
          resume();
        } else if (confirm) {
          if (game::gridOnCell(s_game)) {
            game::gridCycle(s_game, 1);
            sfx::play(sfx::kMenuMove);
          } else if (s_game.gridSel == game::GRID_FOCUS_OUT) {
            sfx::play(game::craftGrid(s_game) ? sfx::kCraft : sfx::kCraftFail);
          } else {
            s_book = 0;
            s_scr = SCR_RECIPES;
            s_lockout = LOCKOUT_FRAMES;
          }
        }
      }
      break;
    }

    case SCR_RECIPES: {
      drawScene();
      ui::hud(s_game);
      ui::recipeBook(s_game, s_book, cardFoot());
      if (!s_lockout) {
        const int n = (int)game::R_COUNT;
        if (b.navUp)   { s_book = (s_book + n - 1) % n; sfx::play(sfx::kMenuMove); }
        if (b.navDown) { s_book = (s_book + 1) % n;     sfx::play(sfx::kMenuMove); }

        if (cancel) {
          // Back is one level up, and one level up from the book is the grid.
          s_scr = SCR_CRAFT;
          s_lockout = LOCKOUT_FRAMES;
        } else if (confirm) {
          // Lays the recipe out on the grid and goes back to it, rather than
          // crafting on the spot. The grid is where crafting happens, and a
          // book that quietly made things behind it would leave the player with
          // no idea what the four cells were for.
          if (game::fillGrid(s_game, (uint8_t)s_book)) {
            sfx::play(sfx::kMenuMove);
            s_game.gridSel = game::GRID_FOCUS_OUT;   // cursor on the thing to press
            s_scr = SCR_CRAFT;
            s_lockout = LOCKOUT_FRAMES;
          } else {
            sfx::play(sfx::kCraftFail);
          }
        }
      }
      break;
    }

    case SCR_PAUSE:
      // The world stays on screen behind the card, frozen. Cutting to a flat
      // background would lose where the player was standing, which is the one
      // thing they came back to the game to remember.
      drawScene();
      ui::hud(s_game);
      s_menu.draw(menuFoot());
      if (!s_lockout) {
        if (b.navUp)   { s_menu.move(-1); sfx::play(sfx::kMenuMove); }
        if (b.navDown) { s_menu.move(1);  sfx::play(sfx::kMenuMove); }
        if (cancel) resume();
        else if (confirm) {
          // Spelled out rather than leaning on default:, because the row that
          // falls through is whichever one was added last, and that is not a
          // thing the next person to add a row should have to notice.
          switch (s_menu.index()) {
            case 0: resume(); break;
            case 1:
              s_ctrlFrom = SCR_PAUSE;
              s_scr = SCR_CONTROLS;
              s_lockout = LOCKOUT_FRAMES;
              break;
            case 2:
              s_sound = !s_sound;
              s_prefs.putBool("snd2", s_sound);
              sfx::setEnabled(s_sound);
              // Turning it on says so out loud. Turning it off cannot, which is
              // the confirmation for that direction.
              if (s_sound) sfx::play(sfx::kBuy);
              openPauseMenu();          // relabel the row; the cursor survives
              break;
            case 3:
              s_showFps = !s_showFps;
              s_prefs.putBool("fps1", s_showFps);
              openPauseMenu();          // relabel the row; the cursor survives
              break;
            case 4: startRun(); break;
            default:
              ui::rerollSplash(micros());
              s_scr = SCR_TITLE;
              s_lockout = LOCKOUT_FRAMES;
              break;
          }
        }
      }
      break;

    case SCR_DEAD:
      drawScene();
      ui::deathCard(s_game, s_best, s_record);
      if (confirm) startRun();
      break;
  }

  // Top-left, and only while playing -- it is a reading about the world being
  // drawn, and there is no world behind the title or the controls card.
  if (s_showFps && s_scr == SCR_PLAY)
    render::text(3, 3, s_fpsText, render::pack(120, 220, 140), 1);

#ifdef SHOW_FPS
  const uint32_t cpuUs = micros() - frameStart;
#endif

#ifdef DEV_SERIAL
  if (Serial.available()) {
    const int cmd = Serial.read();
    if (cmd == 'i') {
      // Immortal, for playtesting. Not a heal and not a cheat menu -- it puts
      // the health bar somewhere the night cannot reach so the mobs can be
      // watched for as long as it takes, which is the only way to judge whether
      // a wander looks like a wander.
      if (s_game.maxHp > 1000) { s_game.maxHp = 10; s_game.hp = 10; }
      else { s_game.maxHp = 30000; s_game.hp = 30000; }
      Serial.printf("immortal=%d hp=%d\n", s_game.maxHp > 1000, (int)s_game.hp);
    } else if (cmd == 'k') {
      // A night, and a ring of mobs far enough out that they have to FIND you.
      // 'm' poses three at seven cells, which is close enough that all three
      // notice immediately and turn to face you -- fine for judging the front
      // art, useless for judging anything else. These start outside their own
      // sight range, wandering, so the approach is the thing on display.
      s_game.phase = game::PH_NIGHT;
      s_game.phaseTick = game::NIGHT_TICKS / 4;
      int n = 0;
      for (int k = 0; k < game::MAX_MOBS && n < 10; ++k) {
        game::Mob& m = s_game.mobs[k];
        if (m.alive) continue;
        const float a = (float)k * 0.9f;
        const float r = 13.0f + (float)(k % 4) * 1.5f;
        const float mx = s_game.cam.px + cosf(a) * r;
        const float my = s_game.cam.py + sinf(a) * r;
        if (!world::standable((int)mx, (int)my)) continue;
        m = game::Mob{};
        m.alive = true;
        m.kind  = (uint8_t)(k % game::MOB_COUNT);
        m.hp    = 20;
        m.x = mx; m.y = my;
        m.z = world::groundAt(mx, my);
        m.bestDist = 1e9f;
        m.side  = (uint8_t)(k & 1);
        m.state = game::MS_WANDER;
        m.voice = (uint16_t)(30 + k * 17);
        ++n;
      }
      Serial.printf("spawned=%d night\n", n);
    } else if (cmd == 'y') {
      // Back to the title card, which is otherwise reachable only by quitting
      // through the pause menu. The board boots into it, so without this the
      // screenshot tour can only photograph it on the first run after a flash.
      ui::rerollSplash(micros());
      s_scr = SCR_TITLE;
      s_titleSel = 0;
      s_lockout = LOCKOUT_FRAMES;
      Serial.printf("title\n");
    } else if (cmd == 'x' || cmd == 'z') {
      // Put a tool in the hand and select it -- 'x' a pickaxe, 'z' a sword. The
      // held item is drawn in first person and there is no way to reach one
      // over the wire otherwise: crafting takes a keypress on the craft card.
      const uint8_t kind = (cmd == 'z') ? (uint8_t)game::TK_SWORD
                                        : (uint8_t)game::TK_PICK;
      const uint8_t item = game::toolId(kind, game::TT_IRON);
      int slot = -1;
      for (int i = 0; i < game::SLOT_N; ++i)
        if (s_game.slot[i] == item) { slot = i; break; }
      if (slot < 0)
        for (int i = 0; i < game::SLOT_N; ++i)
          if (s_game.slot[i] == game::SLOT_EMPTY) { slot = i; break; }
      if (slot < 0) slot = 0;
      s_game.slot[slot] = item;
      s_game.dur[slot]  = game::toolInfo(kind, game::TT_IRON).durability;
      s_game.sel = (uint8_t)slot;
      Serial.printf("held=%s slot=%d\n",
                    game::toolInfo(kind, game::TT_IRON).name, slot);
    } else if (cmd == 'w') {
      // Freeze the swing at a frame worth photographing. The arc is what makes
      // a held tool read as held rather than as a sticker in the corner, and a
      // grab lands wherever the animation happens to be otherwise.
      s_game.toolPhase = (uint8_t)((s_game.toolPhase + 3) % game::TOOL_ANIM);
      Serial.printf("swing=%d\n", (int)s_game.toolPhase);
    } else if (cmd == 's') {
      static const char* const kName[] = { "title", "play", "pause", "craft",
                                          "recipes", "controls", "dead" };
      // Indexed by the screen, so adding one without a name here would read
      // off the end and hand a wild pointer to the screenshot writer.
      static_assert(sizeof(kName) / sizeof(kName[0]) == SCR_DEAD + 1,
                    "every Screen needs a screenshot name");
      shot::capture(kName[s_scr]);
    } else if (cmd == 't') {
      // Build a bridge across the view and stand back from it. Arches and cave
      // mouths are a handful of cells on a 64x64 map, and framing one by hand
      // over a serial link is not a thing worth doing; this makes the overhang
      // path unambiguous to look at.
      const int cx = (int)s_game.cam.px;
      const int cy = (int)s_game.cam.py;
      // Far enough back to actually be in frame. With a fixed downward tilt and
      // no pitch, anything above eye level projects near or above the horizon,
      // so a bridge overhead at close range is off the top of the panel — you
      // see an overhang by approaching it, not by standing under it.
      const int by = cy - 15;
      for (int x = cx - 8; x <= cx + 8; ++x) {
        for (int y = by - 1; y <= by + 1; ++y) {
          while (world::height(x, y) > world::GROUND)
            { uint8_t m, b2; world::mineTop(x, y, 100000, m, b2); }
          while (world::height(x, y) < world::GROUND)
            world::place(x, y, world::B_DIRT);
        }
      }
      for (int x = cx - 8; x <= cx + 8; ++x) {
        const bool pier = (x == cx - 8 || x == cx + 8);
        for (int y = by - 1; y <= by + 1; ++y) {
          if (pier) { for (int k = 0; k < 4; ++k) world::place(x, y, world::B_STONE); }
          else      { world::devSlab(x, y, world::GROUND + 3, world::GROUND + 4,
                                     world::B_STONE); }
        }
      }
      s_game.angle = -1.5708f;                     // face -y, along the bridge's normal
      raycast::setAngle(s_game.cam, s_game.angle);
      int slabs = 0;
      for (int x = cx - 8; x <= cx + 8; ++x)
        for (int y = by - 1; y <= by + 1; ++y)
          if (world::hasSlab(x, y)) ++slabs;
      Serial.printf("bridge: player(%d,%d,z=%.1f) deck y=%d slabs=%d pierH=%d\n",
                    cx, cy, s_game.cam.z, by, slabs, world::height(cx - 8, by));
    } else if (cmd == 'd') {
      // Force midday. The benchmark jumps to a late-night wave, which is the
      // right thing to measure and the wrong thing to photograph.
      s_game.phase = game::PH_DAY;
      s_game.phaseTick = game::DAY_TICKS / 3;
      for (int i = 0; i < game::MAX_MOBS; ++i) s_game.mobs[i].alive = false;
    } else if (cmd == 'm') {
      // One of each kind, lined up in front of the camera in daylight. Mob art
      // can only be judged by looking at it, and waiting for a wave to walk one
      // into frame at night is not a thing worth doing twice.
      s_game.phase = game::PH_DAY;
      s_game.phaseTick = game::DAY_TICKS / 3;
      for (int i = 0; i < game::MAX_MOBS; ++i) s_game.mobs[i].alive = false;

      // Clear a yard to pose them in. The bench walks the player into whatever
      // it runs into, and a mob shot taken from inside a wall shows a wall.
      const int px = (int)s_game.cam.px, py = (int)s_game.cam.py;
      for (int y = py - 12; y <= py + 12; ++y)
        for (int x = px - 12; x <= px + 12; ++x) {
          if (world::isBorder(x, y)) continue;
          uint8_t dm, db;
          while (world::height(x, y) > world::GROUND)
            world::mineTop(x, y, 100000, dm, db);
          while (world::height(x, y) < world::GROUND)
            world::place(x, y, world::B_DIRT);
        }

      for (int k = 0; k < game::MOB_COUNT; ++k) {
        game::Mob& m = s_game.mobs[k];
        m = game::Mob{};
        m.alive = true;
        m.kind  = (uint8_t)k;
        m.hp    = 20;
        m.x = s_game.cam.px + s_game.cam.dx * 7.0f
                            - s_game.cam.dy * (float)(k - 1) * 2.2f;
        m.y = s_game.cam.py + s_game.cam.dy * 7.0f
                            + s_game.cam.dx * (float)(k - 1) * 2.2f;
        m.bestDist = 99.0f;
        // One per view, so the row is a contact sheet rather than three copies
        // of the same picture: the left mob walks at the camera, the middle one
        // crosses in profile, the right one walks away. Without it they all pose
        // front-on -- a zeroed Mob has no heading at all -- and the side and
        // back art cannot be looked at.
        //
        // Given a destination to WALK to rather than a heading and a freeze.
        // Setting hx/hy alone does not survive: the mob keeps walking, and the
        // heading is re-derived from the step it actually took, so within a
        // second all three faced wherever their wander had taken them. A real
        // destination makes the heading genuine and keeps it that way.
        //
        // In camera space: -1 is toward the eye, +1 away, and the second
        // component runs across the view.
        static const int8_t kPose[3][2] = { { -1, 0 }, { 0, 1 }, { 1, 0 } };
        const float fwd = kPose[k][0], side = kPose[k][1];
        const float wx = s_game.cam.dx * fwd - s_game.cam.dy * side;
        const float wy = s_game.cam.dy * fwd + s_game.cam.dx * side;
        m.hx = (int8_t)(wx * 127.0f);
        m.hy = (int8_t)(wy * 127.0f);
        m.state = game::MS_WANDER;
        m.repos = 255;                       // one long leg, no re-targeting
        m.memX  = (uint8_t)(m.x + wx * 5.0f);
        m.memY  = (uint8_t)(m.y + wy * 5.0f);
      }
      Serial.printf("mobs: 3 posed at %.1f,%.1f (front / side / back)\n",
                    s_game.cam.px, s_game.cam.py);
    } else if (cmd == 'h') {
      // Flash every mob, and hold it far longer than a real hit does, so the
      // reaction can be photographed over a serial link. The renderer clamps
      // the strength, so a long hold looks exactly like a real one.
      // ...and throw the blood, which is the other half of what a landed blow
      // looks like and the half that could not be reviewed at all before: a
      // burst lasts a third of a second, so catching one by timing a capture
      // against a real swing was not a thing worth attempting twice.
      for (int i = 0; i < game::MAX_MOBS; ++i) {
        game::Mob& m = s_game.mobs[i];
        if (!m.alive) continue;
        m.hitFlash = 90;
        game::Spark sp{};
        sp.x = m.x; sp.y = m.y; sp.z = (float)m.z + 0.95f;
        sp.kind = game::SP_HIT;
        sp.mat  = 0;
        sp.mag  = 255;
        render::emit(sp);
      }
    } else if (cmd == 'n') {
      // Start a run. Same reason 'p' and 'c' exist: the title card is dismissed
      // with a key, so without this the screenshot tour cannot get past it
      // unattended and everything behind it is unreachable over the wire.
      startRun();
    } else if (cmd == 'p') {
      // Lets the screenshot tool reach the pause card, which is otherwise only
      // openable from the keyboard.
      if (s_scr == SCR_PLAY) { openPauseMenu(); s_scr = SCR_PAUSE; }
      else if (s_scr == SCR_PAUSE) { resume(); }
    } else if (cmd == 'c') {
      // The craft card and the book, for the same reason 'p' exists: they are
      // openable only from the keyboard, and both are now pictures rather than
      // text, so the only way to review them is to look at one.
      //
      // Stocked on the way in. An empty grid photographs as an empty grid, and
      // what wants reviewing is a card with a recipe laid out in it.
      if (s_scr == SCR_PLAY || s_scr == SCR_RECIPES) {
        s_game.inv[world::B_PLANK] = 6;
        s_game.inv[world::B_STONE] = 8;
        s_game.inv[world::B_WOOD]  = 4;
        s_game.inv[world::B_COAL]  = 3;
        s_game.inv[world::B_IRON]  = 2;
        for (uint8_t m = 0; m < world::B_COUNT; ++m) {
          if (!s_game.inv[m]) continue;
          bool shown = false;
          for (int i = 0; i < game::SLOT_N; ++i)
            if (s_game.slot[i] == m) shown = true;
          if (shown) continue;
          for (int i = 0; i < game::SLOT_N; ++i)
            if (s_game.slot[i] == game::SLOT_EMPTY) { s_game.slot[i] = m; break; }
        }
        game::fillGrid(s_game, game::R_PICK_STONE);
        s_game.gridSel = game::GRID_FOCUS_OUT;
        s_scr = SCR_CRAFT;
      } else if (s_scr == SCR_CRAFT) {
        resume();
      }
    } else if (cmd == 'r') {
      if (s_scr == SCR_CRAFT) { s_book = 4; s_scr = SCR_RECIPES; }
      else if (s_scr == SCR_RECIPES) { s_scr = SCR_CRAFT; }
    } else if (cmd == 'f') {
      // The player-facing frame counter, for the same reason 'p' and 'v' exist:
      // it lives behind a row on the pause card and the pause card takes a
      // keypress to reach, so over the wire it is otherwise unreachable.
      s_showFps = !s_showFps;
      Serial.printf("showfps=%d\n", (int)s_showFps);
    } else if (cmd == 'v') {
      // The controls card, for the same reason 'p' and 'c' exist: it is only
      // openable from the keyboard, and it is the card most worth checking by
      // eye because everything on it is generated from hal::Caps.
      if (s_scr == SCR_CONTROLS) { s_scr = s_ctrlFrom; }
      else { s_ctrlFrom = s_scr; s_scr = SCR_CONTROLS; }
    } else if (cmd == 'g') {
      // Stand the player in front of something worth looking at, and face it.
      // A house is two or three cells on a 64x64 map and a tree not much more;
      // steering to one over a serial link is not a thing worth doing twice,
      // which is the same reason 't' exists. Cycles house, then tree.
      static int which = 0;
      const bool house = ((which++ & 1) == 0);
      const uint8_t want = house ? world::B_WOOD : world::B_LEAVES;
      // A tree's crown is a SLAB, so its cells are topped with whatever the
      // ground is. Looking for leaves in topMat finds the odd leaf that landed
      // on a column and almost never an actual canopy.
      // Nearest the middle of the map, not the first one scanned: the first is
      // always the one hard against the bedrock ring, and a photograph of a
      // tree with the border wall behind it is a photograph of the border wall.
      int bx = -1, by = -1, best = 1 << 30;
      for (int y = 3; y < world::H - 3; ++y)
        for (int x = 3; x < world::W - 3; ++x) {
          // A trunk is a wood column with a leaf slab over it or beside it,
          // which is what separates a tree from a house post or a dead snag.
          bool leafy = false;
          for (int k = 0; k < 5 && !leafy; ++k) {
            const int lx = x + (k == 1) - (k == 2), ly = y + (k == 3) - (k == 4);
            leafy = world::hasSlab(lx, ly)
                    && world::slabMat(lx, ly) == world::B_LEAVES;
          }
          const bool hit = world::topMat(x, y) == world::B_WOOD
                        && world::height(x, y) >= world::GROUND + 3
                        && (house ? !leafy : leafy);
          if (!hit) continue;
          // For a tree, prefer one standing near a grass-capped step: a flat
          // lawn photographs as a green field, and what says "blocks" is a rise
          // with grass on top of it and the dirt showing down the side.
          if (!house) {
            bool hill = false;
            for (int oy = -6; oy <= 6 && !hill; ++oy)
              for (int ox = -6; ox <= 6 && !hill; ++ox) {
                const int hx = x + ox, hy = y + oy;
                if (world::isBorder(hx, hy)) continue;
                hill = world::topMat(hx, hy) == world::B_GRASS
                       && world::height(hx, hy) >= world::height(x, y) + 2;
              }
            if (!hill) continue;
          }
          const int dx = x - world::W / 2, dy = y - world::H / 2;
          if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; bx = x; by = y; }
        }
      if (bx < 0) { Serial.printf("go: nothing found\n"); }
      else {
        // A spot to stand: far enough back to get the whole thing in frame,
        // and somewhere a body could actually be.
        int sx = -1, sy = -1;
        // A tree is photographed from well inside the draw distance or the fog
        // greys it out, and eight blocks at five cells is taller than the panel
        // -- so the distance cannot fix the framing and the pitch has to. Stand
        // close and tilt up; see below.
        const int rFar = house ? 10 : 6, rNear = house ? 6 : 4;
        for (int r = rFar; r >= rNear && sx < 0; --r)
          for (int a = 0; a < 24 && sx < 0; ++a) {
            const float th = (float)a * 0.2618f;
            const int tx = bx + (int)(cosf(th) * (float)r);
            const int ty = by + (int)(sinf(th) * (float)r);
            // Clear where it stands and clear all round it, or the shot is of
            // whatever happens to be a cell in front of the lens.
            bool clear = true;
            for (int dy = -1; dy <= 1 && clear; ++dy)
              for (int dx = -1; dx <= 1 && clear; ++dx)
                clear = !world::isBorder(tx + dx, ty + dy)
                        && world::height(tx + dx, ty + dy) <= world::GROUND
                        && !world::hasSlab(tx + dx, ty + dy);
            if (!clear) continue;
            sx = tx; sy = ty;
          }
        if (sx < 0) { sx = bx + 6; sy = by; }
        s_game.cam.px = (float)sx + 0.5f;
        s_game.cam.py = (float)sy + 0.5f;
        s_game.cam.z  = (float)world::groundAt(s_game.cam.px, s_game.cam.py)
                      + raycast::EYE;
        s_game.eyeZ   = s_game.cam.z;
        s_game.angle  = atan2f((float)by + 0.5f - s_game.cam.py,
                               (float)bx + 0.5f - s_game.cam.px);
        raycast::setAngle(s_game.cam, s_game.angle);
        // Tilted up for a tree, level for a house. Part way, not the full stop
        // the look keys reach: 'l' goes to sixty degrees, which is all sky.
        s_game.pitch = (float)(raycast::HORIZON + (house ? 0 : 34));
        raycast::setPitch(s_game.cam, (int)(s_game.pitch + 0.5f));
        Serial.printf("go: %s at (%d,%d), standing (%d,%d)\n",
                      house ? "house" : "tree", bx, by, sx, sy);
      }
    } else if (cmd == 'q') {
      render::verifyPanel();
    } else if (cmd == 'l') {
      // Up, then down, then let it drift home.
      s_look = (s_look == 0) ? 1 : (s_look == 1 ? -1 : 0);
      Serial.printf("look=%d horizon=%d rest=%d up=%d down=%d\n",
                    (int)s_look, (int)s_game.cam.horizon, raycast::HORIZON,
                    raycast::HORIZON + raycast::PITCH_UP,
                    raycast::HORIZON - raycast::PITCH_DOWN);
    } else if (cmd == 'b') {
      s_bench = !s_bench;
      s_benchFrames = 0;
      if (s_bench) {
        // A fixed seed, and always a fresh world — not startRun()'s random one,
        // and not whatever the player happened to be standing in. A benchmark
        // that measures a different island every run cannot tell whether a
        // change made things slower: a house in frame against open ground is
        // worth 37 fps against 75.
        game::begin(s_game, BENCH_SEED);
        s_scr = SCR_PLAY;
        s_lockout = LOCKOUT_FRAMES;
        s_tickAccum = 0;
        s_lastUs = micros();
        // Jump straight into the small hours. A run in daylight measures no
        // mobs at all; the frame rate that has to hold is the one with the dark
        // full to its cap, and the spawner fills it in about seven seconds from
        // here.
        s_game.night = 10;
        s_game.phase = game::PH_NIGHT;
        s_game.phaseTick = 0;
        s_game.spawnTimer = 0;
        s_game.maxHp = s_game.hp = 200;     // survive long enough to be measured
      }
    }
  }
  if (s_bench) ++s_benchFrames;
#endif

  // Daylight has a tune under it, and night does not: the night's soundtrack is
  // the mobs, and a piece of music playing over a creeper fuse would be working
  // against the one thing the audio is actually for.
  //
  // Driven from here every frame rather than fired off the EV_DAWN and EV_DUSK
  // events, because those say what changed and this is a question about what IS
  // -- a run started in daylight, a run reloaded, a death, or a trip out to the
  // title all have to leave the music in the right state, and only one of those
  // four is an event at all.
  const bool wantMusic = inRun() && !s_game.dead && s_game.phase == game::PH_DAY;
  sfx::musicSet(wantMusic);
#ifdef DEV_SERIAL
  // Music is the one thing here that cannot be checked by looking at the panel,
  // and the board is usually on the far end of a USB cable from whoever is
  // testing it. Reported on the transition only -- every frame would drown the
  // frame-time line it shares a wire with.
  {
    static int8_t s_saidMusic = -1;
    if ((int8_t)wantMusic != s_saidMusic) {
      s_saidMusic = (int8_t)wantMusic;
      Serial.printf("music=%d phase=%d\n", (int)wantMusic, (int)s_game.phase);
    }
  }
#endif

#ifdef DEV_SERIAL
  const uint32_t tSfx = micros();
#endif
  sfx::update(millis());
#ifdef DEV_SERIAL
  s_usSfx = micros() - tSfx;
#endif

  fpsCount();

  render::present();

#ifdef SHOW_FPS
  const uint32_t nowUs = micros();
  if (s_prevFrame) fpsSample(nowUs - s_prevFrame, cpuUs);
  s_prevFrame = nowUs;
#endif
}
