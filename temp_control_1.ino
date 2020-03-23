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
#include "DHTesp.h" // Temp sensor
#include "Wire.h" //i2c
#include "Adafruit_BMP280.h"

Adafruit_BMP280 bmp;
#define BMP_ADDR 118
#define BMP_CHIP_ADDR 96

//#define I2C_SCAN

#ifdef ESP32
#pragma message(THIS CODE IS FOR ESP8266 ONLY!)
#error Select ESP8266 board.
#endif

#define RELAIS D0  // 0: on, 1: off
bool relais_state = 1; // 1: off, 0: on

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

#define LED 13
static unsigned long warmMinutes = 0;

#define TEMP_SAMPLES 1000
int8_t temp_5min[TEMP_SAMPLES]; // 83 hours of temperature recordings 
word next_sample = 0;
static unsigned long temp5minSampleTimeMarker = 0;
#define isTimeToUpdateTempLog() ((millis() - temp5minSampleTimeMarker) > 5 * 60 * 1000)

float cur_temp = -100.0;
int8_t target_temp = 62;  // default is 63 degrees Celsius
uint8_t target_hours = 16; // how many hours before shut off
bool target_reached = false;
#define MAX_TEMP 65  // emergency temperature we never want to exceed
#define HYSTERESIS 1  // ºC up and down from target temp when heating is switched on and off

void handleRoot() {
  digitalWrite(LED, 1);
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
    <p>Current Temperature: %3.1f deg. C</p>\
    <p>Target Temperature: %3d deg. C</p>\
    <p>Current Max. Temperature: %3d deg. C</p>\
    <p>Current Heater Control Relais State: %s</p>\
    <p>Next Temperature Sample number: %3d</p>\
    <p>Minutes with Temperature >= target - 2 deg. C: %3d min.</p>\
    <img src=\"/test.svg\" />\
  </body>\
</html>",

           hr, min % 60, sec % 60, 
           cur_temp,
           target_temp,
           MAX_TEMP,
           relais_state ? "off" : "on",
           next_sample,
           warmMinutes
          );
  server.send(200, "text/html", temp);
  digitalWrite(LED, 0);
}

void handleNotFound() {
  digitalWrite(LED, 1);
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
  digitalWrite(LED, 0);
}

void drawGraph() {
  String out;
  out.reserve(60000);
  char temp[150];
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"1200\" height=\"180\">\n";
  out += "<rect x=\"20\" y=\"1\" width=\"1000\" height=\"150\" fill=\"rgb(250, 230, 210)\" stroke-width=\"2\" stroke=\"rgb(0, 0, 0)\" />\n";
  sprintf(temp, "<line stroke-dasharray=\"5, 5\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"red\" stroke-width=\"1\" />\n", 22, 145-(2* target_temp), 22 + 1000 - 2, 145-(2* target_temp));
  out += temp;
  for (int tmp = 10; tmp < 71; tmp += 20) {
    sprintf(temp, "<text x=\"0\" y=\"%d\" fill=\"black\">%d</text>", 145-(2*tmp)+7, tmp);
    out += temp;
    sprintf(temp, "<line stroke-dasharray=\"1, 5\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"grey\" stroke-width=\"1\" />\n", 22, 145-(2* tmp), 22 + 1000 - 2, 145-(2* tmp));
    out += temp;
  }
  for (int min = 0; min < 1000; min += 60) {
    sprintf(temp, "<text x=\"%d\" y=\"165\" fill=\"black\">%d</text>", min + 20, min);
    out += temp;
  }
  out += "<g stroke=\"black\">\n";
  for (int x = 1; x < next_sample; x++) {
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"2\" />\n", x + 20, 145-(2*temp_5min[x-1]), x+1+20, 145-(2*temp_5min[x]));
    out += temp;
 }
  out += "</g>\n</svg>\n";
  server.send(200, "image/svg+xml", out);
  Serial.print("Out buffer used:");Serial.println(out.length());
}

