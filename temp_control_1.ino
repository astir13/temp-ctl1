/*
  Copyright (c) 2020, Stefan Pielmeier.
  All rights reserved.

  
   Thanks for the example code of a web server:
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
#include <OneWire.h>  // OneWire by Jim Studt 2.3.5
#include <DallasTemperature.h>  // DallasTemperature by Miles Burton 3.8.0

#define FW_VERSION "1.00_20200420-002"

// a well protected error variable (start of memory)
#define MAX_ERROR_LENGTH 150
char error[MAX_ERROR_LENGTH] = "";
bool error_flag = false;  // if true, the system is in error

#define DS18B20_PIN D5
#define DALLAS_ERROR_TEMP -127  // Dallas lib. indicates sensor disconnected
long sensor_retry_count = 0;  // count problems with this sensor

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(DS18B20_PIN);

// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);


#ifdef ESP32
#pragma message(THIS CODE IS FOR ESP8266 ONLY!)
#error Select ESP8266 board.
#endif

#define RELAIS D0  // 0: on, 1: off
bool relais_state = 1; // 1: off, 0: on

// for development, use that code
//#define STASSID "MyAccessPointAtMyLab"
//#define STAPSK  "MyPassword"
#ifdef STASSID
const char *ssid = STASSID;
const char *password = STAPSK;
#endif

#define MDNS_NAME "temp-ctl"


static unsigned long DHTSampleTimeMarker = 0;
#define isTimeToSampleDHT() ((millis() - DHTSampleTimeMarker) > 1000)


ESP8266WebServer server(80);

#define LED 13
static unsigned long warmMinutes = 0;

#define TEMP_SAMPLES 200
int8_t temp_5min[TEMP_SAMPLES]; // 20 hours of temperature recordings 
word next_sample = 0;
static unsigned long temp5minSampleTimeMarker = 0;
#define isTimeToUpdateTempLog() ((millis() - temp5minSampleTimeMarker) > 5 * 60 * 1000)

float cur_temp = -100.0;
int8_t target_temp = 62;  // default is 63 degrees Celsius
#define TARGET_TOLERANCE 4 // °C tolerance to count warm minutes
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

  snprintf(temp, 2000,

           "<html>\
  <head>\
    <title>Temperature Curve for Temp controller</title>\
    <style>\
      body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }\
    </style>\
  </head>\
  <body>\
    <h1>Temperature Curve since power on</h1>\
    <h2 style=\"color:red;\">%s</h2>\
    <p>sensor retries: %d</p>\
    <p>Firmware Version: %s</p>\
    <p>Registration: <input type=\"text\" value=\"OY-CBX\"/></p>\
    <p>Part/location: <input type=\"text\" value=\"right wing, root rib\"/></p>\
    <p>Date: <input type=\"text\" value=\"2020-03-23\"/></p>\
    <p>Uptime: %02d:%02d:%02d</p>\
    <p>Current Temperature: %3.1f deg. C</p>\
    <p>Target Temperature: %3d deg. C</p>\
    <p>Current Max. Temperature: %3d deg. C</p>\
    <p>Current Heater Control Relais State: %s</p>\
    <p>Next Temperature Sample number: %3d</p>\
    <p>Hours with Temperature >= target - %d deg. C: %3.1f hr.</p>\
    <p>Target time reached: %s</p>\
    <img src=\"/test.svg\" />\
    <button onClick=\"window.location.reload();\">Refresh Page</button>\
  </body>\
</html>",
           error,
           sensor_retry_count,
           FW_VERSION,
           hr, min % 60, sec % 60, 
           cur_temp,
           target_temp,
           MAX_TEMP,
           relais_state ? "off" : "on",
           next_sample,
           TARGET_TOLERANCE,
           (float)warmMinutes / 60.0,
           target_reached ? "yes, stopped heating" : "not reached, still heating"
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
  out.reserve(30000);
  char temp[150];
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"1200\" height=\"200\">\n";
  out += "<text x=\"1000\" y=\"185\" fill=\"black\">hours</text>";
  out += "<text x=\"0\" y=\"165\" fill=\"black\">&#8451;</text>";
  out += "<rect x=\"20\" y=\"1\" width=\"1000\" height=\"150\" fill=\"rgb(250, 230, 210)\" stroke-width=\"2\" stroke=\"rgb(0, 0, 0)\" />\n";
  sprintf(temp, "<line stroke-dasharray=\"5, 5\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"red\" stroke-width=\"1\" />\n", 22, 145-(2* target_temp), 22 + 1000 - 2, 145-(2* target_temp));
  out += temp;
  for (int tmp = 10; tmp < 71; tmp += 20) {
    sprintf(temp, "<text x=\"0\" y=\"%d\" fill=\"black\">%d</text>", 145-(2*tmp)+7, tmp);
    out += temp;
    sprintf(temp, "<line stroke-dasharray=\"1, 5\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"grey\" stroke-width=\"1\" />\n", 22, 145-(2* tmp), 22 + 1000 - 2, 145-(2* tmp));
    out += temp;
  }
  for (int min = 0; min < TEMP_SAMPLES * 5; min += 60) {
    sprintf(temp, "<text x=\"%d\" y=\"165\" fill=\"black\">%d</text>", min + 20, min / 60);
    out += temp;
  }
  out += "<g stroke=\"black\">\n";
  for (int x = 1; x < next_sample; x++) {
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"2\" />\n", (x * 5) + 20, 145-(2*temp_5min[x-1]), ((x+1) * 5) +20, 145-(2*temp_5min[x]));
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
  sensors.begin(); // Temp. sensor setup
  Serial.println("");
  Serial.println("Temp Control by stefan@symlinux.com");
  Serial.print("Version:");
  Serial.println(FW_VERSION);
  Serial.print("WIFI Setup:");

#ifdef STASSID  // used under development
  // Wait for connection + 10 sec. timeout
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  
  // Check connection
  if(WiFi.status() == WL_CONNECTED)
  {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());
  }
  else
#endif
  {
    Serial.println(F("Can not connect to WiFi station. Go into AP mode."));
    
    // Go into software AP mode.
    WiFi.mode(WIFI_AP);
    delay(10);

    Serial.print(F("Setting soft-AP configuration ... "));
    Serial.println(WiFi.softAPConfig(IPAddress(192,168,236,1), // IP
                                     IPAddress(192,168,236,1), //gateway
                                     IPAddress(255,255,255,0)) ? //subnet
      F("Ready") : F("Failed!"));

    Serial.print(F("Setting soft-AP ... "));
    Serial.println(WiFi.softAP("temp-ctl", "GHijK345") ?
      F("Ready") : F("Failed!"));
    Serial.print(F("IP address: "));
    Serial.println(WiFi.softAPIP());
  }

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
}

/* TURN HEATER ON/OFF */
#define TEMP_CTRL_INTERVAL_S 30 // interval in seconds when to take action on temperature control
static unsigned long TempCtrlMarker = 0;
#define isTimeToCtrlTemp() ((millis() - TempCtrlMarker) > TEMP_CTRL_INTERVAL_S * 1000)
void tempCtrlLoop() {
  if (isTimeToCtrlTemp()) {
    if (target_temp > MAX_TEMP) {  // protection against memory overwriting
      target_temp = MAX_TEMP - HYSTERESIS;
    }
    if (error_flag || target_reached || (cur_temp >= target_temp + HYSTERESIS)) {
      relais_state = 1;  // relais off
    } else {
      if ((cur_temp <= target_temp - HYSTERESIS) && !target_reached) {
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
  if (error_flag || cur_temp > MAX_TEMP) {
    digitalWrite(RELAIS, HIGH);  // relais off
  }
}

#define MAX_RETRY_S 10  // seconds to retry sensor connection in case it is disconnected
void tempSensorLoop() {
  if (isTimeToSampleDHT()) {
    sensors.requestTemperatures();
    float sensor_temp = sensors.getTempCByIndex(0);
    if (sensor_temp > DALLAS_ERROR_TEMP) {
      cur_temp = sensor_temp;
      Serial.print("Cur. Temperature measured to be: "); Serial.print(cur_temp); Serial.println(" ºC");
    } else {
      Serial.println("[E]RROR: could not read temperature from sensor, but will try again.");
      sprintf(error, "Sensor Error, could not read temperature\n");
      error_flag = true;
      long start_time = millis();
      sensor_retry_count++;
      while (start_time + (MAX_RETRY_S * 1000) > millis()) {
        Serial.println("try oneWire.reset");
        if (oneWire.reset()) {
          sprintf(error, "OneWire.reset(): found a sensor.");
          Serial.println("try oneWire.reset_search()");
          oneWire.reset_search();
          uint8_t address;
          Serial.println("try oneWire.search()");
          if (oneWire.search(&address)) {
            sprintf(error, "OneWire.search(): found a sensor.");
          }
          sensors.begin();
          Serial.println("try sensors.requestTemperatures()");
          sensors.requestTemperatures();
          Serial.println("try sensors.getTempCByIndex");
          if (float temp = sensors.getTempCByIndex(0) > DALLAS_ERROR_TEMP) {
            error_flag = false;
            sprintf(error, "[W]arn: recovered from sensor error. Could get temperature: %f", temp);
            cur_temp = temp;
            break; // leave the while loop
          }
        } else {
          sprintf(error, "OneWire.reset(): Didn't get temperature, didn't find a sensor. Cabling or power defect?");
          Serial.println("OneWire.reset(): Sensor did not answer.");
        }
        delay(500);
      }
    }
    DHTSampleTimeMarker = millis();
  }
}

// only record until buffer is full
void tempLogLoop() {
  if ((next_sample <= TEMP_SAMPLES - 1) && isTimeToUpdateTempLog()) {
    int8_t temperature = (int8_t) cur_temp;
    Serial.print("Saving temperature "); Serial.print(temperature); Serial.println(" ºC to graph.");
    temp_5min[next_sample] = temperature;
    next_sample ++;
    temp5minSampleTimeMarker = millis();
  }
}

#define FINISHED_LOOP_INTERVAL_S 60 // interval in seconds when to take action on temperature control
static unsigned long finishedLoopMarker = 0;
#define isTimeToCheckFinished() ((millis() - finishedLoopMarker) > FINISHED_LOOP_INTERVAL_S * 1000)
void finishedLoop() {
  if (isTimeToCheckFinished()) {
    finishedLoopMarker = millis();
    if (cur_temp >= target_temp - TARGET_TOLERANCE) {
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
