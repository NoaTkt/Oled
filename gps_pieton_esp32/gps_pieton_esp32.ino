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
bool frameMode = false;
unsigned long lastFrameTime = 0;

void drawArrow(int cx, int cy, int len, float angleDeg) {
  float rad = angleDeg * PI / 180.0;
  // 0° = flèche vers le haut de l'écran
  int tipX = cx + len * sin(rad);
  int tipY = cy - len * cos(rad);
  float r1 = (angleDeg + 150) * PI / 180.0;
  float r2 = (angleDeg - 150) * PI / 180.0;
  int b1X = cx + (len*0.6) * sin(r1), b1Y = cy - (len*0.6) * cos(r1);
  int b2X = cx + (len*0.6) * sin(r2), b2Y = cy - (len*0.6) * cos(r2);
  u8g2.drawTriangle(tipX, tipY, b1X, b1Y, b2X, b2Y);
}

void updateDisplay() {
  u8g2.clearBuffer();
  if (currentDistance < 0) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(10, 32, "En attente...");
  } else {
    drawArrow(64, 26, 20, currentAngle);
    u8g2.setFont(u8g2_font_7x14_tf);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d m", currentDistance);
    int w = u8g2.getStrWidth(buf);
    u8g2.drawStr(64 - w/2, 58, buf);
  }
  u8g2.sendBuffer();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_FRAME_TOPIC) == 0) {
    if (length == sizeof(frameBuffer)) {
      memcpy(frameBuffer, payload, sizeof(frameBuffer));
      frameReady = true;
      frameMode = true;
      lastFrameTime = millis();
    }
    return;
  }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, length)) return;
  currentDistance = doc["d"] | -1;
  currentAngle = doc["a"] | 0;
  lastMsgTime = millis();
  if (!frameMode) updateDisplay();
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
  updateDisplay();
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

  // Les trames de l'éditeur ont priorité sur le rendu GPS intégré.
  // Si l'éditeur s'arrête, le mode GPS autonome reprend après un court délai.
  if (frameMode && millis() - lastFrameTime > 2000) {
    frameMode = false;
    updateDisplay();
  }

  // Si aucune donnée reçue depuis 10s, on repasse en "en attente"
  if (!frameMode && currentDistance >= 0 && millis() - lastMsgTime > 10000) {
    currentDistance = -1;
    updateDisplay();
  }
}
