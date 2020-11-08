/*
 * Renato Bonomini https://github.com/renatobo/arduino_garage_opener
 * Refer to https://raw.githubusercontent.com/renatobo/arduino_garage_opener/main/README.md
*/

#include <ESP8266WiFi.h>
// for OTA updates
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
// Add MQTT
#include <PubSubClient.h>
// onewire dst 18b20
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <Ticker.h>
//DST
#include <simpleDSTadjust.h>

// include header for ESP functions
#ifdef ESP8266
extern "C" {
#include "user_interface.h"
}
#endif

#include "arduino_garage_opener.h"

// Start of configurable settings

// MQTT settings for library PubSubClient
const char* mqtt_server = "192.168.0.1";
const char* MQTT_IN_TOPIC_GARAGE = "sensors/garage/action";
const char* MQTT_OUT_TOPIC_TEMP = "sensors/mydevicename/temp";
const char* MQTT_OUT_SENSOR_TEMP = "mydevicename_temp";
const char* MQTT_OUT_UNIT_TEMP = "celsius";

// I used IFTT to notify my phone upon opening/closing: create a trigger
#define IFTTT_TRIGGER_KEY "/trigger/garageopen/with/key/<yourownkey>"

// relay is on D1 -> one of the safest for relay
const int relayPin = D1;
// optical sensors to verify open/moving/closed positions
const int IRClosedPosition = D2; // sensor has obstacle when garage door completely closed
const int IROpenPosition = D6; // sensor has obstacle when garage door completely open

// End of configurable settings

// define responses for how we have connected to wifi
const char * const PHY_MODE_NAMES[]
{
  "",
  "PHY_MODE_11B",
  "PHY_MODE_11G",
  "PHY_MODE_11N"
};

// set hostname
#define HOSTNAME "MYDEVICENAME-"
// Wifi signal strenght
long rssi = -1000;

// create a webserver
ESP8266WebServer server(80);

// Activate IFTTT recipe
const char* host = "maker.ifttt.com";
const int httpsPort = 443;

// garage sensors on D2 (bottom) and D6 (top)
// int isObstacle = HIGH; // HIGH MEANS NO OBSTACLE
// start assuming  GARAGEUNKNOWN garage
#define OBSTACLENO HIGH
#define OBSTACLEYES LOW
#define GARAGECLOSED 0
#define GARAGEOPEN 1
#define GARAGEMOVING 2
#define GARAGEUNKNOWN 10
int GarageStatus = GARAGEUNKNOWN;
int lastGarageStatus = GARAGEUNKNOWN;

// Uncomment whatever location you're using!
#define Boston
#ifdef Boston
//DST rules for US Eastern Time Zone (New York, Boston)
#define UTC_OFFSET -5
struct dstRule StartRule = {"EDT", Second, Sun, Mar, 2, 3600}; // Eastern Daylight time = UTC/GMT -4 hours
struct dstRule EndRule = {"EST", First, Sun, Nov, 1, 0};       // Eastern Standard time = UTC/GMT -5 hour

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);

// Uncomment for 24 Hour style clock
// #define STYLE_24HR
#define NTP_SERVERS "us.pool.ntp.org", "time.nist.gov", "pool.ntp.org"
const boolean IS_METRIC = false;
#endif

// Dallas onewire
#define ONE_WIRE_BUS D8
#define ONE_WIRE_POWER D7
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);
DeviceAddress insideThermometer;

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);

float celsius;
float fahrenheit;
bool validtempreading = false;

// smooth to reduce noice
float celsius_ma;
int celsius_count;
float celsius_sum;
int ma_size = 20;

char FormattedTemperature[10];

// Tickers
Ticker tickermqtt;
Ticker tickerdht;
Ticker tickerwifi;
Ticker tickerheartbeat;
Ticker tickergarage;

const int UPDATE_MQTT_INTERVAL_SECS = 300; // Update every X seconds
const int UPDATE_TEMP_INTERVAL_SECS = 30 ; // Update every X seconds
const int UPDATE_WIFI_INTERVAL_SECS = 10; // Update every X seconds
const int UPDATE_HEARTBEAT_INTERVAL_SECS = 1; // Update every X seconds
const int UPDATE_GARAGEPOSITION_SECS = 1; // Update every X seconds
// flags changed in the ticker function to manage schedules
bool readyForSensorsUpdate = false;
bool readyForMQTTUpdate = false;
bool readyForWiFiUpdate = false;
bool readyForHeartBeatUpdate = false;
bool readyForGarageUpdate = false;