void setup(void) {
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  pinMode(RELAIS, OUTPUT);
  digitalWrite(RELAIS, HIGH);  // relais off
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
    Serial.println(F("Error setting up MDNS responder!"));
    while (1) {
      delay(1000);
    }
  }
  MDNS.addService("http", "tcp", 80);
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

  //BMP280
  Wire.begin(D2, D1);
  Wire.setClock(10000); // 10KHz for cable length ~3m
  #if defined(I2C_SCAN)
  i2c_scan();
  #endif
  if (!bmp.begin(BMP_ADDR, BMP_CHIP_ADDR)) {
    Serial.print("Could not find a valid BMP280 sensor: chipid =");
    Serial.println(bmp.get_chip_id());
  } else {
    Serial.println("Successfully initialized BMP280");
  }
}
 
#if defined(I2C_SCAN)
void i2c_scan() {
  Serial.println("Scanning i2c bus");
  for(uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (! Wire.endTransmission()) {
      Serial.print("I2C Device at DEC "); Serial.println(address);
    }
    delay(500);
  }
}
#endif

/* TURN HEATER ON/OFF */
#define TEMP_CTRL_INTERVAL_S 30 // interval in seconds when to take action on temperature control
static unsigned long TempCtrlMarker = 0;
#define isTimeToCtrlTemp() ((millis() - TempCtrlMarker) > TEMP_CTRL_INTERVAL_S * 1000)
void tempCtrlLoop() {
  if (isTimeToCtrlTemp()) {
    if (target_temp > MAX_TEMP) {
      target_temp = MAX_TEMP - 1;
    }
    if (target_reached || (cur_temp > target_temp)) {
      relais_state = 1;  // relais off
    } else {
      if ((cur_temp < target_temp) && !target_reached) {
        relais_state = 0;  // realis on
      }
    }
    digitalWrite(RELAIS, relais_state);
    Serial.print("Heat Relais in State: ");
    Serial.println(relais_state ? "off" : "on");
    if (target_reached) {
      Serial.println("because target is reached.");
    }
    TempCtrlMarker = millis();
  }
}

// ensure that relais is off if temp is exceeded, and sound alarm
void tempEmergencyLoop() {
  if (cur_temp > MAX_TEMP) {
    digitalWrite(RELAIS, HIGH);  // relais off
  }
}

void tempSensorLoop() {
  if (isTimeToSampleDHT()) {
    cur_temp = bmp.readTemperature();
    Serial.print("Cur. Temperature measured to be: "); Serial.print(cur_temp); Serial.println(" ºC");
    DHTSampleTimeMarker = millis();
  }
}

void tempLogLoop() {
  if (isTimeToUpdateTempLog()) {
    int8_t temperature = (int8_t) cur_temp;
    Serial.print("Saving temperature "); Serial.print(temperature); Serial.println(" ºC to graph.");
    temp_5min[next_sample] = temperature;
    next_sample = (next_sample + 1) % TEMP_SAMPLES; // round robin buffering
    temp5minSampleTimeMarker = millis();
  }
}

#define FINISHED_LOOP_INTERVAL_S 60 // interval in seconds when to take action on temperature control
static unsigned long finishedLoopMarker = 0;
#define isTimeToCheckFinished() ((millis() - finishedLoopMarker) > FINISHED_LOOP_INTERVAL_S * 1000)
void finishedLoop() {
  if (isTimeToCheckFinished()) {
    finishedLoopMarker = millis();
    if (cur_temp >= target_temp - 2) {
      warmMinutes ++;
    }
    if (warmMinutes >= target_hours * 60) {
      target_reached = true;
      Serial.println("shutting off the heat, because we reached the target.");
    }
  }
}

void loop(void) {
  server.handleClient();
  tempSensorLoop();
  tempCtrlLoop();
  tempEmergencyLoop();
  tempLogLoop();
  finishedLoop();
  MDNS.update();
}
