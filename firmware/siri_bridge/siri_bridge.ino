// Siri Remote (3. Gen) → USB-HID-Bridge für den ESP32-S3.
//
// Koppelt die Remote per BLE (NimBLE, Central), abonniert den Button-Report 0xFB und gibt die Tasten
// als USB-Tastatur + Consumer Control aus. Protokoll-Details und Messwerte: siehe docs/PROTOCOL.md.
//
// Build/Flash: firmware/build.sh siri_bridge [upload]
//
// Bedienung am Dongle:
//   BOOT kurz drücken   Setup-Modus ein/aus (WLAN + Weboberfläche: Status, Mapping, LED, Timing, OTA)
//   BOOT 3 s halten     Bond löschen und neu starten (danach Remote in den Pairing-Modus bringen)
//   BOOT 10 s halten    Werkszustand: Kopplung, Einstellungen und WLAN löschen
//   Siri 5 s halten     Setup-Modus ein/aus von der Remote aus (abschaltbar)
// Ab Werk bleibt das WLAN im Heimnetz dauerhaft an; einstellbar ist eine Abschaltzeit ohne Zugriff.
// Der eigene offene Hotspot schaltet sich dagegen immer nach spätestens 10 min ohne Zugriff ab.
// Status-LED: schnelles Blinken = kein Bond; Aufblitzen alle 2 s = gekoppelt, nicht verbunden;
// Dauerlicht = verbunden; langsames Blinken (violett) = eigener Hotspot offen; Flackern (weiß) = Update.

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "config.h"
#include "web_ui.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const uint8_t BOOT_BUTTON = 0;
static const int PAIRING_MIN_RSSI = -55;      // unbekannte Remote nur koppeln, wenn sie direkt daneben liegt
static const uint32_t RETRY_DELAY_MS = 3000;  // Pause nach einem Fehlversuch, schont den Akku der Remote
static const uint32_t SIRI_HOLD_MS = 5000;

// Apple-Manufacturer-Data der Remote: 4C 00 | 07 0D 02 15 03 02 | <Identity-Adresse, 6 Byte> | …
static const uint8_t FINGERPRINT[] = {0x4C, 0x00, 0x07, 0x0D, 0x02, 0x15, 0x03, 0x02};
static const size_t IDENTITY_OFFSET = sizeof(FINGERPRINT);

static const NimBLEUUID HID_SERVICE((uint16_t)0x1812);
static const NimBLEUUID REPORT_CHAR((uint16_t)0x2A4D);
static const NimBLEUUID REPORT_REFERENCE((uint16_t)0x2908);
static const NimBLEUUID BATTERY_SERVICE((uint16_t)0x180F);
static const NimBLEUUID BATTERY_LEVEL((uint16_t)0x2A19);
static const NimBLEUUID POWER_STATE((uint16_t)0x2A1A);  // 0xAB lädt, 0xAF entlädt, 0xBB eingesteckt (laut Quellen)
static const uint8_t BUTTON_REPORT_ID = 0xFB;
static const uint8_t REPORT_TYPE_INPUT = 1;

// Die Remote verlangt per PPCP 7,5–15 ms, Latency 80, Timeout 6 s; mit anderen Werten reißt die
// Verbindung ab, sobald sie zwischen Tastendrücken schläft. Das Intervall ist einstellbar (cfg.connInterval).
static const uint16_t CONN_LATENCY = 80, CONN_TIMEOUT = 600;

static const char *AP_SSID = "siri-bridge-setup";  // offenes Netz, nur im Setup-Modus und ohne gespeichertes WLAN
static const char *HOSTNAME = "siri-bridge";

struct ButtonEvent {
  uint16_t mask;
  uint32_t micros;
};

enum WifiState : uint8_t { WIFI_IDLE, WIFI_JOINING, WIFI_STATION, WIFI_ACCESS_POINT };

USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
WebServer server(80);
DNSServer dns;  // nur im eigenen Netz: beantwortet jede Anfrage mit der eigenen Adresse (Captive Portal)

static Config cfg;
static QueueHandle_t buttonQueue;
static NimBLEClient *client = nullptr;
static const NimBLEAdvertisedDevice *candidate = nullptr;
static volatile bool connectPending = false;
static volatile bool linkUp = false;
static volatile bool releaseAll = false;
static volatile uint32_t advSeen = 0;  // Advertising-Pakete der Remote seit der letzten Statuszeile
static volatile int advRssi = 0;
static uint16_t currentMask = 0;
static uint32_t scanAt = 0;       // millis(), ab wann nach einem Fehlversuch wieder gescannt wird (0 = nichts geplant)
static uint32_t readyAt = 0;      // millis() des letzten erfolgreichen Abonnierens
static uint32_t siriDownAt = 0;
static int lastKey = -1;
static uint32_t lastKeyAt = 0;
static volatile int batteryPercent = -1;  // -1 = unbekannt
static volatile int powerState = -1;
static uint32_t pressSeq = 0;  // zählt Tastendrücke, damit die Weboberfläche auch sehr kurze Drücke bemerkt

