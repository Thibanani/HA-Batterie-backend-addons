/*
 * SIMULATEUR DE BATTERIE — ESP32 en peripherique BLE
 *
 * Imite le BMS d'une batterie PowerQueen "Smart" : meme service GATT (FFE0),
 * memes commandes, meme format de trame de statut. Sert a tester la
 * passerelle (esp32_gateway) sans toucher a la vraie batterie.
 *
 * Ce que fait le simulateur :
 *   - annonce un nom "LT-SIM01" et le service 0xFFE0
 *   - repond a la commande "statut" (0x13) par une trame de 104 octets,
 *     decoupee en notifications de 20 octets (comme avec le MTU BLE par
 *     defaut) -> teste aussi le reassemblage cote passerelle
 *   - applique les commandes de coupure/retablissement de la decharge
 *     (0x0D / 0x0C) : le courant tombe a 0 A et le bit "decharge coupee"
 *     est positionne dans la trame suivante
 *   - accepte les commandes de charge (0x0A / 0x0B) sans exposer leur etat
 *     (le bit correspondant du vrai BMS n'est pas connu)
 *
 * Commandes de test sur le port serie (115200 bauds, fin de ligne "Nouvelle ligne") :
 *   soc 40            change l'etat de charge (la tension suit)
 *   discharge off|on  coupe / retablit la decharge "a la main"
 *   status            affiche l'etat simule
 *   help              rappelle ces commandes
 *
 * Dependance (Arduino IDE > Gestionnaire de bibliotheques) :
 *   - "NimBLE-Arduino" (auteur h2zero), VERSION 2.x (la derniere proposee).
 *     Le code utilise l'API 2.x ; avec une 1.4 il ne compilera pas.
 *
 * Au demarrage, le simulateur affiche son adresse BLE : c'est celle a
 * recopier dans bleAddress (esp32_gateway.ino).
 */

#include <NimBLEDevice.h>
#include "../bms_protocol.h"

// ---------------- Configuration ----------------
static const char*  SIM_NAME     = "LT-SIM01";
static const size_t NOTIFY_CHUNK = 20;      // octets par notification (20 = MTU BLE par defaut)
static const bool   REPLY_STATUS_AFTER_CONTROL = false; // true : repond aussi par une trame apres 0x0A..0x0D
                                                         // (pour verifier que la passerelle tolere une reponse inattendue)

// ---------------- Etat simule ----------------
struct SimState {
  uint16_t soc;
  uint16_t soh;
  bool     dischargeOff;
  bool     chargeOff;
  uint32_t voltage_mV;
  int32_t  current_mA;
};
static SimState sim = {72, 98, false, false, 13232, -2500};

NimBLEServer*         g_server     = nullptr;
NimBLECharacteristic* g_notifyChar = nullptr;

// ---------------- File des commandes recues ----------------
// Le callback BLE ne fait que ranger le code de commande ; le traitement
// (et l'envoi des notifications) se fait dans loop().
static const size_t QUEUE_SIZE = 8;
static volatile uint8_t g_queue[QUEUE_SIZE];
static volatile size_t  g_qHead = 0;
static volatile size_t  g_qTail = 0;
static portMUX_TYPE     g_qMux  = portMUX_INITIALIZER_UNLOCKED;

void pushCommand(uint8_t code) {
  portENTER_CRITICAL(&g_qMux);
  size_t next = (g_qHead + 1) % QUEUE_SIZE;
  if (next != g_qTail) {
    g_queue[g_qHead] = code;
    g_qHead = next;
  }
  portEXIT_CRITICAL(&g_qMux);
}

bool popCommand(uint8_t &code) {
  bool got = false;
  portENTER_CRITICAL(&g_qMux);
  if (g_qTail != g_qHead) {
    code = g_queue[g_qTail];
    g_qTail = (g_qTail + 1) % QUEUE_SIZE;
    got = true;
  }
  portEXIT_CRITICAL(&g_qMux);
  return got;
}

// ---------------- Callback d'ecriture (commandes de la passerelle) ----------------
class WriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue v = chr->getValue();
    const uint8_t* d = v.data();
    size_t n = v.size();
    if (!bmsCommandValid(d, n)) {
      Serial.printf("[SIM] Ecriture invalide ignoree (%u octets)\n", (unsigned)n);
      return;
    }
    pushCommand(d[BMS_CMD_CODE_INDEX]);
  }
};
static WriteCallbacks g_writeCallbacks;

// ---------------- Construction et envoi de la trame de statut ----------------
void buildStatusFrame(uint8_t* f) {
  memset(f, 0, BMS_STATUS_LEN);
  f[BMS_OFF_MARKER] = BMS_MARKER_STATUS;

  bmsWriteLE(f, BMS_OFF_VOLTAGE_MV, 4, sim.voltage_mV);
  for (int i = 0; i < 4; i++) {                       // 4 cellules LiFePO4 (batterie 12 V)
    int32_t cell = (int32_t)(sim.voltage_mV / 4) + (i - 1) * 3;
    bmsWriteLE(f, BMS_OFF_CELLS_MV + 2 * i, 2, (uint32_t)cell);
  }
  bmsWriteLE(f, BMS_OFF_CURRENT_MA, 4, (uint32_t)sim.current_mA);
  bmsWriteLE(f, BMS_OFF_STATE, 4, sim.dischargeOff ? BMS_STATE_DISCHARGE_OFF : 0);
  bmsWriteLE(f, BMS_OFF_SOC, 2, sim.soc);
  bmsWriteLE(f, BMS_OFF_SOH, 2, sim.soh);

  f[BMS_STATUS_LEN - 1] = bmsChecksum(f, BMS_STATUS_LEN - 1);
}

