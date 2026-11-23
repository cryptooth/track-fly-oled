// Screen layouts for the 256x64 OLED.
//
//   y  1-10   meta line   source · zone · 3/12              1:23 / 4:56
//   y 14-35   title       Kode Mono Bold 20, scrolls with soft edges
//   y 36-52   artist · album
//   y 55-61   43 dots: played ones lit, the playhead lifted out of the row
//
// The dot row is the Track Fly mark: playing = the dot is lifted, paused = it sits back
// in the row as a ring, nothing playing = a flat row (or the clock).
#include "screens.h"
#include "gfx.h"
#include "fonts.h"
#include "tf_qrcode.h"
#include <string.h>
#include <stdio.h>

namespace screens {

static const int DOTS = 43, PITCH = 6, DOT_X0 = 2, ROW_Y = 60, LIFT_Y = 55;
static const int GAP = 56;                      // space between the end and the start of a scrolling line

void setShift(int dx, int dy) { gfx::shiftX = dx; gfx::shiftY = dy; }

// Smooth filled circle (4x4 supersampling), for the larger logo dots.
static void disc(float cx, float cy, float r, uint8_t level) {
  for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
    for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
      int in = 0;
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
          float dx = x + (i + 0.5f) / 4 - cx, dy = y + (j + 0.5f) / 4 - cy;
          if (dx * dx + dy * dy <= r * r) in++;
        }
      if (in) gfx::pixel(x, y, (uint8_t)((level * in + 8) / 16));
    }
}

// Marquee: hold at the start, glide left, wrap round to the start again.
static int scrollOffset(int w, int avail, uint32_t t, uint8_t speed) {
  if (w <= avail) return 0;
  static const int pxs[] = {18, 30, 48};
  const uint32_t hold = 2600;
  uint32_t travel = (uint32_t)(w + GAP) * 1000 / pxs[speed > 2 ? 2 : speed];
  uint32_t ph = t % (hold + travel);
  return ph < hold ? 0 : (int)((uint64_t)(ph - hold) * (w + GAP) / travel);
}

// One line of segments (text, level), scrolled as a whole.
struct Seg { const char *s; uint8_t level; };

static void line(const Font &f, int baseline, int y0, int y1, const Seg *seg, int n,
                 uint32_t t, uint8_t speed) {
  int w = 0;
  for (int i = 0; i < n; i++) w += gfx::textWidth(f, seg[i].s);
  int off = scrollOffset(w, GFX_W, t, speed);
  for (int copy = 0; copy < (w > GFX_W ? 2 : 1); copy++) {
    int x = -off + copy * (w + GAP);
    for (int i = 0; i < n; i++) x += gfx::text(f, x, baseline, seg[i].s, seg[i].level);
  }
  if (w > GFX_W) gfx::fadeEdges(0, GFX_W, y0, y1, off ? 14 : 0, 18);
}

static void dotRow(const Now &n, uint32_t ms) {
  if (n.volume >= 0) {           // volume: lit dots up to the level, nothing lifted
    int lit = (n.volume * DOTS + 50) / 100;
    for (int i = 0; i < DOTS; i++) gfx::dot(DOT_X0 + i * PITCH, ROW_Y, i < lit ? 13 : 2);
    return;
  }
  int head;
  if (n.progress >= 0) {
    float p = n.progress > 1 ? 1 : n.progress;
    head = (int)(p * (DOTS - 1) + 0.5f);
  } else {
    head = n.playing ? (int)((ms / 900) % DOTS) : 0;   // stream: the dot drifts along
  }
  for (int i = 0; i < DOTS; i++) {
    int cx = DOT_X0 + i * PITCH;
    if (i == head) {
      if (n.playing) gfx::dot(cx, LIFT_Y, 15);
      else gfx::ring(cx, ROW_Y, 11);
    } else {
      bool played = n.progress >= 0 && i < head;
      gfx::dot(cx, ROW_Y, played ? (n.playing ? 8 : 5) : 2);
    }
  }
}

void nowPlaying(const Now &n, uint32_t ms) {
  gfx::clear();
  uint8_t dim = n.playing ? 0 : 1;
  // meta line
  int wR = gfx::textWidth(F_META, n.metaR);
  gfx::text(F_META, GFX_W - wR, 9, n.metaR, dim ? 4 : 6);
  gfx::text(F_META, 0, 9, n.metaL, dim ? 4 : 6, 0, GFX_W - wR - 12);

  uint32_t t = ms - n.since;
  Seg title[] = {{n.title, (uint8_t)(dim ? 9 : 15)}};
  line(F_TITLE, 31, 12, 36, title, 1, t, n.speed);

  Seg sub[3] = {{n.artist, (uint8_t)(dim ? 6 : 11)}, {"  \xC2\xB7  ", 4}, {n.album, (uint8_t)(dim ? 4 : 7)}};
  int segs = 3;
  if (!*n.album) segs = 1;
  else if (!*n.artist) { sub[0] = sub[2]; segs = 1; }
  line(F_SUB, 49, 36, 53, sub, segs, t, n.speed);

  if (n.dots || n.volume >= 0) dotRow(n, ms);
}

