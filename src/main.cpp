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

enum Screen : uint8_t { SCR_TITLE, SCR_PLAY, SCR_UPGRADE, SCR_PAUSE, SCR_CRAFT, SCR_DEAD };

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
// Four is the pause card, which is the longest list here: the craft menu has
// R_COUNT rows and the dawn card has three. Spelled as a floor rather than left
// to happen to be R_COUNT, so removing a recipe cannot quietly shrink it under
// what openPauseMenu() writes.
ui::MenuItem     s_items[game::R_COUNT > 4 ? game::R_COUNT : 4];
char             s_title[28];                     // menu heading, rebuilt per open
char             s_soundRow[16];                  // "SOUND: ON", rebuilt on toggle
char             s_detail[game::R_COUNT][26];     // recipe cost lines
uint32_t         s_tickAccum = 0;
uint32_t         s_lastUs = 0;
int              s_lockout = 0;
uint16_t         s_mineChirp = 0;

// ---- audio ------------------------------------------------------------------

// Every event that fired this frame, each on the channel its role belongs to.
//
// This used to be a priority chain that played exactly one sound and discarded
// the other twelve, because a single voice was all there was. It is not: the
// Cardputer's speaker is an I2S amplifier and M5Unified mixes eight channels
// through it. Mining while a creeper hisses at you is now two sounds, which is
// what it always should have been — the hiss is the only warning the player
// gets, and it used to be silenced by a pickaxe.
void playEvents(uint32_t ev) {
  if (ev & game::EV_DIED)        sfx::play(sfx::kDied);
  if (ev & game::EV_EXPLODE)     sfx::play(sfx::kExplode);
  if (ev & game::EV_HURT)        sfx::play(sfx::kHurt);
  if (ev & game::EV_MOB_DIED)    sfx::play(sfx::kMobDied);
  if (ev & game::EV_HISS)        sfx::play(sfx::kHiss);
  if (ev & game::EV_TELEGRAPH)   sfx::play(sfx::kTelegraph);
  if (ev & game::EV_DUSK)        sfx::play(sfx::kDusk);
  if (ev & game::EV_DAWN)        sfx::play(sfx::kDawn);
  if (ev & game::EV_BLOCK_BROKE) sfx::play(sfx::kBlockBroke);
  if (ev & game::EV_MOB_HIT)     sfx::play(sfx::kMobHit);
  else if (ev & game::EV_SWING)  sfx::play(sfx::kSwing);
  else if (ev & game::EV_WHIFF)  sfx::play(sfx::kWhiff);
  if (ev & game::EV_ARROW_FIRE)  sfx::play(sfx::kArrowFire);
  if (ev & game::EV_ARROW_HIT)   sfx::play(sfx::kArrowHit);
  if (ev & game::EV_PLACE)       sfx::play(sfx::kPlace);
  if (ev & game::EV_NO_BLOCKS)   sfx::play(sfx::kNoBlocks);
  if (ev & game::EV_MINE_STEP) {
    // Mining fires every tick; a sound per tick is a buzz. Every eighth is a
    // pickaxe rhythm instead.
    if (++s_mineChirp >= 8) { s_mineChirp = 0; sfx::play(sfx::kMineTick); }
  }
}

// ---- frame-time overlay -----------------------------------------------------

#ifdef SHOW_FPS
uint32_t s_fpsSum = 0, s_cpuSum = 0, s_cpuMax = 0;
int      s_fpsN = 0;
char     s_fpsLine[24] = "";

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
                "fspans=%u ftall=%u fpix=%u fseg=%u\n",
                fps, (unsigned)avgCpu, (unsigned)s_cpuMax, mobs,
                (unsigned)render::g_usWorld, (unsigned)render::g_usMobs,
                (unsigned)render::g_usSky, (unsigned)render::g_usSel,
                (unsigned)render::g_usShade,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)render::g_floorSpans, (unsigned)render::g_floorTall,
                (unsigned)render::g_floorPix, (unsigned)render::g_floorSeg);
  render::g_floorSpans = render::g_floorTall = 0;
  render::g_floorPix = render::g_floorSeg = 0;
#endif
  s_fpsSum = s_cpuSum = s_cpuMax = 0;
  s_fpsN = 0;
}

// Below the objective line, not on top of it.
void fpsDraw() { render::text(3, 14, s_fpsLine, render::pack(120, 220, 140), 1); }
#endif