static WifiState wifiState = WIFI_IDLE;
static uint32_t wifiDeadline = 0;
static uint32_t lastHttpAt = 0;
static bool otaRunning = false;
static bool otaOk = false;
static bool unpairRequested = false;
static bool setupOffRequested = false;
static bool factoryResetRequested = false;
static uint32_t apSince = 0;
static uint32_t rebootAt = 0;
static uint8_t ledPinInUse = 255;
static uint32_t wifiSignalAt = 0;  // millis() beim Start des gelben Blinksignals (0 = keins)
static uint8_t bootStage = 0;      // BOOT gehalten: 1 = ab 3 s (entkoppeln), 2 = ab 10 s (Werkszustand)

static bool isBonded() { return NimBLEDevice::getNumBonds() > 0; }
static bool setupActive() { return wifiState != WIFI_IDLE; }

// ---------------------------------------------------------------------------------------------------
// Status-LED

static void ledWrite(bool on, uint8_t r, uint8_t g, uint8_t b) {
  if (cfg.ledType == LED_WS2812) {
    uint16_t scale = on ? cfg.ledBrightness : 0;
    rgbLedWrite(cfg.ledPin, r * scale / 255, g * scale / 255, b * scale / 255);
  } else if (cfg.ledType == LED_GPIO) {
    if (ledPinInUse != cfg.ledPin) {
      pinMode(cfg.ledPin, OUTPUT);
      ledPinInUse = cfg.ledPin;
    }
    digitalWrite(cfg.ledPin, on != (bool)cfg.ledInvert ? HIGH : LOW);
  }
}

static void updateLed(bool force = false) {
  static uint32_t bondCheckedAt = 0;
  static bool bonded = false;
  static int shown = -1;
  uint32_t now = millis();
  if (now - bondCheckedAt > 500 || bondCheckedAt == 0) {
    bondCheckedAt = now;
    bonded = isBonded();
  }
  // Muster und Farbe je Zustand; die Farbe wirkt nur bei einer RGB-LED.
  bool on;
  uint8_t color;  // 0 blau, 1 violett, 2 weiß, 3 gelb, 4 rot
  if (bootStage == 2) {  // BOOT ab 10 s gehalten: Loslassen stellt den Werkszustand her
    on = true;
    color = 2;
  } else if (bootStage == 1) {  // BOOT ab 3 s gehalten: Loslassen entkoppelt die Remote
    on = (now % 200) < 100;
    color = 4;
  } else if (wifiSignalAt && now - wifiSignalAt < 3000) {  // WLAN ein-/ausgeschaltet: 3x gelb
    on = ((now - wifiSignalAt) % 1000) < 500;
    color = 3;
  } else if (otaRunning) {
    on = (now % 100) < 50;
    color = 2;
  } else if (wifiState == WIFI_ACCESS_POINT) {  // Hotspot offen: WLAN muss eingerichtet werden
    on = (now % 1000) < 500;
    color = 1;
  } else if (linkUp) {
    on = !cfg.ledDarkWhenConnected;
    color = 0;
  } else if (bonded) {
    on = (now % 2000) < 100;
    color = 0;
  } else {
    on = (now % 400) < 200;
    color = 0;
  }
  int state = on ? color + 1 : 0;
  if (state == shown && !force) return;
  shown = state;
  static const uint8_t COLORS[5][3] = {{0, 0, 255}, {150, 0, 255}, {255, 255, 255}, {255, 150, 0}, {255, 0, 0}};
  ledWrite(on, COLORS[color][0], COLORS[color][1], COLORS[color][2]);
}

// ---------------------------------------------------------------------------------------------------
// BLE

