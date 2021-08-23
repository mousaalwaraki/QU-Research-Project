#include <pgmspace.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AMG88xx.h>
#include <ArduinoJson.h>
#include <FirebaseArduino.h>
#include <WiFiUdp.h>

Adafruit_AMG88xx amg;
#define AMGSAMPLING 1000

// 8X8 Pixels camera so 8 rows, 8 columns
#define AMG_COLS 8
#define AMG_ROWS 8

// Firebase project host link and Auth link
#define FIREBASE_HOST "esp8266*******************.com"
#define FIREBASE_AUTH "YfT8**********************Reyb"

float pixels[AMG_COLS * AMG_ROWS];

// Wifi you're connected to 
const char* ssid = "SSID";
const char* password = "PASSWORD";

// MQTT Server 
const char* mqtt_server = "192.168.4.2"; 

// MQTT topic to subscribe and post to
const char* clientID = "thermal";
const char* topicStatus = "/thermal/status";
const char* topicThermal = "/thermal/thermal";
const char* topicThermalHigh = "/thermal/thermal/high";
const char* topicThermalLow = "/thermal/thermal/low";
const char* topicThermalAverage = "/thermal/thermal/average";

bool firstLoop;
const int pin_led = 16;
int numberOfTimes;
String numberOfTimesString;
int intRet;
int numberOfLoops = 0;
String readings;
unsigned long lastStatus, lastAMG;
int vSTATUSINTERVAL, vAMGSAMPLING;

WiFiClient espClient;
PubSubClient mqtt(mqtt_server, 1883, 0, espClient);
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);

// Websocket connecting function
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      break;
  }
}

void toggle() {
  static bool last_led = false;
  last_led = !last_led;
}

void handleRoot() {
  auto ip = WiFi.localIP();
  String ip_str = String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
  server.send(200, "text/html", String(ws_html_1()) +  ip_str + ws_html_2());
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// WiFi connecting function
void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
      case WIFI_EVENT_STAMODE_DISCONNECTED:
        Serial.println("WiFi lost connection: reconnecting...");
        WiFi.begin();
        break;
      case WIFI_EVENT_STAMODE_CONNECTED:
        Serial.print("Connected to ");
        Serial.println(ssid);
        break;
      case WIFI_EVENT_STAMODE_GOT_IP:
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        if (MDNS.begin("esp8266-amg8833")) {
          Serial.println("MDNS responder started");
        }
        enableOTA();
        break;
    }
}

void enableOTA() {
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
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
}

void setup(void) {
  pinMode(pin_led, OUTPUT);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(WiFiEvent);
  WiFi.begin(ssid, password);

  server.on("/", handleRoot);

  server.on("/current", [](){
    String str;
    server.send(200, "text/plain", get_current_values_str(str));
  });

  server.onNotFound(handleNotFound);

  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  Serial.println("HTTP server started");

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Set up the MQTT server connection
  if (mqtt_server!="") {
    mqtt.setServer(mqtt_server, 1883);
    mqtt.setBufferSize(512);
    mqtt.setCallback(callback);
  }

  if (!amg.begin()) {
    Serial.println("Could not find a valid AMG88xx sensor, check wiring!");
    while (1) { delay(1); }
  }

  vAMGSAMPLING = AMGSAMPLING;
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  amg.begin(0x69);
  delay(100); // let sensor boot up
}

