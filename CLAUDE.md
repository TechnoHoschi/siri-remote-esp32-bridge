# Siri Remote ESP32 Bridge

Ein ESP32-S3 koppelt die Apple **Siri Remote (3. Gen, A2854, USB-C)** per BLE und gibt sich am
Mediaplayer als USB-Tastatur aus. Zielgeräte: Kodi auf CoreELEC/LibreELEC, später Fire TV.

Dieses Repo ist öffentlich. **Keine persönlichen Daten committen:** keine Bluetooth- oder MAC-Adressen,
IP-Adressen, WLAN-Namen, Hostnamen, Klarnamen, Seriennummern, Passwörter oder Schlüssel – auch nicht in
Beispielen, Screenshots, Logs oder Commit-Nachrichten. Angaben zur eigenen Entwicklungsumgebung gehören
in `CLAUDE.local.md` (steht in der `.gitignore`).

## Aufbau

| Wo | Was |
|---|---|
| `firmware/siri_bridge/siri_bridge.ino` | Firmware: BLE-Central (NimBLE), USB-HID, Setup-Modus mit WLAN und Webserver, OTA |
| `firmware/siri_bridge/config.h` | Einstellungen (Belegung, LED, Timing) als ein Blob im NVS |
| `firmware/siri_bridge/web/index.html` | Weboberfläche, eine Datei ohne externe Abhängigkeiten |
| `firmware/build.sh` | Baut, erzeugt `web_ui.h` aus der HTML-Datei, legt Images in `firmware/dist/` ab, flasht optional |
| `flasher/index.html` | Browser-Installer (ESP Web Tools), wird per GitHub Pages ausgeliefert |
| `.github/workflows/release.yml` | Baut bei jedem `v*`-Tag, hängt Images ans Release, veröffentlicht den Installer |
| `tools/` | `ui_dev.py` (Weboberfläche ohne ESP), `serial_log.py`, `hid_watch.py`, `esp_rescue.py` |
| `tools/linux/` | Remote direkt unter Linux mitlesen (BlueZ + bleak), aus der Erkundungsphase |
| `docs/PROTOCOL.md` | Protokoll-Erkenntnisse mit Messwerten (englisch) – bei neuen Erkenntnissen dort nachtragen |

## Arbeitsweise

- Bauen und Flashen: `firmware/build.sh siri_bridge [upload]`. Toolchain: `arduino-cli`, ESP32-Core
  3.3.12, NimBLE-Arduino 2.5.1. Board-Optionen (TinyUSB, 4 MB Flash, `min_spiffs`, PSRAM) stehen im Skript.
- Weboberfläche: `web/index.html` bearbeiten, mit `python3 tools/ui_dev.py` ansehen (Port 8766,
  Beispieldaten, lädt bei Änderungen neu; `--esp http://<ip>` reicht an einen echten Dongle weiter).
  `web_ui.h` nie von Hand ändern.
- Ein Serial-Logger darf beim Flashen nicht am Port hängen (`build.sh` beendet `serial_log.py`), und es
  darf immer nur einer laufen – zwei teilen sich die Ausgabe zeichenweise.
- Updates am laufenden Dongle per OTA: `curl -F firmware=@firmware/dist/…-ota.bin http://<ip>/api/ota`.
- Release: Tag `vX.Y.Z` pushen, den Rest erledigt der Workflow.
- Sprache: Oberfläche, Kommentare, Log-Meldungen und README deutsch; `README.en.md` und `docs/PROTOCOL.md`
  englisch. Die README hat bewusst einen humorvollen Ton. Screenshots in `docs/` stammen aus `ui_dev.py`
  mit erfundenen Adressen.

## HTTP-API der Firmware (solange das WLAN an ist)

`GET /api/state` (Zustand + Einstellungen), `GET /api/live` (gedrückte Tasten, wird alle 150 ms
abgefragt), `GET /api/scan` (WLANs), `GET /api/builtin` (mitgelieferte Profile), `POST /api/config` (`active`, je Profil `n<i>` = Name und
`p<i>` = 13 Aktionen als `typ,mods,code;…` – der Webserver nimmt höchstens 32 Formularfelder an), `/api/defaults`, `/api/wifi` (leere SSID =
vergessen; `keep=1` behält das Passwort; `dhcp=0` mit `ip`, `mask`, `gw`, `dns` für eine feste Adresse), `/api/unpair`, `/api/setup-off`, `/api/reboot`, `/api/factory-reset`, `/api/ota`.
Ändert sich die API, auch den Beispieldaten-Server in `tools/ui_dev.py` nachziehen.

## Entscheidungen, die bewusst so sind

- **Kein Passwort** für Hotspot und Weboberfläche.
- **WLAN ab Werk dauerhaft an** (im Heimnetz); eine Abschaltzeit ist einstellbar, BOOT kurz oder Siri 5 s
  schalten um. Der eigene offene Hotspot geht dagegen immer nach 10 min ohne Zugriff aus.