#ifdef DEV_SERIAL
// Synthetic play, so the frame rate can be measured the same way twice. Held
// forward + turn + mine sweeps the camera over the whole island and keeps the
// mining path hot; left running it walks into dusk and meets a real wave,
// which is the case the 30 fps target actually has to survive.
// The world every benchmark run measures. Any value would do; what matters is
// that it never changes, so two runs are comparable.
constexpr uint32_t BENCH_SEED = 0xCA2DC4A7u;
bool     s_bench = false;
uint32_t s_benchFrames = 0;

// Holds a look key down: -1 down, 0 neither, +1 up. Pitch is the one control
// that cannot be exercised over a serial link, and it is the control that says
// whether a canopy or a bridge deck is in frame at all — the same reason the
// 't' command exists. It drives the real Input flags rather than writing the
// horizon, so what gets measured is the clamp and the drift as the game runs
// them, not a value poked past both.
int8_t   s_look = 0;
#endif

// ---- helpers ----------------------------------------------------------------

// Builds the dawn card from the three offers. The labels come from the game's
// upgrade table so the menu never disagrees with what buying one actually does.
void openUpgradeMenu() {
  for (int i = 0; i < 3; ++i) {
    const game::UpgradeInfo& u = game::upgradeInfo(s_game.offer[i]);
    s_items[i].label   = u.name;
    s_items[i].detail  = u.detail;
    s_items[i].cost    = u.cost;
    s_items[i].enabled = (s_game.ore >= u.cost);
  }
  snprintf(s_title, sizeof(s_title), "NIGHT %u SURVIVED",
           (unsigned)(s_game.night - 1));
  s_menu.open(s_title, s_items, 3);
}

// Recipe rows carry their inputs in the detail line, because a recipe you have
// to memorise is a recipe nobody uses.
void openCraftMenu() {
  for (int i = 0; i < game::R_COUNT; ++i) {
    const game::RecipeInfo& r = game::recipeInfo((uint8_t)i);
    int n = snprintf(s_detail[i], sizeof(s_detail[i]), "%u %s",
                     (unsigned)r.inQty[0], world::info(r.inMat[0]).name);
    if (r.inQty[1])
      snprintf(s_detail[i] + n, sizeof(s_detail[i]) - n, " + %u %s",
               (unsigned)r.inQty[1], world::info(r.inMat[1]).name);
    s_items[i].label   = r.name;
    s_items[i].detail  = s_detail[i];
    s_items[i].cost    = 0;
    s_items[i].enabled = game::canCraft(s_game, (uint8_t)i);
  }
  snprintf(s_title, sizeof(s_title), "CRAFT");
  s_menu.open(s_title, s_items, game::R_COUNT);
}

