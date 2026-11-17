// WiFi (setup portal + home network), web UI, firmware upload, mDNS, clock.
#include <esp_wifi.h>
#include "web_html.h"

extern Settings cfg;
extern Preferences prefs;
extern WebServer server;
extern DNSServer dnsServer;

#define AP_PASS "trackfly"
#define CONNECT_TIMEOUT_MS 20000

enum NetMode { NET_IDLE, NET_CONNECTING, NET_STA, NET_AP };
static NetMode netMode = NET_IDLE;
static char apSsid[24];
static String savedSsid, savedPass;
static uint32_t connectStart = 0, showMainAt = 0, lastRetry = 0;

bool wifiReady() { return netMode == NET_STA && WiFi.status() == WL_CONNECTED; }
bool netInSetup() { return netMode == NET_AP; }

// "192.168.1.20:8765/now?x" -> "http://192.168.1.20:8765"
String feedBase() {
  String u = cfg.feed; u.trim();
  if (!u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
  int s = u.indexOf('/', u.indexOf("//") + 2);
  if (s > 0) u = u.substring(0, s);
  return u;
}

static String sanitizeHost(String s) {
  s.toLowerCase();
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') o += c;
  }
  while (o.startsWith("-")) o.remove(0, 1);
  while (o.endsWith("-")) o.remove(o.length() - 1);
  if (!o.length()) o = "track-fly-" + macSuffix();
  return o.substring(0, 31);
}

static void applyClock() { configTime(cfg.tz, 0, "pool.ntp.org", "time.google.com"); }

static void startMdns() {
  MDNS.end();
  if (MDNS.begin(cfg.name.c_str())) MDNS.addService("http", "tcp", 80);
}

// ---------------- Modes ----------------
static void beginConnect() {
  netMode = NET_CONNECTING;
  connectStart = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.name.c_str());
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_15dBm);   // C3 SuperMini boards brown out / lose WiFi at full power
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  uiMessage("Connecting", savedSsid.c_str());
}

static void onConnected() {
  netMode = NET_STA;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  startMdns();
  applyClock();
  String host = cfg.name + ".local", ip = WiFi.localIP().toString();
  uiMessage("Connected", host.c_str(), ip.c_str());
  showMainAt = millis() + 4000;
  feedKick();
  Serial.println("WiFi OK " + ip);
}

static void startSetupPortal(const char *why) {
  netMode = NET_AP;
  WiFi.disconnect(false);          // drop the home network while the portal is open
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  lastRetry = millis();
  String l1 = String("Join ") + apSsid, l2 = "password " AP_PASS;
  String qr = String("WIFI:T:WPA;S:") + apSsid + ";P:" AP_PASS ";;";
  uiMessage(why, l1.c_str(), l2.c_str(), qr.c_str());
  Serial.printf("Setup portal %s\n", apSsid);
}

