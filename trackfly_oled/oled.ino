// SSD1322 256x64 OLED over SPI.
//   CLK GPIO4   DIN GPIO6   CS GPIO7   RST GPIO2   DC GPIO5
// The module's resistors pick the bus: R19 + R21 fitted = 4-wire SPI (with a D/C line),
// R19 + R20 = 3-wire SPI (no D/C: every byte becomes a 9-bit word). Set OLED_DC to -1
// for a 3-wire module.
//
// The frame is sent whole (8192 bytes, about 8 ms at 8 MHz). An unchanged frame is not
// sent again.
#include <SPI.h>
#include <rom/crc.h>
#include "gfx.h"

#define OLED_CLK 4
#define OLED_DIN 6
#define OLED_CS  7
#define OLED_RST 2
#define OLED_DC  5               // -1 = 3-wire module
#define OLED_HZ  8000000
#define COL0     0x1C            // the panel's 256 columns start at RAM column 28

static SPISettings oledSpi(OLED_HZ, MSBFIRST, SPI_MODE0);
static uint32_t lastCrc = 0;
static bool oledOn = true;

#if OLED_DC >= 0
// ---- 4-wire: D/C low = command, high = data ----
static void sendBytes(const uint8_t *d, size_t n, bool data) {
  digitalWrite(OLED_DC, data ? HIGH : LOW);
  SPI.beginTransaction(oledSpi);
  digitalWrite(OLED_CS, LOW);
  SPI.writeBytes(d, n);
  digitalWrite(OLED_CS, HIGH);
  SPI.endTransaction();
}
static void cmdArgs(uint8_t c, int a, int b) {
  sendBytes(&c, 1, false);
  uint8_t args[2]; size_t n = 0;
  if (a >= 0) args[n++] = a;
  if (b >= 0) args[n++] = b;
  if (n) sendBytes(args, n, true);
}
static void sendFrame() { sendBytes(gfx::fb, sizeof gfx::fb, true); }
#else
// ---- 3-wire: bytes packed as 9-bit words (dc bit + 8 data bits, MSB first) ----
static uint8_t wire[GFX_W * GFX_H / 2 * 9 / 8 + 16];
struct Packer {
  size_t n = 0; uint32_t acc = 0; int bits = 0;
  void put(uint8_t b, bool dc) {
    acc = (acc << 9) | (dc ? 0x100 : 0) | b;
    bits += 9;
    while (bits >= 8) { bits -= 8; wire[n++] = acc >> bits; }
    acc &= (1u << bits) - 1;
  }
  size_t finish() { if (bits) wire[n++] = acc << (8 - bits); bits = 0; acc = 0; return n; }   // CS high drops the stub
};
static void sendPacked(size_t len) {
  SPI.beginTransaction(oledSpi);
  digitalWrite(OLED_CS, LOW);
  SPI.writeBytes(wire, len);
  digitalWrite(OLED_CS, HIGH);
  SPI.endTransaction();
}
static void cmdArgs(uint8_t c, int a, int b) {
  Packer p;
  p.put(c, false);
  if (a >= 0) p.put(a, true);
  if (b >= 0) p.put(b, true);
  sendPacked(p.finish());
}
static void sendFrame() {
  Packer p;
  for (size_t i = 0; i < sizeof gfx::fb; i++) p.put(gfx::fb[i], true);
  sendPacked(p.finish());
}
#endif

static void cmd(uint8_t c) { cmdArgs(c, -1, -1); }
static void cmd(uint8_t c, int a) { cmdArgs(c, a, -1); }
static void cmd(uint8_t c, int a, int b) { cmdArgs(c, a, b); }

void oledFlip(bool flip) {
  // Remap: column/nibble order and COM scan direction; 180 degrees = the other pair.
  if (flip) cmd(0xA0, 0x14, 0x11); else cmd(0xA0, 0x06, 0x11);
  lastCrc = 0;
}

void oledBegin() {
  pinMode(OLED_CS, OUTPUT);
  digitalWrite(OLED_CS, HIGH);
#if OLED_DC >= 0
  pinMode(OLED_DC, OUTPUT);
#endif
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);
  delay(120);
  SPI.begin(OLED_CLK, -1, OLED_DIN, -1);

  cmd(0xFD, 0x12);          // unlock
  cmd(0xAE);                // display off
  cmd(0xB3, 0x91);          // clock divider / oscillator (~80 frames/s)
  cmd(0xCA, 0x3F);          // multiplex 1/64
  cmd(0xA2, 0x00);          // display offset
  cmd(0xA1, 0x00);          // start line
  oledFlip(false);
  cmd(0xAB, 0x01);          // internal VDD regulator
  cmd(0xB4, 0xA0, 0xFD);    // display enhancement A (external VSL off, better low grey)
  cmd(0xC1, 0x9F);          // contrast
  cmd(0xC7, 0x0F);          // master current
  cmd(0xB9);                // linear grey scale table
  cmd(0xB1, 0xE2);          // phase 1 / phase 2 lengths
  cmd(0xD1, 0xA2, 0x20);    // display enhancement B
  cmd(0xBB, 0x1F);          // pre-charge voltage
  cmd(0xB6, 0x08);          // second pre-charge period
  cmd(0xBE, 0x07);          // VCOMH
  cmd(0xA6);                // normal (not inverted)
  cmd(0xA9);                // exit partial display
  gfx::clear();
  oledPush();
  cmd(0xAF);                // display on
}

void oledPush() {
  if (!oledOn) return;
  uint32_t crc = crc32_le(0, gfx::fb, sizeof gfx::fb);
  if (crc == lastCrc) return;
  lastCrc = crc;
  cmd(0x15, COL0, COL0 + 63);   // 64 column addresses x 4 pixels
  cmd(0x75, 0, 63);
  cmd(0x5C);                    // write RAM
  sendFrame();
}

// Steps 0..3; 255 = night (dimmer than step 0, lower drive current too).
void oledBrightness(uint8_t step) {
  static const uint8_t contrast[] = {0x18, 0x40, 0x88, 0xFF};
  if (step == 255) { cmd(0xC7, 0x05); cmd(0xC1, 0x10); return; }
  cmd(0xC7, 0x0F);
  cmd(0xC1, contrast[step > 3 ? 3 : step]);
}

void oledPower(bool on) {
  if (on == oledOn) return;
  oledOn = on;
  cmd(on ? 0xAF : 0xAE);
  if (on) { lastCrc = 0; }
}
