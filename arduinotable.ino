#include <Wire.h>
#include <WiFiS3.h>
#include <WiFiSSLClient.h>
#include <U8g2lib.h>

WiFiSSLClient client;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// =====================================================
// WLAN + STATION CONFIG
// =====================================================
const char WIFI_SSID[] = "xxxx";
const char WIFI_PASS[] = "xxxx";

const char STATION[]   = "Bern, Wyleregg";

// =====================================================
// SETTINGS
// =====================================================
const char HOST[]              = "transport.opendata.ch";
const int  HTTPS_PORT          = 443;
const unsigned long UPDATE_MS        = 60000UL;  // 1 Minute
const uint8_t       LIMIT            = 4;        // Anzahl Abfahrten
const unsigned long SCROLL_PAUSE_MS  = 2000UL;   // Pause vor Scroll
const int           SCROLL_PX_PER_S  = 30;       // Scrollgeschwindigkeit px/s

int wifiStatus            = WL_IDLE_STATUS;
unsigned long lastUpdate  = 0;

struct Departure {
  char line[8];      // Linie, z.B. "S3", "IC1", "Bus"
  char time[6];      // "HH:MM"
  int8_t delay;      // Verspaetung in Minuten, -1 = unbekannt
  char platform[6];  // Gleis, z.B. "3", "14"
  char dest[36];     // Ziel
};

static Departure deps[LIMIT];
static uint8_t   depCount    = 0;
static char stationName[22] = "";


struct ScrollState {
  int           offset;      // Pixel nach links verschoben
  unsigned long phaseStart;  // millis() beim Start der aktuellen Phase
  bool          scrolling;   // false = Pause, true = Scrollt
};
static ScrollState scrollState[LIMIT];

void resetScrollStates() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < LIMIT; i++) {
    scrollState[i] = { 0, now, false };
  }
}

// -----------------------------------------------------
// UTF-8 / JSON helper
// -----------------------------------------------------