// Four rows is what the card can hold: Menu::draw sizes itself to its contents
// and four comes to 127 px of the 135 the panel has. A fifth would not fit.
void openPauseMenu() {
  snprintf(s_soundRow, sizeof(s_soundRow), "SOUND: %s", s_sound ? "ON" : "OFF");
  s_items[0] = ui::MenuItem{ "RESUME",    nullptr,         0, true };
  s_items[1] = ui::MenuItem{ s_soundRow,  nullptr,         0, true };
  s_items[2] = ui::MenuItem{ "NEW WORLD", "start over",    0, true };
  s_items[3] = ui::MenuItem{ "QUIT",      "back to title", 0, true };
  snprintf(s_title, sizeof(s_title), "PAUSED  NIGHT %u", (unsigned)s_game.night);
  s_menu.open(s_title, s_items, 4);
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

  // Key "snd2", not "sound". Flipping the default alone would have silenced
  // only the boards that had never touched the toggle: the old key is written
  // on every toggle, so anyone who had ever switched sound on carried a stored
  // true past the new default. Renaming the key retires those values in one
  // step and starts every board silent, which is what the setting is for right
  // now. The toggle still persists, under the new name.
  s_sound = s_prefs.getBool("snd2", false);
  sfx::setEnabled(s_sound);

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
  const bool confirm = (s_lockout == 0) && b.startEdge;

  switch (s_scr) {
    case SCR_TITLE:
      ui::title(hal::boardName(), s_best);
      if (confirm) startRun();
      break;

    case SCR_PLAY: {
      if (b.pauseEdge && !s_lockout) {
        openPauseMenu();
        s_scr = SCR_PAUSE;
        s_lockout = LOCKOUT_FRAMES;
        drawScene();
        s_menu.draw("\x18\x19 PICK   E OK");
        break;
      }
      if (b.craftEdge && !s_lockout) {
        openCraftMenu();
        s_scr = SCR_CRAFT;
        s_lockout = LOCKOUT_FRAMES;
        drawScene();
        s_menu.draw(nullptr);
        break;
      }
      if (b.cycleEdge && !s_lockout) {
        game::cycleBlock(s_game, 1);
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
      in.lookUp   = b.lookUp;
      in.lookDown = b.lookDown;
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
      // full pitch is the measurement worth having now that the world carries
      // four times the overhangs it used to.
      if (s_look > 0)      in.lookUp   = true;
      else if (s_look < 0) in.lookDown = true;
#endif

      int steps = 0;
      uint32_t ev = 0;
#ifdef DEV_SERIAL
      if (s_bench) {
        // Exactly one tick a frame, and the wall clock ignored.
        //
        // The benchmark is otherwise not comparable between two builds, which
        // is the only thing it exists for. Catch-up ticks are driven by elapsed
        // time, so a build that renders more slowly takes MORE ticks per frame,
        // walks further per frame, and by a few seconds in is standing
        // somewhere else entirely looking at a different scene. That is a
        // feedback loop, not noise: measured over forty windows it put two runs
        // of the SAME build 12% apart on frame rate and 33% apart on CPU.
        //
        // Pinned at one tick per frame the walk is frame-indexed, so window N
        // covers the same simulated moment in every build and the question the
        // benchmark answers becomes "what did this frame cost". The game runs
        // slower than real time while benching, which does not matter — nothing
        // here is measuring how the game feels.
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
      ui::phaseBar(s_game);
      ui::objective(s_game);
      ui::crosshair(s_game);
      ui::hud(s_game);

      if (s_game.dead) endRun();
      else if (s_game.awaitingUpgrade) {
        openUpgradeMenu();
        s_scr = SCR_UPGRADE;
        s_lockout = LOCKOUT_FRAMES;
      }
      break;
    }

    case SCR_UPGRADE:
      drawScene();
      s_menu.draw("\x18\x19 PICK   E TAKE");
      if (!s_lockout) {
        if (b.fwdEdge  || b.leftEdge)  { s_menu.move(-1); sfx::play(sfx::kMenuMove); }
        if (b.backEdge || b.rightEdge) { s_menu.move(1);  sfx::play(sfx::kMenuMove); }
        if (b.actEdge) {
          game::chooseUpgrade(s_game, s_game.offer[s_menu.index()]);
          sfx::play(sfx::kBuy);
          resume();
        }
      }
      break;

    case SCR_CRAFT: {
      drawScene();
      ui::phaseBar(s_game);
      ui::hud(s_game);
      char foot[40];
      snprintf(foot, sizeof(foot), "\x18\x19 PICK   %s MAKE   %s BACK",
               hal::caps().kAct, hal::caps().kCraft);
      s_menu.draw(foot);
      if (!s_lockout) {
        if (b.fwdEdge)  { s_menu.move(-1); sfx::play(sfx::kMenuMove); }
        if (b.backEdge) { s_menu.move(1);  sfx::play(sfx::kMenuMove); }
        if (b.craftEdge || b.pauseEdge) resume();
        else if (b.actEdge) {
          if (game::craft(s_game, (uint8_t)s_menu.index())) {
            sfx::play(sfx::kCraft);
            openCraftMenu();          // refresh what is now affordable
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
      ui::phaseBar(s_game);
      ui::hud(s_game);
      s_menu.draw("\x18\x19 PICK   E OK");
      if (!s_lockout) {
        if (b.fwdEdge)  { s_menu.move(-1); sfx::play(sfx::kMenuMove); }
        if (b.backEdge) { s_menu.move(1);  sfx::play(sfx::kMenuMove); }
        if (b.pauseEdge) resume();
        else if (b.actEdge) {
          // Spelled out rather than leaning on default:, because the row that
          // falls through is whichever one was added last, and that is not a
          // thing the next person to add a row should have to notice.
          switch (s_menu.index()) {
            case 0: resume(); break;
            case 1:
              s_sound = !s_sound;
              s_prefs.putBool("snd2", s_sound);
              sfx::setEnabled(s_sound);
              // Turning it on says so out loud. Turning it off cannot, which is
              // the confirmation for that direction.
              if (s_sound) sfx::play(sfx::kBuy);
              openPauseMenu();          // relabel the row; the cursor survives
              break;
            case 2: startRun(); break;
            default:
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

#ifdef SHOW_FPS
  if (s_scr == SCR_PLAY) fpsDraw();
  const uint32_t cpuUs = micros() - frameStart;
#endif

#ifdef DEV_SERIAL
  if (Serial.available()) {
    const int cmd = Serial.read();
    if (cmd == 's') {
      static const char* const kName[] = { "title", "play", "upgrade", "pause", "dead" };
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
            { uint8_t m, b2, o; world::mine(x, y, 100000, m, b2, o); }
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
          uint8_t dm, db, dro;
          while (world::height(x, y) > world::GROUND)
            world::mine(x, y, 100000, dm, db, dro);
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
      }
      Serial.printf("mobs: 3 posed at %.1f,%.1f\n", s_game.cam.px, s_game.cam.py);
    } else if (cmd == 'h') {
      // Flash every mob, and hold it far longer than a real hit does, so the
      // reaction can be photographed over a serial link. The renderer clamps
      // the strength, so a long hold looks exactly like a real one.
      for (int i = 0; i < game::MAX_MOBS; ++i)
        if (s_game.mobs[i].alive) s_game.mobs[i].hitFlash = 90;
    } else if (cmd == 'p') {
      // Lets the screenshot tool reach the pause card, which is otherwise only
      // openable from the keyboard.
      if (s_scr == SCR_PLAY) { openPauseMenu(); s_scr = SCR_PAUSE; }
      else if (s_scr == SCR_PAUSE) { resume(); }
    } else if (cmd == 'g') {
      // Stand the player in front of something worth looking at, and face it.
      // A house is two or three cells on a 64x64 map and a tree not much more;
      // steering to one over a serial link is not a thing worth doing twice,
      // which is the same reason 't' exists. Cycles house, then tree.
      static int which = 0;
      const bool house = ((which++ & 1) == 0);
      const uint8_t want = house ? world::B_WOOD : world::B_LEAVES;
      // Nearest the middle of the map, not the first one scanned: the first is
      // always the one hard against the bedrock ring, and a photograph of a
      // tree with the border wall behind it is a photograph of the border wall.
      int bx = -1, by = -1, best = 1 << 30;
      for (int y = 3; y < world::H - 3; ++y)
        for (int x = 3; x < world::W - 3; ++x) {
          if (world::topMat(x, y) != want
              || world::height(x, y) < world::GROUND + 3) continue;
          const int dx = x - world::W / 2, dy = y - world::H / 2;
          if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; bx = x; by = y; }
        }
      if (bx < 0) { Serial.printf("go: nothing found\n"); }
      else {
        // A spot to stand: far enough back to get the whole thing in frame,
        // and somewhere a body could actually be.
        int sx = -1, sy = -1;
        for (int r = 10; r >= 6 && sx < 0; --r)
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
        Serial.printf("go: %s at (%d,%d), standing (%d,%d)\n",
                      house ? "house" : "tree", bx, by, sx, sy);
      }
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
        // and not whatever the player happened to be standing in.
        //
        // The benchmark used to measure a different island every time it ran,
        // which makes it useless for the only thing a benchmark is for: telling
        // whether a change made things slower. Two runs of the SAME build came
        // back at 37 fps and 75 fps purely because one of them was looking at a
        // house and the other at open ground.
        game::begin(s_game, BENCH_SEED);
        s_scr = SCR_PLAY;
        s_lockout = LOCKOUT_FRAMES;
        s_tickAccum = 0;
        s_lastUs = micros();
        // Jump straight to a late-night wave. A benchmark that measures night
        // one measures five mobs; the frame rate that has to hold is the one
        // with a full field of twenty-four, and waiting ten real minutes to
        // reach it is not a test anyone will run twice.
        s_game.night = 10;
        s_game.phase = game::PH_NIGHT;
        s_game.phaseTick = 0;
        s_game.spawnBudget = game::MAX_MOBS;
        s_game.spawnTimer = 0;
        s_game.maxHp = s_game.hp = 200;     // survive long enough to be measured
      }
    }
  }
  if (s_bench) ++s_benchFrames;
#endif

  sfx::update(millis());

  render::present();

#ifdef SHOW_FPS
  const uint32_t nowUs = micros();
  if (s_prevFrame) fpsSample(nowUs - s_prevFrame, cpuUs);
  s_prevFrame = nowUs;
#endif
}