// MQTT initialize
WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char mqttmsg[50];

void timenow(char* time_str) {
  //compute datestring
  //char time_str[18];
  time_t now = dstAdjusted.time(nullptr);
  struct tm * timeinfo = localtime (&now);
  snprintf (time_str, 20, "%04d-%02d-%02d %02d:%02d:%02d", timeinfo->tm_year + 1900,
            timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min,
            timeinfo->tm_sec);
}

// Web Server
void handleRoot() {
  String message = FPSTR(bonogarhead);
  message += F("<h1>BonoGarage @ My Location</h1><ul><li>Temperature: <b>");
  if (celsius_ma == 0) {
    message += "</b> n/a &#128545;";
  } else {
    message += FormattedTemperature;
    message += " &#8451;</b>";
  }
  message += F("</li><li>Show <a href=/info>extended information<a></li>");
  message += F("<li>Garage is ");
  switch (GarageStatus) {
      case GARAGECLOSED:
        message += F("<b>Closed</b>");
        break;
      case GARAGEMOVING:
         message += F("<b>Moving</b>");
        break;
      case GARAGEOPEN:
        message += F("<b>Open</b>");
        break;
      default:
        message += F("unknown");
    }
  
  message += F("</li><li>");
  message += FPSTR(bonopushtopen);
  message += F("</li></ul> ");

  message += FPSTR(bonogarfstart);
  char time_str[18];
  timenow(time_str);
  message += time_str;
  message += F(" EST<br>");
  message += FPSTR(bonogarversion);
  message += FPSTR(bonogarfend);
  
  server.send(200, "text/html", message);
}