// Dekodiert JSON-Unicode-Escapes (\uXXXX) in-place zu UTF-8.
// \u00e4 (ä) → 0xC3 0xA4 usw.  Ausgabe ist immer <= Eingabe (6 Bytes → 2-3 Bytes).
void jsonDecodeUnicode(char *s) {
  char *src = s, *dst = s;
  while (*src) {
    if (src[0] == '\\' && src[1] == 'u' &&
        isxdigit(src[2]) && isxdigit(src[3]) &&
        isxdigit(src[4]) && isxdigit(src[5])) {
      uint16_t cp = 0;
      for (int i = 2; i < 6; i++) {
        cp <<= 4;
        char h = src[i];
        if      (h >= '0' && h <= '9') cp |= h - '0';
        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
        else                           cp |= h - 'A' + 10;
      }
      if      (cp < 0x0080) { *dst++ = (char)cp; }
      else if (cp < 0x0800) { *dst++ = (char)(0xC0 | (cp >> 6));
                               *dst++ = (char)(0x80 | (cp & 0x3F)); }
      else                  { *dst++ = (char)(0xE0 | (cp >> 12));
                               *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                               *dst++ = (char)(0x80 | (cp & 0x3F)); }
      src += 6;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

// Advances pointer p by n glyphs (not bytes), safe for multi-byte sequences.
const char* utf8Advance(const char* p, int n) {
  while (n > 0 && *p) {
    if      ((*p & 0xF8) == 0xF0) p += 4;
    else if ((*p & 0xF0) == 0xE0) p += 3;
    else if ((*p & 0xE0) == 0xC0) p += 2;
    else                           p += 1;
    n--;
  }
  return p;
}

// -----------------------------------------------------
// OLED helper
// -----------------------------------------------------
void showOledStatus(const char* line1, const char* line2 = "",
                    const char* line3 = "", const char* line4 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(0, 12, line1);
  u8g2.drawUTF8(0, 26, line2);
  u8g2.drawUTF8(0, 40, line3);
  u8g2.drawUTF8(0, 54, line4);
  u8g2.sendBuffer();
}

void showOledError(const char* where, const char* detail = "", const char* hint = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(0, 12, "Fahrplan-Fehler");
  u8g2.drawUTF8(0, 26, where);
  u8g2.drawUTF8(0, 40, detail);
  u8g2.drawUTF8(0, 54, hint);
  u8g2.sendBuffer();
  Serial.print("[ERR] "); Serial.print(where);
  if (detail[0]) { Serial.print(" | "); Serial.print(detail); }
  if (hint[0])   { Serial.print(" | "); Serial.print(hint); }
  Serial.println();
}

void drawTimetableScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // Stationsname oben (y=10)
  u8g2.drawUTF8(0, 10, stationName);
  u8g2.drawHLine(0, 13, 128);

  if (depCount == 0) {
    u8g2.drawUTF8(0, 30, "Keine Abfahrten");
    u8g2.sendBuffer();
    return;
  }

  // Bis zu 4 Abfahrtszeilen: y = 24, 35, 46, 57
  const uint8_t rowY[4] = { 24, 35, 46, 57 };
  unsigned long now = millis();

  for (uint8_t i = 0; i < depCount && i < 4; i++) {
    char delayStr[4];
    if (deps[i].delay <= 0) {
      strncpy(delayStr, "   ", sizeof(delayStr));
    } else {
      snprintf(delayStr, sizeof(delayStr), "+%d", deps[i].delay);
      uint8_t l = strlen(delayStr);
      while (l < 3) delayStr[l++] = ' ';
      delayStr[3] = '\0';
    }

    // Voller Zeilentext ohne Kuerzen des Ziels
    char row[56];
    snprintf(row, sizeof(row), "%-4s%s%s %s",
             deps[i].line, deps[i].time, delayStr, deps[i].dest);

    int textW = (int)u8g2.getUTF8Width(row);

    if (textW > 128) {
      // u8g2_uint_t ist uint8_t fuer 128px-Displays: negative x-Werte wuerden
      // zu grossen positiven Zahlen wrappen und den Text unsichtbar machen.
      // Stattdessen: ganze Zeichen ueberspringen (Font ist monospace, 6 px/Zeichen).
      static const int CHAR_W = 6;
      ScrollState &ss = scrollState[i];
      if (!ss.scrolling) {
        // Pause-Phase: 2 Sekunden statisch
        if (now - ss.phaseStart >= SCROLL_PAUSE_MS) {
          ss.scrolling  = true;
          ss.phaseStart = now;
          ss.offset     = 0;
        }
        u8g2.drawUTF8(0, rowY[i], row);
      } else {
        // Scroll-Phase: Zeichen ueberspringen statt negativer x-Position
        ss.offset = (int)((now - ss.phaseStart) * (unsigned long)SCROLL_PX_PER_S / 1000UL);
        int maxOff = textW - 128;
        if (ss.offset >= maxOff) {
          // Ende erreicht: zurueck zur Pause
          ss.scrolling  = false;
          ss.phaseStart = now;
          ss.offset     = 0;
          u8g2.drawUTF8(0, rowY[i], row);
        } else {
          u8g2.drawUTF8(0, rowY[i], utf8Advance(row, ss.offset / CHAR_W));
        }
      }
    } else {
      u8g2.drawUTF8(0, rowY[i], row);
    }
  }

  u8g2.sendBuffer();
}

// -----------------------------------------------------
// WiFi
// -----------------------------------------------------
void showConnectingScreen() {
  static int dots = 0;
  char line2[24] = "Bitte warten";

  dots = (dots + 1) % 4;
  for (int i = 0; i < dots; i++) line2[13 + i] = '.';
  line2[13 + dots] = '\0';

  showOledStatus("Verbinde WLAN...", line2);
}

void connectWiFi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("[WiFi] FEHLER: kein WiFi-Modul gefunden");
    while (true) {
      showOledError("WiFi hardware", "kein Modul", "Board/Firmware pruefen");
      delay(500);
    }
  }

  Serial.print("[WiFi] Verbinde mit SSID: ");
  Serial.println(WIFI_SSID);

  while (wifiStatus != WL_CONNECTED) {
    wifiStatus = WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (millis() - start < 10000 && wifiStatus != WL_CONNECTED) {
      showConnectingScreen();
      Serial.print(".");
      delay(200);
      wifiStatus = WiFi.status();
    }
    Serial.println();

    if (wifiStatus != WL_CONNECTED) {
      Serial.print("[WiFi] Verbindung fehlgeschlagen (status=");
      Serial.print(wifiStatus);
      Serial.println("), neuer Versuch...");
    }
  }

  // WL_CONNECTED kommt vor DHCP – warten bis eine gueltige IP vorliegt
  Serial.print("[WiFi] Warte auf DHCP");
  unsigned long dhcpStart = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - dhcpStart > 10000) {
      Serial.println(" TIMEOUT");
      Serial.println("[WiFi] WARNUNG: Kein DHCP nach 10s, starte WiFi neu");
      wifiStatus = WL_IDLE_STATUS;
      WiFi.disconnect();
      delay(1000);
      connectWiFi();  // rekursiver Neuversuch
      return;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println(" OK");

  Serial.print("[WiFi] Verbunden. IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[WiFi] Signal (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  showOledStatus("WLAN verbunden", "Hole Fahrplan...");
  delay(500);
}

// -----------------------------------------------------
// HTTP + JSON helper
// -----------------------------------------------------

// Sendet eine Zeile an den HTTP-Client und gibt sie auf Serial aus.
void httpSend(const String& line) {
  client.println(line);
  Serial.print("  > ");
  Serial.println(line);
}

// Parst einen einzelnen Stationboard-Eintrag aus einem null-terminierten char*.
// Schreibt in deps[depCount], ohne depCount zu inkrementieren.
// Gibt true zurück wenn ein gültiger Eintrag gefunden wurde.
bool parseEntry(const char *entry) {
  Departure &dep = deps[depCount];
  memset(&dep, 0, sizeof(dep));
  strncpy(dep.time, "??:??", sizeof(dep.time) - 1);
  dep.delay = -1;

  char cat[8] = "";
  char num[8] = "";
  extractStr(entry, "\"category\"", cat, sizeof(cat));
  extractStr(entry, "\"number\"",   num, sizeof(num));
  snprintf(dep.line, sizeof(dep.line), "%.3s%.3s", cat, num);
  extractStr(entry, "\"to\"", dep.dest, sizeof(dep.dest));
  jsonDecodeUnicode(dep.dest);

  const char *stopPtr = strstr(entry, "\"stop\"");
  if (stopPtr) {
    char isoTime[32] = "";
    if (extractStr(stopPtr, "\"departure\"", isoTime, sizeof(isoTime))) {
      isoToHHMM(isoTime, dep.time, sizeof(dep.time));
    }
    int delayVal = 0;
    if (extractIntC(stopPtr, "\"delay\"", delayVal)) {
      dep.delay = (int8_t)delayVal;
    }
    extractStr(stopPtr, "\"platform\"", dep.platform, sizeof(dep.platform));
  } else {
    Serial.print("[JSON] WARNUNG: stop fuer Eintrag ");
    Serial.println(depCount);
  }

  if (dep.line[0] != '\0' && dep.dest[0] != '\0') {
    Serial.print("[JSON] ["); Serial.print(depCount);
    Serial.print("] Linie=");  Serial.print(dep.line);
    Serial.print(" Zeit=");    Serial.print(dep.time);
    Serial.print(" Delay=");   Serial.print(dep.delay);
    Serial.print(" Gleis=");   Serial.print(dep.platform);
    Serial.print(" Ziel=");    Serial.println(dep.dest);
    return true;
  }
  return false;
}









// Sucht key ab json, schreibt Wert nach out. Gibt Zeiger hinter schliessende
// Anführungszeichen zurück, oder NULL bei Fehler.
const char* extractStr(const char *json, const char *key,
                       char *out, size_t outSize) {
  const char *p = strstr(json, key);
  if (!p) return NULL;

  const char *colon = strchr(p + strlen(key), ':');
  if (!colon) return NULL;
  colon++;
  while (*colon == ' ') colon++;
  if (*colon != '"') return NULL;
  colon++;  // öffnendes " überspringen

  const char *end = strchr(colon, '"');
  if (!end) return NULL;

  size_t len = (size_t)(end - colon);
  if (len >= outSize) len = outSize - 1;
  memcpy(out, colon, len);
  out[len] = '\0';
  return end + 1;
}

// Sucht key ab json, schreibt Integer-Wert nach value.
// Gibt false zurück wenn Wert null oder nicht gefunden.
bool extractIntC(const char *json, const char *key, int &value) {
  const char *p = strstr(json, key);
  if (!p) return false;

  const char *colon = strchr(p + strlen(key), ':');
  if (!colon) return false;
  colon++;
  while (*colon == ' ') colon++;

  if (!*colon || *colon == 'n') return false;  // null

  bool neg = (*colon == '-');
  if (neg) colon++;
  if (!isdigit(*colon)) return false;

  value = 0;
  while (isdigit(*colon)) value = value * 10 + (*colon++ - '0');
  if (neg) value = -value;
  return true;
}

// Extracts "HH:MM" from an ISO-8601 string like "2024-03-18T14:32:00+0100"
void isoToHHMM(const char *iso, char *out, size_t outSize) {
  if (!iso || strlen(iso) < 16) {
    strncpy(out, "??:??", outSize);
    return;
  }
  snprintf(out, outSize, "%.5s", iso + 11);
}

// -----------------------------------------------------
// Fahrplan holen
// -----------------------------------------------------
bool fetchTimetable() {
  client.stop();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] FEHLER: WiFi nicht verbunden");
    showOledError("WiFi status", "nicht verbunden", "WLAN pruefen");
    return false;
  }

  Serial.print("[HTTP] Verbinde mit ");
  Serial.print(HOST); Serial.print(":"); Serial.println(HTTPS_PORT);

  if (!client.connect(HOST, HTTPS_PORT)) {
    Serial.println("[HTTP] FEHLER: TLS-Verbindung fehlgeschlagen");
    showOledError("TLS connect", HOST, "Server nicht erreicht");
    return false;
  }
  Serial.println("[HTTP] TLS-Verbindung hergestellt");

  String url = "/v1/stationboard?station=";
  for (size_t i = 0; i < strlen(STATION); i++) {
    url += (STATION[i] == ' ') ? "%20" : String(STATION[i]);
  }
  url += "&limit="; url += LIMIT;

  Serial.println("[HTTP] Sende Anfrage:");
  httpSend("GET " + url + " HTTP/1.1");
  httpSend("Host: " + String(HOST));
  httpSend("User-Agent: Arduino-UNO-R4-Fahrplan");
  httpSend("Accept: application/json");
  httpSend("Accept-Encoding: identity");
  httpSend("Connection: close");
  client.println();
  Serial.println("  > (Leerzeile)");

  // ===== Streaming-Parser =====
  // Liest die Antwort byte-weise. Kein Gesamt-Buffer nötig –
  // Stationsname und Einträge werden on-the-fly extrahiert.
  depCount = 0;
  stationName[0] = '\0';

  static const char PAT_SB[]   = "\"stationboard\":[";  // Marker stationboard-Array
  static const char PAT_NAME[] = "\"name\":\"";          // Marker Stationsname

  int  sbIdx     = 0;   // Fortschritt im PAT_SB-Muster
  int  nameIdx   = 0;   // Fortschritt im PAT_NAME-Muster
  bool capName   = false;
  bool sbFound   = false;

  char nameBuf[24] = {};
  int  nameLen     = 0;

  // Eintrags-Puffer: hält jeweils einen stationboard-Eintrag
  static char entryBuf[2500];
  int  entryLen    = 0;
  int  braceDepth  = 0;
  bool inEntry     = false;
  bool entryParsed = false;  // true: Eintrag wurde bereits aus gekürztem Puffer geparst
  bool entryFull   = false;  // true: Puffer war voll, Rest wird nur gezählt

  unsigned long timeout = millis();
  bool done = false;

  while (!done && (client.connected() || client.available())) {
    while (client.available()) {
      char c = (char)client.read();
      timeout = millis();

      if (!sbFound) {
        // --- Phase 1: Stationsname und "stationboard":[ suchen ---

        // "name":"..." erkennen (erstes Vorkommen = Stationsname)
        if (!capName) {
          nameIdx = (c == PAT_NAME[nameIdx]) ? nameIdx + 1 : (c == PAT_NAME[0] ? 1 : 0);
          if (nameIdx == (int)strlen(PAT_NAME)) {
            capName = true; nameIdx = 0; nameLen = 0;
          }
        } else {
          if (c == '"') {
            nameBuf[nameLen] = '\0';
            if (stationName[0] == '\0') {
              strncpy(stationName, nameBuf, sizeof(stationName) - 1);
              jsonDecodeUnicode(stationName);
            }
            capName = false;
          } else if (nameLen < (int)sizeof(nameBuf) - 1) {
            nameBuf[nameLen++] = c;
          }
        }

        // "stationboard":[ erkennen
        sbIdx = (c == PAT_SB[sbIdx]) ? sbIdx + 1 : (c == PAT_SB[0] ? 1 : 0);
        if (sbIdx == (int)strlen(PAT_SB)) {
          sbFound = true;
          Serial.print("[JSON] Station: "); Serial.println(stationName);
          Serial.println("[JSON] Parse Abfahrten:");
        }

      } else {
        // --- Phase 2: Einträge im stationboard-Array einlesen ---

        if (!inEntry) {
          if      (c == '{') {
            inEntry = true; braceDepth = 1;
            entryLen = 0; entryParsed = false; entryFull = false;
            entryBuf[entryLen++] = '{';
          }
          else if (c == ']') { done = true; break; }  // Array-Ende

        } else {
          // Zeichen in Puffer schreiben solange Platz vorhanden
          if (!entryFull) {
            if (entryLen < (int)sizeof(entryBuf) - 1) {
              entryBuf[entryLen++] = c;
            } else {
              // Puffer voll: sofort mit vorhandenem Inhalt parsen,
              // Rest des Eintrags weiter zählen (aber nicht speichern)
              entryBuf[entryLen] = '\0';
              entryFull = true;
              Serial.print("[JSON] WARNUNG: Eintrag ");
              Serial.print(depCount);
              Serial.println(" > 2500 Bytes, parse Anfang");
              if (parseEntry(entryBuf)) depCount++;
              entryParsed = true;
            }
          }

          // Klammertiefe immer verfolgen (auch wenn Puffer voll)
          if      (c == '{') braceDepth++;
          else if (c == '}') {
            if (--braceDepth == 0) {
              // Eintrag abgeschlossen
              if (!entryParsed) {
                entryBuf[entryLen] = '\0';
                if (parseEntry(entryBuf)) depCount++;
              }
              inEntry = false;
              if (depCount >= LIMIT) { done = true; break; }
            }
          }
        }
      }
    }

    if (!done) {
      if (millis() - timeout > 12000) {
        Serial.println("[HTTP] Timeout beim Lesen");
        break;
      }
      delay(1);
    }
  }

  client.stop();
  Serial.println("[HTTP] Verbindung geschlossen");

  if (!sbFound) {
    Serial.println("[JSON] FEHLER: stationboard nicht gefunden");
    showOledError("JSON", "stationboard fehlt", "");
    return false;
  }

  Serial.print("[JSON] "); Serial.print(depCount); Serial.println(" Abfahrten geladen");
  return depCount > 0;
}



