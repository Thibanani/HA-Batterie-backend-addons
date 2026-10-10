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
 *   network.h/.cpp      connexion internet (WiFi, GSM plus tard) et HTTP
 *   main.cpp            cycle complet et mise en veille
 *
 * Modes de compilation (voir platformio.ini) :
 *   *-debug    traces serie actives
 *   *-release  aucune trace serie
 *
 * Dependances (declarees dans platformio.ini) : NimBLE-Arduino 2.x, ArduinoJson 6.x.
 */

#include <Arduino.h>
#include "ble_battery.h"
#include "debug.h"
#include "network.h"
#include "config.h"

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

  if (!networkIsConnected()) networkConnect();

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

void setup() {
  DEBUG_BEGIN(115200);
  DEBUG_PRINTLN();
  DEBUG_PRINTLN("=== Passerelle ESP32 <-> batterie BLE <-> serveur ===");

  networkConnect();
  bleInit();
}

void loop() {
  DEBUG_PRINTLN("\n--- Nouveau cycle ---");
  runCycle();
  DEBUG_PRINTF("Prochain cycle dans %u s\n", (unsigned)(CYCLE_INTERVAL_MS / 1000));
  DEBUG_FLUSH();
  esp_sleep_enable_timer_wakeup(CYCLE_INTERVAL_MS * uS_TO_mS_FACTOR);
  esp_deep_sleep_start();
}