void sendStatusFrame() {
  if (g_server->getConnectedCount() == 0) {
    Serial.println("[SIM] Aucun client connecte : trame non envoyee.");
    return;
  }
  uint8_t frame[BMS_STATUS_LEN];
  buildStatusFrame(frame);

  size_t sent = 0;
  int chunks = 0;
  while (sent < BMS_STATUS_LEN) {
    size_t n = BMS_STATUS_LEN - sent;
    if (n > NOTIFY_CHUNK) n = NOTIFY_CHUNK;
    g_notifyChar->setValue(frame + sent, n);
    g_notifyChar->notify();
    sent += n;
    chunks++;
    delay(20);                                        // laisse la pile BLE evacuer chaque notification
  }
  Serial.printf("[SIM] Trame de statut envoyee (%u octets en %d notifications) : %.2f V, %.2f A, SOC %u %%, decharge %s\n",
                (unsigned)BMS_STATUS_LEN, chunks, sim.voltage_mV / 1000.0, sim.current_mA / 1000.0,
                sim.soc, sim.dischargeOff ? "COUPEE" : "active");
}

// ---------------- Traitement des commandes ----------------
void processCommand(uint8_t code) {
  switch (code) {
    case BMS_CODE_STATUS:
      Serial.println("[SIM] << requete de statut");
      sendStatusFrame();
      break;
    case BMS_CODE_DISCHARGE_OFF:
      Serial.println("[SIM] << COUPURE de la decharge");
      sim.dischargeOff = true;
      sim.current_mA = 0;
      if (REPLY_STATUS_AFTER_CONTROL) sendStatusFrame();
      break;
    case BMS_CODE_DISCHARGE_ON:
      Serial.println("[SIM] << RETABLISSEMENT de la decharge");
      sim.dischargeOff = false;
      sim.current_mA = -2500;
      if (REPLY_STATUS_AFTER_CONTROL) sendStatusFrame();
      break;
    case BMS_CODE_CHARGE_OFF:
      Serial.println("[SIM] << coupure de la charge (etat non expose dans la trame)");
      sim.chargeOff = true;
      if (REPLY_STATUS_AFTER_CONTROL) sendStatusFrame();
      break;
    case BMS_CODE_CHARGE_ON:
      Serial.println("[SIM] << retablissement de la charge (etat non expose dans la trame)");
      sim.chargeOff = false;
      if (REPLY_STATUS_AFTER_CONTROL) sendStatusFrame();
      break;
    default:
      Serial.printf("[SIM] << commande inconnue 0x%02X ignoree\n", code);
      break;
  }
}

// ---------------- Evolution simulee ----------------
void updateSimulation() {
  static uint32_t last = 0;
  if (millis() - last < 3000) return;
  last = millis();
  int32_t jitter = (int32_t)random(-15, 16);
  sim.voltage_mV = (uint32_t)(12800 + (int32_t)sim.soc * 6 + jitter);   // 12.8 V (0 %) a ~13.4 V (100 %)
  sim.current_mA = sim.dischargeOff ? 0 : (-2500 + (int32_t)random(-200, 201));
}

void printHelp() {
  Serial.println("Commandes : soc <0-100> | discharge on|off | status | help");
}

void printState() {
  Serial.printf("[SIM] Etat : %.2f V, %.2f A, SOC %u %%, SOH %u %%, decharge %s, charge %s\n",
                sim.voltage_mV / 1000.0, sim.current_mA / 1000.0, sim.soc, sim.soh,
                sim.dischargeOff ? "COUPEE" : "active", sim.chargeOff ? "coupee" : "active");
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  if (line.startsWith("soc ")) {
    int v = line.substring(4).toInt();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    sim.soc = (uint16_t)v;
    updateSimulation();
    printState();
  } else if (line == "discharge off") {
    processCommand(BMS_CODE_DISCHARGE_OFF);
  } else if (line == "discharge on") {
    processCommand(BMS_CODE_DISCHARGE_ON);
  } else if (line == "status") {
    printState();
  } else {
    printHelp();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));

  NimBLEDevice::init(SIM_NAME);
  g_server = NimBLEDevice::createServer();
  g_server->advertiseOnDisconnect(true);              // 2.x : reprend l'annonce apres une deconnexion

  NimBLEService* svc = g_server->createService(NimBLEUUID((uint16_t)BMS_SERVICE_UUID16));
  g_notifyChar = svc->createCharacteristic(NimBLEUUID((uint16_t)BMS_NOTIFY_UUID16),
                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* writeChar = svc->createCharacteristic(NimBLEUUID((uint16_t)BMS_WRITE_UUID16),
                                           NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  writeChar->setCallbacks(&g_writeCallbacks);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(SIM_NAME);
  adv->addServiceUUID(NimBLEUUID((uint16_t)BMS_SERVICE_UUID16));
  adv->enableScanResponse(true);
  adv->start();

  String addr = String(NimBLEDevice::getAddress().toString().c_str());
  addr.toUpperCase();
  Serial.println();
  Serial.println("=== Simulateur de batterie PowerQueen ===");
  Serial.println("Adresse BLE (a recopier dans la passerelle) : " + addr);
  printHelp();
  printState();
}

void loop() {
  static uint8_t lastClients = 0;
  uint8_t clients = g_server->getConnectedCount();
  if (clients != lastClients) {
    Serial.printf("[SIM] Clients connectes : %u\n", clients);
    lastClients = clients;
  }

  uint8_t code;
  while (popCommand(code)) processCommand(code);

  updateSimulation();
  handleSerial();
  delay(10);
}
