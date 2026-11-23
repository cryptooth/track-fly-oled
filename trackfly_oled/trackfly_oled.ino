/*
 * Track Fly OLED — ESP32-C3 SuperMini + 256x64 SSD1322 OLED (4-wire SPI)
 * ---------------------------------------------------------------------------
 * Shows the Track Fly now-playing JSON feed (http://<mac>:8765/now) in 16 grey
 * levels: title, artist and album in Kode Mono, the Track Fly dot row as progress,
 * a clock when nothing plays. Optional 1x4 keypad (keys.ino) for play/pause, skip,
 * volume, zone and brightness; everything else is on the device's web page.
 *
 *  - First boot: the device opens its own WiFi "TRACK-FLY-XXXX" (password trackfly)
 *    and shows a QR code. Join it, pick the home network, done.
 *  - Then open http://track-fly-xxxx.local (or the IP) for settings.
 *  - New firmware: the device checks trackfly.stream/firmware/oled/latest.json once a
 *    day and installs a newer version while nothing plays (can be turned off). A .bin
 *    can also be uploaded on the web page.
 *
 * Build: ./build_firmware.sh  (Arduino core esp32 3.x, partition min_spiffs = two
 *        1.9 MB app slots for OTA)
 */

#define FW_VERSION "0.3.0"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Update.h>
#include "ArduinoJson-v7.4.3.h"   // vendored: the shared library copy is split by iCloud
#include "app.h"
#include "gfx.h"
#include "screens.h"

Settings cfg;
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

// Last 4 hex digits of the WiFi MAC, e.g. 10:20:ba:3d:30:68 -> "3068"
String macSuffix() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char b[5];
  snprintf(b, sizeof b, "%02x%02x", m[4], m[5]);
  return b;
}

void settingsLoad() {
  prefs.begin("tfo", true);
  cfg.feed    = prefs.getString("feed", cfg.feed);
  cfg.zone    = prefs.getString("zone", cfg.zone);
  cfg.source  = prefs.getString("source", cfg.source);
  cfg.name    = prefs.getString("name", String("track-fly-") + macSuffix());
  cfg.webPass = prefs.getString("wpass", cfg.webPass);
  cfg.tz      = prefs.getInt("tz", cfg.tz);
  cfg.bright  = prefs.getUChar("bright", cfg.bright);
  cfg.flip    = prefs.getBool("flip", cfg.flip);
  cfg.night     = prefs.getBool("night", cfg.night);
  cfg.nightFrom = prefs.getUShort("nfrom", cfg.nightFrom);
  cfg.nightTo   = prefs.getUShort("nto", cfg.nightTo);
  cfg.nightMode = prefs.getUChar("nmode", cfg.nightMode);
  cfg.speed   = prefs.getUChar("speed", cfg.speed);
  cfg.show    = prefs.getUShort("show", cfg.show);
  cfg.newsOn  = prefs.getBool("newson", cfg.newsOn);
  cfg.newsUrl = prefs.getString("newsurl", cfg.newsUrl);
  cfg.newsN   = constrain(prefs.getUChar("newsn", cfg.newsN), 1, 30);
  cfg.autoUpdate = prefs.getBool("autoupd", cfg.autoUpdate);
  prefs.end();
  if (cfg.bright > 3) cfg.bright = 3;
}

void settingsSave() {
  prefs.begin("tfo", false);
  prefs.putString("feed", cfg.feed);
  prefs.putString("zone", cfg.zone);
  prefs.putString("source", cfg.source);
  prefs.putString("name", cfg.name);
  prefs.putString("wpass", cfg.webPass);
  prefs.putInt("tz", cfg.tz);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("flip", cfg.flip);
  prefs.putBool("night", cfg.night);
  prefs.putUShort("nfrom", cfg.nightFrom);
  prefs.putUShort("nto", cfg.nightTo);
  prefs.putUChar("nmode", cfg.nightMode);
  prefs.putUChar("speed", cfg.speed);
  prefs.putUShort("show", cfg.show);
  prefs.putBool("newson", cfg.newsOn);
  prefs.putString("newsurl", cfg.newsUrl);
  prefs.putUChar("newsn", cfg.newsN);
  prefs.putBool("autoupd", cfg.autoUpdate);
  prefs.end();
}

// Debug over USB serial:  "wifi <ssid> <password>"  saves a network like the setup page does
void serialCommands() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { line += c; continue; }
    if (line.startsWith("wifi ")) {
      String rest = line.substring(5);
      int sp = rest.indexOf(' ');
      netSaveWifi(sp < 0 ? rest : rest.substring(0, sp), sp < 0 ? "" : rest.substring(sp + 1));
    } else if (line == "update") {
      updateCheckNow();
    }
    line = "";
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nTrack Fly OLED v%s, reset reason %d\n", FW_VERSION, (int)esp_reset_reason());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);      // network stack first: the web server needs it
  settingsLoad();
  oledBegin();
  oledFlip(cfg.flip);
  oledBrightness(cfg.bright);
  uiBootLogo();
  updateBegin();
  keysBegin();
  feedBegin();               // background task, waits for WiFi
  netBegin();                // connects or opens the setup portal
}

void loop() {
  static uint32_t beat = 0;
  if (millis() - beat > 10000) {   // heartbeat on the serial port, for debugging
    beat = millis();
    NowPlaying n; uint32_t rev;
    feedTake(n, rev);
    Serial.printf("[%lus] wifi=%d ip=%s heap=%u feed=%s state=%s title=%s\n",
                  millis() / 1000, WiFi.status(), WiFi.localIP().toString().c_str(),
                  ESP.getFreeHeap(), n.reachable ? "ok" : "-", n.state.c_str(), n.title.c_str());
  }
  serialCommands();
  netLoop();
  keysLoop();
  uiLoop();
  delay(2);
}
