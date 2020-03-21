/*
   Copyright (c) 2015, Majenko Technologies
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

 * * Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

 * * Redistributions in binary form must reproduce the above copyright notice, this
     list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.

 * * Neither the name of Majenko Technologies nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
   ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include "DHTesp.h"

#ifdef ESP32
#pragma message(THIS CODE IS FOR ESP8266 ONLY!)
#error Select ESP8266 board.
#endif

#define RELAIS D0
bool relais_state = 0;
#define BEEPER D1

#ifndef STASSID
#define STASSID "mahewakan71"
#define STAPSK  "EinHaseSprangUbersHaus"
#endif
#define MDNS_NAME "temp-ctl"

DHTesp dht;
static unsigned long DHTSampleTimeMarker = 0;
#define isTimeToSampleDHT() ((millis() - DHTSampleTimeMarker) > 1000)

const char *ssid = STASSID;
const char *password = STAPSK;

ESP8266WebServer server(80);

const int led = 13;

#define TEMP_SAMPLES 1000
int8_t temp_5min[TEMP_SAMPLES]; // 83 hours of temperature recordings 
word next_sample = 0;
static unsigned long temp5minSampleTimeMarker = 0;
#define isTimeToUpdateTempLog() ((millis() - temp5minSampleTimeMarker) > 5 * 60 * 1000)

float cur_temp = -100.0;
float cur_hum = -100.0;
int8_t target_temp = 63;  // default is 63 degrees Celsius
#define MAX_TEMP 65  // emergency temperature we never want to exceed
#define HYSTERESIS 1  // ºC up and down from target temp when heating is switched on and off

void handleRoot() {
  digitalWrite(led, 1);
  char temp[1400];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  snprintf(temp, 1400,

           "<html>\
  <head>\
    <meta http-equiv='refresh' content='5'/>\
    <title>Temperature Curve for Temp controller</title>\
    <style>\
      body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }\
    </style>\
  </head>\
  <body>\
    <h1>Temperature Curve since power on</h1>\
    <p>Uptime: %02d:%02d:%02d</p>\
    <p>Current Humidity: %3.1f Percent</p>\
    <p>Current Temperature: %3.1f deg. C</p>\
    <p>Target Temperature: %3d deg. C</p>\
    <p>Current Max. Temperature: %3d deg. C</p>\
    <p>Current Heater Control Relais State: %3d</p>\
    <img src=\"/test.svg\" />\
  </body>\
</html>",

           hr, min % 60, sec % 60, 
           cur_hum,
           cur_temp,
           target_temp,
           MAX_TEMP,
           relais_state
          );
  server.send(200, "text/html", temp);
  digitalWrite(led, 0);
}

void handleNotFound() {
  digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
  digitalWrite(led, 0);
}

void drawGraph() {
  String out;
  out.reserve(2600);
  char temp[70];
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"400\" height=\"150\">\n";
  out += "<rect width=\"400\" height=\"150\" fill=\"rgb(250, 230, 210)\" stroke-width=\"1\" stroke=\"rgb(0, 0, 0)\" />\n";
  out += "<g stroke=\"black\">\n";
  int y = rand() % 130;
  for (int x = 10; x < 390; x += 10) {
    int y2 = rand() % 130;
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\n", x, 140 - y, x + 10, 140 - y2);
    out += temp;
    y = y2;
  }
  out += "</g>\n</svg>\n";

  server.send(200, "image/svg+xml", out);
}

void setup(void) {
  pinMode(led, OUTPUT);
  digitalWrite(led, 0);
  pinMode(RELAIS, OUTPUT);
  digitalWrite(RELAIS, 0);
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  dht.setup(D4, DHTesp::DHT11);
  Serial.print("WIFI Setup:");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (!MDNS.begin(MDNS_NAME)) {
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.print("mDNS responder started: ");
  Serial.println(MDNS_NAME);

  server.on("/", handleRoot);
  server.on("/test.svg", drawGraph);
  server.on("/inline", []() {
    server.send(200, "text/plain", "this works as well");
  });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

#define TEMP_CTRL_INTERVAL_S 30 // interval in seconds when to take action on temperature control
static unsigned long TempCtrlMarker = 0;
#define isTimeToCtrlTemp() ((millis() - TempCtrlMarker) > TEMP_CTRL_INTERVAL_S * 1000)

void tempCtrlLoop() {
  if (target_temp > MAX_TEMP) {
    target_temp = MAX_TEMP - 1;
  }
  if (isTimeToCtrlTemp()) {
    if (cur_temp > target_temp) {
      relais_state = 0;
    } else {
      if (cur_temp < target_temp) {
        relais_state = 1;
      }
    }
    digitalWrite(RELAIS, relais_state);
    Serial.print("Heat Relais in State: "); Serial.println(relais_state);
  }
}

// ensure that relais is off if temp is exceeded, and sound alarm
void tempEmergencyLoop() {
  if (cur_temp > MAX_TEMP) {
    digitalWrite(RELAIS, LOW);
  }
  digitalWrite(BEEPER, HIGH);
}

void loop(void) {
  server.handleClient();
  tempCtrlLoop();
  tempEmergencyLoop();
  if (isTimeToSampleDHT()) {
    cur_temp = dht.getTemperature();
    cur_hum = dht.getHumidity();
    Serial.print("Cur. Humidity measured to be: "); Serial.print(cur_hum); Serial.println(" %");
    Serial.print("Cur. Temperature measured to be: "); Serial.print(cur_temp); Serial.println(" ºC");
    DHTSampleTimeMarker = millis();
  }
  if (isTimeToUpdateTempLog()) {
    int8_t temperature = (int8_t) cur_temp;
    Serial.print("Saving temperature "); Serial.print(temperature); Serial.println(" ºC to graph.");
    temp_5min[next_sample] = temperature;
    next_sample = (next_sample + 1) % TEMP_SAMPLES; // round robin buffering
    temp5minSampleTimeMarker = millis();
  }
  MDNS.update();
}