// Identity-Adresse aus dem Advertising (dort in Leserichtung, NimBLE speichert sie umgekehrt).
static bool identityMatchesBond(const std::string &mfg) {
  NimBLEAddress bonded = NimBLEDevice::getBondedAddress(0);
  const uint8_t *val = bonded.getVal();
  for (int i = 0; i < 6; i++) {
    if ((uint8_t)mfg[IDENTITY_OFFSET + i] != val[5 - i]) return false;
  }
  return true;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (connectPending || linkUp) return;
    bool bonded = isBonded();

    // Die gebondete Remote meldet sich nach dem Aufwachen mit einem anonymen Advertising (Apple-Typ 0x07,
    // 17 Byte wechselnde Daten, kein HID-Service, keine Identity-Adresse). Erkennbar ist sie dann nur
    // daran, dass der Controller ihre rotierende Adresse per IRK zur gebondeten Identity-Adresse auflöst.
    bool known = bonded && memcmp(dev->getAddress().getVal(), NimBLEDevice::getBondedAddress(0).getVal(), 6) == 0;

    if (!known) {
      // Fingerprint-Advertising: so meldet sie sich im Pairing-Modus und direkt nach einem Verbindungsabbruch.
      if (!dev->isAdvertisingService(HID_SERVICE)) return;
      std::string mfg = dev->getManufacturerData();
      if (mfg.size() < IDENTITY_OFFSET + 6 || memcmp(mfg.data(), FINGERPRINT, sizeof(FINGERPRINT)) != 0) return;
      if (bonded && !identityMatchesBond(mfg)) return;  // fremde Siri Remote
    }
    advSeen = advSeen + 1;
    advRssi = dev->getRSSI();

    if (!bonded && dev->getRSSI() < PAIRING_MIN_RSSI) {
      Serial.printf("[scan] Remote gesehen, aber zu weit weg zum Koppeln (RSSI %d)\n", dev->getRSSI());
      return;
    }
    Serial.printf("[scan] Remote %s RSSI %d, %s\n", dev->getAddress().toString().c_str(), dev->getRSSI(),
                  bonded ? (known ? "gebondet (Adresse aufgeloest)" : "gebondet (Fingerprint)") : "neu -> Pairing");
    candidate = dev;
    connectPending = true;
    NimBLEDevice::getScan()->stop();
  }
} scanCallbacks;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    Serial.printf("[ble] getrennt, Grund 0x%03x\n", reason);
    if (linkUp) releaseAll = true;  // fehlgeschlagene Versuche räumt connectToCandidate() selbst auf
    linkUp = false;
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    Serial.printf("[ble] Pairing/Verschluesselung fertig: encrypted=%d bonded=%d\n", info.isEncrypted(), info.isBonded());
  }
} clientCallbacks;

static void onButtonReport(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  if (len < 2) return;
  ButtonEvent ev = {(uint16_t)(data[0] | (data[1] << 8)), (uint32_t)micros()};
  xQueueSend(buttonQueue, &ev, 0);
}

static void onBatteryLevel(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  if (len >= 1) batteryPercent = data[0];
}

static void onPowerState(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  if (len >= 1) powerState = data[0];
}

// Akkustand der Remote lesen und Änderungen abonnieren. Läuft erst, wenn die Tasten schon funktionieren.
static void readBattery() {
  NimBLERemoteService *service = client->getService(BATTERY_SERVICE);
  if (!service) return;
  struct { const NimBLEUUID &uuid; volatile int *value; void (*callback)(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool); } items[] = {
    {BATTERY_LEVEL, &batteryPercent, onBatteryLevel}, {POWER_STATE, &powerState, onPowerState}};
  for (auto &item : items) {
    NimBLERemoteCharacteristic *chr = service->getCharacteristic(item.uuid);
    if (!chr) continue;
    if (chr->canRead()) {
      NimBLEAttValue value = chr->readValue();
      if (value.size() >= 1) *item.value = value[0];
    }
    if (chr->canNotify()) chr->subscribe(true, item.callback);
  }
  Serial.printf("[ble] Akku der Remote: %d %%, Power-Status 0x%02x\n", (int)batteryPercent, (unsigned)(powerState & 0xFF));
}

static void startScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(false);  // alles Nötige steht im Advertising selbst
  // BLE und WLAN teilen sich ein Funkmodul. Ein Scan über die volle Zeit lässt dem WLAN nichts übrig:
  // ohne gekoppelte Remote (Dauer-Scan) konnte sich dann niemand mit dem Hotspot verbinden. Mit aktivem
  // WLAN deshalb nur 30 von 150 ms scannen.
  scan->setInterval(240);                      // 150 ms
  scan->setWindow(setupActive() ? 48 : 240);  // 30 ms bzw. durchgehend
  scan->start(0, false, true);
  Serial.printf("[scan] gestartet (%s)\n", isBonded() ? "warte auf gebondete Remote" : "kein Bond: warte auf Remote im Pairing-Modus");
}

// Sucht den Button-Report über den Report-Reference-Descriptor (alle Reports teilen UUID 0x2A4D).
static bool subscribeButtons() {
  NimBLERemoteService *hid = client->getService(HID_SERVICE);
  if (!hid) {
    Serial.println("[ble] HID-Service nicht gefunden");
    return false;
  }
  for (NimBLERemoteCharacteristic *chr : hid->getCharacteristics(true)) {
    if (chr->getUUID() != REPORT_CHAR || !chr->canNotify()) continue;
    NimBLERemoteDescriptor *ref = chr->getDescriptor(REPORT_REFERENCE);
    if (!ref) continue;
    NimBLEAttValue value = ref->readValue();
    if (value.size() != 2 || value[0] != BUTTON_REPORT_ID || value[1] != REPORT_TYPE_INPUT) continue;
    if (!chr->subscribe(true, onButtonReport)) {
      Serial.println("[ble] Abonnieren des Button-Reports fehlgeschlagen");
      return false;
    }
    Serial.printf("[ble] Button-Report 0xFB abonniert (Handle 0x%04x)\n", chr->getHandle());
    return true;
  }
  Serial.println("[ble] Button-Report 0xFB nicht gefunden");
  return false;
}

