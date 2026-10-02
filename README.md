# ArduinoTimeTableCH 🚆

Live-Abfahrtsanzeige für Schweizer ÖV-Haltestellen auf einem **Arduino UNO R4 WiFi** mit **1,3" OLED-Display (SH1106, 128×64)**.

Der Sketch holt jede Minute die nächsten Abfahrten einer Haltestelle über die offene [transport.opendata.ch](https://transport.opendata.ch/)-API (HTTPS) und zeigt sie auf dem Display an, inklusive Verspätung. Zu lange Zeilen scrollen automatisch.

```
Bern, Wyleregg
──────────────────────
B20 14:32+2 Bern, Bahnhof
B20 14:36   Bern, Wankdorf
B20 14:40   Bern, Bahnhof
B20 14:44   Bern, Wankdorf
```

*(Beispiel – Zeilen, die breiter als das Display sind, laufen als Laufschrift durch.)*

## Features

- Abfahrten von jeder Haltestelle in der Schweiz (Bahn, Tram, Bus, Schiff …)
- Linie, Abfahrtszeit, Verspätung (`+N` Minuten) und Ziel
- Automatisches Laufschrift-Scrolling für lange Zielnamen
- Umlaute und Sonderzeichen (UTF-8) werden korrekt dargestellt
- Speicherschonender Streaming-JSON-Parser – keine JSON-Bibliothek, kein Gesamt-Buffer
- Automatischer WLAN-Reconnect und Fehlermeldungen direkt auf dem Display
- Ausführliches Logging über den Serial Monitor (115200 Baud)

## Hardware

| Bauteil | Hinweis |
| --- | --- |
| Arduino UNO R4 WiFi | Das WLAN-Modul (ESP32-S3) wird über `WiFiS3` angesprochen |
| OLED 1,3" SH1106, 128×64, I²C | Typische Adresse `0x3C` |
| 4 Jumper-Kabel | |

### Verdrahtung

| OLED | UNO R4 WiFi |
| --- | --- |
| VCC | 3.3V oder 5V (je nach Modul) |
| GND | GND |
| SDA | SDA (bzw. A4) |
| SCL | SCL (bzw. A5) |

> **Hinweis:** Der Sketch verwendet den Standard-I²C-Bus (`Wire`). Der Qwiic-Anschluss des UNO R4 WiFi liegt auf `Wire1` und wird daher **nicht** genutzt.

## Software-Voraussetzungen

- [Arduino IDE 2.x](https://www.arduino.cc/en/software) oder [arduino-cli](https://arduino.github.io/arduino-cli/)
- Board-Paket **Arduino UNO R4 Boards** (`arduino:renesas_uno`) – enthält `WiFiS3` und `WiFiSSLClient`
- Bibliothek **U8g2** von olikraus (über den Bibliotheksverwalter)

## Installation

1. Repository klonen:
   ```bash
   git clone https://github.com/<user>/ArduinoTimeTableCH.git
   ```
2. In `ArduinoTimeTableCH.ino` WLAN-Zugangsdaten und Haltestelle eintragen:
   ```cpp
   const char WIFI_SSID[] = "MeinWLAN";
   const char WIFI_PASS[] = "geheim";

   const char STATION[]   = "Bern, Wyleregg";
   ```
3. Board **Arduino UNO R4 WiFi** wählen, Port auswählen und hochladen.

Mit `arduino-cli`:

```bash
arduino-cli core install arduino:renesas_uno
arduino-cli lib install U8g2
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi ArduinoTimeTableCH
arduino-cli upload  --fqbn arduino:renesas_uno:unor4wifi -p COM3 ArduinoTimeTableCH
```

> ⚠️ **Keine echten WLAN-Zugangsdaten committen!** Vor einem `git push` die Werte wieder durch Platzhalter ersetzen.

## Konfiguration

Alle Einstellungen befinden sich am Anfang von `ArduinoTimeTableCH.ino`:

| Konstante | Standard | Bedeutung |
| --- | --- | --- |
| `STATION` | `"Bern, Wyleregg"` | Haltestellenname wie in der SBB-/ÖV-Suche |
| `UPDATE_MS` | `60000` | Aktualisierungsintervall in ms |
| `LIMIT` | `4` | Anzahl angezeigter Abfahrten (Display fasst max. 4) |
| `SCROLL_PAUSE_MS` | `2000` | Pause vor dem Scrollen einer langen Zeile |
| `SCROLL_PX_PER_S` | `30` | Scrollgeschwindigkeit in Pixel pro Sekunde |

Den exakten Haltestellennamen findet man z. B. über
`https://transport.opendata.ch/v1/locations?query=Wyleregg`.

## Funktionsweise

1. **Boot:** Display initialisieren, mit dem WLAN verbinden und auf eine DHCP-Adresse warten.
2. **Abruf:** HTTPS-Request an `GET /v1/stationboard?station=<STATION>&limit=<LIMIT>`.
3. **Parsing:** Die Antwort wird byteweise gelesen. Zuerst werden Stationsname und der Beginn des `stationboard`-Arrays gesucht, danach wird jeder Eintrag einzeln in einen 2,5-kB-Puffer gelesen und daraus Linie, Zeit, Verspätung, Gleis und Ziel extrahiert.
4. **Anzeige:** Ca. 20-mal pro Sekunde wird der Bildschirm neu gezeichnet; Zeilen breiter als 128 px scrollen zeichenweise.
5. **Loop:** Jede Minute neu laden; bei WLAN-Verlust automatisch neu verbinden.

## Fehlerbehebung

| Anzeige / Log | Ursache | Lösung |
| --- | --- | --- |
| `WiFi hardware – kein Modul` | WLAN-Modul nicht erkannt | Board-Auswahl und Firmware des UNO R4 WiFi prüfen |
| `Verbinde WLAN...` hört nicht auf | Falsche Zugangsdaten / kein 2,4-GHz-Netz | SSID/Passwort prüfen, 2,4-GHz-Band verwenden |
| `TLS connect` | Server nicht erreichbar | Internetverbindung prüfen; ggf. WLAN-Firmware aktualisieren |
| `JSON – stationboard fehlt` | Unbekannte Haltestelle oder API-Fehler | `STATION` über die `locations`-API verifizieren |
| `Keine Abfahrten` | Keine Abfahrten im Zeitfenster (z. B. nachts) | – |

Details zu jedem Schritt erscheinen im Serial Monitor (115200 Baud).

## Datenquelle

Fahrplandaten: [transport.opendata.ch](https://transport.opendata.ch/) – Swiss public transport API von Opendata.ch. Bitte die API fair nutzen; das Standard-Intervall von 60 Sekunden ist dafür ausgelegt.
