#include "gfx.h"
#include <string.h>

namespace gfx {

uint8_t fb[GFX_W * GFX_H / 2];
int shiftX = 0, shiftY = 0;

void clear() { memset(fb, 0, sizeof fb); }

void pixel(int x, int y, uint8_t level) {
  x += shiftX; y += shiftY;
  if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || !level) return;
  if (level > 15) level = 15;
  uint8_t &b = fb[y * (GFX_W / 2) + (x >> 1)];
  if (x & 1) { if ((b & 0x0F) < level) b = (b & 0xF0) | level; }
  else       { if ((b >> 4) < level)    b = (b & 0x0F) | (level << 4); }
}

void fill(int x, int y, int w, int h, uint8_t level) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) pixel(i, j, level);
}

void dot(int cx, int cy, uint8_t level) {
  uint8_t soft = level > 2 ? level * 2 / 5 : level;
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) pixel(cx + i, cy + j, (i && j) ? soft : level);
}

void ring(int cx, int cy, uint8_t level) {
  uint8_t soft = level > 2 ? level * 2 / 5 : level;
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++)
      if (i || j) pixel(cx + i, cy + j, (i && j) ? soft : level);
}

static uint32_t nextCp(const char *&s) {
  uint8_t c = *s++;
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
  return cp;
}

static const Glyph *find(const Font &f, uint32_t cp) {
  int lo = 0, hi = f.n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (f.cp[m] == cp) return &f.gl[m];
    if (f.cp[m] < cp) lo = m + 1; else hi = m - 1;
  }
  return nullptr;
}

// Characters the font lacks: close look-alikes, else '?'.
static const Glyph *glyphFor(const Font &f, uint32_t cp) {
  const Glyph *g = find(f, cp);
  if (g) return g;
  if (cp == 0x2019 || cp == 0x2018) g = find(f, '\'');
  else if (cp == 0x201C || cp == 0x201D) g = find(f, '"');
  else if (cp >= 0x2010 && cp <= 0x2015) g = find(f, '-');
  return g ? g : find(f, '?');
}

int textWidth(const Font &f, const char *s) {
  uint32_t w16 = 0;
  while (*s) { const Glyph *g = glyphFor(f, nextCp(s)); if (g) w16 += g->adv; }
  return (w16 + 8) >> 4;
}

int text(const Font &f, int x, int baseline, const char *s, uint8_t level, int clipL, int clipR) {
  uint32_t pen16 = 0;
  while (*s) {
    const Glyph *g = glyphFor(f, nextCp(s));
    if (!g) continue;
    int gx = x + (int)((pen16 + 8) >> 4) + g->x;
    int gy = baseline - g->y - g->h;
    pen16 += g->adv;
    if (gx >= clipR || gx + g->w <= clipL) continue;
    const uint8_t *p = f.bmp + g->off;
    int n = g->w * g->h;
    for (int k = 0; k < n; k++) {
      uint8_t a = (k & 1) ? (p[k >> 1] & 0x0F) : (p[k >> 1] >> 4);
      if (!a) continue;
      int px = gx + k % g->w;
      if (px < clipL || px >= clipR) continue;
      pixel(px, gy + k / g->w, (a * level + 7) / 15);
    }
  }
  return (pen16 + 8) >> 4;
}

int capHeight(const Font &f) {
  const Glyph *g = find(f, 'H');
  if (!g) g = find(f, '0');
  return g ? g->h + g->y : f.ascent;
}

static void scaleAt(int x, int y, int num, int den) {
  x += shiftX; y += shiftY;
  if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
  uint8_t &b = fb[y * (GFX_W / 2) + (x >> 1)];
  if (x & 1) b = (b & 0xF0) | ((b & 0x0F) * num / den);
  else       b = (b & 0x0F) | ((((b >> 4) * num / den)) << 4);
}

void fadeEdges(int x0, int x1, int y0, int y1, int fadeL, int fadeR) {
  for (int y = y0; y < y1; y++) {
    for (int i = 0; i < fadeL; i++) scaleAt(x0 + i, y, i + 1, fadeL + 1);
    for (int i = 0; i < fadeR; i++) scaleAt(x1 - 1 - i, y, i + 1, fadeR + 1);
  }
}

}  // namespace gfx
