/*
 * Transport LTE-M (-DNETWORK_GSM) : modem SIM7080G de la LILYGO T-SIM7080G-S3.
 * Voir network.h.
 *
 * A chaque cycle : alimentation du modem par le PMU AXP2101, demarrage (PWRKEY),
 * configuration radio + APN, inscription sur le reseau (itinerance acceptee),
 * activation de la liaison de donnees. Avant la veille : extinction propre puis
 * coupure de l'alimentation, sinon le modem consommerait pendant tout le deep
 * sleep de l'ESP32.
 *
 * Broches et sequences reprises des exemples officiels LilyGO (depot
 * Xinyuan-LilyGO/LilyGo-T-SIM7080G, utilities.h).
 */
#ifdef NETWORK_GSM

#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#ifdef DEBUG_ENABLE
  #define TINY_GSM_DEBUG Serial        // messages internes de TinyGSM (build debug)
#endif
#define XPOWERS_CHIP_AXP2101

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <TinyGsmClient.h>
#include "network.h"
#include "debug.h"
#include "config.h"

#if defined(GSM_DUMP_AT_COMMANDS) && defined(DEBUG_ENABLE)
  #include <StreamDebugger.h>
#endif

// ---------------- Broches LILYGO T-SIM7080G-S3 ----------------
static const int PMU_SDA      = 15;
static const int PMU_SCL      = 7;
static const int MODEM_PWRKEY = 41;   // via transistor : HIGH = PWRKEY a la masse
static const int MODEM_DTR    = 42;   // LOW = modem eveille
static const int MODEM_RX     = 4;    // RX de l'ESP32 (sortie du modem)
static const int MODEM_TX     = 5;

// ---------------- Etat ----------------
static XPowersPMU g_pmu;
static bool       g_pmuReady    = false;
static bool       g_serialReady = false;
static bool       g_dataActive  = false;

static HardwareSerial &SerialAT = Serial1;
#if defined(GSM_DUMP_AT_COMMANDS) && defined(DEBUG_ENABLE)
static StreamDebugger g_atDebug(SerialAT, Serial);
static TinyGsm        g_modem(g_atDebug);
#else
static TinyGsm        g_modem(SerialAT);
#endif
static TinyGsmClient  g_tcp(g_modem, 0);

// Code PIN refuse : on ne le retente plus avant un redemarrage complet, pour ne
// pas epuiser les essais et bloquer la carte SIM (code PUK). Conserve en veille.
static RTC_DATA_ATTR bool g_simPinRejected = false;

// ---------------- Alimentation (PMU AXP2101) ----------------
static bool pmuBegin() {
  if (g_pmuReady) return true;
  g_pmuReady = g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);
  if (!g_pmuReady) {
    DEBUG_PRINTLN("[GSM] PMU AXP2101 introuvable.");
    return false;
  }
  g_pmu.disableTSPinMeasure();   // pas de thermistance : sinon la charge est bloquee (exemples LilyGO)
  return true;
}

static void modemRails(bool on) {
  if (!pmuBegin()) return;
  if (on) {
    g_pmu.setBLDO1Voltage(3300);   // convertisseur de niveaux ESP32 <-> modem
    g_pmu.enableBLDO1();
    g_pmu.setDC3Voltage(3000);     // alimentation principale du SIM7080G (2,7 a 3,4 V)
    g_pmu.enableDC3();
    delay(100);                    // tension stable avant PWRKEY
  } else {
    g_pmu.disableDC3();
    g_pmu.disableBLDO1();
  }
}

static void serialBegin() {
  if (g_serialReady) return;
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  g_serialReady = true;
}

static void serialEnd() {
  if (!g_serialReady) return;
  SerialAT.end();
  g_serialReady = false;
}

// ---------------- Demarrage du modem ----------------
static void pulsePwrKey() {
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);   // PWRKEY a la masse plus d'1 s : marche / arret
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
}

static bool waitModemAT(uint32_t timeoutMs) {
  uint32_t start = millis();
  do {
    if (g_modem.testAT(500)) return true;
  } while (millis() - start < timeoutMs);
  return false;
}

static bool modemPowerOn() {
  modemRails(true);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  pinMode(MODEM_DTR, OUTPUT);
  digitalWrite(MODEM_DTR, LOW);
  serialBegin();

  // Deja allume (ex. ESP32 redemarre seul) : une impulsion l'eteindrait.
  if (waitModemAT(500)) return true;
  for (int attempt = 1; attempt <= 2; attempt++) {
    pulsePwrKey();
    if (waitModemAT(MODEM_BOOT_TIMEOUT_MS)) return true;
  }
  return false;
}

// ---------------- Inscription sur le reseau ----------------
static bool simReady() {
  SimStatus sim = g_modem.getSimStatus();
  if (sim == SIM_LOCKED && GSM_SIM_PIN[0] != '\0' && !g_simPinRejected) {
    DEBUG_PRINTLN("[GSM] Deverrouillage de la carte SIM...");
    if (!g_modem.simUnlock(GSM_SIM_PIN)) g_simPinRejected = true;   // AT+CPIN refuse
    sim = g_modem.getSimStatus();
  }
  if (sim == SIM_READY) return true;
  if (sim == SIM_LOCKED) {
    DEBUG_PRINTLN(g_simPinRejected
                    ? "[GSM] Code PIN refuse : plus d'essai jusqu'au prochain redemarrage complet."
                    : "[GSM] Carte SIM verrouillee : renseigner SECRET_SIM_PIN dans include/secrets.h.");
  } else {
    DEBUG_PRINTLN("[GSM] Carte SIM absente ou illisible.");
  }
  return false;
}

