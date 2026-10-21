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
  // 2 decimales suffisent (10 mV, 10 mA) : "13.24" au lieu de "13.23799992".
  doc["voltage_v"]     = serialized(String(r.voltage_v, 2));
  doc["current_a"]     = serialized(String(r.current_a, 2));
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

// Une seule connexion TLS par cycle : les rapports suivants (autres batteries,
// confirmation apres une commande) la reutilisent au lieu de refaire une
// negociation complete (~4 Ko de data, surtout la chaine de certificats).
// Elle n'est pas fermee en fin de cycle : l'extinction du modem suffit et evite
// l'echange de fermeture TLS + TCP.
static SSLClient           g_tls;
static BufferedClient<512> g_out(g_tls);
static HttpClient          g_http(g_out, SERVER_HOST, SERVER_PORT);
static bool                g_httpReady = false;

static void setupHttp() {
  if (g_httpReady) return;
  g_tls.setClient(&networkTcpClient());
  // Pas de g_tls.setTimeout() : SSLClient attendrait que le client TCP annonce tout
  // l'enregistrement TLS d'un coup, ce que le client du modem (TinyGSM, tampon de
  // 1 Ko) ne fait jamais : blocage puis coupure par le serveur. Sans delai, il lit
  // ce qui est arrive et mbedtls assemble les morceaux.
  g_tls.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);   // en secondes
  if (SERVER_ROOT_CA != nullptr) {
    g_tls.setCACert(SERVER_ROOT_CA);         // chaine de certificats ET nom d'hote verifies
  } else {
    g_tls.setInsecure();
    DEBUG_PRINTLN("[HTTP] ATTENTION : certificat du serveur NON verifie (SERVER_ROOT_CA = nullptr).");
  }
  g_http.setHttpResponseTimeout(HTTP_TIMEOUT_MS);
  g_http.noDefaultRequestHeaders();          // Host et User-Agent ecrits ci-dessous
  g_http.connectionKeepAlive();              // reconnexion automatique si le serveur a ferme
  g_httpReady = true;
}

enum class SendResult : uint8_t { Ok, Refused, LinkError };

// Une requete POST ; Refused = reponse HTTP autre que 200, LinkError = echec
// de connexion, d'envoi ou de lecture (connexion fermee, delai depasse...).
static SendResult sendOnce(const String &payload, Cmd &cmdOut) {
  HttpClient &http = g_http;
  uint32_t start = millis();
  http.beginRequest();
  int err = http.post(SERVER_PATH);
  if (err != HTTP_SUCCESS) {
    DEBUG_PRINTF("[HTTP] Connexion au serveur impossible (%d).\n", err);
#ifdef DEBUG_ENABLE
    char errBuf[128];
    g_tls.lastError(errBuf, sizeof(errBuf));
    DEBUG_PRINTF("[HTTP] Detail TLS : %s\n", errBuf);
#endif
    http.stop();
    return SendResult::LinkError;
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
    return code < 0 ? SendResult::LinkError : SendResult::Refused;
  }

  // Reponse attendue : {"commands": [...]}, quelques dizaines d'octets.
  // Une reponse plus grande n'est pas lue (protection de la memoire).
  long size = http.contentLength();          // -1 si inconnue (reponse chunked)
  if (size > (long)MAX_RESPONSE_SIZE) {
    DEBUG_PRINTF("[HTTP] Reponse trop grande (%ld octets) : ignoree.\n", size);
    http.stop();
    return SendResult::Ok;                   // rapport accepte, sans commande exploitable
  }
  String body = http.responseBody();         // connexion gardee pour le rapport suivant
  DEBUG_PRINTLN("[HTTP] " + body);
  if (body.length() <= MAX_RESPONSE_SIZE) cmdOut = parseCommand(body);
  return SendResult::Ok;
}

bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut) {
  cmdOut = Cmd::None;
  if (!networkIsConnected()) {
    DEBUG_PRINTLN("[HTTP] Pas de reseau : rapport ignore.");
    return false;
  }
  setupHttp();

  String payload = buildPayload(cfg, r);
  DEBUG_PRINTLN("[HTTP] POST " + payload);

  // Le serveur a pu fermer la connexion gardee depuis le rapport precedent :
  // dans ce cas seulement, un second essai sur une connexion neuve.
  bool reused = g_out.connected();
  SendResult res = sendOnce(payload, cmdOut);
  if (res == SendResult::LinkError && reused) {
    DEBUG_PRINTLN("[HTTP] Connexion gardee perdue : nouvel essai.");
    res = sendOnce(payload, cmdOut);
  }
  return res == SendResult::Ok;
}
