// What each screen looks like. Plain C++ (no Arduino): every frame is a pure function
// of the inputs and the time, so tools/preview.cpp can render the same pixels on a Mac.
#pragma once
#include <stdint.h>

namespace screens {

struct Now {
  const char *title = "", *artist = "", *album = "";
  const char *metaL = "";       // top left: source / zone / place in the album
  const char *metaR = "";       // top right: elapsed / total
  bool playing = true;          // false = paused
  float progress = -1;          // 0..1, < 0 = unknown length (stream)
  bool dots = true;             // dot row at the bottom
  uint32_t since = 0;           // ms when this track appeared (scrolling starts from here)
  uint8_t speed = 1;            // scroll speed 0 slow, 1 normal, 2 fast
};

struct Clock {
  const char *time = "", *date = "", *note = "";   // note: e.g. the followed zone
  bool seconds = false;         // colon blink phase
};

void nowPlaying(const Now &n, uint32_t ms);
void clock(const Clock &c, uint32_t ms);
// Title + up to two lines; with qr (a QR payload) the code sits on the right.
void message(const char *title, const char *line1, const char *line2, const char *qr, uint32_t ms);
void logo(uint32_t ms);
void toast(const char *s);      // drawn over whatever is on screen

// Burn-in care: the whole picture wanders by a pixel or two over time.
void setShift(int dx, int dy);
}
