// Einstellungen der Bridge: Tasten-Mapping, Status-LED, Timing. Liegen als ein Blob im NVS und werden
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

enum ActionType : uint8_t { ACTION_NONE = 0, ACTION_KEY = 1, ACTION_CONSUMER = 2 };
enum LedType : uint8_t { LED_NONE = 0, LED_WS2812 = 1, LED_GPIO = 2 };
static const uint8_t MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4;

struct Action {
  uint8_t type;
  uint8_t mods;   // MOD_*-Bits, nur bei ACTION_KEY
  uint16_t code;  // HID-Usage: Keyboard-Page (ACTION_KEY) bzw. Consumer-Page (ACTION_CONSUMER)
};

struct Config {
  uint8_t version;
  Action map[BUTTON_COUNT];
  uint8_t ledType;
  uint8_t ledPin;
  uint8_t ledInvert;             // nur LED_GPIO: LED leuchtet bei LOW
  uint8_t ledBrightness;         // nur LED_WS2812, 1…255
  uint8_t ledDarkWhenConnected;  // LED aus, solange die Remote verbunden ist
  uint8_t connInterval;          // Verbindungsintervall in 1,25-ms-Schritten, 6…12 (7,5…15 ms)
  uint16_t staleMs;              // Tastendrücke in den ersten staleMs nach dem Verbinden verwerfen (0 = aus)
  uint8_t setupMinutes;          // Setup-Modus endet nach so vielen Minuten ohne Zugriff (0 = nie, WLAN dauerhaft an)
  uint8_t siriSetup;             // Siri 5 s halten schaltet den Setup-Modus um
};

static const uint8_t CONFIG_VERSION = 1;

static Config defaultConfig() {
  Config c = {};
  c.version = CONFIG_VERSION;
  // Kodi-Mapping, als HID-Usages (unabhängig vom Tastaturlayout der Firmware)
  c.map[0] = {ACTION_KEY, 0, 0x06};        // Home/TV    -> C (Kontextmenü)
  c.map[1] = {ACTION_CONSUMER, 0, 0x00E9}; // Vol+
  c.map[2] = {ACTION_CONSUMER, 0, 0x00EA}; // Vol-
  c.map[3] = {ACTION_KEY, 0, 0x28};        // Click      -> Enter
  c.map[4] = {ACTION_KEY, 0, 0x16};        // Power      -> S (Shutdown-Menü)
  c.map[5] = {ACTION_NONE, 0, 0};          // Siri
  c.map[6] = {ACTION_KEY, 0, 0x2A};        // Back       -> Backspace
  c.map[7] = {ACTION_CONSUMER, 0, 0x00E2}; // Mute
  c.map[8] = {ACTION_KEY, 0, 0x2C};        // Play/Pause -> Leertaste
  c.map[9] = {ACTION_KEY, 0, 0x52};        // Up
  c.map[10] = {ACTION_KEY, 0, 0x4F};       // Right
  c.map[11] = {ACTION_KEY, 0, 0x51};       // Down
  c.map[12] = {ACTION_KEY, 0, 0x50};       // Left
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

static void sanitizeConfig(Config &c) {
  for (Action &a : c.map) {
    if (a.type > ACTION_CONSUMER) a.type = ACTION_NONE;
    a.mods &= MOD_CTRL | MOD_SHIFT | MOD_ALT;
    if (a.type == ACTION_KEY && (a.code < 0x04 || a.code > 0xE7)) a.type = ACTION_NONE;
    if (a.type == ACTION_CONSUMER && (a.code == 0 || a.code > 0x03FF)) a.type = ACTION_NONE;
  }
  if (c.ledType > LED_GPIO) c.ledType = LED_NONE;
  if (c.ledPin > 48) c.ledType = LED_NONE;
  if (c.ledBrightness == 0) c.ledBrightness = 1;
  c.connInterval = constrain(c.connInterval, 6, 12);
  if (c.staleMs > 5000) c.staleMs = 5000;
  if (c.setupMinutes > 60) c.setupMinutes = 60;
}

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

static String configJson(const Config &c) {
  String s = "{\"map\":[";
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (i) s += ',';
    s += "{\"n\":\"" + String(BUTTON_NAMES[i]) + "\",\"t\":" + c.map[i].type + ",\"m\":" + c.map[i].mods + ",\"c\":" + c.map[i].code + "}";
  }
  s += "],\"ledType\":" + String(c.ledType) + ",\"ledPin\":" + c.ledPin + ",\"ledInv\":" + c.ledInvert +
       ",\"ledBri\":" + c.ledBrightness + ",\"ledDark\":" + c.ledDarkWhenConnected + ",\"itv\":" + c.connInterval +
       ",\"stale\":" + c.staleMs + ",\"mins\":" + c.setupMinutes + ",\"siri\":" + c.siriSetup + "}";
  return s;
}
