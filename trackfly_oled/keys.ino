// 1x4 membrane keypad, common to GND (internal pull-ups). Each key is read on two pins,
// whichever is wired:
//   key 1  GPIO0 or GPIO8      (the carrier board uses 8, 9, 10, 20; the hand-wired
//   key 2  GPIO1 or GPIO9       build 0, 1, 3 or 10, 20)
//   key 3  GPIO10 or GPIO3
//   key 4  GPIO20
// GPIO9 is also the SuperMini's BOOT button (so BOOT is play / pause), and GPIO8 its
// LED, which lights while key 1 is held. Holding key 2 while powering up enters the
// USB bootloader. A battery divider needs GPIO3 free. GPIO21 is the serial log.
//
//            short                       long (0.6 s)
//   key 1    previous                    volume down (repeats while held)
//   key 2    play / pause                follow the next Roon zone (Auto, zone, zone...)
//   key 3    next                        volume up (repeats while held)
//   key 4    track details (7 s)         brightness step; after Max the screen goes off
//   1 + 4    held 3 s: WiFi setup mode
//   2 + 3    held 1 s: clock instead of the track (again: back)
//
// A press while the screen is dark only lights it. Two keys pressed together never
// also do their single-key action.

extern Settings cfg;

static const uint8_t KEY_PIN[4] = {0, 1, 10, 20};
static const int8_t KEY_ALT[4] = {8, 9, 3, -1};
static const uint32_t DEBOUNCE = 25, LONG_MS = 600, REPEAT_MS = 180;

struct Key {
  bool down = false;          // debounced state
  bool raw = false;
  uint32_t rawAt = 0;         // when raw last changed
  uint32_t downAt = 0;
  bool longDone = false;      // long action fired for this press
  bool swallow = false;       // this press only woke the screen, or was part of a combo
  uint32_t nextRepeat = 0;
};
static Key keys[4];
static bool comboFired = false;
static int volTarget = -1;            // volume being set with keys 1/3
static uint32_t volSentAt = 0;
static bool volDirty = false;

void keysBegin() {
  for (int i = 0; i < 4; i++) {
    pinMode(KEY_PIN[i], INPUT_PULLUP);
    if (KEY_ALT[i] >= 0) pinMode(KEY_ALT[i], INPUT_PULLUP);
  }
}

static void sendVolume(bool force) {
  if (!volDirty) return;
  if (!force && millis() - volSentAt < 250) return;   // Track Fly gets at most 4 per second
  feedControl("volume", volTarget);
  volSentAt = millis();
  volDirty = false;
}

static void stepVolume(int dir) {
  if (volTarget < 0) volTarget = uiVolumeNow();
  if (volTarget < 0) { uiToast("No volume control here"); return; }
  volTarget = constrain(volTarget + dir * 2, 0, 100);
  volDirty = true;
  uiVolume(volTarget);
  sendVolume(false);
}

static void nextZone() {
  Target tg[12];
  int n = feedTargets(tg, 12);
  // Auto plus the Roon zones; without zones, every source Track Fly offers.
  Target list[13]; int m = 0;
  list[m++] = Target{"Auto", "", ""};
  for (int i = 0; i < n; i++) if (tg[i].zone.length()) list[m++] = tg[i];
  if (m == 1) for (int i = 0; i < n; i++) if (tg[i].source.length()) list[m++] = tg[i];
  if (m == 1) { uiToast("Nothing else to follow"); return; }
  int at = 0;
  for (int i = 0; i < m; i++)
    if (list[i].zone == cfg.zone && (list[i].zone.length() ? true : list[i].source == cfg.source)) { at = i; break; }
  const Target &t = list[(at + 1) % m];
  cfg.zone = t.zone;
  cfg.source = t.zone.length() ? String("") : t.source;
  settingsSave();
  feedKick();
  String label = t.zone.length() ? "Roon \xC2\xB7 " + t.zone : (t.source.length() ? t.label : String("Auto"));
  uiToast("Following " + label);
}

static void brightnessStep() {
  if (cfg.bright >= 3) {        // Max -> off; the next key lights it again at Low
    cfg.bright = 0;
    settingsSave();
    uiApplySettings();
    uiScreenOff();
    return;
  }
  cfg.bright++;
  settingsSave();
  uiApplySettings();
  static const char *names[] = {"Low", "Medium", "High", "Max"};
  uiToast(String("Brightness ") + names[cfg.bright]);
}

static void onShort(int k) {
  switch (k) {
    case 0: feedControl("prev"); uiToast("Previous"); break;
    case 1: feedControl("playpause"); uiToast("Play / pause"); break;
    case 2: feedControl("next"); uiToast("Next"); break;
    case 3: uiInfo(); break;
  }
}

static void onLong(int k) {
  switch (k) {
    case 0: stepVolume(-1); break;
    case 1: nextZone(); break;
    case 2: stepVolume(+1); break;
    case 3: brightnessStep(); break;
  }
}

void keysLoop() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    Key &k = keys[i];
    bool r = digitalRead(KEY_PIN[i]) == LOW || (KEY_ALT[i] >= 0 && digitalRead(KEY_ALT[i]) == LOW);
    if (r != k.raw) { k.raw = r; k.rawAt = now; }
    if (r == k.down || now - k.rawAt < DEBOUNCE) continue;
    k.down = r;
    if (r) {                                   // pressed
      k.downAt = now; k.longDone = false; k.nextRepeat = 0;
      k.swallow = uiKeyWake();
    } else {                                   // released
      if (!k.swallow && !k.longDone && !comboFired) onShort(i);
      if (i == 0 || i == 2) sendVolume(true);
    }
  }

  // Combos: two keys down together.
  int held = 0;
  for (int i = 0; i < 4; i++) if (keys[i].down) held++;
  if (held == 0) { comboFired = false; if (volDirty) sendVolume(true); volTarget = -1; return; }
  if (held >= 2) {
    for (int i = 0; i < 4; i++) if (keys[i].down) keys[i].swallow = true;
    if (!comboFired) {
      uint32_t since = max(max(keys[0].down ? keys[0].downAt : 0, keys[1].down ? keys[1].downAt : 0),
                           max(keys[2].down ? keys[2].downAt : 0, keys[3].down ? keys[3].downAt : 0));
      uint32_t both = now - since;           // how long all of them have been down
      if (held == 2 && keys[0].down && keys[3].down && both > 3000) {
        comboFired = true;
        netStartSetup();
      } else if (held == 2 && keys[1].down && keys[2].down && both > 1000) {
        comboFired = true;
        uiToggleClock();
      }
    }
    return;
  }

  // One key down: long press and repeat.
  for (int i = 0; i < 4; i++) {
    Key &k = keys[i];
    if (!k.down || k.swallow) continue;
    if (!k.longDone && now - k.downAt >= LONG_MS) {
      k.longDone = true;
      onLong(i);
      k.nextRepeat = now + REPEAT_MS;
    } else if (k.longDone && (i == 0 || i == 2) && now >= k.nextRepeat) {
      onLong(i);                              // volume keeps moving while held
      k.nextRepeat = now + REPEAT_MS;
    }
  }
  sendVolume(false);
}
