// =============================================================================
//  screenshot.cpp — see screenshot.h
// =============================================================================
#ifdef DEV_SERIAL

#include "screenshot.h"

#include <Arduino.h>

#include "render.h"

namespace shot {

// How long a screenshot's writes may wait for the host. A frame is 64 KB down a
// 64-byte endpoint, so those writes MUST be allowed to block or the capture
// arrives full of holes -- but only those, and only while a capture is running.
constexpr uint32_t SHOT_TX_MS = 2000;

void begin() {
  Serial.begin(115200);
  // Never let telemetry block the game loop.
  //
  // USB CDC writes wait for the host to drain the buffer, and the default wait
  // is long enough to be catastrophic: with no terminal attached the buffer
  // fills within seconds and then every printf in the frame-time overlay stalls
  // the frame. From the outside that looks like a board whose keyboard has died
  // -- the input is read, it is just read seconds late. Zero means a write
  // nobody is listening for is dropped instead, which is the right trade for
  // telemetry, and capture() below borrows a real timeout back for the one
  // thing that genuinely needs delivery.
  Serial.setTxTimeoutMs(0);
}

void capture(const char* name) {
  // A frame has to arrive whole, so this is the one place that may wait.
  Serial.setTxTimeoutMs(SHOT_TX_MS);
  const uint16_t* buf = render::buildBuffer();
  Serial.printf("<<SHOT %s %d %d be\n", name, render::W, render::H);
  // In chunks: the CDC endpoint is 64 bytes and a single 64 KB write stalls
  // long enough for the host to time out its read.
  const uint8_t* p = (const uint8_t*)buf;
  const size_t total = (size_t)render::W * render::H * 2;
  for (size_t off = 0; off < total; off += 1024) {
    const size_t n = (total - off < 1024) ? (total - off) : 1024;
    Serial.write(p + off, n);
    Serial.flush();
  }
  Serial.print("\n>>SHOT\n");
  Serial.flush();
  Serial.setTxTimeoutMs(0);      // back to never blocking the game
}

}  // namespace shot

#endif  // DEV_SERIAL