static void connectToCandidate() {
  uint32_t t0 = millis();
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectTimeout(5000);
  }
  client->setConnectionParams(cfg.connInterval, cfg.connInterval, CONN_LATENCY, CONN_TIMEOUT);
  bool ok = client->connect(candidate, true);
  uint32_t tConnected = millis();
  if (ok) {
    ok = client->secureConnection();
    if (!ok) Serial.printf("[ble] Verschluesselung/Pairing fehlgeschlagen (rc=%d)\n", client->getLastError());
  } else {
    Serial.printf("[ble] Verbindungsaufbau fehlgeschlagen (rc=%d)\n", client->getLastError());
  }
  uint32_t tSecured = millis();
  if (ok) ok = subscribeButtons();

  if (ok) {
    readyAt = millis();
    linkUp = true;
    NimBLEConnInfo info = client->getConnInfo();
    Serial.printf("[ble] bereit: verbunden %lu ms, verschluesselt +%lu ms, abonniert +%lu ms | Intervall %.2f ms, Latency %u, Timeout %u ms, bonded=%d\n",
                  (unsigned long)(tConnected - t0), (unsigned long)(tSecured - tConnected), (unsigned long)(readyAt - tSecured),
                  info.getConnInterval() * 1.25, info.getConnLatency(), info.getConnTimeout() * 10, info.isBonded());
    readBattery();
  } else {
    if (client->isConnected()) client->disconnect();
    scanAt = millis() + RETRY_DELAY_MS;
  }
  connectPending = false;
}

static void unpair() {
  Serial.println("[ble] Bond wird geloescht");
  bool wasUp = linkUp;
  if (client && client->isConnected()) client->disconnect();
  NimBLEDevice::deleteAllBonds();
  if (!wasUp && !connectPending) {  // sonst startet der Scan über onDisconnect -> releaseAll
    NimBLEDevice::getScan()->stop();
    scanAt = millis() + 500;
  }
}

// ---------------------------------------------------------------------------------------------------
// Tasten → USB-HID

static void sendAction(const Action &a, bool down) {
  if (a.type == ACTION_KEY) {
    static const uint8_t MOD_USAGE[3] = {0xE0, 0xE1, 0xE2};  // linke Strg, Umschalt, Alt
    if (down) {
      for (int i = 0; i < 3; i++) if (a.mods & (1 << i)) keyboard.pressRaw(MOD_USAGE[i]);
      keyboard.pressRaw(a.code);
    } else {
      keyboard.releaseRaw(a.code);
      for (int i = 0; i < 3; i++) if (a.mods & (1 << i)) keyboard.releaseRaw(MOD_USAGE[i]);
    }
  } else if (a.type == ACTION_CONSUMER) {
    if (down) consumer.press(a.code); else consumer.release();
  }
}

static void applyMask(const ButtonEvent &ev) {
  // Altlasten-Filter: Die Remote liefert beim Verbinden nach, was während der Trennung gedrückt wurde.
  if (cfg.staleMs && millis() - readyAt < cfg.staleMs) {
    Serial.printf("[key] 0x%04x verworfen (%lu ms nach dem Verbinden)\n", ev.mask, (unsigned long)(millis() - readyAt));
    return;
  }
  uint16_t changed = ev.mask ^ currentMask;
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    uint16_t bit = 1 << i;
    if (!(changed & bit)) continue;
    bool down = ev.mask & bit;
    sendAction(cfg.profiles[cfg.active].map[i], down);
    if (i == SIRI_INDEX) siriDownAt = down ? millis() : 0;
    if (down) {
      lastKey = i;
      lastKeyAt = millis();
      pressSeq++;
    }
    Serial.printf("[key] %-10s %s  (+%lu us bis USB)\n", BUTTON_NAMES[i], down ? "gedrueckt" : "los", (unsigned long)(micros() - ev.micros));
  }
  currentMask = ev.mask;
}

static void releaseEverything() {
  keyboard.releaseAll();
  consumer.release();
  currentMask = 0;
  siriDownAt = 0;
  xQueueReset(buttonQueue);
}

// ---------------------------------------------------------------------------------------------------
// Setup-Modus: WLAN + Weboberfläche

static String jsonEscape(const String &in) {
  String out;
  for (char c : in) {
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

// Nach dem Ein- oder Ausschalten des WLANs einen laufenden Scan mit passender Funkzeit neu starten.
static void retuneScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) return;
  scan->stop();
  scanAt = millis() + 100;
}