// ---------------- Web handlers ----------------
static bool requireAuth() {
  if (netMode == NET_AP || cfg.webPass.length() == 0) return true;
  if (server.authenticate("admin", cfg.webPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (!requireAuth()) return;
  String page = FPSTR(netMode == NET_AP ? SETUP_HTML : CONTROL_HTML);
  page.replace("__FW__", FW_VERSION);
  page.replace("__FEED__", feedBase());
  server.send(200, "text/html; charset=utf-8", page);
}

static void handleScan() {
  int n = WiFi.scanNetworks(false, false);
  for (int t = 0; n <= 0 && t < 2; t++) { WiFi.scanDelete(); delay(300); n = WiFi.scanNetworks(false, false); }
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void netSaveWifi(const String &ssid, const String &pass) {
  Serial.printf("Saving WiFi '%s'\n", ssid.c_str());
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid; savedPass = pass;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  beginConnect();
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  if (!ssid.length()) { server.send(400, "text/plain", "network name is empty"); return; }
  if (server.hasArg("feed") && server.arg("feed").length()) { cfg.feed = server.arg("feed"); settingsSave(); }
  server.send(200, "text/plain", "OK");
  delay(300);
  netSaveWifi(ssid, server.arg("pass"));
}

static void handleState() {
  if (!requireAuth()) return;
  NowPlaying n; uint32_t rev;
  feedTake(n, rev);
  Target tg[12]; int tc = feedTargets(tg, 12);
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["uptime"] = millis() / 1000;
  doc["time"] = (uint32_t)time(nullptr);
  doc["name"] = cfg.name;
  doc["mac"] = WiFi.macAddress();
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["feed"] = feedBase();
  doc["zone"] = cfg.zone;
  doc["source"] = cfg.source;
  doc["bright"] = cfg.bright;
  doc["flip"] = cfg.flip;
  doc["night"] = cfg.night;
  doc["nightFrom"] = cfg.nightFrom;
  doc["nightTo"] = cfg.nightTo;
  doc["nightMode"] = cfg.nightMode;
  doc["tz"] = cfg.tz;
  doc["speed"] = cfg.speed;
  doc["show"] = cfg.show;
  doc["autoUpdate"] = cfg.autoUpdate;
  doc["latest"] = updateLatest();
  doc["updStatus"] = updateStatus();
  doc["hasPass"] = cfg.webPass.length() > 0;
  JsonArray s = doc["targets"].to<JsonArray>();   // the "Follow" menu
  for (int i = 0; i < tc; i++) {
    if (!tg[i].source.length() && !tg[i].zone.length()) continue;   // "Auto" is added by the page
    JsonObject t = s.add<JsonObject>();
    t["label"] = tg[i].zone.length() ? "Roon \xC2\xB7 " + tg[i].zone : tg[i].label;
    t["source"] = tg[i].zone.length() ? String("") : tg[i].source;
    t["zone"] = tg[i].zone;
  }
  JsonObject o = doc["now"].to<JsonObject>();
  o["reachable"] = n.reachable;
  o["state"] = n.state;
  o["title"] = n.title;
  o["artist"] = n.artist;
  o["text"] = n.text;
  o["source"] = n.source;
  o["zone"] = n.zone;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleSettings() {
  if (!requireAuth()) return;
  if (server.hasArg("feed"))   { cfg.feed = server.arg("feed"); cfg.feed.trim(); }
  if (server.hasArg("zone"))   { cfg.zone = server.arg("zone"); cfg.zone.trim(); }
  if (server.hasArg("source")) cfg.source = server.arg("source");
  if (server.hasArg("bright")) cfg.bright = constrain(server.arg("bright").toInt(), 0, 3);
  if (server.hasArg("flip"))   cfg.flip = server.arg("flip") == "1";
  if (server.hasArg("night"))     cfg.night = server.arg("night") == "1";
  if (server.hasArg("nightFrom")) cfg.nightFrom = constrain(server.arg("nightFrom").toInt(), 0, 1439);
  if (server.hasArg("nightTo"))   cfg.nightTo = constrain(server.arg("nightTo").toInt(), 0, 1439);
  if (server.hasArg("nightMode")) cfg.nightMode = constrain(server.arg("nightMode").toInt(), 0, 1);
  if (server.hasArg("tz")) { cfg.tz = constrain(server.arg("tz").toInt(), -43200, 50400); applyClock(); }
  if (server.hasArg("name")) {
    String nm = sanitizeHost(server.arg("name"));
    if (nm != cfg.name) { cfg.name = nm; startMdns(); }
  }
  if (server.hasArg("speed")) cfg.speed = constrain(server.arg("speed").toInt(), 0, 2);
  if (server.hasArg("show"))  cfg.show = (uint16_t)server.arg("show").toInt();
  if (server.hasArg("autoUpdate")) cfg.autoUpdate = server.arg("autoUpdate") == "1";
  if (server.hasArg("wpass") && server.arg("wpass").length()) cfg.webPass = server.arg("wpass");
  if (server.hasArg("nopass")) cfg.webPass = "";
  settingsSave();
  uiApplySettings();
  feedKick();
  server.send(200, "text/plain", "OK");
}

static void handleUpdateCheck() {
  if (!requireAuth()) return;
  updateCheckNow();
  server.send(200, "text/plain", "OK");
}

static void handleUpdateInstall() {
  if (!requireAuth()) return;
  server.send(200, "text/plain", updateInstallNow() ? "OK" : "nothing to install");
}

static void handleForget() {
  if (!requireAuth()) return;
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  server.send(200, "text/plain", "OK");
  delay(400);
  ESP.restart();
}

static void handleFactory() {
  if (!requireAuth()) return;
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  prefs.begin("tfo", false); prefs.clear(); prefs.end();
  server.send(200, "text/plain", "OK");
  delay(400);
  ESP.restart();
}

// Firmware upload (multipart field "fw"). Only images built for the ESP32-C3 are taken:
// the image header carries the chip id (bytes 12-13, 5 = C3).
static bool fwAuth = false;
static String fwErr;

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    fwErr = "";
    fwAuth = netMode == NET_AP || cfg.webPass.length() == 0 || server.authenticate("admin", cfg.webPass.c_str());
    if (!fwAuth) { fwErr = "not authorized"; return; }
    uiMessage("Updating", "Do not unplug");
    uiDrawNow();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) fwErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!fwAuth || fwErr.length()) return;
    if (up.totalSize == 0 && up.currentSize >= 16) {
      if (up.buf[0] != 0xE9) { fwErr = "not an ESP32 firmware file"; Update.abort(); return; }
      if ((up.buf[12] | (up.buf[13] << 8)) != 5) {
        fwErr = "this firmware is for another board (this one is an ESP32-C3)"; Update.abort(); return;
      }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) fwErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!fwAuth || fwErr.length()) return;
    if (!Update.end(true)) fwErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); fwErr = "upload aborted";
  }
}

