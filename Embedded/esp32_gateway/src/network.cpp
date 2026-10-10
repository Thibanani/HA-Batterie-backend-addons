/*
 * Module reseau : connexion a internet et dialogue HTTP avec le serveur
 * (add-on Home Assistant). Actuellement en WiFi ; le mode GSM/LTE-M viendra
 * remplacer networkIsConnected() / networkConnect() et le client de postReport().
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "network.h"
#include "debug.h"
#include "config.h"

bool networkIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool networkConnect() {
  if (networkIsConnected()) return true;
  DEBUG_PRINTF("Connexion WiFi a %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (!networkIsConnected() && millis() - start < 20000) {
    delay(500);
    DEBUG_PRINT(".");
  }
  if (networkIsConnected()) {
    DEBUG_PRINTLN("\nWiFi connecte, IP: " + WiFi.localIP().toString());
    return true;
  }
  DEBUG_PRINTLN("\nEchec de connexion WiFi.");
  return false;
}

// Extrait la derniere commande de decharge de {"commands": [...]}.
Cmd parseCommand(const String &body) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) {
    DEBUG_PRINTLN("[HTTP] Reponse illisible.");
    return Cmd::None;
  }
  Cmd result = Cmd::None;
  for (JsonVariant v : doc["commands"].as<JsonArray>()) {
    const char* action = v.as<const char*>();
    if (action == nullptr) continue;
    if (strcmp(action, "discharge_off") == 0) {
      result = Cmd::DischargeOff;
    } else if (strcmp(action, "discharge_on") == 0) {
      result = Cmd::DischargeOn;
    } else {
      DEBUG_PRINTF("[HTTP] Commande inconnue ignoree : %s\n", action);
    }
  }
  return result;
}

// Envoie un rapport ; renvoie true si le serveur a repondu 200.
// cmdOut recoit la commande eventuellement renvoyee par le serveur.
bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut) {
  cmdOut = Cmd::None;
  if (!networkIsConnected()) {
    DEBUG_PRINTLN("[HTTP] Pas de reseau : rapport ignore.");
    return false;
  }

  WiFiClientSecure client;
  if (SERVER_ROOT_CA != nullptr) {
    client.setCACert(SERVER_ROOT_CA);   // chaine de certificats ET nom d'hote verifies
  } else {
    client.setInsecure();
    DEBUG_PRINTLN("[HTTP] ATTENTION : certificat du serveur NON verifie (SERVER_ROOT_CA = nullptr).");
  }

  HTTPClient http;
  if (!http.begin(client, SERVER_URL)) {
    DEBUG_PRINTLN("[HTTP] URL du serveur invalide.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  StaticJsonDocument<256> doc;
  doc["battery_mac"]   = cfg.reportId;
  doc["voltage_v"]     = r.voltage_v;
  doc["current_a"]     = r.current_a;
  doc["soc_pct"]       = r.soc;
  doc["soh_pct"]       = r.soh;
  doc["discharge_off"] = r.dischargeOff;

  String payload;
  serializeJson(doc, payload);
  DEBUG_PRINTLN("[HTTP] POST " + payload);

  int code = http.POST(payload);
  DEBUG_PRINTF("[HTTP] Reponse : %d\n", code);
  if (code != 200) {
#ifdef DEBUG_ENABLE
    if (code <= 0) {
      char errBuf[128];
      client.lastError(errBuf, sizeof(errBuf));
      DEBUG_PRINTF("[HTTP] Detail : %s\n", errBuf);
    }
#endif
    http.end();
    return false;
  }

  // Reponse attendue : {"commands": [...]}, quelques dizaines d'octets.
  // Une reponse plus grande n'est pas lue (protection de la memoire).
  int size = http.getSize();
  if (size > (int)MAX_RESPONSE_SIZE) {
    DEBUG_PRINTF("[HTTP] Reponse trop grande (%d octets) : ignoree.\n", size);
    http.end();
    return true;   // rapport accepte (200), sans commande exploitable
  }

  String body = http.getString();
  http.end();
  DEBUG_PRINTLN("[HTTP] " + body);
  cmdOut = parseCommand(body);
  return true;
}
