# CLAUDE.md

Hinweise für Claude Code bei der Arbeit in diesem Repository.

> Die `AGENTS.md` im übergeordneten Ordner (`Code/`) beschreibt das Projekt **Doc2Date** und gilt **nicht** für dieses Repository.

## Projekt

Arduino-Sketch für eine Live-Abfahrtsanzeige des Schweizer ÖV:
**Arduino UNO R4 WiFi** + **SH1106 128×64 OLED (I²C)**, Daten von `transport.opendata.ch` per HTTPS.

Der gesamte Code liegt in einer einzigen Datei: `ArduinoTimeTableCH.ino`.

## Build & Upload

- Board: Arduino UNO R4 WiFi — FQBN `arduino:renesas_uno:unor4wifi`
- Bibliotheken: `U8g2` (Library Manager); `WiFiS3`, `WiFiSSLClient`, `Wire` kommen mit dem Board-Core
- Die `.ino` muss gleich heissen wie der Ordner (`ArduinoTimeTableCH`), sonst verweigern IDE und arduino-cli den Build. Bei Umbenennungen beides anpassen.

```bash
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi .
arduino-cli upload  --fqbn arduino:renesas_uno:unor4wifi -p <PORT> .
arduino-cli monitor -p <PORT> -c baudrate=115200
```

Es gibt keine automatisierten Tests. Änderungen mindestens kompilieren; Laufzeitverhalten kann nur auf echter Hardware geprüft werden – das dem Nutzer klar sagen statt Erfolg zu behaupten.

## Architektur (`ArduinoTimeTableCH.ino`)

| Bereich | Funktionen | Zweck |
| --- | --- | --- |
| Konfiguration | Konstanten oben | WLAN, `STATION`, `UPDATE_MS`, `LIMIT`, Scroll-Parameter |
| UTF-8 / JSON | `jsonDecodeUnicode`, `utf8Advance` | `\uXXXX` in-place zu UTF-8; Glyph-weises Vorspulen |
| OLED | `showOledStatus`, `showOledError`, `drawTimetableScreen` | Status-/Fehlerbildschirme, Fahrplananzeige mit Scrolling |
| WiFi | `connectWiFi`, `showConnectingScreen` | Verbinden mit Retry, Warten auf DHCP (rekursiver Neustart nach 10 s) |
| JSON-Helfer | `extractStr`, `extractIntC`, `isoToHHMM`, `parseEntry` | Minimal-Parser per `strstr`, kein ArduinoJson |
| HTTP | `fetchTimetable`, `httpSend` | Roher HTTP/1.1-Request über TLS, Streaming-Parser |
| App | `setup`, `loop` | Boot, Update alle `UPDATE_MS`, Reconnect, Redraw ~20 fps |

### Streaming-Parser (`fetchTimetable`)

- Phase 1: Byteweises Pattern-Matching auf `"name":"` (erstes Vorkommen = Stationsname) und `"stationboard":[`.
- Phase 2: Jeder Array-Eintrag `{...}` wird per Klammertiefe in `entryBuf[2500]` gesammelt und mit `parseEntry` ausgewertet. Ist der Puffer voll, wird der Anfang geparst und der Rest nur noch gezählt.
- Abbruch bei `]`, nach `LIMIT` Einträgen oder 12 s ohne Daten.
- Felder pro Eintrag: `category`+`number` → Linie, `to` → Ziel, aus `stop`: `departure`, `delay`, `platform`.

### Anzeige

- Font `u8g2_font_6x10_tf` (monospace, 6 px/Zeichen). Stationsname auf y=10, Trennlinie y=13, Zeilen auf y=24/35/46/57 → max. 4 Abfahrten.
- Zeilenformat: `%-4s%s%s %s` = Linie, Zeit, Verspätung (3 Zeichen, `+N` oder Leerzeichen), Ziel.
- Scrolling: `u8g2_uint_t` ist bei 128 px `uint8_t` → **keine negativen x-Koordinaten verwenden**. Stattdessen ganze Zeichen mit `utf8Advance` überspringen.
- `platform` wird geparst, aber derzeit nicht angezeigt.

## Konventionen

- Kommentare, Serial-Logs und Display-Texte sind **Deutsch**; auf dem Display Umlaute meist als `ae/oe/ue` geschrieben.
- Log-Präfixe: `[BOOT]`, `[WiFi]`, `[HTTP]`, `[JSON]`, `[Loop]`, `[ERR]`.
- Abschnitte mit `// ----- ... -----`-Bannern gliedern.
- RAM sparen: feste `char`-Puffer, `snprintf`/`strncpy` mit `sizeof`, große Puffer `static`. `String` nur für den Request-Aufbau. Keine zusätzlichen JSON-Bibliotheken ohne Rückfrage.
- Puffergrößen in `struct Departure` beachten (`line[8]`, `time[6]`, `platform[6]`, `dest[36]`, `stationName[22]`) – Änderungen an Darstellung oder Parser darauf prüfen.
- Hilfsfunktionen werden teils vor ihrer Definition benutzt; das funktioniert dank automatischer Prototypen der Arduino-IDE. Bei Umbenennung in `.cpp` wären Forward-Deklarationen nötig.

## Sicherheit

- `WIFI_SSID` / `WIFI_PASS` stehen im Klartext im Sketch. **Niemals echte Zugangsdaten committen** – im Repo bleiben die Platzhalter `"xxxx"`.
