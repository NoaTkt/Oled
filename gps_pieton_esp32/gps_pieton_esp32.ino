#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <math.h>
#include "config.h"

// Branchement : VCC->3V3, GND->GND, DIN->D10, CLK->D8, CS->D3, DC->D2, RST->D1
// Suppose un contrôleur SSD1306 ; si l'écran est un SH1106, remplacer le nom du constructeur.
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/D3, /*dc=*/D2, /*reset=*/D1);

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

int currentDistance = -1;
int currentAngle = 0;
unsigned long lastMsgTime = 0;
uint8_t frameBuffer[128 * 64 / 8];
bool frameReady = false;

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_FRAME_TOPIC) == 0) {
    if (length == sizeof(frameBuffer)) {
      memcpy(frameBuffer, payload, sizeof(frameBuffer));
      frameReady = true;
    }
    return;
  }

  // Le GPS sert aux donnees de navigation; l OLED est rendu par les trames binaires.
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, length)) return;
  currentDistance = doc["d"] | -1;
  currentAngle = doc["a"] | 0;
  lastMsgTime = millis();
}

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
}

void connectMqtt() {
  espClient.setInsecure(); // acceptable pour un broker public de test
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1400);
  while (!mqttClient.connected()) {
    String clientId = "xiao-gps-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      mqttClient.subscribe(MQTT_TOPIC);
      mqttClient.subscribe(MQTT_FRAME_TOPIC);
    } else {
      delay(2000);
    }
  }
}

void setup() {
  u8g2.begin();
  connectWifi();
  connectMqtt();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
}

void loop() {
  if (!mqttClient.connected()) connectMqtt();
  mqttClient.loop();

  if (frameReady) {
    frameReady = false;
    u8g2.clearBuffer();
    u8g2.drawXBMP(0, 0, 128, 64, frameBuffer);
    u8g2.sendBuffer();
  }

}
