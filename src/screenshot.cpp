// =============================================================================
//  screenshot.cpp — see screenshot.h
// =============================================================================
#ifdef DEV_SERIAL

#include "screenshot.h"

#include <Arduino.h>

#include "render.h"

namespace shot {

void begin() {
  Serial.begin(115200);
}

void capture(const char* name) {
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
}

}  // namespace shot

#endif  // DEV_SERIAL
