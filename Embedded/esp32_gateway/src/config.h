/*
 * Configuration de la passerelle.
 * Les valeurs a personnaliser (reseau, serveur, batteries) sont dans config.cpp.
 */
#pragma once
#include "types.h"

// ---------------- A personnaliser (definis dans config.cpp) ----------------
extern const char* WIFI_SSID;          // secrets : include/secrets.h
extern const char* WIFI_PASSWORD;
extern const char* SERVER_URL;
extern const char* SERVER_ROOT_CA;     // certificats racines acceptes (nullptr = pas de verification)
extern const char* API_KEY;            // identique a l'option "api_key" de l'add-on

extern const BatteryConfig BATTERIES[];
extern const size_t        BATTERY_COUNT;

// ---------------- Reglages ----------------
static const uint32_t CYCLE_INTERVAL_MS      = 60000;  // duree entre deux cycles (test)
static const uint32_t BLE_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t HTTP_TIMEOUT_MS        = 10000;  // connexion TCP, negociation TLS et reponse
static const int      BLE_CONNECT_ATTEMPTS   = 2;
static const uint32_t STATUS_TIMEOUT_MS      = 4000;   // attente maximale de la trame de statut
static const uint32_t FRAME_QUIET_MS         = 150;    // silence apres le dernier morceau = trame complete
static const uint32_t AFTER_COMMAND_DELAY_MS = 800;    // pause entre l'ecriture d'une commande et la relecture
static const int      MAX_ROUNDS             = 3;      // rapports successifs par cycle (rapport + confirmations)
static const size_t   MAX_RESPONSE_SIZE      = 512;    // taille maximale lue dans une reponse du serveur
static const bool     DEBUG_DUMP_FRAME       = false;  // true : affiche chaque trame recue en hexadecimal (build debug seulement)
                                                        // (utile pour valider les offsets sur la vraie batterie)
