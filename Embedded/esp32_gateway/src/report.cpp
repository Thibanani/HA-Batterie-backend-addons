/*
 * Rapport au serveur en HTTPS : voir report.h.
 *
 * Pile, identique pour tous les transports :
 *   networkTcpClient()  client TCP brut (WiFi ou modem, network_*.cpp)
 *   SSLClient           TLS mbedtls sur l'ESP32 : racines de root_ca.h, SNI et nom d'hote verifies
 *   BufferedClient      la requete part en un seul envoi
 *   ArduinoHttpClient   HTTP/1.1 (Content-Length ou chunked)
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <SSLClient.h>
#include "report.h"
#include "network.h"
#include "buffered_client.h"
#include "debug.h"
#include "config.h"

// Meme User-Agent que l'ancien client HTTP du coeur ESP32, deja accepte par Cloudflare.
static const char* USER_AGENT = "ESP32HTTPClient";

static String buildPayload(const BatteryConfig &cfg, const BatteryReading &r) {
  StaticJsonDocument<256> doc;
  doc["battery_mac"]   = cfg.reportId;
  doc["voltage_v"]     = r.voltage_v;
  doc["current_a"]     = r.current_a;
  doc["soc_pct"]       = r.soc;
  doc["soh_pct"]       = r.soh;
  doc["discharge_off"] = r.dischargeOff;
  String payload;
  serializeJson(doc, payload);
  return payload;
}

// Extrait la derniere commande de decharge de {"commands": [...]}.
static Cmd parseCommand(const String &body) {
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

bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut) {
  cmdOut = Cmd::None;
  if (!networkIsConnected()) {
    DEBUG_PRINTLN("[HTTP] Pas de reseau : rapport ignore.");
    return false;
  }

  SSLClient tls(&networkTcpClient());
  // Pas de tls.setTimeout() : SSLClient attendrait que le client TCP annonce tout
  // l'enregistrement TLS d'un coup, ce que le client du modem (TinyGSM, tampon de
  // 1 Ko) ne fait jamais : blocage puis coupure par le serveur. Sans delai, il lit
  // ce qui est arrive et mbedtls assemble les morceaux.
  tls.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);   // en secondes
  if (SERVER_ROOT_CA != nullptr) {
    tls.setCACert(SERVER_ROOT_CA);           // chaine de certificats ET nom d'hote verifies
  } else {
    tls.setInsecure();
    DEBUG_PRINTLN("[HTTP] ATTENTION : certificat du serveur NON verifie (SERVER_ROOT_CA = nullptr).");
  }
  BufferedClient<512> out(tls);
  HttpClient http(out, SERVER_HOST, SERVER_PORT);
  http.setHttpResponseTimeout(HTTP_TIMEOUT_MS);
  http.noDefaultRequestHeaders();            // Host et User-Agent ecrits ci-dessous

  String payload = buildPayload(cfg, r);
  DEBUG_PRINTLN("[HTTP] POST " + payload);

  uint32_t start = millis();
  http.beginRequest();
  int err = http.post(SERVER_PATH);
  if (err != HTTP_SUCCESS) {
    DEBUG_PRINTF("[HTTP] Connexion au serveur impossible (%d).\n", err);
#ifdef DEBUG_ENABLE
    char errBuf[128];
    tls.lastError(errBuf, sizeof(errBuf));
    DEBUG_PRINTF("[HTTP] Detail TLS : %s\n", errBuf);
#endif
    http.stop();
    return false;
  }
  http.sendHeader("Host", SERVER_HOST);
  http.sendHeader("User-Agent", USER_AGENT);
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", (int)payload.length());
  http.sendHeader("X-API-Key", API_KEY);
  http.beginBody();
  http.print(payload);
  http.endRequest();

  int code = http.responseStatusCode();
  DEBUG_PRINTF("[HTTP] Reponse : %d (%u ms)\n", code, (unsigned)(millis() - start));
  if (code != 200) {
    http.stop();
    return false;
  }

  // Reponse attendue : {"commands": [...]}, quelques dizaines d'octets.
  // Une reponse plus grande n'est pas lue (protection de la memoire).
  long size = http.contentLength();          // -1 si inconnue (reponse chunked)
  if (size > (long)MAX_RESPONSE_SIZE) {
    DEBUG_PRINTF("[HTTP] Reponse trop grande (%ld octets) : ignoree.\n", size);
    http.stop();
    return true;   // rapport accepte (200), sans commande exploitable
  }
  String body = http.responseBody();
  http.stop();
  DEBUG_PRINTLN("[HTTP] " + body);
  if (body.length() > MAX_RESPONSE_SIZE) return true;
  cmdOut = parseCommand(body);
  return true;
}
