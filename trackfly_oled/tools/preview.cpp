// Renders the OLED screens to PNG on a Mac, using the firmware's own drawing code.
//   tools/preview.sh            -> tools/preview/*.png
#include "../gfx.h"
#include "../screens.h"
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

static const int S = 4;   // screen pixel -> 4x4 image pixels (with a dark gap, like a real OLED)

static void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back(x >> (i * 8)); }
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> c; put32(c, data.size());
  std::vector<uint8_t> body(type, type + 4); body.insert(body.end(), data.begin(), data.end());
  c.insert(c.end(), body.begin(), body.end());
  put32(c, crc32(0, body.data(), body.size()));
  fwrite(c.data(), 1, c.size(), f);
}

static void save(const std::string &name) {
  const int W = GFX_W * S + 2 * 24, H = GFX_H * S + 2 * 24;
  std::vector<uint8_t> raw;
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    for (int x = 0; x < W; x++) {
      int sx = (x - 24) / S, sy = (y - 24) / S;
      uint8_t r = 12, g = 12, b = 13;   // panel glass
      if (x >= 24 && y >= 24 && sx < GFX_W && sy < GFX_H && (x - 24) % S != S - 1 && (y - 24) % S != S - 1) {
        uint8_t byte = gfx::fb[sy * (GFX_W / 2) + sx / 2];
        int l = (sx & 1) ? (byte & 15) : (byte >> 4);
        // white OLED, with the panel's gamma-ish response
        float k = l / 15.0f; k = k * k * 0.35f + k * 0.65f;
        r = 18 + (int)(k * 232); g = 18 + (int)(k * 230); b = 19 + (int)(k * 222);
      }
      raw.push_back(r); raw.push_back(g); raw.push_back(b);
    }
  }
  uLongf zl = compressBound(raw.size());
  std::vector<uint8_t> z(zl);
  compress2(z.data(), &zl, raw.data(), raw.size(), 9);
  z.resize(zl);
  FILE *f = fopen(("tools/preview/" + name + ".png").c_str(), "wb");
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr; put32(ihdr, W); put32(ihdr, H);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ihdr); chunk(f, "IDAT", z); chunk(f, "IEND", {});
  fclose(f);
  printf("tools/preview/%s.png\n", name.c_str());
}

int main() {
  screens::Now n;
  n.title = "Wish You Were Here";
  n.artist = "Pink Floyd";
  n.album = "Wish You Were Here";
  n.metaL = "ROON \xC2\xB7 KITCHEN \xC2\xB7 4/5";
  n.metaR = "2:41 / 5:34";
  n.progress = 161.0f / 334;
  screens::nowPlaying(n, 0); save("1-playing");

  n.playing = false;
  screens::nowPlaying(n, 0); save("2-paused");

  n.playing = true;
  n.title = "Shine On You Crazy Diamond (Parts I\xE2\x80\x93V)";
  n.artist = "Pink Floyd"; n.album = "Wish You Were Here (2011 Remaster)";
  n.metaL = "APPLE MUSIC \xC2\xB7 1/5"; n.metaR = "0:52 / 13:31"; n.progress = 52.0f / 811;
  screens::nowPlaying(n, 5200); save("3-scrolling");

  n.title = "\xC4\xB0stanbul\xE2\x80\x99u Dinliyorum";   // Istanbul'u Dinliyorum with Turkish I
  n.artist = "Zeki M\xC3\xBCren"; n.album = "Gazeller"; n.metaL = "SPOTIFY"; n.metaR = "1:05 / 3:58";
  n.progress = 65.0f / 238;
  screens::nowPlaying(n, 0); save("4-turkish");

  n.title = "Radio Paradise \xE2\x80\x94 Main Mix"; n.artist = "Radio Paradise"; n.album = "";
  n.metaL = "ROON \xC2\xB7 DAC"; n.metaR = "LIVE"; n.progress = -1;
  screens::nowPlaying(n, 12000); save("5-stream");

  screens::Clock c; c.time = "21:08"; c.date = "TUE 17 NOV"; c.note = "ROON \xC2\xB7 DAC";
  screens::clock(c, 0); save("6-clock");

  screens::message("Setup", "Join TRACK-FLY-3068", "password trackfly", "WIFI:T:WPA;S:TRACK-FLY-3068;P:trackfly;;", 0);
  save("7-setup");

  screens::message("Connected", "track-fly-3068.local", "192.168.1.42", nullptr, 0); save("8-connected");

  screens::logo(2000); save("9-logo");
  n.title = "Wish You Were Here"; n.artist = "Pink Floyd"; n.album = "Wish You Were Here";
  n.metaL = "ROON \xC2\xB7 KITCHEN \xC2\xB7 4/5"; n.metaR = "VOLUME 42"; n.progress = 0.48f; n.volume = 42;
  screens::nowPlaying(n, 0); save("10-volume");
  screens::info("Wish You Were Here", "Pink Floyd", "ROON \xC2\xB7 KITCHEN \xC2\xB7 4/5 \xC2\xB7 VOL 42",
                "192.168.1.42 \xC2\xB7 v0.2.0");
  save("11-info");
  return 0;
}
