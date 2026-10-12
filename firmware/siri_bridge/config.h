// Einstellungen der Bridge: Belegungsprofile, Status-LED, Timing. Liegen als ein Blob im NVS und werden
// über das Webinterface geändert. WLAN-Zugangsdaten stehen getrennt daneben.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

static const uint8_t BUTTON_COUNT = 13;
// Reihenfolge = Bit-Reihenfolge im Button-Report 0xFB (Bit 0 … Bit 12).
static const char *const BUTTON_NAMES[BUTTON_COUNT] = {
  "Home/TV", "Vol+", "Vol-", "Click", "Power", "Siri", "Back", "Mute", "Play/Pause", "Up", "Right", "Down", "Left",
};
static const uint8_t SIRI_INDEX = 5;

static const uint8_t PROFILE_COUNT = 4;
static const uint8_t PROFILE_NAME_SIZE = 24;  // inklusive abschließender Null

enum ActionType : uint8_t { ACTION_NONE = 0, ACTION_KEY = 1, ACTION_CONSUMER = 2 };
enum LedType : uint8_t { LED_NONE = 0, LED_WS2812 = 1, LED_GPIO = 2 };
static const uint8_t MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4;

struct Action {
  uint8_t type;
  uint8_t mods;   // MOD_*-Bits, nur bei ACTION_KEY
  uint16_t code;  // HID-Usage: Keyboard-Page (ACTION_KEY) bzw. Consumer-Page (ACTION_CONSUMER)
};

// Ein Belegungsprofil: ein frei wählbarer Name und je Taste eine Aktion.
struct Profile {
  char name[PROFILE_NAME_SIZE];
  Action map[BUTTON_COUNT];
};

struct Config {
  uint8_t version;
  uint8_t active;                // Index des Profils, das der Dongle gerade verwendet
  Profile profiles[PROFILE_COUNT];
  uint8_t ledType;
  uint8_t ledPin;
  uint8_t ledInvert;             // nur LED_GPIO: LED leuchtet bei LOW
  uint8_t ledBrightness;         // nur LED_WS2812, 1…255
  uint8_t ledDarkWhenConnected;  // LED aus, solange die Remote verbunden ist
  uint8_t connInterval;          // Verbindungsintervall in 1,25-ms-Schritten, 6…12 (7,5…15 ms)
  uint16_t staleMs;              // Tastendrücke in den ersten staleMs nach dem Verbinden verwerfen (0 = aus)
  uint8_t setupMinutes;          // WLAN endet nach so vielen Minuten ohne Zugriff (0 = nie, dauerhaft an)
  uint8_t siriSetup;             // Siri 5 s halten schaltet das WLAN um
};

static const uint8_t CONFIG_VERSION = 2;

// Häufig gebrauchte Aktionen, als HID-Usages (unabhängig vom Tastaturlayout der Firmware).
static const Action A_NONE = {ACTION_NONE, 0, 0};
static const Action A_UP = {ACTION_KEY, 0, 0x52}, A_DOWN = {ACTION_KEY, 0, 0x51}, A_LEFT = {ACTION_KEY, 0, 0x50}, A_RIGHT = {ACTION_KEY, 0, 0x4F};
static const Action A_ENTER = {ACTION_KEY, 0, 0x28}, A_BACKSPACE = {ACTION_KEY, 0, 0x2A}, A_SPACE = {ACTION_KEY, 0, 0x2C};
static const Action A_KEY_C = {ACTION_KEY, 0, 0x06}, A_KEY_S = {ACTION_KEY, 0, 0x16}, A_KEY_MENU = {ACTION_KEY, 0, 0x65};
static const Action A_VOL_UP = {ACTION_CONSUMER, 0, 0xE9}, A_VOL_DOWN = {ACTION_CONSUMER, 0, 0xEA}, A_MUTE = {ACTION_CONSUMER, 0, 0xE2};
static const Action A_PLAY_PAUSE = {ACTION_CONSUMER, 0, 0xCD}, A_AC_HOME = {ACTION_CONSUMER, 0, 0x223}, A_AC_BACK = {ACTION_CONSUMER, 0, 0x224};
// „Menu Pick“: die OK-Taste einer Fernbedienung. Android macht daraus DPAD_CENTER; anders als Enter öffnet sie
// in einem Suchfeld die Bildschirmtastatur.
static const Action A_OK = {ACTION_CONSUMER, 0, 0x41};
// „AC Search“: öffnet in Android-Apps die Suche (z. B. YouTube). Vorgabe für die Siri-Taste.
static const Action A_SEARCH = {ACTION_CONSUMER, 0, 0x221};

