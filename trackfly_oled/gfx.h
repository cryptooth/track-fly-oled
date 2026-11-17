// 256x64 frame buffer, 16 grey levels (4 bits per pixel, left pixel in the high nibble,
// the SSD1322's own layout). Plain C++, no Arduino: the same code renders the PNG
// previews on a Mac (tools/preview.cpp).
#pragma once
#include <stdint.h>

#define GFX_W 256
#define GFX_H 64

struct Glyph { uint32_t off; uint16_t adv; uint8_t w, h; int8_t x, y; };   // adv in 1/16 px
struct Font  { const uint8_t *bmp; const Glyph *gl; const uint16_t *cp; uint16_t n; uint8_t lineH, ascent; };

namespace gfx {
extern uint8_t fb[GFX_W * GFX_H / 2];
extern int shiftX, shiftY;                          // added to every pixel (burn-in care)

void clear();
void pixel(int x, int y, uint8_t level);           // brightest wins
void fill(int x, int y, int w, int h, uint8_t level);
void dot(int cx, int cy, uint8_t level);           // 3x3 dot with soft corners
void ring(int cx, int cy, uint8_t level);          // 3x3 dot outline (centre dark)

// Text is UTF-8. x/baseline in pixels; glyphs outside [clipL, clipR) are cut.
int  textWidth(const Font &f, const char *s);
int  text(const Font &f, int x, int baseline, const char *s, uint8_t level,
          int clipL = 0, int clipR = GFX_W);
int  capHeight(const Font &f);                     // height of "H" (or "0")
// Fades text out over `fade` pixels at the clip edges (for scrolling lines).
void fadeEdges(int x0, int x1, int y0, int y1, int fadeL, int fadeR);
}
