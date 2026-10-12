/*
 * Rapport au serveur (add-on Home Assistant) en HTTPS, au-dessus de la liaison
 * ouverte par networkConnect(). Le certificat du serveur est verifie par l'ESP32.
 */
#pragma once
#include "types.h"

// Envoie un rapport ; renvoie true si le serveur a repondu 200.
// cmdOut recoit la commande eventuellement renvoyee par le serveur.
bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut);