static String savedSsid() {
  Preferences prefs;
  prefs.begin("bridge", true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  return ssid;
}

static void touch() { lastHttpAt = millis(); }

// Nach so vielen Millisekunden ohne Zugriff geht das WLAN aus; 0 = nie. Der offene Hotspot bleibt nie
// dauerhaft an, auch wenn für das Heimnetz „nie“ eingestellt ist.
static uint32_t setupLimitMs() {
  uint32_t minutes = cfg.setupMinutes;
  if (minutes == 0 && wifiState == WIFI_ACCESS_POINT) minutes = 10;
  return minutes * 60000UL;
}

static void handleState() {
  touch();
  const char *ble = linkUp ? "connected" : connectPending ? "connecting" : isBonded() ? "searching" : "pairing";
  String s = "{\"version\":\"" FW_VERSION "\",\"uptime\":" + String(millis() / 1000) + ",\"heap\":" + ESP.getFreeHeap() + ",\"ble\":\"" + ble + "\"";
  s += ",\"bond\":\"" + String(isBonded() ? NimBLEDevice::getBondedAddress(0).toString().c_str() : "") + "\"";
  if (linkUp && client) {
    s += ",\"interval\":" + String(client->getConnInfo().getConnInterval() * 1.25, 2) + ",\"rssi\":" + client->getRssi();
  }
  if (batteryPercent >= 0) s += ",\"batt\":" + String((int)batteryPercent);
  if (powerState >= 0) s += ",\"power\":" + String((int)powerState);
  if (lastKey >= 0) {
    s += ",\"lastKey\":\"" + String(BUTTON_NAMES[lastKey]) + "\",\"lastKeyAge\":" + (millis() - lastKeyAt) / 1000;
  }
  bool ap = wifiState == WIFI_ACCESS_POINT;
  s += ",\"wifiMode\":\"" + String(ap ? "ap" : wifiState == WIFI_STATION ? "sta" : "verbindet") + "\"";
  s += ",\"ssid\":\"" + jsonEscape(ap ? String(AP_SSID) : WiFi.SSID()) + "\",\"ip\":\"" + (ap ? WiFi.softAPIP() : WiFi.localIP()).toString() + "\"";
  {
    Preferences prefs;
    prefs.begin("bridge", true);
    String ip = prefs.getString("ip", "");
    s += ",\"savedSsid\":\"" + jsonEscape(prefs.getString("ssid", "")) + "\",\"net\":{\"dhcp\":" + (ip.isEmpty() ? "true" : "false") +
         ",\"ip\":\"" + ip + "\",\"mask\":\"" + prefs.getString("mask", "") + "\",\"gw\":\"" + prefs.getString("gw", "") +
         "\",\"dns\":\"" + prefs.getString("dns", "") + "\"}";
    prefs.end();
  }
  uint32_t limit = setupLimitMs(), idle = millis() - lastHttpAt;  // limit 0 = dauerhaft an
  s += ",\"setupLeft\":" + String(idle < limit ? (limit - idle) / 1000 : 0) + ",\"cfg\":" + configJson(cfg) + "}";
  server.send(200, "application/json", s);
}

// Für die Live-Anzeige der Weboberfläche: wird mehrmals pro Sekunde abgefragt, deshalb so klein wie möglich.
static void handleLive() {
  touch();
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"mask\":%u,\"seq\":%lu,\"last\":%d}", currentMask, (unsigned long)pressSeq, lastKey);
  server.send(200, "application/json", buf);
}

static void handleConfig() {
  touch();
  Config next = cfg;
  // Profile: n<i> = Name, p<i> = 13 Aktionen als "typ,mods,code;…". Je Profil ein Feld, weil der Webserver
  // nur eine begrenzte Zahl von Formularfeldern annimmt.
  for (uint8_t i = 0; i < PROFILE_COUNT; i++) {
    if (server.hasArg("n" + String(i))) setProfileName(next.profiles[i], server.arg("n" + String(i)));
    String list = server.arg("p" + String(i));
    int pos = 0;
    for (uint8_t k = 0; k < BUTTON_COUNT && pos < (int)list.length(); k++) {
      int end = list.indexOf(';', pos);
      if (end < 0) end = list.length();
      String v = list.substring(pos, end);
      int c1 = v.indexOf(','), c2 = v.indexOf(',', c1 + 1);
      if (c1 > 0 && c2 > c1) {
        next.profiles[i].map[k] = {(uint8_t)v.substring(0, c1).toInt(), (uint8_t)v.substring(c1 + 1, c2).toInt(), (uint16_t)v.substring(c2 + 1).toInt()};
      }
      pos = end + 1;
    }
  }
  if (server.hasArg("active")) next.active = server.arg("active").toInt();
  if (server.hasArg("ledType")) next.ledType = server.arg("ledType").toInt();
  if (server.hasArg("ledPin")) next.ledPin = constrain(server.arg("ledPin").toInt(), 0, 255);
  if (server.hasArg("ledBri")) next.ledBrightness = constrain(server.arg("ledBri").toInt(), 1, 255);
  if (server.hasArg("ledInv")) next.ledInvert = server.arg("ledInv").toInt() != 0;
  if (server.hasArg("ledDark")) next.ledDarkWhenConnected = server.arg("ledDark").toInt() != 0;
  if (server.hasArg("itv")) next.connInterval = constrain(server.arg("itv").toInt(), 6, 12);
  if (server.hasArg("stale")) next.staleMs = constrain(server.arg("stale").toInt(), 0, 5000);
  if (server.hasArg("mins")) next.setupMinutes = constrain(server.arg("mins").toInt(), 0, 60);
  if (server.hasArg("siri")) next.siriSetup = server.arg("siri").toInt() != 0;
  sanitizeConfig(next);

  releaseEverything();       // keine Taste mit alter Belegung hängen lassen
  ledWrite(false, 0, 0, 0);  // alte LED ausschalten, bevor Pin oder Art wechseln
  cfg = next;
  saveConfig(cfg);
  updateLed(true);
  Serial.println("[cfg] Einstellungen gespeichert");
  server.send(200, "text/plain", "ok");
}

