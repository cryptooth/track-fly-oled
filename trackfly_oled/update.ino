// Self-update from the Track Fly website.
//   https://trackfly.stream/firmware/oled/latest.json   (made by release_firmware.sh)
// Checked a minute after WiFi comes up and then once a day. With "auto update" on, a
// newer version is installed as soon as nothing plays. The .bin is checked against the
// manifest's SHA-256 before the new slot is made bootable; settings stay.
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include "ca_certs.h"

#define MANIFEST_URL "https://trackfly.stream/firmware/oled/latest.json"

static String updLatest, updUrl, updSha, updStatus = "Not checked yet";
static uint32_t updSize = 0;
static volatile bool updWorking = false;     // a check or an install runs in its task
static volatile bool updWantCheck = false, updWantInstall = false;
static uint32_t updLastCheck = 0, updWifiSince = 0;
static volatile int updPct = -1;

// The strings are written by the update task and read by the web page: guard them.
static SemaphoreHandle_t updMux;
struct UpdLock { UpdLock() { xSemaphoreTake(updMux, portMAX_DELAY); } ~UpdLock() { xSemaphoreGive(updMux); } };
static void setStatus(const String &s) { UpdLock l; updStatus = s; }

bool updateBusy() { return updWorking && updWantInstall; }
String updateStatus() { UpdLock l; return updStatus; }
String updateLatest() { UpdLock l; return updLatest; }

static int vcmp(const String &a, const String &b) {
  int i = 0, j = 0;
  while (i < (int)a.length() || j < (int)b.length()) {
    long x = 0, y = 0;
    while (i < (int)a.length() && a[i] != '.') x = x * 10 + (isdigit(a[i]) ? a[i] - '0' : 0), i++;
    while (j < (int)b.length() && b[j] != '.') y = y * 10 + (isdigit(b[j]) ? b[j] - '0' : 0), j++;
    if (x != y) return x < y ? -1 : 1;
    i++; j++;
  }
  return 0;
}

static bool newer() { UpdLock l; return updLatest.length() && vcmp(FW_VERSION, updLatest) < 0 && updUrl.length(); }

static void doCheck() {
  WiFiClientSecure tls;
  tls.setCACert(TRACKFLY_CA);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(tls, MANIFEST_URL)) { setStatus("Website not reachable"); return; }
  int code = http.GET();
  if (code != 200) {
    setStatus(code == 404 ? "No firmware published yet" : "Website answered " + String(code));
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { setStatus("Bad manifest"); return; }
  JsonObject r = doc["releases"][0];
  {
    UpdLock l;
    updLatest = r["version"] | "";
    updUrl = r["url"] | "";
    updSha = r["sha256"] | "";
    updSha.toLowerCase();
    updSize = r["size"] | 0;
  }
  String v = updateLatest();
  setStatus(newer() ? "Version " + v + " available" : "Up to date");
  Serial.println("update check: " + updateStatus());
}

static bool doInstall() {
  WiFiClientSecure tls;
  tls.setCACert(TRACKFLY_CA);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(tls, updUrl)) { setStatus("Download failed"); return false; }
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len <= 0) { setStatus("Download failed (" + String(code) + ")"); http.end(); return false; }
  if (!Update.begin(len, U_FLASH)) { setStatus(Update.errorString()); http.end(); return false; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[2048];
  int got = 0;
  uint32_t lastData = millis();
  bool ok = true;
  while (got < len && ok) {
    size_t avail = s->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > 15000) { setStatus("Download stopped"); ok = false; break; }
      delay(5);
      continue;
    }
    int n = s->readBytes(buf, min(avail, sizeof buf));
    if (n <= 0) continue;
    if (got == 0 && (n < 16 || buf[0] != 0xE9 || (buf[12] | (buf[13] << 8)) != 5)) {
      setStatus("Not a firmware for this board"); ok = false; break;
    }
    mbedtls_sha256_update(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) { setStatus(Update.errorString()); ok = false; break; }
    got += n;
    lastData = millis();
    updPct = (int)((int64_t)got * 100 / len);
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (updSha.length() && updSha != hex) { setStatus("Checksum mismatch, not installed"); ok = false; }
  }
  if (!ok) { Update.abort(); updPct = -1; return false; }
  if (!Update.end(true)) { setStatus(Update.errorString()); updPct = -1; return false; }
  return true;
}

static volatile bool updDone = false;
static uint32_t updFailAt = 0;

// Runs the slow network work; the screen is handled by updateLoop() in the main loop.
static void updTask(void *) {
  if (updWantCheck) { updWantCheck = false; doCheck(); updLastCheck = millis(); }
  if (updWantInstall) {
    if (newer() && doInstall()) updDone = true;
    else { updFailAt = millis(); updWantInstall = false; updPct = -1; }
  }
  updWorking = false;
  vTaskDelete(NULL);
}

static void startTask() {
  if (updWorking) return;
  updWorking = true;
  if (xTaskCreate(updTask, "update", 10240, NULL, 1, NULL) != pdPASS) updWorking = false;
}

void updateBegin() { updMux = xSemaphoreCreateMutex(); }

void updateCheckNow() { if (wifiReady()) { updWantCheck = true; startTask(); } }

bool updateInstallNow() {
  if (!newer() || updWorking) return false;
  updWantInstall = true;
  startTask();
  return true;
}

void updateLoop(bool idle) {
  static bool showing = false;
  if (updDone) {
    String v = "v" + updateLatest();
    uiMessage("Updated", v.c_str(), "Restarting");
    uiDrawNow();
    delay(800);
    ESP.restart();
  }
  if (updWantInstall) {
    static int shown = -2;
    if (!showing || updPct != shown) {
      showing = true;
      shown = updPct;
      String from = "to v" + updateLatest();
      String p = updPct >= 0 ? String(updPct) + "%" : String("Starting");
      uiMessage("Updating", from.c_str(), p.c_str());
    }
    return;
  }
  if (showing) { showing = false; uiShowMain(); uiToast(updateStatus()); }
  if (!wifiReady()) { updWifiSince = 0; return; }
  if (!updWifiSince) updWifiSince = millis();
  if (updWorking) return;
  bool due = updLastCheck ? millis() - updLastCheck > 24UL * 3600 * 1000 : millis() - updWifiSince > 60000;
  if (due) { updWantCheck = true; startTask(); return; }
  // Install by itself only after two quiet minutes, and not again soon after a failure.
  static uint32_t idleSince = 0;
  if (!idle) { idleSince = 0; return; }
  if (!idleSince) idleSince = millis();
  bool backoff = updFailAt && millis() - updFailAt < 6UL * 3600 * 1000;
  if (cfg.autoUpdate && newer() && !backoff && millis() - idleSince > 120000) updateInstallNow();
}