// Function to reconnect to WiFi
void reconnect() {
  //String mytopic;
  // Loop until we're reconnected
  while (!mqtt.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect(clientID)) {
      Serial.println(F("connected"));
      // ... and resubscribe
      //mqtt.subscribe(topicSleep);
    } else {
      Serial.print(F("failed, rc="));
      Serial.print(mqtt.state());
      Serial.println(F(" try again in 5 seconds"));
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  // Convert the incoming byte array to a string
  String strTopic = String((char*)topic);
  payload[length] = '\0'; // Null terminator used to terminate the char array
  String message = (char*)payload;

  Serial.print(F("Message arrived on topic: ["));
  Serial.print(topic);
  Serial.print(F("], "));
  Serial.println(message);
}

void sendStatus() {
  //Serial.print("Status | RSSI: ");
  //Serial.print(WiFi.RSSI());
  //Serial.print(", Uptime: ");
  //Serial.println(millis()/ 60000);
  //Serial.println();

String mqttStat = "";
  mqttStat = "{\"rssi\":";
  mqttStat += WiFi.RSSI();
  mqttStat += ",\"uptime\":";
  mqttStat += millis()/ 60000;
  mqttStat += "}";
  if (mqtt_server!="") {
    mqtt.publish(topicStatus, mqttStat.c_str());
  }
}

// MQTT function to send image and values to topics
void sendAMGImage() {
  //read all the pixels
  String image = "";
  float thermalHigh = 0;
  float thermalLow = 50.0;
  float thermalTotal = 0;
  float thermalAverage = 0;
  amg.readPixels(pixels);  
  for(int i=1; i<=AMG88xx_PIXEL_ARRAY_SIZE; i++){
    image = image + pixels[i-1] + ",";
    //if( i%8 == 0 ) Serial.println();
    if ((pixels[i]) > thermalHigh) { thermalHigh = pixels[i]; }
    Serial.println(i);
    if (((pixels[i]) < thermalLow) && (i < 64)) { thermalLow = pixels[i]; }
    thermalTotal += pixels[i];
  }
  image = image.substring(0, image.length() - 1);
  thermalAverage = thermalTotal/64;
  if (mqtt_server!="") {
    mqtt.publish(topicThermal, image.c_str());
    mqtt.publish(topicThermalHigh, String(thermalHigh).c_str());
    mqtt.publish(topicThermalLow, String(thermalLow).c_str());
    mqtt.publish(topicThermalAverage, String(thermalAverage).c_str());
  } 
}

void loop(void) {
  ArduinoOTA.handle();
  server.handleClient();
  webSocket.loop();

  // Wait for connection
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long last_ms;
    unsigned long t = millis();
    if (t - last_ms > 500) {
      Serial.print(".");
      toggle();
      last_ms = t;
    }
  } else {

  }

  static unsigned long last_read_ms = millis();
  unsigned long now = millis();
  if (now - last_read_ms > 100) {
    last_read_ms += 100;
    String str;
    numberOfLoops += 1;
    get_current_values_str(str);
    webSocket.broadcastTXT(str);
  }

  if (mqtt_server!="") {
    if (!mqtt.connected()) {
      reconnect();
    }
    mqtt.loop();
  }
  if (millis()-lastStatus >= vSTATUSINTERVAL) {
    lastStatus=millis();
    sendStatus();
  }
  if (millis()-lastAMG >= vAMGSAMPLING) {
    lastAMG=millis();
    sendAMGImage();
  }
}

String& get_current_values_str(String& ret) {
  float pixels[AMG88xx_PIXEL_ARRAY_SIZE];
  amg.readPixels(pixels);
  ret = "[";
  firstLoop = true;
  for(int i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    if( i % 8 == 0 ) ret += "\r\n";
    ret += pixels[i];
    if (i != AMG88xx_PIXEL_ARRAY_SIZE - 1) ret += ", ";
  }
  ret += "\r\n]\r\n";
  return ret;
}


int& firebaseFunction() {
  float pixels[AMG88xx_PIXEL_ARRAY_SIZE];
  amg.readPixels(pixels);
  firstLoop = true;
  for(int i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    intRet = int(pixels[i]);
    if ((intRet > 30) && (firstLoop = true)) {
      firstLoop = false;
      numberOfTimes += 1;
      return numberOfTimes;
    } else if ((i == 63) && (firstLoop == true)) {
      numberOfTimes = 0;
      return numberOfTimes;
    }
  }
}


const __FlashStringHelper* ws_html_1() {
  return F("<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<title>thermo</title>\n"
    "<style>\n"
    "body {\n"
    "    background-color: #667;\n"
    "}\n"
    "table#tbl td {\n"
    "    width: 64px;\n"
    "    height: 64px;\n"
    "    border: solid 1px grey;\n"
    "    text-align: center;\n"
    "}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<table border id=\"tbl\"></table>\n"
    "<script>\n"
    "function bgcolor(t) {\n"
    "    if (t < 0) t = 0;\n"
    "    if (t > 30) t = 30;\n"
    "    return \"hsl(\" + (360 - t * 12) + \", 100%, 80%)\";\n"
    "}\n"
    "\n"
    "var t = document.getElementById('tbl');\n"
    "var tds = [];\n"
    "for (var i = 0; i < 8; i++) {\n"
    "    var tr = document.createElement('tr');\n"
    "    for (var j = 0; j < 8; j++) {\n"
    "        var td = tds[i*8 + 7 - j] = document.createElement('td');\n"
    "        tr.appendChild(td);\n"
    "    }\n"
    "    t.appendChild(tr);\n"
    "}\n"
    "var connection = new WebSocket('ws://");
}

const __FlashStringHelper* ws_html_2() {
  return F(":81/');\n"
    "connection.onmessage = function(e) {\n"
    "    const data = JSON.parse(e.data);\n"
    "    for (var i = 0; i < 64; i++) {\n"
    "        tds[i].innerHTML = data[i].toFixed(2);\n"
    "        tds[i].style.backgroundColor = bgcolor(data[i]);\n"
    "    }\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n");
}
