// Track Fly feed reader, in its own task so slow HTTP never stalls the screen.
//   GET <base>/now?zone=..&source=..   -> current track
//   GET <base>/zones                   -> what can be followed (for the web page)

extern Settings cfg;

static SemaphoreHandle_t feedMux;
static TaskHandle_t feedTaskHandle;
static NowPlaying shared;
static uint32_t sharedRev = 0;
static Target targets[12];
static QueueHandle_t ctrlQueue;
static int targetCount = 0;

static String urlEncode(const String &s) {
  String o;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

static bool httpGet(const String &url, String &body) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3000);
  if (!http.begin(client, url)) return false;
  // Not "X-TrackFly-Display": Track Fly would offer this board the Waveshare firmware.
  // This one updates itself from trackfly.stream/firmware/oled/.
  http.addHeader("X-TrackFly-Device", cfg.name + ";" FW_VERSION ";" + WiFi.macAddress() + ";oled-ssd1322");
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code == 200;
}

static bool fetchNow(NowPlaying &n) {
  String url = feedBase() + "/now";
  char sep = '?';
  if (cfg.zone.length())   { url += sep; url += "zone=" + urlEncode(cfg.zone); sep = '&'; }
  if (cfg.source.length()) { url += sep; url += "source=" + urlEncode(cfg.source); }
  String body;
  if (!httpGet(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  n.reachable = true;
  n.state    = doc["state"]  | "";
  n.title    = doc["title"]  | "";
  n.artist   = doc["artist"] | "";
  n.album    = doc["album"]  | "";
  n.text     = doc["text"]   | "";
  n.source   = doc["source"] | "";
  n.zone     = doc["zone"]   | "";
  n.error    = doc["error"]  | "";
  n.position = doc["position"] | 0.0f;
  n.duration = doc["duration"] | 0.0f;
  n.trackNumber = doc["track_number"] | 0;
  n.trackCount  = doc["track_count"] | 0;
  n.playlist    = doc["playlist"] | "";
  n.queueIndex  = doc["queue_index"] | 0;
  n.queueCount  = doc["queue_count"] | 0;
  n.queueRemaining = doc["queue_remaining"].isNull() ? -1 : (int)(doc["queue_remaining"] | -1);
  n.volume = doc["volume"].isNull() ? -1 : (int)(doc["volume"] | -1);
  n.rxMillis = millis();
  return true;
}

static void fetchTargets() {
  String body;
  if (!httpGet(feedBase() + "/zones", body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  Target tmp[12]; int n = 0;
  for (JsonObject t : doc["targets"].as<JsonArray>()) {
    if (n >= 12) break;
    tmp[n].label  = t["label"]  | "";
    tmp[n].source = t["source"] | "";
    tmp[n].zone   = t["zone"]   | "";
    n++;
  }
  if (!n) return;
  xSemaphoreTake(feedMux, portMAX_DELAY);
  for (int i = 0; i < n; i++) targets[i] = tmp[i];
  targetCount = n;
  xSemaphoreGive(feedMux);
}

// Track Fly announces itself as _trackfly._tcp. After a few failed reads, look it up
// on the LAN; if it now lives at another address, the loop task saves the new one.
static String foundFeed;          // guarded by feedMux

static void discover() {
  int n = MDNS.queryService("trackfly", "tcp");
  if (n <= 0) { Serial.println("discover: no Track Fly on the LAN"); return; }
  String base = "http://" + MDNS.address(0).toString() + ":" + String(MDNS.port(0));
  Serial.println("discover: Track Fly at " + base);
  if (base == feedBase()) return;
  xSemaphoreTake(feedMux, portMAX_DELAY);
  foundFeed = base;
  xSemaphoreGive(feedMux);
}

bool feedTakeDiscovered(String &out) {
  xSemaphoreTake(feedMux, portMAX_DELAY);
  bool has = foundFeed.length() > 0;
  out = foundFeed;
  foundFeed = "";
  xSemaphoreGive(feedMux);
  return has;
}

static bool sameTrack(const NowPlaying &a, const NowPlaying &b) {
  return a.state == b.state && a.title == b.title && a.artist == b.artist && a.album == b.album &&
         a.text == b.text && a.source == b.source && a.zone == b.zone && a.reachable == b.reachable &&
         a.duration == b.duration && a.trackNumber == b.trackNumber && a.trackCount == b.trackCount &&
         a.playlist == b.playlist && a.queueIndex == b.queueIndex && a.queueCount == b.queueCount &&
         a.queueRemaining == b.queueRemaining && a.volume == b.volume;
}

// GET /control for the target the display follows (Track Fly picks the same one /now shows).
static void sendControl(const CtrlMsg &m) {
  String url = feedBase() + "/control?action=" + m.action;
  if (m.value >= 0) url += "&value=" + String(m.value);
  if (cfg.zone.length())   url += "&zone=" + urlEncode(cfg.zone);
  if (cfg.source.length()) url += "&source=" + urlEncode(cfg.source);
  String body;
  bool ok = httpGet(url, body);
  Serial.printf("control %s %d -> %s\n", m.action, m.value, ok ? "ok" : "failed");
}

void feedControl(const char *action, int value) {
  CtrlMsg m;
  strlcpy(m.action, action, sizeof m.action);
  m.value = value;
  xQueueSend(ctrlQueue, &m, 0);
  feedKick();
}

// ---------- news: headlines of an RSS / Atom feed, read by Track Fly ----------
//   GET <base>/news?url=<feed>&n=10 -> {"title": "...", "items": ["...", ...], "pending": true?}
// Asked every 10 minutes; while Track Fly is still reading a new feed, every 5 s.
static String newsLine, newsStat;   // guarded by feedMux
static uint32_t newsRevNo = 0;
static String newsKey;
static uint32_t newsAt = 0, newsNext = 0;
static volatile bool newsAgain = false;

static void fetchNews() {
  String key = cfg.newsOn && cfg.newsUrl.length() ? cfg.newsUrl + "|" + String(cfg.newsN) : "";
  if (key != newsKey || newsAgain) { newsKey = key; newsAt = 0; newsAgain = false; }
  if (!key.length()) {
    xSemaphoreTake(feedMux, portMAX_DELAY);
    if (newsLine.length() || newsStat.length()) { newsLine = ""; newsStat = ""; newsRevNo++; }
    xSemaphoreGive(feedMux);
    return;
  }
  if (newsAt && millis() - newsAt < newsNext) return;
  newsAt = millis() | 1;
  newsNext = 600000;
  String body, line, stat;
  if (!httpGet(feedBase() + "/news?url=" + urlEncode(cfg.newsUrl) + "&n=" + String(cfg.newsN), body)) {
    stat = "Track Fly did not answer (it needs Track Fly 1.10.6 / Server 1.2.6)";
    newsNext = 60000;
  } else {
    JsonDocument doc;
    if (deserializeJson(doc, body)) { stat = "Track Fly sent something unreadable"; newsNext = 60000; }
    else {
      int n = 0;
      for (JsonVariant v : doc["items"].as<JsonArray>()) {
        String h = v | "";
        if (!h.length()) continue;
        if (line.length()) line += "   \xE2\x80\xA2   ";
        line += h;
        if (++n >= cfg.newsN) break;
      }
      String title = doc["title"] | "";
      String err = doc["error"] | "";
      if (n) stat = String(n) + (n == 1 ? " headline" : " headlines") + (title.length() ? " from " + title : "");
      else if (doc["pending"] | false) { stat = "Track Fly is reading the feed"; newsNext = 5000; }
      else stat = err.length() ? err : "no headlines in this feed";
      if (!n && err.length() && newsLine.length()) line = newsLine;   // keep the old ones on a hiccup
    }
  }
  xSemaphoreTake(feedMux, portMAX_DELAY);
  if (line != newsLine || stat != newsStat) { newsLine = line; newsStat = stat; newsRevNo++; }
  xSemaphoreGive(feedMux);
}

bool newsTake(String &text, uint32_t &rev) {
  if (xSemaphoreTake(feedMux, 0) != pdTRUE) return false;
  text = newsLine;
  rev = newsRevNo;
  xSemaphoreGive(feedMux);
  return true;
}

String newsStatus() {
  xSemaphoreTake(feedMux, portMAX_DELAY);
  String s = cfg.newsOn ? newsStat : String("");
  xSemaphoreGive(feedMux);
  return s;
}

void newsRefresh() { newsAgain = true; feedKick(); }

static void feedTask(void *) {
  uint32_t lastTargets = 0;
  for (;;) {
    uint32_t wait = 3000;
    if (wifiReady() && !updateBusy()) {
      CtrlMsg m;
      bool sent = false;
      while (xQueueReceive(ctrlQueue, &m, 0) == pdTRUE) { sendControl(m); sent = true; }
      if (sent) vTaskDelay(pdMS_TO_TICKS(500));   // let Track Fly see the change before we read it
      NowPlaying n;
      bool ok = fetchNow(n);
      if (!ok) { n.reachable = false; n.rxMillis = millis(); }
      xSemaphoreTake(feedMux, portMAX_DELAY);
      bool changed = !sameTrack(n, shared);
      float expect = shared.position + (shared.state == "playing" ? (n.rxMillis - shared.rxMillis) / 1000.0f : 0);
      if (fabsf(n.position - expect) > 2.5f) changed = true;   // seek
      shared = n;
      if (changed) sharedRev++;
      xSemaphoreGive(feedMux);
      if (ok) fetchNews();
      if (!lastTargets || millis() - lastTargets > 30000) { fetchTargets(); lastTargets = millis(); }
      static int fails = 0;
      static uint32_t lastDiscover = 0;
      fails = ok ? 0 : fails + 1;
      if (fails >= 3 && (!lastDiscover || millis() - lastDiscover > 60000)) {
        lastDiscover = millis();
        discover();
      }
      if (!ok) wait = 8000;
      else if (n.state == "playing") wait = 2000;
      else if (n.state == "paused") wait = 3000;
      else wait = 4000;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));   // feedKick() wakes us early
  }
}

void feedBegin() {
  feedMux = xSemaphoreCreateMutex();
  ctrlQueue = xQueueCreate(8, sizeof(CtrlMsg));
  xTaskCreate(feedTask, "feed", 8192, NULL, 1, &feedTaskHandle);
}

void feedKick() { if (feedTaskHandle) xTaskNotifyGive(feedTaskHandle); }

// Copies the latest reading. rev changes when anything worth redrawing changed.
bool feedTake(NowPlaying &out, uint32_t &rev) {
  if (xSemaphoreTake(feedMux, 0) != pdTRUE) return false;
  out = shared;
  rev = sharedRev;
  xSemaphoreGive(feedMux);
  return true;
}

uint32_t feedRevision() { return sharedRev; }

int feedTargets(Target *out, int max) {
  xSemaphoreTake(feedMux, portMAX_DELAY);
  int n = min(targetCount, max);
  for (int i = 0; i < n; i++) out[i] = targets[i];
  xSemaphoreGive(feedMux);
  return n;
}