void handleInfo() {
  String message = FPSTR(bonogarhead);
  message += F("<h1>BonoGarage information</h1><ul><li>SSID: ");
  message += WiFi.SSID();
  // print the received signal strength:
  rssi = WiFi.RSSI();
  message += F("</li><li>signal strength (RSSI): ");
  message += rssi;
  message += F(" dBm </li>");

  // print your WiFi channel:
  message += F("<li>Channel: ");
  message += wifi_get_channel();

  // print your WiFi channel:
  message += F("</li><li>WiFi Mode: ");
  message += PHY_MODE_NAMES[wifi_get_phy_mode()];

  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  message += F("</li><li>IP Address: ");
  message += ip.toString();

  // print your MAC address:
  byte mac[6];
  WiFi.macAddress(mac);
  char macAddr[18];
  sprintf(macAddr, "%2X:%2X:%2X:%2X:%2X:%2X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  message += F("</li><li>MAC address: ");
  message += macAddr;
  message += F("</li>");

  // Show system frequency
  message += F("<li>ESP8266 Frequency: ");
  message += system_get_cpu_freq();
  message += F(" MHz</li>");

  message += F("<li>Dallas sensor temperature resolution: ");
  message += sensors.getResolution(insideThermometer);
  message += F(" bits</li>");

  message += F("<li>Software compiled on: ");
  message += FPSTR(compile_date);
  message += F("</li></ul>");

  message += FPSTR(bonogarfstart);
  char time_str[18];
  timenow(time_str);
  message += time_str;
  message += FPSTR(bonogarTZ);
  message += FPSTR(bonogarversion);
  message += FPSTR(bonogarfend);
  server.send(200, "text/html", message);
}

void handleSwitch() {
  String message = FPSTR(bonogarhead);
  message += F("<h1>BonoGarage Open/Close request</h1>");
  if (server.method() == HTTP_POST) {
    triggerRelay();
    WiFiClientSecure clientssl;
    if (!clientssl.connect(host, httpsPort)) {
      message += F("<h2>Error</h2><p>Garage switch <b>triggered</b> but IFTTT unreachable<br><form action=/switch method=POST><button type=submit style='width: 200;height:50;'>Open/Close garage door</button></form>");

    } else {
      String url = F(IFTTT_TRIGGER_KEY);
      clientssl.print(String("GET ") + url + " HTTP/1.1\r\n" +
                      "Host: " + host + "\r\n" +
                      "User-Agent: ArduinoBonoGarage\r\n" +
                      "Connection: close\r\n\r\n");
      message += F("Garage switch <b>triggered</b> and IFTTT notification sent<br><form action=/switch method=POST><button type=submit style='width: 200;height:50;'>Open/Close garage door</button></form>");
    }
  } else {
    message += F("<p>Invalid action");
  }
  message += FPSTR(bonogarfstart);
  char time_str[18];
  timenow(time_str);
  message += time_str;
  message += FPSTR(bonogarTZ);
  message += FPSTR(bonogarversion);
  message += FPSTR(bonogarfend);
  server.send(200, "text/html", message);
}

void handleNotFound() {
  String message = FPSTR(bonogarhead);
  message += F("File Not Found\n\n");
  message += F("URI: ");
  message += server.uri();
  message += F("\nMethod: ");
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += F("\nArguments: ");
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  message += FPSTR(bonogarfstart);
  char time_str[18];
  timenow(time_str);
  message += time_str;
  message += FPSTR(bonogarTZ);
  message += FPSTR(bonogarversion);
  message += FPSTR(bonogarfend);
  server.send(404, "text/plain", message);
}


// WiFiManager
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println(F("Entered config mode"));
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

// Quickly blink a pin for times time and specified delay
void ledblink(int led_pin, int times, int led_delay) {
  int pinstate = digitalRead(led_pin);
  for (int i = 0; i < times; i++) {
    digitalWrite(led_pin, !pinstate);
    delay(led_delay / 2);
    digitalWrite(led_pin, pinstate);
    delay(led_delay / 2);
  }
}

// turn on and off the relay shield
void triggerRelay() {
  // turn on relay with voltage HIGH
  digitalWrite(relayPin, HIGH);
  delay(200);
  digitalWrite(relayPin, LOW);
}


void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("Message arrived ["));
  Serial.print(topic);
  Serial.print(F("] payload: "));
  for (int i = 0; i < length; i++) {
    char receivedChar = (char)payload[i];
    Serial.println(receivedChar);
    if (receivedChar == '0') {
      triggerRelay();
      Serial.println(F("Close garage - relay closed for 200 msec"));
      ledblink(LED_BUILTIN, 5, 50);
    }
    if (receivedChar == '1') {
      triggerRelay();
      Serial.println(F("Open garage - relay closed for 200 msec"));
      ledblink(LED_BUILTIN, 5, 50);
    }
    Serial.println();
  }
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print(F("Attempting MQTT connection to:"));
    Serial.print(mqtt_server);
    // Attempt to connect
    if (client.connect("BonoGarage-mydevicename")) {
      Serial.println(F(" connected"));
      // Once connected, publish an announcement...
      // client.publish(MQTT_OUT_TOPIC_TEMP, mqttmsg);
      // ... and resubscribe
      client.subscribe(MQTT_IN_TOPIC_GARAGE);
    } else {
      Serial.print(F("failed, rc="));
      Serial.print(client.state());
      Serial.println(F(" try again in 5 seconds"));
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void updateMQTTtemp() {
  if (!client.connected()) {
    reconnectMQTT();
    client.loop();
  }

  //snprintf (mqttmsg, 75, "MQTT message #%ld", ++mqttcounter);
  Serial.print(F("Publish message: "));

  // client.publish("outTopic", mqttmsg);
  // Format of message is: ts;source;unit;value
  // Example: 2016-02-28 17:56:28;dht22_hum;relhum;37.4
  char time_str[18];
  timenow(time_str);
  snprintf (mqttmsg, 75, "%s;%s;%s;%s", time_str, MQTT_OUT_SENSOR_TEMP, MQTT_OUT_UNIT_TEMP, FormattedTemperature);
  Serial.println(mqttmsg);
  client.publish(MQTT_OUT_TOPIC_TEMP, mqttmsg);

  readyForMQTTUpdate = false;
}

void setReadyForMQTTUpdate() {
  // Serial.println(F("Time to send MQTT message:  readyForMQTTUpdate ->o true"));
  // if there is no valid temp reading, we do not send MQTT messages
  readyForMQTTUpdate = validtempreading;
}

float moving_average(float newvalue) {
  // add the new value to the sum, then divide by count
  // celsius_sum and celsius_count are global
  if (++celsius_count > ma_size) {
    celsius_count = 1;
    celsius_sum = 0;
  }
  celsius_sum += newvalue;
  return (celsius_sum / celsius_count);

}

// Called by ticker
void updatetemperature() {
  sensors.requestTemperatures(); // Send the command to get temperatures
  celsius = sensors.getTempCByIndex(0);
  if ((celsius < -126)|(celsius >80)) {
    Serial.println(F("  can't read from OneWire"));
    snprintf(FormattedTemperature, 4, "%s", "n/a");
    validtempreading = false;
  } else {
    Serial.print(F("  Temperature from sensor = "));
    Serial.print(celsius);
    Serial.print(F(" Celsius "));

    celsius_ma = moving_average(celsius);

    Serial.print(F("  MA = "));
    Serial.print(celsius_ma);
    Serial.print(F(" Celsius ["));
    Serial.print(celsius_count);
    Serial.println(F(" readings]"));

    // Serial.print(fahrenheit);
    // Serial.println(" Fahrenheit");

    dtostrf(celsius_ma, 5, 2, FormattedTemperature);
    //dtostrf(humidity,4, 1, FormattedHumidity);
    readyForSensorsUpdate = false;
    validtempreading = true;
  }
}

// Used by Garage ticker

int updategarageposition() {
  int IROpenPositionStatus = digitalRead(IROpenPosition);
  int IRClosedPositionStatus = digitalRead(IRClosedPosition);
  readyForGarageUpdate = false;

  if ((IRClosedPositionStatus == OBSTACLEYES) & (IROpenPositionStatus == OBSTACLENO)) {
    // garage is closed: IRClosedPositionStatus has obstacle, IROpenPositionStatus no obstacle
    return GARAGECLOSED;
    //    Serial.println("IRClosedPositionStatus == OBSTACLEYES AND IROpenPositionStatus == OBSTACLENO");
  } else if ((IRClosedPositionStatus == OBSTACLENO) & (IROpenPositionStatus == OBSTACLENO)) {
    // garage is in between: both open
    return GARAGEMOVING;
    //    Serial.println("IRClosedPositionStatus == OBSTACLENO AND IROpenPositionStatus == OBSTACLENO");
  } else if ((IRClosedPositionStatus == OBSTACLENO) & (IROpenPositionStatus == OBSTACLEYES)) {
    // garage is open: IRClosedPositionStatus has no obstacle, IROpenPositionStatus has obstacle
    return GARAGEOPEN;
    //    Serial.println("IRClosedPositionStatus == OBSTACLENO AND IROpenPositionStatus == OBSTACLEYES");
  } else {
    return GARAGEUNKNOWN;
    Serial.println(F("--GARAGEUNKNOWN--"));
  }
}

// Sensors 

void setReadyForSensorsUpdate() {
  // Serial.println("Time to update sensor values: readyForSensorsUpdate -> true");
  readyForSensorsUpdate = true;
}

// Heartbeat

void setReadyForHeartBeatUpdate() {
  readyForHeartBeatUpdate = true;
}

// Garage

void setReadyForGarageUpdate() {
  //Serial.println("Time to update garage values: readyForGarageUpdate -> true");
  readyForGarageUpdate = true;
}

// Wifi functions

void setReadyForWiFiUpdate() {
  readyForWiFiUpdate = true;
}

void printWifiData() {
  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print(F("IP Address: "));
  Serial.println(ip);

  // print your MAC address:
  byte mac[6];
  WiFi.macAddress(mac);
  Serial.print(F("MAC address: "));
  Serial.print(mac[5], HEX);
  Serial.print(F(":"));
  Serial.print(mac[4], HEX);
  Serial.print(F(":"));
  Serial.print(mac[3], HEX);
  Serial.print(F(":"));
  Serial.print(mac[2], HEX);
  Serial.print(F(":"));
  Serial.print(mac[1], HEX);
  Serial.print(F(":"));
  Serial.println(mac[0], HEX);

}

void printCurrentNet() {
  // print the SSID of the network you're attached to:
  Serial.print(F("SSID: "));
  Serial.println(WiFi.SSID());

  // print the received signal strength:
  rssi = WiFi.RSSI();
  Serial.print(F("signal strength (RSSI):"));
  Serial.println(rssi);

}

///////////////// Initialize /////////////////

void setup() {

  // prepare builtin led
  pinMode(LED_BUILTIN, OUTPUT);  // set onboard LED as output

  // prepare relay
  pinMode(relayPin, OUTPUT);

  // prepare sensors for garage position
  pinMode(IRClosedPosition, INPUT_PULLUP);
  pinMode(IROpenPosition, INPUT_PULLUP);

  //prepare DS
  pinMode(ONE_WIRE_BUS, INPUT);

  // Turn On VCC
  Serial.begin(115200);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  // Uncomment for testing wifi manager
  // wifiManager.resetSettings();
  wifiManager.setAPCallback(configModeCallback);
  //or use this for auto generated name ESP + ChipID
  wifiManager.autoConnect();

  String hostname(HOSTNAME);
  hostname += String(ESP.getChipId(), HEX);
  WiFi.hostname(hostname);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("Connecting to WiFi .. "));
    counter++;
  }

  // you're connected now, so print out the data:
  Serial.print("You're connected to the network");
  printCurrentNet();
  printWifiData();

  // Setup NTP
  configTime(UTC_OFFSET * 3600, 0, NTP_SERVERS);

  // Setup OTA
  Serial.println("Hostname: " + hostname);
  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.onStart([]() {
    Serial.println(F("Start"));
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nEnd"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  // setup mqtt
  client.setServer(mqtt_server, 1883);
  client.setCallback(callbackMQTT);

  // respond via mDNS
  if (MDNS.begin("bonogarage")) {
    Serial.println(F("MDNS responder started"));
  }

  // setup web server
  server.on("/", handleRoot);
  server.on("/info", handleInfo);
  server.on("/switch", handleSwitch);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("HTTP server started"));

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

  // setup Dallas
  Serial.println(F("Dallas Temperature IC Control Library "));
  pinMode(ONE_WIRE_POWER, OUTPUT);
  digitalWrite(ONE_WIRE_POWER, HIGH);
  sensors.begin();
  sensors.getAddress(insideThermometer, 0);
  sensors.setResolution(insideThermometer, 10);
  // report parasite power requirements
  Serial.print(F("Parasite power is: "));
  if (sensors.isParasitePowerMode()) Serial.println("ON");
  else Serial.println("OFF");
  Serial.print(F("Device 0 Resolution: "));
  Serial.print(sensors.getResolution(insideThermometer), DEC);
  Serial.println();

  Serial.println(F("Looping:"));

  // set tickers
  tickerdht.attach(UPDATE_TEMP_INTERVAL_SECS, setReadyForSensorsUpdate);
  tickermqtt.attach(UPDATE_MQTT_INTERVAL_SECS, setReadyForMQTTUpdate);
  tickerwifi.attach(UPDATE_WIFI_INTERVAL_SECS, setReadyForWiFiUpdate);
  tickerheartbeat.attach(UPDATE_HEARTBEAT_INTERVAL_SECS, setReadyForHeartBeatUpdate);
  tickergarage.attach(UPDATE_GARAGEPOSITION_SECS, setReadyForGarageUpdate);
}

