/*
 * PASSERELLE ESP32 — lit la batterie en BLE, remonte les donnees au serveur
 * (add-on Home Assistant) et applique les commandes recues.
 *
 * Cycle execute toutes les CYCLE_INTERVAL_MS :
 *   1. lecture BLE de chaque batterie (connexion, requete de statut, deconnexion)
 *   2. POST /report au serveur pour chaque batterie ; la reponse peut
 *      contenir une commande ("discharge_off" / "discharge_on")
 *   3. si une commande est recue : connexion BLE, ecriture de la commande,
 *      relecture du statut pour verifier, puis nouveau rapport de
 *      confirmation (Home Assistant se met a jour sans attendre le cycle suivant)
 *
 * Organisation du code (src/) :
 *   config.h/.cpp       reglages, reseau, serveur, liste des batteries (A PERSONNALISER)
 *   types.h             structures communes
 *   debug.h             macros de traces serie (mode debug / release)
 *   ble_battery.h/.cpp  dialogue Bluetooth avec le BMS
 *   network.h           liaison internet (transport TCP), une implementation par
 *                       transport : network_wifi.cpp
 *   report.h/.cpp       rapport HTTPS au serveur (commun a tous les transports)
 *   buffered_client.h   regroupe la requete HTTP en un seul envoi
 *   main.cpp            cycle complet et mise en veille
 *
 * Modes de compilation (voir platformio.ini) :
 *   *-debug    traces serie actives
 *   *-release  aucune trace serie
 *
 * Dependances (declarees dans platformio.ini) : NimBLE-Arduino 2.x, ArduinoJson 6.x.
 */

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "ble_battery.h"
#include "debug.h"
#include "network.h"
#include "report.h"
#include "config.h"

// Pile de la tache loop() : la negociation TLS (mbedtls) plus HTTP, et en GSM le
// pilote du modem, depassent confortablement les 8 Ko par defaut.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Etat d'une batterie pendant un cycle.
struct BatterySlot {
  BatteryReading reading;
  bool           needsReport;
  Cmd            pending;
  BatterySlot() : needsReport(false), pending(Cmd::None) {}
};

// ---------------- Cycle complet ----------------
void runCycle() {
  BatterySlot slots[BATTERY_COUNT];

  // 1. Lecture BLE de chaque batterie
  for (size_t i = 0; i < BATTERY_COUNT; i++) {
    slots[i].reading = readBattery(BATTERIES[i]);
    slots[i].needsReport = slots[i].reading.valid;
  }

  // Rien a envoyer : la radio reseau n'est pas allumee (economie d'energie).
  bool anyValid = false;
  for (size_t i = 0; i < BATTERY_COUNT; i++) anyValid = anyValid || slots[i].needsReport;
  if (!anyValid) {
    DEBUG_PRINTLN("[CYCLE] Aucune batterie lue : reseau non active.");
    return;
  }
  if (!networkConnect()) {
    DEBUG_PRINTLN("[CYCLE] Pas de reseau : rapports au prochain cycle.");
    return;
  }

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

#define uS_TO_mS_FACTOR 1000ULL

// ---------------- Veille et surveillance ----------------
[[noreturn]] static void goToSleep() {
  DEBUG_PRINTF("Prochain cycle dans %u s\n", (unsigned)(CYCLE_INTERVAL_MS / 1000));
  DEBUG_FLUSH();
  esp_sleep_enable_timer_wakeup(CYCLE_INTERVAL_MS * uS_TO_mS_FACTOR);
  esp_deep_sleep_start();
}

// Un cycle bloque (pile BLE, reseau...) ne doit pas garder l'ESP32 eveille et
// vider la batterie : au-dela de MAX_CYCLE_DURATION_S, le watchdog redemarre la carte.
static void armCycleWatchdog() {
  esp_task_wdt_init(MAX_CYCLE_DURATION_S, true);   // true : redemarrage a l'expiration
  esp_task_wdt_add(NULL);                          // surveille la tache de setup() / loop()
}

static bool lastResetWasAbnormal() {
  switch (esp_reset_reason()) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
    case ESP_RST_PANIC:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

void setup() {
  DEBUG_BEGIN(115200);
  DEBUG_PRINTLN();
  DEBUG_PRINTLN("=== Passerelle ESP32 <-> batterie BLE <-> serveur ===");
  armCycleWatchdog();

  // Apres un blocage, un plantage ou une baisse de tension, on attend le cycle
  // suivant au lieu de recommencer aussitot (pas de boucle de redemarrages).
  if (lastResetWasAbnormal()) {
    DEBUG_PRINTF("[CYCLE] Redemarrage anormal (raison %d) : cycle saute.\n", (int)esp_reset_reason());
    goToSleep();
  }

  // Le reseau n'est plus allume ici : seulement apres les lectures BLE,
  // et uniquement s'il y a quelque chose a envoyer (voir runCycle).
  bleInit();
}

void loop() {
  DEBUG_PRINTLN("\n--- Nouveau cycle ---");
  runCycle();
  networkStop();
  goToSleep();
}