static void handleDefaults() {
  touch();
  releaseEverything();
  ledWrite(false, 0, 0, 0);
  cfg = defaultConfig();
  saveConfig(cfg);
  updateLed(true);
  server.send(200, "text/plain", "ok");
}

static void startAccessPoint() {
  server.stop();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // Station-Anteil nur, um nach Netzen suchen zu können
  WiFi.softAP(AP_SSID);
  wifiState = WIFI_ACCESS_POINT;
  apSince = millis();
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  Serial.printf("[wifi] eigenes Netz \"%s\", http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void joinWifi() {
  Preferences prefs;
  prefs.begin("bridge", true);
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  String ip = prefs.getString("ip", ""), mask = prefs.getString("mask", ""), gw = prefs.getString("gw", ""), nameserver = prefs.getString("dns", "");
  prefs.end();
  if (ssid.isEmpty()) {
    startAccessPoint();
    return;
  }
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  IPAddress address, netmask, gateway, dnsServer;
  if (address.fromString(ip) && netmask.fromString(mask)) {  // feste Adresse; Gateway und DNS sind optional
    gateway.fromString(gw);
    dnsServer.fromString(nameserver);
    WiFi.config(address, gateway, netmask, dnsServer);
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);  // DHCP
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiState = WIFI_JOINING;
  wifiDeadline = millis() + 15000;
  Serial.printf("[wifi] verbinde mit \"%s\" ...\n", ssid.c_str());
}

// signal: gelbes Blinksignal an der LED, wenn jemand das WLAN von Hand umschaltet (nicht beim Start).
static void startSetup(bool signal = true) {
  if (setupActive()) return;
  Serial.println("[setup] Setup-Modus EIN");
  lastHttpAt = millis();
  if (signal) wifiSignalAt = millis() | 1;
  joinWifi();
  retuneScan();
}

static void stopSetup() {
  if (!setupActive()) return;
  Serial.println("[setup] Setup-Modus AUS");
  server.stop();
  dns.stop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiState = WIFI_IDLE;
  wifiSignalAt = millis() | 1;
  retuneScan();
}

static void handleWifi() {
  touch();
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  // Adressvergabe: dhcp=1 automatisch, dhcp=0 fest (ip und mask Pflicht, gw und dns optional).
  // Fehlt das Feld, bleibt die Adressvergabe unverändert. keep=1 behält das gespeicherte Passwort.
  bool manual = server.arg("dhcp") == "0";
  IPAddress check;
  if (manual && (!check.fromString(server.arg("ip")) || !check.fromString(server.arg("mask")) ||
                 (!server.arg("gw").isEmpty() && !check.fromString(server.arg("gw"))) ||
                 (!server.arg("dns").isEmpty() && !check.fromString(server.arg("dns"))))) {
    server.send(400, "text/plain", "bad-ip");  // Kennung, den Text dazu hat die Weboberfläche
    return;
  }
  Preferences prefs;
  prefs.begin("bridge", false);
  if (ssid.isEmpty()) {
    for (const char *key : {"ssid", "pass", "ip", "mask", "gw", "dns"}) prefs.remove(key);
  } else {
    prefs.putString("ssid", ssid);
    if (server.arg("keep") != "1") prefs.putString("pass", pass);
    if (server.hasArg("dhcp")) {
      for (const char *key : {"ip", "mask", "gw", "dns"}) {
        if (manual) prefs.putString(key, server.arg(key)); else prefs.remove(key);
      }
    }
  }
  prefs.end();
  Serial.printf("[wifi] Zugangsdaten %s\n", ssid.isEmpty() ? "geloescht" : "gespeichert");
  server.send(200, "text/plain", "ok");
  delay(300);  // Antwort noch ausliefern, bevor das Netz wechselt
  joinWifi();  // ohne gespeichertes WLAN führt das direkt in den eigenen Hotspot
}

// Sucht WLANs in Reichweite (blockiert ca. 2 s) und liefert sie nach Signalstärke sortiert, je SSID einmal.
static void handleScan() {
  touch();
  int n = WiFi.scanNetworks(false, false, false, 120);
  String s = "[";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool duplicate = false;  // scanNetworks sortiert nach Signalstärke: der erste Treffer je SSID ist der stärkste
    for (int j = 0; j < i; j++) duplicate = duplicate || WiFi.SSID(j) == ssid;
    if (duplicate) continue;
    if (s.length() > 1) s += ',';
    s += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + WiFi.RSSI(i) + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  WiFi.scanDelete();
  server.send(200, "application/json", s + "]");
}

static void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    touch();
    Serial.printf("[ota] Upload %s\n", upload.filename.c_str());
    otaRunning = true;
    otaOk = false;
    updateLed(true);
    releaseEverything();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    otaOk = Update.end(true);
    if (!otaOk) Update.printError(Serial);
    Serial.printf("[ota] %s, %u Byte\n", otaOk ? "ok" : "FEHLER", (unsigned)upload.totalSize);
    otaRunning = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaRunning = false;
  }
}

