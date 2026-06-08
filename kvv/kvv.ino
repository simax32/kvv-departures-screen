/*
   KVV ePaper Display - ESP8266
   Anzeige der Abfahrtszeiten für die Haltestelle "Knielinger Allee" (7001002)
   mit 2.9" tri-color eInk Display (GxEPD2_290_C90c, 90° gedreht)
   Filtert Abfahrten in den nächsten 5 Minuten und zeigt lokale Namenskonvertierungen.
   Dynamische Kürzung langer Zielnamen + Verspätungserkennung (rote Abfahrtszeit).
*/

#define STOP_ID  "7001002"    // Knielinger Allee
#define LIMIT 11               // Maximale Anzahl der angezeigten Abfahrten

#include "wifi.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <time.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include "FreeSansBold9pt8b.h"

// Display-Pins
#define CS_PIN   15
#define DC_PIN   4
#define RST_PIN  5
#define BUSY_PIN 16

// Display-Objekt (90° gedreht)
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(GxEPD2_290_C90c(CS_PIN, DC_PIN, RST_PIN, BUSY_PIN));
ESP8266WiFiMulti WiFiMulti;

// Anpassungen für den gedrehten Modus
#define TOP 15
#define LEFT 15
#define SKIP 18
#define COL0_WIDTH 36

// UTF8-Decoder
byte utf8ascii(byte ascii) {
  static byte c1;
  if (ascii < 128) {
    c1 = 0;
    return ascii;
  }
  byte last = c1;
  c1 = ascii;
  switch (last) {
    case 0xC2: return ascii;
    case 0xC3: return (ascii | 0xC0);
    case 0x82: if (ascii == 0xAC) return 0x80;
  }
  return 0;
}

String utf8ascii(String s) {
  String r = "";
  char c;
  for (int i = 0; i < s.length(); i++) {
    c = utf8ascii(s.charAt(i));
    if (c != 0) r += c;
  }
  return r;
}

uint16_t get_dsp_length(String str) {
  int16_t x, y;
  uint16_t w, h;
  display.getTextBounds(utf8ascii(str), 0, 0, &x, &y, &w, &h);
  return w;
}

// Konvertiert Stop-Namen in lokale Dialektform oder kürzere Varianten
String convertStopName(String stopName) {
  if (stopName == "KA Tullastraße/Alter Schlachthof") return "Tullastr./VBK";
  if (stopName == "Knielingen Nord über Hbf") return "Knielingen ü Hbf";
  if (stopName == "Germersheim, Bahnhof") return "Germersheim";
  if (stopName == "Rappenwört über Hbf") return "Rappele";
  if (stopName == "Karlsruhe, Marktplatz") return "KA Marktplatz";
  if (stopName == "Karlsruhe, Durlacher Tor") return "KA Durlacher Tor";
  if (stopName == "Karlsruhe, Hauptbahnhof") return "KA Hbf";
  if (stopName == "Söllingen (b. Karlsruhe)") return "Söllingen (b. KA)";
  if (stopName == "Rheinstrandsiedlung (Umleitung)") return "Rheinstrandsiedl. (Uml)";
  if (stopName == "Rheinstetten (Umleitung)") return "Rheinstetten (Uml)";
  if (stopName == "KA Marktplatz (Pyramide U)") return "Marktplatz (Pyramide)";
  if (stopName == "Karlsruhe Albtalbahnhof") return "Karlsruhe Albtalbhf";
  return stopName;
}

void parse_time(struct tm *timeinfo, const JsonObject &obj) {
  memset(timeinfo, 0, sizeof(struct tm));
  if (obj.containsKey("year")) timeinfo->tm_year = atoi(obj["year"]) - 1900;
  if (obj.containsKey("month")) timeinfo->tm_mon = atoi(obj["month"]) - 1;
  if (obj.containsKey("day")) timeinfo->tm_mday = atoi(obj["day"]);
  if (obj.containsKey("hour")) timeinfo->tm_hour = atoi(obj["hour"]);
  if (obj.containsKey("minute")) timeinfo->tm_min = atoi(obj["minute"]);
}