static void configureRadio() {
  // Radio coupee pendant la configuration, appliquee a la remise en route.
  g_modem.sendAT("+CFUN=0");
  g_modem.waitResponse(10000L);
  g_modem.setNetworkMode(38);                  // AT+CNMP=38 : LTE seulement
  g_modem.setPreferredMode(GSM_RADIO_MODE);    // AT+CMNB : LTE-M, NB-IoT ou les deux
  g_modem.sendAT("+CGDCONT=1,\"IP\",\"", GSM_APN, "\"");
  g_modem.waitResponse();
  g_modem.sendAT("+CNCFG=0,1,\"", GSM_APN, "\"");
  g_modem.waitResponse();
  g_modem.sendAT("+CFUN=1");
  g_modem.waitResponse(10000L);
}

static bool waitForRegistration() {
  uint32_t start = millis();
  int reg = REG_NO_RESULT;
  while (millis() - start < NETWORK_REG_TIMEOUT_MS) {
    reg = g_modem.getRegistrationStatus();
    if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) {
      DEBUG_PRINTF("[GSM] Inscrit sur le reseau%s en %u ms.\n",
                   reg == REG_OK_ROAMING ? " (itinerance)" : "", (unsigned)(millis() - start));
      return true;
    }
    if (reg == REG_DENIED) break;              // refus explicite : inutile d'insister
    delay(1000);
  }
#ifdef DEBUG_ENABLE
  int csq = g_modem.getSignalQuality();
  DEBUG_PRINTF("[GSM] Inscription impossible : etat %d, signal %d (99 = aucun).\n", reg, csq);
  if (reg == REG_DENIED) {
    DEBUG_PRINTLN("[GSM] Refus du reseau : SIM non autorisee ici ou sur cette technologie "
                  "(profil d'itinerance, LTE-M / NB-IoT).");
  } else if (csq == 99) {
    DEBUG_PRINTLN("[GSM] Aucun reseau capte : antenne, couverture LTE-M, GSM_RADIO_MODE.");
  }
#endif
  return false;
}

static bool activateData() {
  // AT+CNACT=0,1 : liaison de donnees des applications (contexte 0 = CGDCONT 1).
  g_modem.sendAT("+CNACT=0,1");
  int res = g_modem.waitResponse(DATA_ACTIVATION_TIMEOUT_MS, GF("+APP PDP: 0,ACTIVE"),
                                 GF("+APP PDP: 0,DEACTIVE"), GF("ERROR"));
  g_modem.waitResponse(200);                   // reliquat eventuel (OK)
  if (res != 1) {
    DEBUG_PRINTLN("[GSM] Activation de la liaison de donnees impossible (APN ?).");
    return false;
  }
  return true;
}

#ifdef DEBUG_ENABLE
static void logLinkInfo(uint32_t startMs) {
  DEBUG_PRINTF("[GSM] Liaison prete en %u ms : operateur %s, signal %d (0-31), IP %s\n",
               (unsigned)(millis() - startMs), g_modem.getOperator().c_str(),
               (int)g_modem.getSignalQuality(), g_modem.getLocalIP().c_str());
  // Technologie reellement utilisee (Cat-M1 / NB-IoT), operateur, bande.
  g_modem.sendAT("+CPSI?");
  if (g_modem.waitResponse(2000L, GF("+CPSI: ")) == 1) {
    String info = g_modem.stream.readStringUntil('\n');
    g_modem.waitResponse();
    info.trim();
    DEBUG_PRINTF("[GSM] %s\n", info.c_str());
  }
}
#endif

// ---------------- Interface network.h ----------------
bool networkIsConnected() {
  return g_dataActive && g_modem.isGprsConnected();
}

bool networkConnect() {
  if (networkIsConnected()) return true;
#ifdef DEBUG_ENABLE
  uint32_t start = millis();
#endif
  DEBUG_PRINTLN("[GSM] Demarrage du modem...");
  if (!modemPowerOn()) {
    DEBUG_PRINTLN("[GSM] Le modem ne repond pas.");
    networkStop();
    return false;
  }
  if (!simReady()) {
    networkStop();
    return false;
  }
  configureRadio();
  if (!waitForRegistration() || !activateData()) {
    networkStop();
    return false;
  }
  g_dataActive = true;
#ifdef DEBUG_ENABLE
  logLinkInfo(start);
#endif
  return true;
}

void networkStop() {
  g_dataActive = false;
  if (!pmuBegin()) return;
  // Modem alimente (reseau utilise pendant ce cycle, ou cycle precedent interrompu) :
  // extinction propre avant de couper le courant.
  if (g_pmu.isEnableDC3()) {
    serialBegin();
    if (g_modem.testAT(500)) {
      g_modem.sendAT("+CPOWD=1");              // se desinscrit du reseau puis s'eteint
      g_modem.waitResponse(MODEM_POWEROFF_TIMEOUT_MS, GF("NORMAL POWER DOWN"));
    }
    serialEnd();
    DEBUG_PRINTLN("[GSM] Modem eteint.");
  }
  modemRails(false);                           // DC3 et convertisseur de niveaux (BLDO1)
}

Client& networkTcpClient() {
  return g_tcp;
}

#endif  // NETWORK_GSM
