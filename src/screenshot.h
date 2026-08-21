// =============================================================================
//  screenshot.h — frame grabber for the README images
//
//  Whole file is behind DEV_SERIAL (see the cardputer-dev env). The shipped
//  build has no serial port open and none of this compiled in.
//
//  The framebuffer is already in the panel's byte order, so a capture is a
//  straight write of the bytes — there is no conversion step to get wrong.
//  Wire format, one capture:
//
//      <<SHOT <name> <w> <h> be\n
//      <w*h*2 raw bytes, RGB565 big-endian>
//      >>SHOT\n
//
//  The host asks for one by sending 's'; main.cpp owns the command byte (it
//  also answers 'b' for the benchmark) and calls capture() here. Interactive
//  rather than a scripted tour, because the screens worth capturing are the
//  ones mid-play.
// =============================================================================
#pragma once

#ifdef DEV_SERIAL

namespace shot {

void begin();

// Writes the finished frame out. Call after the last draw call and before
// render::present(), so what lands on disk is what lands on the panel.
void capture(const char* name);

}  // namespace shot

#endif  // DEV_SERIAL
