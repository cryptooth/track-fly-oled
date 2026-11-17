// Shared types and function prototypes for the Track Fly OLED firmware.
#pragma once
#include <Arduino.h>

// ---------- Settings (stored in NVS namespace "tfo") ----------
struct Settings {
  String feed    = "http://192.168.86.251:8765";  // Track Fly address (host:port)
  String zone    = "";              // ?zone= (Roon zone), empty = none
  String source  = "";              // &source= filter, empty = auto
  String name    = "";              // mDNS name -> http://<name>.local (default track-fly-<mac4>)
  String webPass = "";              // web UI password (user "admin"), empty = no password
  int32_t tz     = 10800;           // UTC offset in seconds (Turkey = +3)
  uint8_t bright = 2;               // brightness step 0..3
  bool flip      = false;           // mounted upside down
  bool night     = false;           // night mode on
  uint16_t nightFrom = 23 * 60;     // minutes after midnight
  uint16_t nightTo   = 7 * 60;
  uint8_t nightMode  = 0;           // 0 dim to the lowest step, 1 screen off
  uint8_t speed  = 1;               // text scroll: 0 slow, 1 normal, 2 fast
  uint16_t show  = 0xFFFF;          // SHOW_* bits: what the screen shows
  bool autoUpdate = true;           // install new firmware from trackfly.stream by itself
};

#define SHOW_ALBUM   0x01   // album after the artist
#define SHOW_SOURCE  0x02   // source / zone in the top line
#define SHOW_DOTS    0x04   // dot progress row
#define SHOW_TIME    0x08   // elapsed / total time
#define SHOW_CLOCK   0x20   // clock when idle; off = screen goes dark when nothing plays
#define SHOW_POS     0x40   // place on the album / in the playlist, in the top line

// ---------- One reading of the /now feed ----------
struct NowPlaying {
  String state;          // playing | paused | stopped | not_running | "" (no data)
  String title, artist, album, text, source, zone, error;
  float position = 0;    // seconds, at rxMillis
  float duration = 0;    // seconds, 0 = unknown / stream
  int trackNumber = 0, trackCount = 0;   // place on the album
  String playlist;                       // playlist being played
  int queueIndex = 0, queueCount = 0;    // place in the playlist
  int queueRemaining = -1;               // tracks left in the queue (Roon), -1 unknown
  uint32_t rxMillis = 0;
  bool reachable = false;  // last fetch got an answer from Track Fly
};

// ---------- oled.ino ----------
void oledBegin();
void oledPush();                 // gfx::fb -> panel
void oledBrightness(uint8_t step);   // 0..3, 255 = lowest night level
void oledPower(bool on);
void oledFlip(bool flip);

// ---------- net.ino ----------
void netBegin();
void netLoop();
bool wifiReady();
String feedBase();
void netSaveWifi(const String &ssid, const String &pass);
bool netInSetup();

// ---------- feed.ino ----------
void feedBegin();
void feedKick();
bool feedTake(NowPlaying &out, uint32_t &rev);
uint32_t feedRevision();
struct Target { String label, source, zone; };
int  feedTargets(Target *out, int max);
bool feedTakeDiscovered(String &base);

// ---------- update.ino ----------
void updateBegin();
void updateLoop(bool idle);      // idle = nothing playing, a good moment to install
void updateCheckNow();
String updateStatus();           // for the web page
String updateLatest();           // newest version on the website, "" if unknown
bool updateInstallNow();         // install the newest version now (web button)
bool updateBusy();

// ---------- ui.ino ----------
void uiBootLogo();               // the dot mark lifts once at power on
void uiLoop();
void uiMessage(const char *title, const char *line1 = nullptr, const char *line2 = nullptr,
               const char *qr = nullptr);   // shown until uiShowMain()
void uiShowMain();
void uiToast(const String &s, uint32_t ms = 2200);
void uiApplySettings();
void uiDrawNow();                // draw + push right away (before a long blocking step)

// ---------- settings (trackfly_oled.ino) ----------
void settingsLoad();
void settingsSave();
String macSuffix();