static void handleOtaDone() {
  touch();
  if (!otaOk) {
    server.send(500, "text/plain", Update.errorString());
    return;
  }
  // Nach dem Neustart wieder in den Setup-Modus, damit die Seite die neue Version zeigt.
  Preferences prefs;
  prefs.begin("bridge", false);
  prefs.putBool("setupboot", true);
  prefs.end();
  server.send(200, "text/plain", "ok");
  rebootAt = millis() + 800;
}

static void setupRoutes() {
  server.on("/", HTTP_GET, []() { touch(); server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/builtin", HTTP_GET, []() { touch(); server.send(200, "application/json", builtinJson()); });
  server.on("/api/factory-reset", HTTP_POST, []() { server.send(200, "text/plain", "ok"); factoryResetRequested = true; });
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/defaults", HTTP_POST, handleDefaults);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/unpair", HTTP_POST, []() { touch(); unpairRequested = true; server.send(200, "text/plain", "ok"); });
  server.on("/api/setup-off", HTTP_POST, []() { server.send(200, "text/plain", "ok"); setupOffRequested = true; });
  server.on("/api/reboot", HTTP_POST, []() { server.send(200, "text/plain", "ok"); rebootAt = millis() + 500; });
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.onNotFound([]() {
    // Im eigenen Netz landen hier die Verbindungstests von Handy und Laptop; die Umleitung lässt
    // deren Anmeldeseite mit der Weboberfläche aufgehen.
    if (wifiState == WIFI_ACCESS_POINT) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "nicht gefunden");
    }
  });
}

static void serviceSetup() {
  if (!setupActive()) return;
  if (wifiState == WIFI_JOINING) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiState = WIFI_STATION;
      MDNS.begin(HOSTNAME);
      MDNS.addService("http", "tcp", 80);
      server.begin();
      Serial.printf("[wifi] verbunden, http://%s.local oder http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
    } else if ((int32_t)(millis() - wifiDeadline) >= 0) {
      Serial.println("[wifi] WLAN nicht erreichbar, oeffne eigenes Netz");
      startAccessPoint();
    }
    return;
  }
  if (wifiState == WIFI_ACCESS_POINT) {
    dns.processNextRequest();
    // War das gespeicherte WLAN nur vorübergehend weg, alle 5 min erneut versuchen – aber nicht, solange
    // jemand im Hotspot hängt und vielleicht gerade die Zugangsdaten korrigiert.
    if (millis() - apSince > 300000UL && WiFi.softAPgetStationNum() == 0) {
      apSince = millis();
      if (!savedSsid().isEmpty()) {
        joinWifi();
        return;
      }
    }
  }
  server.handleClient();
  uint32_t limit = setupLimitMs();
  bool busy = wifiState == WIFI_ACCESS_POINT && WiFi.softAPgetStationNum() > 0;  // jemand hängt im Hotspot
  bool timedOut = limit > 0 && !busy && millis() - lastHttpAt > limit;
  if (setupOffRequested || timedOut) {
    setupOffRequested = false;
    delay(200);
    stopSetup();
  }
}

// ---------------------------------------------------------------------------------------------------
// Firmware-Rollback: Nach einem OTA-Update gilt die neue Firmware erst als gut, wenn sie 15 s gelaufen
// ist. Stürzt sie vorher ab, startet der Bootloader beim nächsten Versuch wieder die vorige Version
// (liegt im zweiten App-Slot). Ohne diese Funktion würde der Core die Firmware schon vor setup() bestätigen.

extern "C" bool verifyRollbackLater() { return true; }

static void confirmFirmware() {
  static bool done = false;
  if (done || millis() < 15000) return;
  done = true;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] neue Firmware laeuft stabil und ist bestaetigt");
  }
}

// ---------------------------------------------------------------------------------------------------
// Bedienung am Gerät, Serial

// Alle 5 s eine Statuszeile, damit im Serial-Log auch ohne Tastendruck sichtbar ist, was der Dongle tut.
static void printStatus() {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  const char *state = linkUp ? "verbunden" : connectPending ? "verbinde" : isBonded() ? "suche gebondete Remote" : "warte auf Pairing";
  const char *wifi = wifiState == WIFI_IDLE ? "aus" : wifiState == WIFI_JOINING ? "verbindet" : wifiState == WIFI_STATION ? "WLAN" : "AP";
  Serial.printf("[stat] up=%lus %s | bonds=%d adv=%lu rssi=%d heap=%lu setup=%s\n", (unsigned long)(millis() / 1000), state,
                NimBLEDevice::getNumBonds(), (unsigned long)advSeen, advRssi, (unsigned long)ESP.getFreeHeap(), wifi);
  advSeen = 0;
}

