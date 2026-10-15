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
| `firmware/siri_bridge/web/index.html` | Weboberfläche, eine Datei ohne externe Abhängigkeiten und ohne eigenen Text |
| `firmware/siri_bridge/web/lang/*.json` | Texte der Weboberfläche, je Sprache eine Datei (`de`, `en`) |
| `firmware/build.sh` | Baut, erzeugt `web_ui.h` aus HTML-Datei und Sprachdateien (`tools/web_bundle.py`), legt Images in `firmware/dist/` ab, flasht optional |
| `flasher/index.html` | Browser-Installer (ESP Web Tools), wird per GitHub Pages ausgeliefert |
| `.github/workflows/release.yml` | Baut bei jedem `v*`-Tag, hängt Images ans Release, veröffentlicht den Installer |
| `tools/` | `ui_dev.py` (Weboberfläche ohne ESP), `ui_snap.py` (Bilder davon ohne Bildschirm), `web_bundle.py`, `serial_log.py`, `hid_watch.py`, `esp_rescue.py` |
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
- Release: Tag `vX.Y.Z` pushen; der Workflow baut, legt das Release an und hängt die Images an (seit v0.6.1
  erprobt, ca. 4 min). Danach die Notizen mit `gh release edit vX.Y.Z --notes …` auf Deutsch ersetzen. Kein
  `gh release create` mehr von Hand.
- Installer (GitHub Pages, seit 2026-10-12 öffentlich): Der Workflow veröffentlicht ihn bei jedem Tag. Zwei
  Fallen: Die Umgebung `github-pages` muss Tags `v*` zulassen (ist eingerichtet), und Pages übernimmt eine
  Veröffentlichung nicht, wenn derselbe Commit schon einmal veröffentlicht wurde. Deshalb den Workflow nie von
  Hand auf einem Commit starten, der danach noch ein Tag bekommt – erst taggen. Die Seite wird bis zu
  10 Minuten gepuffert.
- Sprache: Kommentare, Log-Meldungen und README deutsch; Weboberfläche deutsch und englisch; `README.en.md` und `docs/PROTOCOL.md`
  englisch. Die README hat bewusst einen humorvollen Ton. Screenshots in `docs/` (englische in `docs/en/`) stammen aus `ui_snap.py`
  mit erfundenen Adressen.

## Sprachen der Weboberfläche

- Jeder sichtbare Text steht als Schlüssel in `web/lang/de.json` und `web/lang/en.json`; `index.html` selbst
  enthält keinen Text. Feste Stellen tragen `data-i18n` (Text), `data-i18n-html` (Text mit Auszeichnung) oder
  `data-i18n-title` (Tooltip), im Skript holt `t('schlüssel', {platzhalter})` den Text.
- Neuer Text = neuer Schlüssel in **beiden** Dateien. `tools/web_bundle.py` prüft beim Bauen fehlende,
  überzählige und unbekannte Schlüssel sowie abweichende Platzhalter; ohne zweite Angabe prüft es nur.
- Englisch ist Rückfallsprache (fehlender Schlüssel, unbekannte Browsersprache). Die Wahl liegt im Browser
  (`localStorage`), nicht auf dem Dongle. Der Umschalter im Kopf entsteht aus den vorhandenen Dateien.
- Die Firmware schickt der Seite keine Texte, nur Kennungen (z. B. `bad-ip` → Schlüssel `err.bad-ip`).
- Die Übersetzung des Browsers ist abgeschaltet (`translate="no"`), sie hat die Seite verfälscht. Kein
  natives Dateifeld verwenden: Dessen Beschriftung folgt der Browsersprache.
- Bilder zur Kontrolle und für `docs/`: `python3 tools/ui_snap.py <sprache> <reiter> <breite> <höhe> <ziel.png>`
  (braucht laufenden `ui_dev.py` und `chromium`).

## HTTP-API der Firmware (solange das WLAN an ist)

`GET /api/state` (Zustand + Einstellungen), `GET /api/live` (gedrückte Tasten, wird alle 150 ms
abgefragt), `GET /api/scan` (WLANs), `GET /api/builtin` (mitgelieferte Profile), `POST /api/config` (`active`, je Profil `n<i>` = Name und
`p<i>` = 13 Aktionen als `typ,mods,code;…` – der Webserver nimmt höchstens 32 Formularfelder an), `/api/defaults`, `/api/wifi` (leere SSID =
vergessen; `keep=1` behält das Passwort; `dhcp=0` mit `ip`, `mask`, `gw`, `dns` für eine feste Adresse), `/api/unpair`, `/api/setup-off`, `/api/reboot`, `/api/factory-reset`, `/api/ota`.
Ändert sich die API, auch den Beispieldaten-Server in `tools/ui_dev.py` nachziehen.