///////////////// Main Loop /////////////////

void loop() {
  // put your main code here, to run repeatedly:

  if (readyForHeartBeatUpdate) {
    ledblink(LED_BUILTIN, 2, 200);
    Serial.print(F("."));
    ArduinoOTA.handle();
    readyForHeartBeatUpdate = false;
  }

  // check mttq messages for me
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  if (readyForSensorsUpdate) {
    updatetemperature();
  }

  // Garage sensors
  if (readyForGarageUpdate) {
    GarageStatus = updategarageposition();

    if (lastGarageStatus != GarageStatus) {
      switch (lastGarageStatus) {
        case GARAGECLOSED:
          Serial.print(F("Garage was CLOSED"));
          break;
        case GARAGEMOVING:
          Serial.print(F("Garage was MOVING"));
          break;
        case GARAGEOPEN:
          Serial.print(F("Garage was OPEN"));
          break;
        default:
          Serial.print(F("Garage was UNKNOWN"));
      }
      switch (GarageStatus) {
        case GARAGECLOSED:
          Serial.println(F(" now CLOSED"));
          break;
        case GARAGEMOVING:
          Serial.println(F(" now MOVING"));
          break;
        case GARAGEOPEN:
          Serial.println(F(" now OPEN"));
          break;
        default:
          Serial.println(F(" now UNKNOWN"));
      }
      lastGarageStatus = GarageStatus;
    }
    readyForGarageUpdate=false;
  }

  if (readyForMQTTUpdate ) {
    updateMQTTtemp();
    // force reset of MA
    celsius_count = ma_size;
  }

  if (readyForWiFiUpdate) {
    // print the received signal strength:
    rssi = WiFi.RSSI();
    Serial.print(F("signal strength (RSSI):"));
    Serial.println(rssi);
    readyForWiFiUpdate = false;
  }
  server.handleClient();
}
