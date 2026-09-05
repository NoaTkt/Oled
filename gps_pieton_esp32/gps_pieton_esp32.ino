#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>       // Bibliotheque "WiFiManager" par tzapu
#include <Preferences.h>       // Deja fourni par le coeur arduino-esp32 : stockage en memoire flash (NVS)
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <qrcode.h>            // Composant QR code deja fourni par le coeur arduino-esp32 (aucune lib a installer)
#include <math.h>
#include "config.h"

// Branchement : VCC->3V3, GND->GND, DIN->D10, CLK->D8, CS->D3, DC->D2, RST->D1
// Suppose un controleur SSD1306 ; si l'ecran est un SH1106, remplacer le nom du constructeur.
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/D3, /*dc=*/D2, /*reset=*/D1);

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
WiFiManager wifiManager;
Preferences preferences;

int currentDistance = -1;
int currentAngle = 0;
unsigned long lastMsgTime = 0;
uint8_t frameBuffer[128 * 64 / 8];
bool frameReady = false;

// ---------------------------------------------------------------------
// Liste des reseaux WiFi deja utilises, gardee en memoire flash (NVS).
// A chaque demarrage on scanne les reseaux alentour et on essaie ceux
// de cette liste qui sont detectes, plutot qu'un seul reseau "par defaut".
// ---------------------------------------------------------------------
#define MAX_KNOWN_NETWORKS 10
String knownSsid[MAX_KNOWN_NETWORKS];
String knownPass[MAX_KNOWN_NETWORKS];
int knownCount = 0;

void loadKnownNetworks() {
  preferences.begin("wifinets", true); // lecture seule
  String json = preferences.getString("list", "[]");
  preferences.end();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, json)) return;
  knownCount = 0;
  for (JsonObject net : doc.as<JsonArray>()) {
    if (knownCount >= MAX_KNOWN_NETWORKS) break;
    knownSsid[knownCount] = net["s"].as<String>();
    knownPass[knownCount] = net["p"].as<String>();
    knownCount++;
  }
}

void persistKnownNetworks() {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < knownCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["s"] = knownSsid[i];
    o["p"] = knownPass[i];
  }
  String out;
  serializeJson(doc, out);
  preferences.begin("wifinets", false);
  preferences.putString("list", out);
  preferences.end();
}

void rememberNetwork(const String& ssid, const String& password) {
  if (ssid.length() == 0) return;
  for (int i = 0; i < knownCount; i++) {
    if (knownSsid[i] == ssid) {
      knownPass[i] = password; // mot de passe mis a jour si change
      persistKnownNetworks();
      return;
    }
  }
  if (knownCount < MAX_KNOWN_NETWORKS) {
    knownSsid[knownCount] = ssid;
    knownPass[knownCount] = password;
    knownCount++;
  } else {
    // Liste pleine : on decale (on oublie le plus ancien pour garder le nouveau)
    for (int i = 1; i < MAX_KNOWN_NETWORKS; i++) {
      knownSsid[i - 1] = knownSsid[i];
      knownPass[i - 1] = knownPass[i];
    }
    knownSsid[MAX_KNOWN_NETWORKS - 1] = ssid;
    knownPass[MAX_KNOWN_NETWORKS - 1] = password;
  }
  persistKnownNetworks();
}