// -----------------------------------------------------
// App
// -----------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);  // max. 3s auf Serial-Monitor warten
  Serial.println();
  Serial.println("========================================");
  Serial.println(" Fahrplan Arduino UNO R4 WiFi");
  Serial.println("========================================");
  Serial.print("[BOOT] Station: ");
  Serial.println(STATION);
  Serial.print("[BOOT] Update-Intervall: ");
  Serial.print(UPDATE_MS / 1000);
  Serial.println(" s");

  Wire.begin();
  u8g2.begin();

  showOledStatus("Fahrplan startet");
  connectWiFi();
  showOledStatus("Fahrplan...", STATION);
  if (fetchTimetable()) {
    resetScrollStates();
  }
  lastUpdate = millis();
  Serial.println("[BOOT] Setup abgeschlossen, starte Loop");
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Loop] WiFi verloren, verbinde neu...");
    connectWiFi();
    showOledStatus("Fahrplan...", STATION);
    if (fetchTimetable()) {
      resetScrollStates();
    }
    lastUpdate = now;
  }

  if (now - lastUpdate >= UPDATE_MS) {
    lastUpdate = now;
    Serial.print("[Loop] Update nach ");
    Serial.print(now / 1000);
    Serial.println(" s Laufzeit");
    showOledStatus("Fahrplan: ", STATION);
    if (fetchTimetable()) {
      resetScrollStates();
    }
  }

  drawTimetableScreen();
  delay(50);  // ~20 fps
}
