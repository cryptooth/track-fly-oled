// What is on screen: messages (setup, connecting, updating), now playing, the clock.
// Drawing itself is in screens.cpp; this file picks the screen and feeds it.
#include <time.h>

extern Settings cfg;

static bool msgMode = true;
static String msgTitle, msgL1, msgL2, msgQr;
static String toastText;
static uint32_t toastUntil = 0;

static NowPlaying cur;
static uint32_t curRev = 0xFFFFFFFF;
static uint32_t trackSince = 0;
static String trackKey;
static bool screenLit = true;
static bool nightDim = false;
static uint32_t idleSince = 0;
// Keys
static uint32_t infoUntil = 0;       // track details on screen until then
static bool clockView = false;       // clock instead of the track (keys 2+3)
static int volShown = -1;            // volume on the dot row ...
static uint32_t volUntil = 0;        // ... until then
static bool manualOff = false;       // screen switched off with key 4
static uint32_t peekUntil = 0;       // a key lit a dark screen until then
static uint32_t lastKeyAt = 0;
static String newsText;              // headlines for the clock screen (Settings: News)
static uint32_t newsRev = 0xFFFFFFFF;

void uiMessage(const char *title, const char *line1, const char *line2, const char *qr) {
  msgMode = true;
  msgTitle = title ? title : "";
  msgL1 = line1 ? line1 : "";
  msgL2 = line2 ? line2 : "";
  msgQr = qr ? qr : "";
}

void uiShowMain() { msgMode = false; }

void uiToast(const String &s, uint32_t ms) { toastText = s; toastUntil = millis() + ms; }

void uiApplySettings() {
  oledFlip(cfg.flip);
  nightDim = false;              // re-evaluated on the next frame
  oledBrightness(cfg.bright);
}

void uiBootLogo() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) { screens::logo(millis() - t0); oledPush(); delay(16); }
}

static String fmtTime(int s) {
  char b[12];
  if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
  return b;
}

static String sourceName(const String &s) {
  if (s == "music") return "Apple Music";
  if (s == "spotify") return "Spotify";
  if (s == "roon") return "Roon";
  if (s == "plex") return "Plex";
  return s;
}

// Local time without Arduino's getLocalTime(): with a 0 ms timeout that one returns
// "no time" whenever millis() ticks during the call, which showed --:-- now and then.
static bool localNow(struct tm &t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;        // not set by NTP yet
  localtime_r(&now, &t);
  return true;
}

static bool nightNow() {
  if (!cfg.night) return false;
  struct tm t;
  if (!localNow(t)) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  return cfg.nightFrom <= cfg.nightTo ? (m >= cfg.nightFrom && m < cfg.nightTo)
                                      : (m >= cfg.nightFrom || m < cfg.nightTo);
}

static bool playMode() {
  return cur.reachable && (cur.state == "playing" || cur.state == "paused") &&
         (cur.title.length() || cur.text.length());
}

static String metaLeft() {
  String t = sourceName(cur.source);
  if (!(cfg.show & SHOW_SOURCE)) t = "";
  if ((cfg.show & SHOW_SOURCE) && cur.zone.length()) t += "  \xC2\xB7  " + cur.zone;
  if (cfg.show & SHOW_POS) {
    // Playlist place beats album place; Roon only knows what is left in the queue.
    String pos;
    if (cur.playlist.length() && cur.queueIndex > 0)
      pos = String(cur.queueIndex) + (cur.queueCount > 0 ? "/" + String(cur.queueCount) : "");
    else if (cur.trackNumber > 0)
      pos = cur.trackCount > 0 ? String(cur.trackNumber) + "/" + String(cur.trackCount) : "#" + String(cur.trackNumber);
    else if (cur.queueRemaining > 0)
      pos = String(cur.queueRemaining) + " left";
    if (pos.length()) t += (t.length() ? "  \xC2\xB7  " : "") + pos;
  }
  if (cur.state == "paused") t += (t.length() ? "  \xC2\xB7  " : "") + String("paused");
  t.toUpperCase();
  return t;
}

static String followLabel() {
  if (cfg.zone.length()) return "Roon \xC2\xB7 " + cfg.zone;
  if (cfg.source.length()) return sourceName(cfg.source);
  return "";
}