void parse_reply(Stream &payload) {
  DynamicJsonDocument doc(24576);

  // Filter: Füge realDateTime und dateTime hinzu, um Verspätungen zu berechnen
  StaticJsonDocument<512> filter;
  filter["dateTime"] = true;
  filter["dm"]["points"]["point"]["name"] = true;
  for (int i = 0; i < LIMIT; i++) {
    filter["departureList"][i]["countdown"] = true;
    filter["departureList"][i]["servingLine"]["direction"] = true;
    filter["departureList"][i]["servingLine"]["symbol"] = true;
    filter["departureList"][i]["realtimeStatus"] = true;
    filter["departureList"][i]["realtimeTripStatus"] = true;
    filter["departureList"][i]["realtimeDepartureTime"] = true;
    filter["departureList"][i]["plannedDepartureTime"] = true;
  }

  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  const char *stopName = obj["dm"]["points"]["point"]["name"];

  // Stationsname extrahieren
  const char *c = stopName;
  for (int i = 0; i < strlen(c); i++)
    if (c[i] == ',') {
      for (i++; c[i] == ' '; i++);
      stopName = c + i;
      break;
    }

  struct tm timeinfo;
  parse_time(&timeinfo, obj["dateTime"]);

  display.init(115200);
  display.setRotation(1);
  display.setFont(&FreeSansBold9pt8b);
  display.fillScreen(GxEPD_WHITE);
  display.setTextSize(1);

  // Hintergrund für Stationsname
  display.fillRect(0, 0, display.width(), SKIP, GxEPD_BLACK);
  String convertedStopName = convertStopName(String(stopName));
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(LEFT, TOP);
  display.print(utf8ascii(convertedStopName));

  // Sortiere Abfahrten nach Countdown
  int departureCount = min(LIMIT, (int)obj["departureList"].size());
  int order[departureCount];
  for (int i = 0; i < departureCount; i++) order[i] = i;
  for (int i = 0; i < departureCount - 1; i++) {
    for (int j = i + 1; j < departureCount; j++) {
      int ci = obj["departureList"][order[i]]["countdown"];
      int cj = obj["departureList"][order[j]]["countdown"];
      if (ci > cj) {
        int temp = order[i];
        order[i] = order[j];
        order[j] = temp;
      }
    }
  }

  int displayedEntries = 0;
  for (int i = 0; i < departureCount && displayedEntries < LIMIT; i++) {
    JsonObject nobj = obj["departureList"][order[i]];
    const char *direction = nobj["servingLine"]["direction"];
    char *d = (char*)direction;
    while (*d && *d != '>') d++;
    if (*d) *d = '\0';
    String destination = convertStopName(String(direction));
    const char *route = nobj["servingLine"]["symbol"];
    int countdown = nobj["countdown"];

    // Überspringe Fahrten in den nächsten 1 Minute
    if (countdown <= 1) {
      Serial.print("Übersprunge Abfahrt mit Countdown ");
      Serial.println(countdown);
      continue;
    }

    // Verspätung/Absage prüfen
    bool isCancelled = false;
    bool isDelayed = false;
    const char* realtimeStatus = nobj["realtimeStatus"] | "";
    const char* realtimeTripStatus = nobj["realtimeTripStatus"] | "";

    if (strcmp(realtimeStatus, "TRIP_CANCELLED") == 0 || strcmp(realtimeTripStatus, "TRIP_CANCELLED") == 0) {
      isCancelled = true;
    }
    if (strcmp(realtimeStatus, "DELAYED") == 0 || strcmp(realtimeTripStatus, "DELAYED") == 0) {
      isDelayed = true;
    }

    // Falls realtimeDepartureTime und plannedDepartureTime verfügbar sind:
    if (nobj.containsKey("realtimeDepartureTime") && nobj.containsKey("plannedDepartureTime")) {
      long long realtimeDep = nobj["realtimeDepartureTime"];
      long long plannedDep = nobj["plannedDepartureTime"];
      if (delay > 0) {
        isDelayed = true;
      }
    }

    // Zeit formatieren (immer mit "min")
    char time[8];
    if (countdown <= 0) strcpy(time, "sofort");
    else sprintf(time, "%dmin", countdown);

    // Anzeige vorbereiten
    uint16_t routeWidth = get_dsp_length(route);
    uint16_t timeWidth = get_dsp_length(time);
    int currentY = TOP + SKIP + SKIP * displayedEntries;

    // Hintergrund für Routen-Nummer
    display.fillRect(0, currentY - SKIP + 5, COL0_WIDTH, SKIP - 2, GxEPD_RED);
    display.setTextColor(isCancelled ? GxEPD_RED : GxEPD_WHITE);
    display.setCursor((COL0_WIDTH - routeWidth) / 2, currentY);
    display.print(route);

    // Dynamische Kürzung des Zielnamens
    uint16_t maxDestWidth = display.width() - COL0_WIDTH - LEFT - timeWidth - LEFT;
    String displayDestination = destination;
    uint16_t destWidth = get_dsp_length(displayDestination);
    if (destWidth > maxDestWidth) {
      while (destWidth > maxDestWidth && displayDestination.length() > 0) {
        displayDestination = displayDestination.substring(0, displayDestination.length() - 1);
        destWidth = get_dsp_length(displayDestination + "...");
      }
      displayDestination += "...";
    }

    display.setTextColor(GxEPD_BLACK);
    display.setCursor(COL0_WIDTH + LEFT, currentY);
    display.print(utf8ascii(displayDestination));

    // Zeit anzeigen (rot bei Verspätung oder Absage)
    display.setTextColor((isCancelled || isDelayed) ? GxEPD_RED : GxEPD_BLACK);
    uint16_t timeX = display.width() - timeWidth - LEFT;
    display.setCursor(timeX, currentY);
    display.print(time);

    // Durchstreichen, wenn abgesagt
    if (isCancelled) {
      int16_t x1, y1;
      uint16_t w1, h1;
      display.getTextBounds(time, timeX, currentY, &x1, &y1, &w1, &h1);
      display.drawLine(timeX, y1 + h1 * 2 / 3, timeX + timeWidth, y1 + h1 * 2 / 3, GxEPD_RED);
    }

    displayedEntries++;
  }
  display.display();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.println("WiFi connection establishing...");

  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress(8, 8, 8, 8));

  while (WiFiMulti.run() != WL_CONNECTED) {
    Serial.print('.');
    digitalWrite(LED_BUILTIN, HIGH);
    delay(50);
    digitalWrite(LED_BUILTIN, LOW);
    delay(50);
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  https.setTimeout(10000);

  String url = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST?"
               "outputFormat=JSON&coordOutputFormat=WGS84[dd.ddddd]"
               "&depType=stopEvents&locationServerActive=1&mode=direct"
               "&name_dm=" STOP_ID
               "&type_dm=stop&useOnlyStops=1&useRealtime=1"
               "&limit=" + String(LIMIT);

  Serial.print("[HTTPS] GET ");
  Serial.println(url);
  if (https.begin(*client, url)) {
    Serial.print("[HTTPS] GET...");
    int httpCode = https.GET();
    if (httpCode > 0) {
      Serial.printf(" code: %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        parse_reply(https.getStream());
      } else {
        Serial.printf(" failed, error: %s\n", https.errorToString(httpCode).c_str());
      }
      https.end();
    } else {
      Serial.printf("[HTTPS] Unable to connect\n");
    }
  }
  Serial.println("Going to sleep...");
  ESP.deepSleep(0);
}

void loop() {
  // This should never be reached
  Serial.printf("loop() should never be reached!\n");
  delay(1000);
}