// Die mitgelieferten Profile. Reihenfolge der Aktionen wie BUTTON_NAMES:
//   Home/TV, Vol+, Vol-, Click, Power, Siri, Back, Mute, Play/Pause, Up, Right, Down, Left
static Profile builtinProfile(uint8_t index) {
  static const Profile BUILTIN[PROFILE_COUNT] = {
    // Für Kodi und Android zugleich. Menü- und Zurück-Taste so, wie sie ein Flirc-Dongle sendet, der an
    // Fire TV und CoreELEC gleichermaßen funktioniert: Menütaste der Tastatur und „AC Back“. Die Mitte des
    // Clickpads sendet die OK-Taste einer Fernbedienung („Menu Pick“) statt Enter. Siri öffnet die Suche.
    {"Universal",
     {A_KEY_MENU, A_VOL_UP, A_VOL_DOWN, A_OK, A_KEY_S, A_SEARCH, A_AC_BACK, A_MUTE, A_PLAY_PAUSE, A_UP, A_RIGHT, A_DOWN, A_LEFT}},
    // Kodis eigene Tastaturbelegung: Backspace = zurück, C = Kontextmenü, Leertaste = Play/Pause, S = Shutdown-Menü.
    {"Kodi (CoreELEC)",
     {A_KEY_C, A_VOL_UP, A_VOL_DOWN, A_ENTER, A_KEY_S, A_NONE, A_BACKSPACE, A_MUTE, A_SPACE, A_UP, A_RIGHT, A_DOWN, A_LEFT}},
    // Android-Oberfläche: Home/TV führt zum Startbildschirm. Ungetestet.
    {"Android / Fire TV",
     {A_AC_HOME, A_VOL_UP, A_VOL_DOWN, A_OK, A_NONE, A_SEARCH, A_AC_BACK, A_MUTE, A_PLAY_PAUSE, A_UP, A_RIGHT, A_DOWN, A_LEFT}},
    // Freier Platz zum Ausprobieren, anfangs eine Kopie von „Universal“.
    {"Custom",
     {A_KEY_MENU, A_VOL_UP, A_VOL_DOWN, A_OK, A_KEY_S, A_SEARCH, A_AC_BACK, A_MUTE, A_PLAY_PAUSE, A_UP, A_RIGHT, A_DOWN, A_LEFT}},
  };
  return BUILTIN[index < PROFILE_COUNT ? index : 0];
}

static Config defaultConfig() {
  Config c = {};
  c.version = CONFIG_VERSION;
  c.active = 0;
  for (uint8_t i = 0; i < PROFILE_COUNT; i++) c.profiles[i] = builtinProfile(i);
  // ESP32-S3 Super Mini: RGB-LED (WS2812) an GPIO48
  c.ledType = LED_WS2812;
  c.ledPin = 48;
  c.ledInvert = 0;
  c.ledBrightness = 24;
  c.ledDarkWhenConnected = 0;
  c.connInterval = 12;
  c.staleMs = 0;
  c.setupMinutes = 0;  // WLAN ab Werk dauerhaft an
  c.siriSetup = 1;
  return c;
}

// Übernimmt einen Profilnamen: ohne Steuerzeichen, auf die Feldgröße gekürzt, ohne ein UTF-8-Zeichen zu zerteilen.
static void setProfileName(Profile &p, const String &name) {
  size_t n = 0;
  for (size_t i = 0; i < name.length() && n < PROFILE_NAME_SIZE - 1; i++) {
    if ((uint8_t)name[i] >= 0x20) p.name[n++] = name[i];
  }
  // Ein am Ende angeschnittenes UTF-8-Zeichen wieder entfernen.
  size_t i = n;
  while (i > 0 && ((uint8_t)p.name[i - 1] & 0xC0) == 0x80) i--;
  if (i > 0) {
    uint8_t lead = p.name[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && n - (i - 1) < need) n = i - 1;
  }
  p.name[n] = 0;
}

