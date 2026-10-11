/*
 * Module reseau : connexion a internet et dialogue HTTP avec le serveur
 * (add-on Home Assistant). Actuellement en WiFi ; le mode GSM/LTE-M viendra
 * remplacer networkIsConnected() / networkConnect() et le client de postReport().
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
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
  WiFi.persistent(false);   // n'ecrit pas les identifiants en flash a chaque reveil
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (!networkIsConnected() && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);             // scrutation fine : la radio s'arrete des que possible
  }
  if (networkIsConnected()) {
    DEBUG_PRINTF("WiFi connecte en %u ms, IP : %s\n", (unsigned)(millis() - start),
                 WiFi.localIP().toString().c_str());
    return true;
  }
  DEBUG_PRINTLN("Echec de connexion WiFi.");
  networkStop();
  return false;
}

void networkStop() {
  // Avant un deep sleep, arreter la radio suffit (la memoire est perdue de toute facon).
  // WiFi.mode(WIFI_OFF) libere aussi le pilote, ce qui expire juste apres une deconnexion
  // ("wifi:timeout when WiFi un-init").
  if (WiFi.getMode() == WIFI_OFF) return;   // WiFi jamais demarre pendant ce cycle
  esp_wifi_disconnect();
  esp_wifi_stop();
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
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);   // en secondes ; 120 s par defaut
  if (SERVER_ROOT_CA != nullptr) {
    client.setCACert(SERVER_ROOT_CA);   // chaine de certificats ET nom d'hote verifies
  } else {
    client.setInsecure();
    DEBUG_PRINTLN("[HTTP] ATTENTION : certificat du serveur NON verifie (SERVER_ROOT_CA = nullptr).");
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
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