static void handleUpdateDone() {
  if (!fwAuth) { server.requestAuthentication(); return; }
  if (fwErr.length() || !Update.isFinished()) {
    server.send(500, "text/plain", fwErr.length() ? fwErr : String("incomplete"));
    uiShowMain();
    return;
  }
  server.send(200, "text/plain", "OK");
  uiMessage("Updated", "Restarting");
  uiDrawNow();
  delay(600);
  ESP.restart();
}

static void handleNotFound() {
  if (netMode == NET_AP) {   // captive portal: send everything to the setup page
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// ---------------- Public ----------------
void netBegin() {
  String sfx = macSuffix(); sfx.toUpperCase();
  snprintf(apSsid, sizeof(apSsid), "TRACK-FLY-%s", sfx.c_str());   // same 4 digits as the .local name
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  WiFi.mode(WIFI_STA);
  if (!savedSsid.length()) {
    // Nothing saved by us yet: reuse the network the ESP32 itself remembers
    // (e.g. from an earlier firmware on this board).
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
      savedSsid = String((const char *)wc.sta.ssid);
      savedPass = String((const char *)wc.sta.password);
      Serial.println("Using stored WiFi " + savedSsid);
    }
  }
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/update-check", HTTP_POST, handleUpdateCheck);
  server.on("/api/update-install", HTTP_POST, handleUpdateInstall);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/factory", HTTP_POST, handleFactory);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();

  if (savedSsid.length()) beginConnect();
  else startSetupPortal("Setup");
}

void netLoop() {
  server.handleClient();
  switch (netMode) {
    case NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) onConnected();
      else if (millis() - connectStart > CONNECT_TIMEOUT_MS) {
        String why = "No " + savedSsid;
        startSetupPortal(why.c_str());
      }
      break;
    case NET_AP:
      dnsServer.processNextRequest();
      // The saved network may come back (router restart): retry while nobody uses the portal.
      if (savedSsid.length() && millis() - lastRetry > 180000) {
        lastRetry = millis();
        if (WiFi.softAPgetStationNum() == 0) {
          WiFi.begin(savedSsid.c_str(), savedPass.c_str());
          connectStart = millis();
        }
      }
      if (savedSsid.length() && WiFi.status() == WL_CONNECTED) onConnected();
      break;
    case NET_STA: {
      if (showMainAt && millis() > showMainAt) { showMainAt = 0; uiShowMain(); }
      String found;
      if (feedTakeDiscovered(found)) {   // Track Fly moved (new Mac IP): follow it
        cfg.feed = found;
        settingsSave();
        feedKick();
        uiToast("Track Fly at " + found.substring(7));
      }
    }
      break;
    default: break;
  }
}
