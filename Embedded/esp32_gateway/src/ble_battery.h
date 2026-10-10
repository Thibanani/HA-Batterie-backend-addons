/*
 * Module Bluetooth : dialogue avec le BMS de la batterie (ou le simulateur).
 * Chaque appel ouvre sa propre connexion BLE et la referme avant de rendre la main.
 */
#pragma once
#include "types.h"

// A appeler une fois au demarrage (initialise la pile NimBLE).
void bleInit();

// Lecture complete : connexion, statut, deconnexion.
// reading.valid = false si la lecture a echoue.
BatteryReading readBattery(const BatteryConfig &cfg);

// Applique une commande de decharge, puis relit le statut pour verifier.
// Renvoie le statut relu (valid = false si la relecture a echoue).
BatteryReading applyCommand(const BatteryConfig &cfg, Cmd cmd);
