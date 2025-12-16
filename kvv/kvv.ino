#define STOP_ID  "7001103"    // Knielinger Allee
#define LIMIT "6"             // the epaper display can display six text lines


#include "wifi.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <time.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// Display pins
#define CS_PIN   15
#define DC_PIN   4
#define RST_PIN  5
#define BUSY_PIN 16

// Verwende die gedrehte Version des Displays
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(GxEPD2_290_C90c(CS_PIN, DC_PIN, RST_PIN, BUSY_PIN));
ESP8266WiFiMulti WiFiMulti;

// Anpassung für den gedrehten Modus
#define TOP 15
#define LEFT 15
#define SKIP 18
#define COL0_WIDTH 36

#include "FreeSansBold9pt8b.h"

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

void parse_time(struct tm *timeinfo, const JsonObject &obj) {
  memset(timeinfo, 0, sizeof(struct tm));
  if (obj.containsKey("year")) timeinfo->tm_year = atoi(obj["year"]) - 1900;
  if (obj.containsKey("month")) timeinfo->tm_mon = atoi(obj["month"]) - 1;
  if (obj.containsKey("day")) timeinfo->tm_mday = atoi(obj["day"]);
  if (obj.containsKey("hour")) timeinfo->tm_hour = atoi(obj["hour"]);
  if (obj.containsKey("minute")) timeinfo->tm_min = atoi(obj["minute"]);
}

