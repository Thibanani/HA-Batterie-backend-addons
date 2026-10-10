/*
 * Module Bluetooth : voir ble_battery.h.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "bms_protocol.h"   // Embedded/bms_protocol.h (voir build_flags dans platformio.ini)
#include "ble_battery.h"
#include "config.h"

struct BleSession {
  NimBLEClient*               client;
  NimBLERemoteCharacteristic* writeChr;
  BleSession() : client(nullptr), writeChr(nullptr) {}
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

void bleInit() {
  NimBLEDevice::init("");
}
