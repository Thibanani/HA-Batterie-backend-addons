/*
 * Programme de TEST — valide uniquement la remontee d'information vers
 * le serveur (WiFi + HTTP POST). La partie Bluetooth est "bouchonnee" :
 * mockBatteryReading() renvoie des valeurs simulees a la place d'une
 * vraie lecture BLE, pour tester le reseau et le backend independamment
 * du materiel.
 *
 * Une fois ce test concluant, remplace simplement l'appel a
 * mockBatteryReading() par la vraie lecture BLE (voir
 * test_powerqueen_ble.ino) pour repasser sur des donnees reelles.
 *
 * A FAIRE avant utilisation :
 *   1. Renseigne WIFI_SSID / WIFI_PASSWORD (point d'acces du telephone).
 *   2. Renseigne SERVER_URL (ex. cree un bac a sable sur
 *      https://webhook.site et colle l'URL unique generee, ou mets
 *      directement l'URL de ton backend si deja pret).
 *
 * Dependance (Arduino IDE > Gestionnaire de bibliotheques) :
 *   - "ArduinoJson" (auteur Benoit Blanchon)
 *   HTTPClient et WiFiClientSecure sont fournis avec le coeur ESP32.
 *   Pas besoin de NimBLE-Arduino pour ce sketch.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------- A CONFIGURER ----------------
static const char* WIFI_SSID     = "TON_HOTSPOT";
static const char* WIFI_PASSWORD = "TON_MOT_DE_PASSE";
static const char* SERVER_URL    = "https://batteries.thibaultbansha.ovh/report";
static const char* SERVER_HOST   = "batteries.thibaultbansha.ovh"; // juste le nom d'hote (sans https:// ni chemin), pour le diagnostic DNS
static const char* BATTERY_MAC   = "AA:BB:CC:DD:EE:FF"; // valeur bidon pour le test
static const char* API_KEY       = "change-moi-en-vrai-secret"; // doit correspondre exactement a l'option "api_key" de l'add-on

// Intervalle entre deux envois simules (ms)
static const uint32_t REPORT_INTERVAL_MS = 30000;

// ---------------- Bouchon Bluetooth ----------------
// Remplace la vraie lecture BLE par des valeurs plausibles, legerement
// variables a chaque appel pour simuler un capteur reel.
void mockBatteryReading(float &voltage, float &current, uint16_t &soc,
                         uint16_t &soh, bool &dischargeDisabled) {
  voltage = 13.2 + (random(-20, 20) / 100.0);   // ~13.0 a 13.4 V
  current = -2.5 + (random(-50, 50) / 100.0);   // ~-3.0 a -2.0 A (decharge)
  soc = 70 + random(-5, 5);                     // 65 a 75 %
  soh = 98;
  dischargeDisabled = false;
}

// ---------------- Envoi HTTP ----------------
bool sendReport(float voltage, float current, uint16_t soc, uint16_t soh,
                bool dischargeDisabled) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Pas de WiFi : envoi HTTP ignore.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // OK pour un test rapide ; a remplacer par une
                         // verification de certificat sur le vrai backend.

  Serial.printf("Heap libre avant connexion HTTPS : %u octets\n", ESP.getFreeHeap());

  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  StaticJsonDocument<256> doc;
  doc["battery_mac"] = BATTERY_MAC;
  doc["voltage_v"] = voltage;
  doc["current_a"] = current;
  doc["soc_pct"] = soc;
  doc["soh_pct"] = soh;
  doc["discharge_off"] = dischargeDisabled;
  doc["mock"] = true; // pour reperer facilement les envois de test cote serveur

  String payload;
  serializeJson(doc, payload);

  Serial.println("Envoi HTTP POST : " + payload);
  int statusCode = http.POST(payload);
  Serial.printf("Reponse serveur : code %d\n", statusCode);
  if (statusCode > 0) {
    Serial.println(http.getString());
  } else {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Detail erreur TLS/connexion : %s\n", errBuf);
  }
  http.end();
  return statusCode > 0;
}

void connectWiFi() {
  Serial.printf("Connexion WiFi a %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connecte, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nEchec de connexion WiFi.");
  }
}

// Diagnostic reseau : DNS + test HTTPS vers un hote connu (1.1.1.1),
// pour isoler si le souci est general au reseau ou specifique a SERVER_HOST.
void runNetworkDiagnostics() {
  IPAddress resolvedIP;
  if (WiFi.hostByName(SERVER_HOST, resolvedIP)) {
    Serial.print("DNS resolu pour ");
    Serial.print(SERVER_HOST);
    Serial.print(" -> ");
    Serial.println(resolvedIP);
  } else {
    Serial.print("Echec de resolution DNS pour ");
    Serial.println(SERVER_HOST);
  }

  WiFiClientSecure testClient;
  testClient.setInsecure();
  HTTPClient testHttp;
  testHttp.begin(testClient, "https://1.1.1.1/");
  int testCode = testHttp.GET();
  Serial.printf("Test HTTPS generique vers 1.1.1.1 : code %d\n", testCode);
  testHttp.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));
  WiFi.mode(WIFI_STA);
  connectWiFi();
  runNetworkDiagnostics();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  float voltage, current;
  uint16_t soc, soh;
  bool dischargeDisabled;
  mockBatteryReading(voltage, current, soc, soh, dischargeDisabled);
  sendReport(voltage, current, soc, soh, dischargeDisabled);

  delay(REPORT_INTERVAL_MS);
}