- Die LED blinkt nur beim offenen Hotspot violett; im Heimnetz zeigt sie den Zustand der Remote.
  Dreimal Gelb quittiert das Umschalten des WLANs.
- BOOT-Taste, ausgewertet beim Loslassen: kurz = WLAN ein/aus, 3 s = entkoppeln, 10 s = Werkszustand.
- **Lizenz MIT-0**, so offen wie möglich.
- **Tastenwiederholung macht der Host**, nicht die Bridge.
- **Jede Einstellungskarte hat ihren eigenen Speichern-Knopf** und speichert nur ihre Felder; Entwürfe in
  anderen Karten bleiben erhalten. Rückmeldungen (Sicherung, Firmware-Update) stehen in der Karte selbst,
  nicht nur als Einblendung.
- Die Belegung wird als HID-Usage gespeichert, nicht als Zeichen.
- **Vier Belegungsprofile** (`config.h`, `builtinProfile()`): „Universal“ (Standard), „Kodi (CoreELEC)“,
  „Android / Fire TV“ (ungetestet), „Eigenes Profil“. „Universal“ übernimmt Menü (Tastatur `0x65`) und
  Zurück (Consumer `0x224`) von einem Flirc-Dongle des Projektinhabers, der an Fire TV und CoreELEC
  funktioniert (am 2026-10-11 mit `tools/key_scan.py` ausgelesen). Die Vorgaben stehen doppelt: in der
  Firmware und in `tools/ui_dev.py` – beide zusammen ändern.
- Ein Wechsel des Einstellungsformats (`CONFIG_VERSION`) setzt die Einstellungen auf Vorgabe zurück;
  Kopplung und WLAN bleiben.

## Stolperfallen

- Vor `NimBLEDevice::init()` darf nichts den Bond-Speicher abfragen (`getNumBonds()`), sonst stürzt die
  Firmware beim Start ab und der USB-Port meldet sich nicht mehr an. Rettung: `tools/esp_rescue.py`.
- Nie das merged-Image flashen, wenn Kopplung und Einstellungen erhalten bleiben sollen.
- Die gebondete Remote meldet sich nach dem Aufwachen anonym; Wiedererkennen nur über die aufgelöste
  Adresse (siehe `docs/PROTOCOL.md`).
- Der Rollback-Schutz bestätigt neue Firmware erst nach 15 s Laufzeit. Testen mit
  `EXTRA_FLAGS=-DCRASH_TEST firmware/build.sh siri_bridge`.
- BLE-Scan und WLAN teilen sich das Funkmodul: Ein Scan über die volle Zeit (ohne Bond läuft er dauernd)
  macht den Hotspot unbenutzbar. Mit aktivem WLAN deshalb nur 30 von 150 ms scannen (`startScan()`).
- Kein `node` auf dem Pi: JavaScript der Weboberfläche mit `esprima` aus dem venv auf Syntax prüfen.
- Browser-Dialoge (`confirm`, `alert`) in der Weboberfläche vermeiden; heikle Knöpfe bestätigen im Knopf.

## To-do (Reihenfolge des Projektinhabers)

1. Touchpad: Format des Reports `0xFC` entschlüsseln, Wischen als Pfeiltasten. Das Timing muss stimmen.
2. „Android / Fire TV“-Profil testen und nachschärfen. („Universal“ ist vom Projektinhaber am 2026-10-11 an
   CoreELEC-Box und Fire TV bestätigt: funktioniert einwandfrei.)
3. Langer Tastendruck als zweite Belegung.
4. Sprache der Weboberfläche umschaltbar (Deutsch/Englisch).
5. Erstflash im Browser testen: mit esptool-js und dem merged-Image, nach dem Veröffentlichen auch den
   Ein-Klick-Installer (GitHub Pages). Vom Projektinhaber am 2026-10-11 zurückgestellt.
6. Handy-Ansicht der Weboberfläche gezielt durchsehen.
7. Infrarot senden als dritte Belegungsart neben Tastatur und Multimedia (z. B. TV-Lautstärke), braucht
   eine IR-Sendediode am Dongle mit Sicht zum Fernseher. Hintergrund: Die Remote sendet Lautstärke und Mute
   selbst per IR (am Apple TV gelernt); das blieb nach dem Koppeln mit dem Dongle meist erhalten, einmal aber
   nicht. In README und Weboberfläche deshalb nur ein vorsichtiger Hinweis, keine Zusage.
8. Infrarot empfangen: den Dongle zusätzlich als „besseren Flirc“ nutzen (IR-Fernbedienung → USB-Tastatur,
   über dieselben Profile). Idee des Projektinhabers, zurückgestellt.