// ---------------------------------------------------------------------
// Ecran affiche pendant la tentative de connexion au dernier reseau connu,
// avant de basculer sur le point d'acces de configuration si ca echoue.
// ---------------------------------------------------------------------
void showConnectingScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Tentative de");
  u8g2.drawStr(0, 22, "connexion...");
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------
// Ecran affiche pendant que l'ESP32 attend d'etre configure : il a cree
// son propre point d'acces et attend qu'on s'y connecte depuis un telephone.
// ---------------------------------------------------------------------
void showConfigScreen(WiFiManager* wm) {
  String apName = wm->getConfigPortalSSID();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Config WiFi");
  u8g2.drawStr(0, 22, apName.c_str());
  String mdp = "Mdp: " + String(AP_PASSWORD);
  u8g2.drawStr(0, 34, mdp.c_str());
  u8g2.drawStr(0, 46, "Sinon ouvrez :");
  u8g2.drawStr(0, 58, "192.168.4.1");
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------
// Ecran affiche une fois connecte au vrai WiFi : QR code + lien vers la page web.
// L'encodage QR est fait par esp_qrcode_generate() (deja inclus dans le
// coeur arduino-esp32), qui appelle qrDisplayCallback() une fois pret.
// ---------------------------------------------------------------------
String pendingUrl; // URL en attente d'affichage a cote du QR code

void qrDisplayCallback(esp_qrcode_handle_t qrcode) {
  const int size = esp_qrcode_get_size(qrcode);
  int scale = 64 / size;      // adapte automatiquement la taille des pixels a l'ecran 64px de haut
  if (scale < 1) scale = 1;
  const int qrPx = size * scale;
  const int yOff = (64 - qrPx) / 2;

  u8g2.clearBuffer();
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        u8g2.drawBox(x * scale, yOff + y * scale, scale, scale);
      }
    }
  }

  // Texte a droite du QR uniquement s'il reste assez de place (ecran 128px de large)
  if (qrPx + 30 < 128) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(qrPx + 4, 10, "Connecte !");
    u8g2.drawStr(qrPx + 4, 22, "Ouvrez :");
    const int lineLen = 11;
    int ty = 34;
    for (int i = 0; i < (int)pendingUrl.length() && ty < 62; i += lineLen) {
      u8g2.drawStr(qrPx + 4, ty, pendingUrl.substring(i, i + lineLen).c_str());
      ty += 10;
    }
  }
  u8g2.sendBuffer();
}

void showQrScreen(const char* url) {
  pendingUrl = url;
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrDisplayCallback;
  cfg.max_qrcode_version = 10; // plafond ; la plus petite version qui contient l'URL est choisie automatiquement
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, url);
}

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

// Scanne les reseaux alentour et essaie ceux qui sont deja connus (liste NVS).
// Retourne true des qu'une connexion reussit.
bool tryKnownNetworks() {
  if (knownCount == 0) return false;

  int found = WiFi.scanNetworks();
  if (found <= 0) return false;

  for (int i = 0; i < found; i++) {
    String seenSsid = WiFi.SSID(i);
    for (int k = 0; k < knownCount; k++) {
      if (knownSsid[k] != seenSsid) continue;

      WiFi.begin(seenSsid.c_str(), knownPass[k].c_str());
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
        delay(200);
      }
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.scanDelete();
        return true;
      }
    }
  }
  WiFi.scanDelete();
  return false;
}

void connectWifi() {
  showConnectingScreen();
  loadKnownNetworks();

  if (tryKnownNetworks()) return; // deja connecte a un reseau connu

  // Aucun reseau connu detecte / injoignable : on ouvre le point d'acces de
  // configuration directement (pas besoin de retenter l'ancien reseau, deja fait ci-dessus).
  wifiManager.setAPCallback(showConfigScreen);
  // Si personne ne configure sous 3 min, on redemarre pour reessayer plus tard.
  wifiManager.setConfigPortalTimeout(180);

  if (!wifiManager.startConfigPortal(AP_SSID, AP_PASSWORD)) {
    ESP.restart(); // timeout sans configuration : on repart de zero
    return;
  }

  // Nouveau reseau configure via le portail : on le retient pour la prochaine fois
  rememberNetwork(WiFi.SSID(), wifiManager.getWiFiPass());
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
  // Reste affiche tant que la page web n'a pas encore envoye sa premiere trame
  // (voir loop() : l'ecran n'est redessine que lorsque frameReady passe a true).
  showQrScreen(HOSTED_URL);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi(); // relance le portail de config si la connexion WiFi est perdue
  }
  if (!mqttClient.connected()) connectMqtt();
  mqttClient.loop();

  if (frameReady) {
    frameReady = false;
    u8g2.clearBuffer();
    u8g2.drawXBMP(0, 0, 128, 64, frameBuffer);
    u8g2.sendBuffer();
  }
}
