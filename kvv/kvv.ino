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

  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
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
  /*char timeStamp[15];
  sprintf(timeStamp, "%d.%d.%02d %d:%02d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year - 100, timeinfo.tm_hour, timeinfo.tm_min);

  Serial.printf("Name: %s\n", stopName);
serial.printf("Timestamp: %s\n", timeStamp);*/

  display.init(115200);
  display.setRotation(1);
  display.setFont(&FreeSansBold9pt8b);
  display.fillScreen(GxEPD_WHITE);
  display.setTextSize(1);

  // Hintergrund für Stationsname und Zeitstempel
  display.fillRect(0, 0, display.width(), SKIP, GxEPD_BLACK);

  // Stationsname
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(LEFT, TOP);
  display.print(utf8ascii(stopName));

  // Zeitstempel
  /*display.getTextBounds(timeStamp, 0, 0, &x, &y, &w, &h);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(display.width() - w - LEFT, TOP);
  display.print(timeStamp);*/

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

    struct tm deptime;
    if (nobj.containsKey("realDateTime")) parse_time(&deptime, nobj["realDateTime"]);
    else parse_time(&deptime, nobj["dateTime"]);

    char time[8];
    if (countdown <= 0) strcpy(time, "sofort");
    else if (countdown < 10) sprintf(time, "%d min", countdown);
    else sprintf(time, "%d:%02d", deptime.tm_hour, deptime.tm_min);

    Serial.printf("[%s] %s %s\n", route, direction, time);

    uint16_t routeWidth, destWidth, timeWidth;
    routeWidth = get_dsp_length(route);
    destWidth = get_dsp_length(destination);
    timeWidth = get_dsp_length(time);

    // Hintergrund für Routen-Nummer
    display.fillRect(0, TOP + SKIP * displayedEntries + 5, COL0_WIDTH, SKIP - 2, GxEPD_RED);

    // Routen-Nummer
    display.setTextColor(GxEPD_WHITE);
    display.setCursor((COL0_WIDTH - routeWidth) / 2, TOP + SKIP + SKIP * displayedEntries);
    display.print(route);

    // Ziel
    display.setTextColor(GxEPD_BLACK);
    if (COL0_WIDTH + LEFT + destWidth >= display.width() - timeWidth - LEFT) {
      destination.concat("...");
      destWidth = get_dsp_length(destination);
      while (COL0_WIDTH + LEFT + destWidth >= display.width() - timeWidth - LEFT) {
        destination = destination.substring(0, destination.length() - 4);
        destination.concat("...");
        destWidth = get_dsp_length(destination);
      }
    }
    display.setCursor(COL0_WIDTH + LEFT, TOP + SKIP + SKIP * displayedEntries);
    display.print(utf8ascii(destination));

    // Zeit
    display.setTextColor(GxEPD_RED);
    display.setCursor(display.width() - timeWidth - LEFT, TOP + SKIP + SKIP * displayedEntries);
    display.print(time);

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
        "useOnlyStops=1&useRealtime=1&limit=6")) {
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