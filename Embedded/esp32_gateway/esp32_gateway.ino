/*
 * PASSERELLE ESP32 — lit la batterie en BLE, remonte les donnees au serveur
 * (add-on Home Assistant) et applique les commandes recues.
 *
 * C'est l'evolution du sketch "mock" : la lecture simulee est remplacee par
 * une vraie lecture BLE, et la commande de coupure/retablissement de la
 * decharge est vraiment envoyee a la batterie (ou au simulateur).
 *
 * Cycle execute toutes les CYCLE_INTERVAL_MS :
 *   1. lecture BLE de chaque batterie (connexion, requete de statut, deconnexion)
 *   2. POST /report au serveur pour chaque batterie ; la reponse peut
 *      contenir une commande ("discharge_off" / "discharge_on")
 *   3. si une commande est recue : connexion BLE, ecriture de la commande,
 *      relecture du statut pour verifier, puis nouveau rapport de
 *      confirmation (Home Assistant se met a jour sans attendre le cycle suivant)
 *
 * A FAIRE avant utilisation :
 *   1. WIFI_SSID / WIFI_PASSWORD / SERVER_URL / API_KEY (comme le sketch mock).
 *   2. BATTERIES[] : bleAddress = adresse BLE affichee par le simulateur (ou
 *      celle de la vraie batterie, en MAJUSCULES). reportId = identifiant
 *      declare dans BATTERIES de l'add-on (main.py) : garde
 *      "AA:BB:CC:DD:EE:FF" pour continuer a alimenter "Batterie 1" dans HA
 *      sans modifier l'add-on.
 *
 * Dependances (Arduino IDE > Gestionnaire de bibliotheques) :
 *   - "NimBLE-Arduino" (auteur h2zero), VERSION 2.x (la derniere proposee).
 *     Le code utilise l'API 2.x ; avec une 1.4 il ne compilera pas.
 *   - "ArduinoJson" (auteur Benoit Blanchon)
 *   HTTPClient et WiFiClientSecure sont fournis avec le coeur ESP32.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include "../bms_protocol.h"

// ---------------- A CONFIGURER ----------------
static const char* WIFI_SSID     = "TON_WIFI";
static const char* WIFI_PASSWORD = "TON_MOT_DE_PASSE";
static const char* SERVER_URL    = "https://batteries.thibaultbansha.ovh/report";
static const char* API_KEY       = "TON_API_KEY"; // identique a l'option "api_key" de l'add-on

struct BatteryConfig {
  const char* bleAddress;   // adresse BLE de la batterie / du simulateur (MAJUSCULES)
  uint8_t     bleAddrType;  // BLE_ADDR_PUBLIC (par defaut) ou BLE_ADDR_RANDOM si la connexion echoue
  const char* reportId;     // identifiant envoye au serveur (cle de BATTERIES dans l'add-on)
};

static const BatteryConfig BATTERIES[] = {
  { "AA:BB:CC:DD:EE:01", BLE_ADDR_PUBLIC, "AA:BB:CC:DD:EE:FF" },   // -> "Batterie 1" (remplace par l'adresse du simulateur)
  // { "AA:BB:CC:DD:EE:02", BLE_ADDR_PUBLIC, "11:22:33:44:55:66" }, // -> "Batterie 2"
};
static const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);

// ---------------- Reglages ----------------
static const uint32_t CYCLE_INTERVAL_MS      = 60000;  // duree entre deux cycles (test)
static const uint32_t BLE_CONNECT_TIMEOUT_MS = 5000;
static const int      BLE_CONNECT_ATTEMPTS   = 2;
static const uint32_t STATUS_TIMEOUT_MS      = 4000;   // attente maximale de la trame de statut
static const uint32_t FRAME_QUIET_MS         = 150;    // silence apres le dernier morceau = trame complete
static const uint32_t AFTER_COMMAND_DELAY_MS = 800;    // pause entre l'ecriture d'une commande et la relecture
static const int      MAX_ROUNDS             = 3;      // rapports successifs par cycle (rapport + confirmations)
static const bool     DEBUG_DUMP_FRAME       = false;  // true : affiche chaque trame recue en hexadecimal
                                                        // (utile pour valider les offsets sur la vraie batterie)

// ---------------- Types ----------------
struct BatteryReading {
  bool     valid;
  float    voltage_v;
  float    current_a;
  uint16_t soc;
  uint16_t soh;
  bool     dischargeOff;
  BatteryReading() : valid(false), voltage_v(0), current_a(0), soc(0), soh(0), dischargeOff(false) {}
};

enum class Cmd : uint8_t { None, DischargeOff, DischargeOn };

struct BleSession {
  NimBLEClient*               client;
  NimBLERemoteCharacteristic* writeChr;
  BleSession() : client(nullptr), writeChr(nullptr) {}
};

struct BatterySlot {
  BatteryReading reading;
  bool           needsReport;
  Cmd            pending;
  BatterySlot() : needsReport(false), pending(Cmd::None) {}
};

// ---------------- Reception des notifications (reassemblage) ----------------
// La trame de statut (>= 104 octets) arrive souvent en plusieurs morceaux :
// on les accumule, et la trame est consideree complete apres un court silence.
static const size_t RX_BUF_SIZE = 256;
static uint8_t      g_rx[RX_BUF_SIZE];
static size_t       g_rxLen = 0;
static uint32_t     g_rxLastMs = 0;
static portMUX_TYPE g_rxMux = portMUX_INITIALIZER_UNLOCKED;

void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  portENTER_CRITICAL(&g_rxMux);
  size_t room = RX_BUF_SIZE - g_rxLen;
  size_t n = len < room ? len : room;
  memcpy(g_rx + g_rxLen, data, n);
  g_rxLen += n;
  g_rxLastMs = millis();
  portEXIT_CRITICAL(&g_rxMux);
}

void rxReset() {
  portENTER_CRITICAL(&g_rxMux);
  g_rxLen = 0;
  g_rxLastMs = 0;
  portEXIT_CRITICAL(&g_rxMux);
}

void rxSnapshot(size_t &len, uint32_t &lastMs) {
  portENTER_CRITICAL(&g_rxMux);
  len = g_rxLen;
  lastMs = g_rxLastMs;
  portEXIT_CRITICAL(&g_rxMux);
}

size_t rxCopy(uint8_t* dest, size_t destSize) {
  portENTER_CRITICAL(&g_rxMux);
  size_t n = g_rxLen < destSize ? g_rxLen : destSize;
  memcpy(dest, g_rx, n);
  portEXIT_CRITICAL(&g_rxMux);
  return n;
}

void dumpHex(const uint8_t* d, size_t len) {
  Serial.printf("[BLE] Trame recue (%u octets) :", (unsigned)len);
  for (size_t i = 0; i < len; i++) {
    if (i % 16 == 0) Serial.print("\n  ");
    Serial.printf("%02X ", d[i]);
  }
  Serial.println();
}

// ---------------- Decodage de la trame de statut ----------------
bool parseStatus(const uint8_t* d, size_t len, BatteryReading &r) {
  r.valid = false;
  if (len < BMS_STATUS_LEN) {
    Serial.printf("[BLE] Trame trop courte : %u octets (attendu >= %u)\n", (unsigned)len, (unsigned)BMS_STATUS_LEN);
    return false;
  }
  if (d[BMS_OFF_MARKER] != BMS_MARKER_STATUS) {
    Serial.printf("[BLE] Marqueur inattendu 0x%02X (attendu 0x%02X)\n", d[BMS_OFF_MARKER], BMS_MARKER_STATUS);
    return false;
  }
  if (bmsChecksum(d, len - 1) != d[len - 1]) {
    Serial.println("[BLE] Attention : checksum de trame invalide (lecture conservee).");
  }

  r.voltage_v    = bmsReadLE(d, BMS_OFF_VOLTAGE_MV, 4) / 1000.0f;
  r.current_a    = (int32_t)bmsReadLE(d, BMS_OFF_CURRENT_MA, 4) / 1000.0f;
  r.soc          = (uint16_t)bmsReadLE(d, BMS_OFF_SOC, 2);
  r.soh          = (uint16_t)bmsReadLE(d, BMS_OFF_SOH, 2);
  r.dischargeOff = (bmsReadLE(d, BMS_OFF_STATE, 4) & BMS_STATE_DISCHARGE_OFF) != 0;
  r.valid        = true;

  Serial.printf("[BLE] Statut : %.2f V, %.2f A, SOC %u %%, SOH %u %%, decharge %s\n",
                r.voltage_v, r.current_a, r.soc, r.soh, r.dischargeOff ? "COUPEE" : "active");
  return true;
}

// ---------------- Session BLE ----------------
NimBLERemoteService* findService(NimBLEClient* c) {
  NimBLERemoteService* s = c->getService(NimBLEUUID((uint16_t)BMS_SERVICE_UUID16));
  if (!s) s = c->getService(NimBLEUUID(BMS_SERVICE_UUID128));
  return s;
}

NimBLERemoteCharacteristic* findChar(NimBLERemoteService* s, uint16_t uuid16, const char* uuid128) {
  NimBLERemoteCharacteristic* ch = s->getCharacteristic(NimBLEUUID(uuid16));
  if (!ch) ch = s->getCharacteristic(NimBLEUUID(uuid128));
  return ch;
}

void bleClose(BleSession &s) {
  if (s.client) {
    if (s.client->isConnected()) s.client->disconnect();
    NimBLEDevice::deleteClient(s.client);
  }
  s.client = nullptr;
  s.writeChr = nullptr;
  delay(200);   // laisse la pile BLE se stabiliser avant la connexion suivante
}

bool bleOpen(const BatteryConfig &cfg, BleSession &s) {
  s.client = nullptr;
  s.writeChr = nullptr;

  for (int attempt = 1; attempt <= BLE_CONNECT_ATTEMPTS; attempt++) {
    NimBLEClient* c = NimBLEDevice::createClient();
    c->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
    Serial.printf("[BLE] Connexion a %s (essai %d/%d)...\n", cfg.bleAddress, attempt, BLE_CONNECT_ATTEMPTS);
    if (c->connect(NimBLEAddress(cfg.bleAddress, cfg.bleAddrType))) {
      s.client = c;
      break;
    }
    NimBLEDevice::deleteClient(c);
    delay(1000);
  }
  if (!s.client) {
    Serial.println("[BLE] Echec de connexion.");
    return false;
  }

  NimBLERemoteService* svc = findService(s.client);
  if (!svc) {
    Serial.println("[BLE] Service 0xFFE0 introuvable sur ce device.");
    bleClose(s);
    return false;
  }
  NimBLERemoteCharacteristic* notifyChr = findChar(svc, BMS_NOTIFY_UUID16, BMS_NOTIFY_UUID128);
  s.writeChr = findChar(svc, BMS_WRITE_UUID16, BMS_WRITE_UUID128);
  if (!notifyChr || !s.writeChr) {
    Serial.println("[BLE] Caracteristique FFE1 ou FFE2 introuvable (verifie avec nRF Connect).");
    bleClose(s);
    return false;
  }
  if (!notifyChr->canNotify() || !notifyChr->subscribe(true, onNotify)) {
    Serial.println("[BLE] Impossible de s'abonner aux notifications de FFE1.");
    bleClose(s);
    return false;
  }

  Serial.printf("[BLE] Connecte (MTU %u).\n", (unsigned)s.client->getMTU());
  return true;
}

bool bleWrite(BleSession &s, const uint8_t* cmd) {
  // Ecriture avec accuse de reception quand la caracteristique le permet.
  return s.writeChr->writeValue(cmd, BMS_CMD_LEN, s.writeChr->canWrite());
}

bool bleQueryStatus(BleSession &s, BatteryReading &out) {
  rxReset();
  if (!bleWrite(s, BMS_CMD_STATUS)) {
    Serial.println("[BLE] Ecriture de la requete de statut impossible.");
    return false;
  }

  uint32_t start = millis();
  while (millis() - start < STATUS_TIMEOUT_MS) {
    size_t len;
    uint32_t lastMs;
    rxSnapshot(len, lastMs);
    if (len >= BMS_STATUS_LEN && millis() - lastMs > FRAME_QUIET_MS) break;
    delay(20);
  }

  uint8_t frame[RX_BUF_SIZE];
  size_t len = rxCopy(frame, sizeof(frame));
  if (DEBUG_DUMP_FRAME) dumpHex(frame, len);
  return parseStatus(frame, len, out);
}

// Lecture complete : connexion, statut, deconnexion.
BatteryReading readBattery(const BatteryConfig &cfg) {
  BatteryReading r;
  BleSession s;
  if (bleOpen(cfg, s)) {
    bleQueryStatus(s, r);
    bleClose(s);
  }
  return r;
}

// Applique une commande de decharge, puis relit le statut pour verifier.
// Renvoie le statut relu (valid = false si la relecture a echoue).
BatteryReading applyCommand(const BatteryConfig &cfg, Cmd cmd) {
  BatteryReading r;
  BleSession s;
  if (!bleOpen(cfg, s)) return r;

  bool wantOff = (cmd == Cmd::DischargeOff);
  Serial.printf("[BLE] Envoi de la commande : %s\n", wantOff ? "COUPURE de la decharge" : "RETABLISSEMENT de la decharge");

  if (bleWrite(s, wantOff ? BMS_CMD_DISCHARGE_OFF : BMS_CMD_DISCHARGE_ON)) {
    delay(AFTER_COMMAND_DELAY_MS);
    if (bleQueryStatus(s, r)) {
      if (r.dischargeOff == wantOff) {
        Serial.println("[BLE] Commande confirmee par la batterie.");
      } else {
        Serial.println("[BLE] ATTENTION : l'etat lu ne correspond pas a la commande envoyee.");
      }
    }
  } else {
    Serial.println("[BLE] Echec d'ecriture de la commande.");
  }
  bleClose(s);
  return r;
}

// ---------------- WiFi et HTTP ----------------
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

// Extrait la derniere commande de decharge de {"commands": [...]}.
Cmd parseCommand(const String &body) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[HTTP] Reponse illisible.");
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
      Serial.printf("[HTTP] Commande inconnue ignoree : %s\n", action);
    }
  }
  return result;
}

// Envoie un rapport ; renvoie true si le serveur a repondu 200.
// cmdOut recoit la commande eventuellement renvoyee par le serveur.
bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut) {
  cmdOut = Cmd::None;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Pas de WiFi : rapport ignore.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // OK pour un test ; a remplacer par une verification de certificat en production.

  HTTPClient http;
  http.begin(client, SERVER_URL);
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
  Serial.println("[HTTP] POST " + payload);

  int code = http.POST(payload);
  Serial.printf("[HTTP] Reponse : %d\n", code);
  if (code != 200) {
    if (code <= 0) {
      char errBuf[128];
      client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[HTTP] Detail : %s\n", errBuf);
    }
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();
  Serial.println("[HTTP] " + body);
  cmdOut = parseCommand(body);
  return true;
}

// ---------------- Cycle complet ----------------
void runCycle() {
  BatterySlot slots[BATTERY_COUNT];

  // 1. Lecture BLE de chaque batterie
  for (size_t i = 0; i < BATTERY_COUNT; i++) {
    slots[i].reading = readBattery(BATTERIES[i]);
    slots[i].needsReport = slots[i].reading.valid;
  }

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  // 2 et 3. Rapport, puis commande eventuelle, puis rapport de confirmation
  for (int round = 0; round < MAX_ROUNDS; round++) {
    bool anyCommand = false;

    for (size_t i = 0; i < BATTERY_COUNT; i++) {
      if (!slots[i].needsReport) continue;
      Cmd cmd = Cmd::None;
      if (postReport(BATTERIES[i], slots[i].reading, cmd)) {
        slots[i].needsReport = false;
        slots[i].pending = cmd;
        if (cmd != Cmd::None) anyCommand = true;
      }
    }
    if (!anyCommand) break;

    for (size_t i = 0; i < BATTERY_COUNT; i++) {
      if (slots[i].pending == Cmd::None) continue;
      slots[i].reading = applyCommand(BATTERIES[i], slots[i].pending);
      slots[i].pending = Cmd::None;
      slots[i].needsReport = slots[i].reading.valid;   // rapport de confirmation au tour suivant
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== Passerelle ESP32 <-> batterie BLE <-> serveur ===");

  connectWiFi();
  NimBLEDevice::init("");
}

void loop() {
  Serial.println("\n--- Nouveau cycle ---");
  runCycle();
  Serial.printf("Prochain cycle dans %u s\n", (unsigned)(CYCLE_INTERVAL_MS / 1000));
  delay(CYCLE_INTERVAL_MS);
}