void parse_reply(Stream &payload) {
  int16_t x, y;
  uint16_t w, h;
  DynamicJsonDocument doc(4096);

  StaticJsonDocument<300> filter;
  filter["dateTime"] = true;
  filter["dm"]["points"]["point"]["name"] = true;
  filter["departureList"][0]["countdown"] = true;
  filter["departureList"][0]["realDateTime"]["hour"] = true;
  filter["departureList"][0]["realDateTime"]["minute"] = true;
  filter["departureList"][0]["dateTime"]["hour"] = true;
  filter["departureList"][0]["dateTime"]["minute"] = true;
  filter["departureList"][0]["servingLine"]["direction"] = true;
  filter["departureList"][0]["servingLine"]["symbol"] = true;
  filter["departureList"][0]["realtimeStatus"] = true;
  filter["departureList"][0]["realtimeTripStatus"] = true;

  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    // Serial.print(F("deserializeJson() failed: "));
    // Serial.println(error.f_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  const char *stopName = obj["dm"]["points"]["point"]["name"];

  // Entferne den Stadt-Namen
  const char *c = stopName;
  for (int i = 0; i < strlen(c); i++)
    if (c[i] == ',') {
      for (i++; c[i] == ' '; i++);
      stopName = c + i;
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

  // Stationsname
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(LEFT, TOP);
  display.print(utf8ascii(stopName));

  // Sortiere Abfahrten nach Countdown
  int order[obj["departureList"].size()];
  for (size_t i = 0; i < obj["departureList"].size(); i++) order[i] = i;
  for (size_t i = 0; i < obj["departureList"].size() - 1; i++) {
    for (size_t j = i + 1; j < obj["departureList"].size(); j++) {
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
  for (int i = 0; i < obj["departureList"].size() && displayedEntries < 6; i++) {
    JsonObject nobj = obj["departureList"][i];
    const char *direction = nobj["servingLine"]["direction"];
    char *c = (char*)direction;
    while (*c && *c != '>') c++;
    if (*c) {
      c--;
      while (*c == ' ') c--;
      c[1] = '\0';
    }

    String destination(direction);
    const char *route = nobj["servingLine"]["symbol"];
    int countdown = atoi(nobj["countdown"]);

    // Überspringe Fahrten in den nächsten 5 Minuten
    if (countdown <= 5) {
      continue;
    }
    
    // ==========================================================
    // LOGIK ZUR VERSPÄTUNGS- UND ABBRUCH-ERKENNUNG
    // ==========================================================
    bool isDelayed = false;
    bool isCancelled = false;

    // Prüfen auf Abbruch-Status
    const char* realtimeStatus = nobj["realtimeStatus"] | "";
    const char* realtimeTripStatus = nobj["realtimeTripStatus"] | "";
    
    if (strcmp(realtimeStatus, "TRIP_CANCELLED") == 0 || strcmp(realtimeTripStatus, "TRIP_CANCELLED") == 0) {
        isCancelled = true;
    }

    // Nur prüfen auf Verspätung, wenn nicht abgesagt
    if (!isCancelled && nobj.containsKey("realDateTime") && nobj.containsKey("dateTime")) {
        struct tm realDepTime;
        struct tm scheduledDepTime;
        
        parse_time(&realDepTime, nobj["realDateTime"]);
        parse_time(&scheduledDepTime, nobj["dateTime"]);
        
        int realMinutes = realDepTime.tm_hour * 60 + realDepTime.tm_min;
        int scheduledMinutes = scheduledDepTime.tm_hour * 60 + scheduledDepTime.tm_min;

        if (realMinutes > scheduledMinutes) {
            isDelayed = true;
        }
    }
    // ==========================================================

    struct tm deptime;
    
    // Verwende die Zeit, die angezeigt werden soll: Real-Time (falls vorhanden und nicht abgesagt) oder Plan-Zeit
    if (nobj.containsKey("realDateTime") && !isCancelled) {
        parse_time(&deptime, nobj["realDateTime"]);
    } else {
        parse_time(&deptime, nobj["dateTime"]);
    }
    
    char time[8];
    if (countdown <= 0) strcpy(time, "sofort");
    else if (countdown < 10) sprintf(time, "%d min", countdown);
    else sprintf(time, "%d:%02d", deptime.tm_hour, deptime.tm_min); 

    uint16_t routeWidth, destWidth, timeWidth;
    routeWidth = get_dsp_length(route);
    timeWidth = get_dsp_length(time);
    
    // Y-Koordinate der aktuellen Text-Baseline
    int currentY = TOP + SKIP + SKIP * displayedEntries;

    // Hintergrund für Routen-Nummer
    display.fillRect(0, currentY - SKIP + 5, COL0_WIDTH, SKIP - 2, GxEPD_RED);

    // Routen-Nummer
    // Route Rot färben, wenn abgesagt, ansonsten Weiß
    if (isCancelled) {
         display.setTextColor(GxEPD_RED);
    } else {
         display.setTextColor(GxEPD_WHITE);
    }
    
    display.setCursor((COL0_WIDTH - routeWidth) / 2, currentY);
    display.print(route);
    
    // Ziel
    display.setTextColor(GxEPD_BLACK); 
    destWidth = get_dsp_length(destination);

    // Kürzung des Zielnamens (wie im Originalcode)
    if (COL0_WIDTH + LEFT + destWidth >= display.width() - timeWidth - LEFT) {
      destination.concat("...");
      destWidth = get_dsp_length(destination);
      while (COL0_WIDTH + LEFT + destWidth >= display.width() - timeWidth - LEFT) {
        destination = destination.substring(0, destination.length() - 4);
        destination.concat("...");
        destWidth = get_dsp_length(destination);
      }
    }
    display.setCursor(COL0_WIDTH + LEFT, currentY);
    display.print(utf8ascii(destination));
    
    // ==========================================================
    // ZEICHNE DIE ZEIT & DURCHSTREICHEN
    // ==========================================================
    
    // 1. Setze die Farbe für die Zeit
    if (isCancelled || isDelayed) {
        display.setTextColor(GxEPD_RED); // Rot bei Absage ODER Verspätung
    } else {
        display.setTextColor(GxEPD_BLACK); // Schwarz bei Pünktlichkeit
    }
    
    // 2. Zeichne die Zeit
    uint16_t timeX = display.width() - timeWidth - LEFT;
    uint16_t timeY = currentY;
    
    display.setCursor(timeX, timeY);
    display.print(time);
    
    // 3. Durchstreichen, wenn abgesagt
    if (isCancelled) {
      // Bestimme die Textgrenzen der gezeichneten Zeit für die korrekte Position der Linie
      int16_t x1, y1;
      uint16_t w1, h1;
      // Hier ist es wichtig, dass die `time` Variable verwendet wird, die gerade gezeichnet wurde
      display.getTextBounds(time, timeX, timeY, &x1, &y1, &w1, &h1);
      
      // Die Y-Position für die Durchstreichung, basierend auf der tatsächlichen Höhe (h1)
      // `y1` ist die y-Koordinate der oberen Grenze. Die Linie wird von dort + 2/3 der Höhe gezeichnet.
      int lineY = y1 + h1 * 2 / 3; 
      
      // Zeichne die rote Linie
      // Der Startpunkt ist `timeX`, die Länge ist `timeWidth`
      display.drawLine(timeX, lineY, timeX + timeWidth, lineY, GxEPD_RED);
    }
    // ==========================================================

    displayedEntries++;
  }
  display.display();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.println("WiFi connection establishing...");
  Serial.println();

  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress(8, 8, 8, 8));

  while ((WiFiMulti.run() != WL_CONNECTED)) {
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

  Serial.print("[HTTPS] begin...\n");
  if (https.begin(*client, "https://185.201.144.208/sl3-alone/XSLT_DM_REQUEST?"
        "outputFormat=JSON&coordOutputFormat=WGS84[dd.ddddd]&depType=stopEvents&"
        "locationServerActive=1&mode=direct&name_dm=7001103&type_dm=stop&"
        "useOnlyStops=1&useRealtime=1&limit=10")) {
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
  Serial.println("Going to sleep ...");
 ESP.deepSleep(0);
}

void loop() {
  Serial.printf("loop() should never be reached!\n");
  delay(1000);
}