// With headlines the clock moves up and they glide along the bottom without a pause.
void clock(const Clock &c, uint32_t ms) {
  gfx::clear();
  bool news = c.news && *c.news;
  int w = gfx::textWidth(F_CLOCK, c.time);
  gfx::text(F_CLOCK, (GFX_W - w) / 2, news ? 31 : 38, c.time, 11);
  char line[96];
  if (*c.note) snprintf(line, sizeof line, "%s  \xC2\xB7  %s", c.date, c.note);
  else snprintf(line, sizeof line, "%s", c.date);
  w = gfx::textWidth(F_META, line);
  gfx::text(F_META, (GFX_W - w) / 2, news ? 44 : 55, line, 5);
  if (!news) return;
  static const int pxs[] = {18, 30, 48};
  int nw = gfx::textWidth(F_META, c.news) + GAP;
  int off = (int)((uint64_t)ms * pxs[c.speed > 2 ? 2 : c.speed] / 1000 % nw);
  for (int x = -off; x < GFX_W; x += nw) gfx::text(F_META, x, 61, c.news, 8);
  gfx::fadeEdges(0, GFX_W, 50, 64, 18, 18);
}

static void qrBox(const char *payload) {
  static uint8_t buf[256];
  QRCode qr;
  // The library does not report data that is too long for a version (it silently makes
  // a broken code), so pick the version from the byte capacity at ECC level L.
  static const int cap[] = {0, 17, 32, 53, 78, 106};
  int len = strlen(payload), v = 1;
  while (v <= 5 && len > cap[v]) v++;
  if (v > 5 || qrcode_initText(&qr, buf, v, ECC_LOW, payload) != 0) return;
  int scale = (GFX_H - 4) / qr.size;               // 2 px per module for version 3
  int side = qr.size * scale + 4;                  // 2 px light border
  int x0 = GFX_W - side, y0 = (GFX_H - side) / 2;
  gfx::fill(x0, y0, side, side, 15);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) {
        int px = x0 + 2 + x * scale, py = y0 + 2 + y * scale;
        for (int j = 0; j < scale; j++)
          for (int i = 0; i < scale; i++) {
            // the light square is drawn first; dark modules punch holes in it
            int fx = px + i + gfx::shiftX, fy = py + j + gfx::shiftY;
            if ((unsigned)fx < GFX_W && (unsigned)fy < GFX_H) {
              uint8_t &b = gfx::fb[fy * (GFX_W / 2) + (fx >> 1)];
              b &= (fx & 1) ? 0xF0 : 0x0F;
            }
          }
      }
}

void message(const char *title, const char *line1, const char *line2, const char *qr, uint32_t ms) {
  gfx::clear();
  int right = GFX_W;
  if (qr && *qr) { qrBox(qr); right = GFX_W - 70; }
  // small lifted-dot mark before the title, like the logo
  gfx::dot(3, 7, 15);
  for (int i = 1; i < 4; i++) gfx::dot(3 + i * 6, 12, 4);
  gfx::text(F_SUB, 0, 30, title, 15, 0, right);
  if (line1) gfx::text(F_META, 0, 45, line1, 8, 0, right);
  if (line2) gfx::text(F_META, 0, 58, line2, 8, 0, right);
  (void)ms;
}

void info(const char *title, const char *l1, const char *l2, const char *l3) {
  gfx::clear();
  gfx::text(F_SUB, 0, 14, title, 15);
  const char *l[3] = {l1, l2, l3};
  const uint8_t lv[3] = {9, 7, 4};
  for (int i = 0; i < 3; i++) if (l[i]) gfx::text(F_META, 0, 31 + i * 13, l[i], lv[i]);
}

void logo(uint32_t ms) {
  gfx::clear();
  // 7 dots, the 5th lifted out of the row: the Track Fly mark
  const float pitch = 13, cx0 = GFX_W / 2 - 3 * pitch, cy = 26;
  float rise = ms > 900 ? 9 : 9.0f * ms / 900;     // the dot lifts once at power on
  for (int i = 0; i < 7; i++) {
    if (i == 4) disc(cx0 + i * pitch, cy - rise, 3.2f, 15);
    else disc(cx0 + i * pitch, cy, 3.2f, 6);
  }
  const char *name = "TRACK FLY";
  int w = gfx::textWidth(F_META, name);
  gfx::text(F_META, (GFX_W - w) / 2, 52, name, 7);
}

void toast(const char *s) {
  int w = gfx::textWidth(F_META, s);
  if (w > GFX_W - 8) w = GFX_W - 8;
  int x = (GFX_W - w) / 2;
  // clear a band and draw the text in it
  for (int y = 50; y < 64; y++)
    for (int i = x - 6; i < x + w + 6; i++) {
      int fx = i + gfx::shiftX, fy = y + gfx::shiftY;
      if ((unsigned)fx < GFX_W && (unsigned)fy < GFX_H) gfx::fb[fy * (GFX_W / 2) + (fx >> 1)] &= (fx & 1) ? 0xF0 : 0x0F;
    }
  gfx::text(F_META, x, 60, s, 12, x, x + w);
}

}  // namespace screens