// BOOT-Taste, ausgewertet beim Loslassen: kurz = WLAN ein/aus, ab 3 s = Remote entkoppeln, ab 10 s =
// Werkszustand. Die LED zeigt die erreichte Stufe schon während des Haltens (rot blinkend, dann weiß).
static void checkBootButton() {
  static uint32_t pressedSince = 0;
  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (!pressedSince) pressedSince = millis();
    uint32_t held = millis() - pressedSince;
    bootStage = held >= 10000 ? 2 : held >= 3000 ? 1 : 0;
    return;
  }
  if (!pressedSince) return;
  uint32_t held = millis() - pressedSince;
  pressedSince = 0;
  bootStage = 0;
  if (held >= 10000) {
    factoryResetRequested = true;
  } else if (held >= 3000) {
    Serial.println("[sys] BOOT 3 s gehalten: Bonds loeschen und neu starten");
    NimBLEDevice::deleteAllBonds();
    delay(300);
    ESP.restart();
  } else if (held > 50) {
    if (setupActive()) stopSetup(); else startSetup();
  }
}

// Zeilenbefehle über den USB-Serial-Port, für die Entwicklung am Pi:
//   setup              Setup-Modus umschalten
//   wifi <ssid>\t<pw>  WLAN-Zugangsdaten speichern (Passwort wird nicht ausgegeben)
static void readSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 120) line += c;
      continue;
    }
    if (line == "setup") {
      if (setupActive()) stopSetup(); else startSetup();
    } else if (line.startsWith("wifi ")) {
      int tab = line.indexOf('\t');
      if (tab > 5) {
        Preferences prefs;
        prefs.begin("bridge", false);
        prefs.putString("ssid", line.substring(5, tab));
        prefs.putString("pass", line.substring(tab + 1));
        prefs.end();
        Serial.println("[wifi] Zugangsdaten gespeichert");
      }
    }
    line = "";
  }
}

void setup() {
#ifdef CRASH_TEST  // nur zum Testen des Rollbacks: Firmware, die beim Start abstürzt
  abort();
#endif
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // Log nie blockieren lassen, falls ein Host den Port öffnet, aber nicht liest
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  cfg = loadConfig();
  keyboard.begin();
  consumer.begin();
  USB.begin();
  buttonQueue = xQueueCreate(32, sizeof(ButtonEvent));
  WiFi.persistent(false);  // Zugangsdaten verwaltet die Firmware selbst im NVS
  setupRoutes();

  NimBLEDevice::init(HOSTNAME);
  NimBLEDevice::setSecurityAuth(true, false, false);  // Bonding, Just Works; die Remote kann nur Legacy Pairing
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  updateLed(true);  // erst nach NimBLEDevice::init(): die LED fragt den Bond-Speicher ab
  delay(1500);      // dem Host Zeit geben, den CDC-Port zu öffnen, damit die ersten Zeilen ankommen
  Serial.printf("\n[sys] siri-bridge %s, Bonds: %d\n", FW_VERSION, NimBLEDevice::getNumBonds());
  startScan();

  Preferences prefs;
  prefs.begin("bridge", false);
  bool setupOnBoot = prefs.getBool("setupboot", false);
  if (setupOnBoot) prefs.remove("setupboot");
  prefs.end();
  if (setupOnBoot || cfg.setupMinutes == 0) startSetup(false);  // 0 Minuten = WLAN dauerhaft an
}

void loop() {
  checkBootButton();
  confirmFirmware();
  readSerial();
  updateLed();
  printStatus();
  serviceSetup();

  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) ESP.restart();

  if (unpairRequested) {
    unpairRequested = false;
    unpair();
  }
  if (factoryResetRequested) {  // Kopplung, Einstellungen und WLAN löschen, dann wie frisch geflasht starten
    Serial.println("[sys] Werkszustand: loesche Bond, Einstellungen und WLAN");
    delay(300);
    NimBLEDevice::deleteAllBonds();
    WiFi.disconnect(true, true);  // auch die vom WLAN-Treiber selbst gemerkten Zugangsdaten löschen
    Preferences prefs;
    prefs.begin("bridge", false);
    prefs.clear();
    prefs.end();
    ESP.restart();
  }

  if (connectPending && !linkUp) connectToCandidate();

  if (releaseAll) {
    releaseAll = false;
    releaseEverything();
    startScan();
  }
  if (scanAt && (int32_t)(millis() - scanAt) >= 0) {
    scanAt = 0;
    startScan();
  }

  if (cfg.siriSetup && siriDownAt && millis() - siriDownAt > SIRI_HOLD_MS) {
    siriDownAt = 0;
    if (setupActive()) stopSetup(); else startSetup();
  }

  ButtonEvent ev;
  while (xQueueReceive(buttonQueue, &ev, 0) == pdTRUE) applyMask(ev);
  delay(1);
}