Anmeldung: Ist ein Passwort gesetzt (`"auth":true` in `/api/state`), antworten alle `/api`-Aufrufe ohne
Anmeldung mit `401 auth`; nur `/` und `POST /api/login` (`pass`) bleiben frei. Die Anmeldung setzt das Cookie
`sb` (SHA-256 aus Salz und Passwort, liegt so auch im NVS; das Passwort selbst wird nicht gespeichert).
`POST /api/password` (`pass`, leer = entfernen) setzt oder ändert es. Für `curl` geht HTTP-Basic mit
beliebigem Benutzernamen: `curl -u :passwort -F firmware=@… http://<ip>/api/ota`. Neue Routen immer über
`guard()` registrieren. Das Passwort gehört nicht in die Sicherung (JSON) und übersteht `/api/defaults`;
der Werksreset löscht es.

## Entscheidungen, die bewusst so sind

- **Kein Passwort ab Werk** für Hotspot und Weboberfläche. Für die Weboberfläche lässt sich eins setzen
  (nur Passwort, kein Benutzername – Wunsch des Projektinhabers); der Hotspot bleibt immer offen.
- **WLAN ab Werk dauerhaft an** (im Heimnetz); eine Abschaltzeit ist einstellbar, BOOT kurz oder Siri 5 s
  schalten um. Der eigene offene Hotspot geht dagegen immer nach 10 min ohne Zugriff aus.
- Die LED blinkt nur beim offenen Hotspot violett; im Heimnetz zeigt sie den Zustand der Remote.
  Dreimal Gelb quittiert das Umschalten des WLANs.
- BOOT-Taste, ausgewertet beim Loslassen: kurz = WLAN ein/aus, 3 s = entkoppeln, 10 s = Werkszustand.
- **Lizenz MIT-0**, so offen wie möglich.
- **Tastenwiederholung macht der Host**, nicht die Bridge.
- **Altlasten-Filter ab Werk 300 ms** (`staleMs`): Die Remote liefert beim Verbinden alles nach, was während
  der Trennung gedrückt wurde, auch den Druck, mit dem sie geweckt wurde. Im Fenster nach dem Verbinden zählt
  deshalb nur der letzte Druck (`flushStale()`), ältere werden verworfen. Pauschal verwerfen würde den
  Aufweck-Druck verschlucken. Grenze: Verbindet sich die Remote von selbst neu (Dongle war stromlos), geht
  der letzte alte Druck trotzdem einmal durch – die Remote liefert keine Zeitstempel. Am 2026-10-12 getestet:
  drei gepufferte Drücke, nur der letzte kam an.
- **Jede Einstellungskarte hat ihren eigenen Speichern-Knopf** und speichert nur ihre Felder; Entwürfe in
  anderen Karten bleiben erhalten. Rückmeldungen (Sicherung, Firmware-Update) stehen in der Karte selbst,
  nicht nur als Einblendung.
- Die Belegung wird als HID-Usage gespeichert, nicht als Zeichen.
- **Vier Belegungsprofile** (`config.h`, `builtinProfile()`): „Universal“ (Standard), „Kodi (CoreELEC)“,
  „Android / Fire TV“ (ungetestet), „Custom“ (Namen bewusst sprachneutral). „Universal“ übernimmt Menü (Tastatur `0x65`) und
  Zurück (Consumer `0x224`) von einem Flirc-Dongle des Projektinhabers, der an Fire TV und CoreELEC
  funktioniert (am 2026-10-11 mit `tools/key_scan.py` ausgelesen). Clickpad-Mitte sendet in „Universal“
  und „Android / Fire TV“ die Fernbedienungs-OK-Taste (Consumer `0x41` „Menu Pick“, Android: `DPAD_CENTER`)
  statt Enter: Mit Enter öffnet Android in einem Suchfeld keine Bildschirmtastatur. Am 2026-10-12 vom
  Projektinhaber an Fire TV (Suchfeld) und CoreELEC (auch langes OK) bestätigt. Die Siri-Taste sendet dort
  „Suche“ (Consumer `0x221`): öffnet in YouTube die Suche, auf dem Fire-TV-Startbildschirm nichts.
  „Sprachassistent“ (Consumer `0xCF`, Android: `VOICE_ASSIST`) bewirkt am Fire TV nichts; laut Amazon-Doku
  sendet die Mikrofontaste der Originalfernbedienung gar kein Tastenereignis. Die Vorgaben stehen doppelt: in der
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

## Herkunft und Lizenz

- Der Fire TV (Cube/Stick) war von Anfang an das eigentliche Ziel des Projektinhabers, Kodi die Zugabe.
  Das soll in der README vorn stehen.
- MIT-0 ist vertretbar, weil kein fremder Code übernommen wurde: Aus azais-corentin/siri-remote (GPL-3.0),
  Yanndroid/SiriRemote-Linux (ohne Lizenz, also alle Rechte vorbehalten) und zanneth/siriremote stammen nur
  Protokollwissen (Report-IDs, Tastenbits, Verbindungsparameter), am eigenen Gerät nachgemessen. **Niemals
  Code aus diesen Projekten kopieren**, auch keine Tabellen im Wortlaut. Die fertigen Images enthalten
  Arduino-ESP32 (LGPL-2.1) und NimBLE-Arduino (Apache-2.0); das steht im Lizenzabschnitt der README.