// A slow walk over 9 positions, one step every 3 minutes (burn-in care).
static void wander(int amp) {
  static const int8_t P[9][2] = {{0,0},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
  int i = (millis() / 180000) % 9;
  screens::setShift(P[i][0] * amp, P[i][1] * (amp > 1 ? amp / 2 : amp));
}

static void drawFrame() {
  uint32_t now = millis();
  if (infoUntil && now >= infoUntil) infoUntil = 0;
  if (volUntil && now >= volUntil) { volUntil = 0; volShown = -1; }
  if (msgMode) {
    screens::setShift(0, 0);
    screens::message(msgTitle.c_str(), msgL1.c_str(), msgL2.c_str(), msgQr.c_str(), now);
  } else if (infoUntil) {
    drawInfo();
  } else if (playMode() && !clockView) {
    wander(1);
    String title = cur.title.length() ? cur.title : cur.text;
    String meta = metaLeft();
    String time;
    float pos = cur.position;
    if (cur.state == "playing") pos += (now - cur.rxMillis) / 1000.0f;
    if (cur.duration > 0 && pos > cur.duration) pos = cur.duration;
    if (cfg.show & SHOW_TIME)
      time = cur.duration > 0 ? fmtTime((int)pos) + " / " + fmtTime((int)cur.duration) : String("LIVE");
    if (volShown >= 0) time = "VOLUME " + String(volShown);
    screens::Now v;
    v.title = title.c_str();
    v.artist = cur.title.length() ? cur.artist.c_str() : "";
    v.album = (cfg.show & SHOW_ALBUM) ? cur.album.c_str() : "";
    v.metaL = meta.c_str();
    v.metaR = time.c_str();
    v.playing = cur.state == "playing";
    v.progress = cur.duration > 0 ? pos / cur.duration : -1;
    v.dots = cfg.show & SHOW_DOTS;
    v.since = trackSince;
    v.speed = cfg.speed;
    v.volume = volShown;
    screens::nowPlaying(v, now);
  } else {
    wander(4);
    struct tm t;
    char hm[8] = "--:--", date[24] = "";
    if (localNow(t)) {
      strftime(hm, sizeof hm, "%H:%M", &t);
      strftime(date, sizeof date, "%a %d %b", &t);
      for (char *p = date; *p; p++) *p = toupper(*p);
    }
    String note;
    if (!wifiReady()) note = "OFFLINE";
    else if (!cur.reachable && cur.rxMillis) note = "TRACK FLY NOT FOUND";
    else { note = followLabel(); note.toUpperCase(); }
    screens::Clock c;
    c.time = hm; c.date = date; c.note = note.c_str();
    c.news = wifiReady() ? newsText.c_str() : "";
    c.speed = cfg.speed;
    screens::clock(c, now);
  }
  if (toastUntil) {
    if (now < toastUntil) screens::toast(toastText.c_str());
    else toastUntil = 0;
  }
}

void uiDrawNow() { drawFrame(); oledPush(); }

// ---------- keys ----------
bool uiKeyWake() {
  lastKeyAt = millis();
  if (manualOff) { manualOff = false; return true; }
  if (!screenLit) { peekUntil = millis() + 15000; return true; }   // first press only lights it
  if (peekUntil) peekUntil = millis() + 15000;
  return false;
}

void uiInfo() {
  if (infoUntil && millis() < infoUntil) { infoUntil = 0; return; }   // pressed again: back
  infoUntil = millis() + 7000;
}

void uiToggleClock() { clockView = !clockView; infoUntil = 0; }

void uiVolume(int v) { volShown = v; volUntil = millis() + 2500; }

void uiScreenOff() { manualOff = true; }

int uiVolumeNow() { return cur.volume; }

static void drawInfo() {
  screens::setShift(0, 0);
  String title, l1, l2, l3;
  if (playMode()) {
    title = cur.album.length() ? cur.album : (cur.title.length() ? cur.title : cur.text);
    l1 = cur.artist;
    l2 = sourceName(cur.source);
    if (cur.zone.length()) l2 += " \xC2\xB7 " + cur.zone;
    if (cur.trackNumber > 0) l2 += " \xC2\xB7 " + String(cur.trackNumber) + (cur.trackCount > 0 ? "/" + String(cur.trackCount) : "");
    if (cur.volume >= 0) l2 += " \xC2\xB7 VOL " + String(cur.volume);
    l2.toUpperCase();
  } else {
    title = "Nothing playing";
    l1 = followLabel().length() ? "Following " + followLabel() : String("Following whatever plays");
    l2 = wifiReady() ? (cur.reachable ? "Track Fly " + feedBase().substring(7) : String("Track Fly not found")) : String("No WiFi");
  }
  l3 = WiFi.localIP().toString() + " \xC2\xB7 v" FW_VERSION;
  screens::info(title.c_str(), l1.c_str(), l2.c_str(), l3.c_str());
}

void uiLoop() {
  static uint32_t last = 0, lastLight = 0, lastTake = 0;
  uint32_t rev = curRev;
  // Copy the feed when it changed, and once a second for the position.
  if ((feedRevision() != curRev || millis() - lastTake > 1000) && feedTake(cur, rev)) {
    lastTake = millis();
    if (rev != curRev) {
      curRev = rev;
      String key = cur.title + "\x1F" + cur.artist + "\x1F" + cur.album + "\x1F" + cur.text;
      if (key != trackKey) { trackKey = key; trackSince = millis(); }
    }
  }
  { String t; uint32_t r; if (newsTake(t, r) && r != newsRev) { newsRev = r; newsText = t; } }
  bool playing = !msgMode && playMode() && !clockView;
  bool ticker = !msgMode && !infoUntil && !playing && newsText.length() && wifiReady();
  if (clockView && !playMode()) clockView = false;   // nothing to hide any more
  updateLoop(!playMode());

  // Screen on/off and night dimming, checked twice a second.
  if (millis() - lastLight > 500) {
    lastLight = millis();
    if (playing) idleSince = 0; else if (!idleSince) idleSince = millis();
    bool night = nightNow();
    bool lit = true;
    if (!msgMode && !playing && !(peekUntil && millis() < peekUntil)) {
      // Nothing plays: the clock, or a dark screen (after a short grace so a track
      // change does not blink the screen off).
      bool dark = !(cfg.show & SHOW_CLOCK) || (night && cfg.nightMode == 1);
      if (dark && millis() - idleSince > 8000) lit = false;
    }
    if (peekUntil && millis() >= peekUntil) peekUntil = 0;
    if (manualOff) lit = false;
    if (lit != screenLit) { screenLit = lit; oledPower(lit); }
    bool dim = night && cfg.nightMode == 0;
    if (dim != nightDim) { nightDim = dim; oledBrightness(dim ? 255 : cfg.bright); }
  }
  if (!screenLit) return;

  // ~30 frames/s while something moves, slower otherwise. Unchanged frames are not sent.
  uint32_t every = playing || volShown >= 0 || ticker ? 33 : (msgMode || infoUntil || millis() - lastKeyAt < 1000) ? 60 : 250;
  if (millis() - last < every) return;
  last = millis();
  drawFrame();
  oledPush();
}
