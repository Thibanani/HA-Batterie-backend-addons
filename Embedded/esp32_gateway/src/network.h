/*
 * Module reseau : acces internet et envoi des rapports au serveur.
 * Implemente en WiFi dans network.cpp ; le GSM (LTE-M) remplacera cette
 * implementation sans changer cette interface.
 */
#pragma once
#include "types.h"

// true si la liaison internet est etablie.
bool networkIsConnected();

// Etablit la liaison internet si elle ne l'est pas deja. Renvoie true en cas de succes.
bool networkConnect();

// Envoie un rapport au serveur ; renvoie true si le serveur a repondu 200.
// cmdOut recoit la commande eventuellement renvoyee par le serveur.
bool postReport(const BatteryConfig &cfg, const BatteryReading &r, Cmd &cmdOut);