- Siri Remote 2. Generation (A2540): laut Apple/Wikipedia bis auf den Anschluss identisch, Yanndroid bedient
  beide mit demselben Branch. Ungetestet. `FINGERPRINT` in der Firmware enthält `15 03` – vermutlich die
  Produktkennung 0x0315 der 3. Generation; eine 2. würde damit beim Koppeln nicht erkannt. Erst mit einem
  echten Advertising einer 2. Generation ändern, nicht raten.

## To-do (Reihenfolge des Projektinhabers)

1. Touchpad: Format des Reports `0xFC` entschlüsseln, Wischen als Pfeiltasten. Das Timing muss stimmen.
   Stand 2026-10-12: Ein Finger = 11 Byte; ab Byte 4 X und Y als 12 Bit mit Vorzeichen (wie Magic Mouse:
   `x = b4 | (b5 & 0x0F) << 8`, `y = b5 >> 4 | b6 << 4`, Y nach oben positiv), Bereich je rund 1280 Einheiten,
   Mitte etwa (−285, −365); Zustand in den unteren 3 Bit von Byte 10 (3 aufgesetzt, 4 liegt auf, 5–7 hebt
   ab, 0 weg); zwei Finger = 18 Byte. Am ESP ein Report je 15 ms. Rohdaten: `archiv/2026-10-12-touch-esp.log`.
   Branch `touch-maus` enthält einen Prototyp hinter Bauschaltern (`-DTOUCH_LOG`, `-DTOUCH_MOUSE`: Touchfläche
   als USB-Maus, Siri 0,7 s halten schaltet um). Vom Projektinhaber vertagt, er überlegt noch, was davon
   sinnvoll ist; am PC ungetestet. Wunsch: Siri lang = Mausmodus, dann braucht das WLAN-Umschalten eine
   andere Taste.
2. „Android / Fire TV“-Profil testen und nachschärfen. („Universal“ ist vom Projektinhaber am 2026-10-11 an
   CoreELEC-Box und Fire TV bestätigt: funktioniert einwandfrei.)
3. Langer Tastendruck als zweite Belegung.
4. Weitere Sprachen für die Weboberfläche (Deutsch und Englisch sind seit 2026-10-12 da). Nur Sprachen
   ausliefern, die jemand gegenlesen kann.
5. ✅ Ein-Klick-Installer (GitHub Pages): am 2026-10-15 vom Projektinhaber mit v0.6.3 bestanden – Flash
   durchgelaufen, Hotspot erreichbar, nach der WLAN-Einrichtung einwandfreie Funktion. Screenshots in
   `docs/installer.png`. Ebenso bestanden (2026-10-12): händischer Erstflash mit esptool-js und Werksreset
   über BOOT 10 s; die Stolperstellen stehen in der README (`0x0` statt `0x1000`, BOOT + RESET, 115200 Baud).
6. Handy-Ansicht der Weboberfläche gezielt durchsehen.
7. Infrarot senden als dritte Belegungsart neben Tastatur und Multimedia (z. B. TV-Lautstärke), braucht
   eine IR-Sendediode am Dongle mit Sicht zum Fernseher. Hintergrund: Die Remote sendet Lautstärke und Mute
   selbst per IR (am Apple TV gelernt); das blieb nach dem Koppeln mit dem Dongle meist erhalten, einmal aber
   nicht. In README und Weboberfläche deshalb nur ein vorsichtiger Hinweis, keine Zusage.
8. Infrarot empfangen: den Dongle zusätzlich als „besseren Flirc“ nutzen (IR-Fernbedienung → USB-Tastatur,
   über dieselben Profile). Idee des Projektinhabers, zurückgestellt.
9. Weitere Boards unterstützen. Wunsch des Projektinhabers: möglichst kompakter Stick mit USB-A, später mit
   Display und IR-Sender. Kandidaten (2026-10-11 recherchiert, nichts davon getestet): LilyGO T-Dongle-S3
   (0,96″-Display ST7735, APA102-LED, BOOT-Taste) und T-Dongle-S3 Plus (zusätzlich IR-Sender), M5Stack
   AtomS3U (WS2812 an GPIO35, Taste GPIO41, IR-Sender GPIO12, kein Display), Waveshare ESP32-S3-GEEK
   (1,14″-Display), Waveshare ESP32-S3-LCD-1.47 (1,47″-Display ST7789, USB-A – erstes Zielboard, beim
   Projektinhaber bestellt), Waveshare ESP32-S3-Zero (USB-C, gleicher Chip wie der Super Mini). Im Auge
   behalten: ZeroTrace Dongle (zerotrace.one, nackter USB-A-Stick, 16 MB Flash, RGB-LED; gefällt dem
   Projektinhaber, ist ihm mit rund 35–40 € aber derzeit zu teuer; Pinbelegung undokumentiert). Nötig dafür: Taster-Pin einstellbar (bisher GPIO0 fest), LED-Art APA102, Display-Treiber
   mit Statusanzeige (Zustand, Akku, Profil, IP), je Board eine Build-Variante samt Installer-Eintrag.