static void sanitizeConfig(Config &c) {
  if (c.active >= PROFILE_COUNT) c.active = 0;
  for (uint8_t i = 0; i < PROFILE_COUNT; i++) {
    Profile &p = c.profiles[i];
    p.name[PROFILE_NAME_SIZE - 1] = 0;
    if (p.name[0] == 0) snprintf(p.name, PROFILE_NAME_SIZE, "Profile %u", i + 1);
    for (Action &a : p.map) {
      if (a.type > ACTION_CONSUMER) a.type = ACTION_NONE;
      a.mods &= MOD_CTRL | MOD_SHIFT | MOD_ALT;
      if (a.type == ACTION_KEY && (a.code < 0x04 || a.code > 0xE7)) a.type = ACTION_NONE;
      if (a.type == ACTION_CONSUMER && (a.code == 0 || a.code > 0x03FF)) a.type = ACTION_NONE;
    }
  }
  if (c.ledType > LED_GPIO) c.ledType = LED_NONE;
  if (c.ledPin > 48) c.ledType = LED_NONE;
  if (c.ledBrightness == 0) c.ledBrightness = 1;
  c.connInterval = constrain(c.connInterval, 6, 12);
  if (c.staleMs > 5000) c.staleMs = 5000;
  if (c.setupMinutes > 60) c.setupMinutes = 60;
}

// Einstellungen aus älteren Firmware-Versionen (anderes Format) werden nicht übernommen, sondern durch
// die Vorgaben ersetzt. Kopplung und WLAN sind davon nicht betroffen.
static Config loadConfig() {
  Config c;
  Preferences prefs;
  prefs.begin("bridge", true);
  size_t len = prefs.getBytes("cfg", &c, sizeof(c));
  prefs.end();
  if (len != sizeof(c) || c.version != CONFIG_VERSION) return defaultConfig();
  sanitizeConfig(c);
  return c;
}

static void saveConfig(const Config &c) {
  Preferences prefs;
  prefs.begin("bridge", false);
  prefs.putBytes("cfg", &c, sizeof(c));
  prefs.end();
}

static String jsonText(const char *text) {
  String out;
  for (const char *p = text; *p; p++) {
    if (*p == '"' || *p == '\\') out += '\\';
    if ((uint8_t)*p >= 0x20) out += *p;
  }
  return out;
}

static String profileJson(const Profile &p) {
  String s = "{\"name\":\"" + jsonText(p.name) + "\",\"map\":[";
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (i) s += ',';
    s += "{\"n\":\"" + String(BUTTON_NAMES[i]) + "\",\"t\":" + p.map[i].type + ",\"m\":" + p.map[i].mods + ",\"c\":" + p.map[i].code + "}";
  }
  return s + "]}";
}

// Die mitgelieferten Profile, damit die Weboberfläche ein Profil auf seine Vorgabe zurücksetzen kann.
static String builtinJson() {
  String s = "[";
  for (uint8_t i = 0; i < PROFILE_COUNT; i++) s += String(i ? "," : "") + profileJson(builtinProfile(i));
  return s + "]";
}

static String configJson(const Config &c) {
  String s = "{\"active\":" + String(c.active) + ",\"profiles\":[";
  for (uint8_t i = 0; i < PROFILE_COUNT; i++) s += String(i ? "," : "") + profileJson(c.profiles[i]);
  s += "],\"ledType\":" + String(c.ledType) + ",\"ledPin\":" + c.ledPin + ",\"ledInv\":" + c.ledInvert +
       ",\"ledBri\":" + c.ledBrightness + ",\"ledDark\":" + c.ledDarkWhenConnected + ",\"itv\":" + c.connInterval +
       ",\"stale\":" + c.staleMs + ",\"mins\":" + c.setupMinutes + ",\"siri\":" + c.siriSetup + "}";
  return s;
}
