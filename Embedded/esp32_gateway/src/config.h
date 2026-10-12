/*
 * Configuration de la passerelle.
 * Les valeurs a personnaliser (reseau, serveur, batteries) sont dans config.cpp.
 */
#pragma once
#include "types.h"

// ---------------- A personnaliser (definis dans config.cpp) ----------------
extern const char* WIFI_SSID;          // secrets : include/secrets.h
extern const char* WIFI_PASSWORD;
extern const char* SERVER_HOST;        // serveur HTTPS (add-on Home Assistant)
extern const char* SERVER_PATH;
static const uint16_t SERVER_PORT = 443;
extern const char* SERVER_ROOT_CA;     // certificats racines acceptes (nullptr = pas de verification)
extern const char* API_KEY;            // identique a l'option "api_key" de l'add-on

extern const BatteryConfig BATTERIES[];
extern const size_t        BATTERY_COUNT;

// ---------------- Reglages ----------------
static const uint32_t CYCLE_INTERVAL_MS      = 60000;  // duree entre deux cycles (test)
static const uint32_t MAX_CYCLE_DURATION_S   = 180;    // au-dela, le watchdog redemarre la carte
static const uint32_t BLE_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t HTTP_TIMEOUT_MS        = 10000;  // connexion TCP, negociation TLS et reponse
static const int      BLE_CONNECT_ATTEMPTS   = 2;
static const uint32_t STATUS_TIMEOUT_MS      = 4000;   // attente maximale de la trame de statut
static const uint32_t FRAME_QUIET_MS         = 150;    // silence apres le dernier morceau = trame complete
static const uint32_t AFTER_COMMAND_DELAY_MS = 800;    // pause entre l'ecriture d'une commande et la relecture
static const int      MAX_ROUNDS             = 3;      // rapports successifs par cycle (rapport + confirmations)
static const size_t   MAX_RESPONSE_SIZE      = 512;    // taille maximale lue dans une reponse du serveur
// Trame au checksum faux : rejetee. Passer a false (avec DEBUG_DUMP_FRAME) seulement
// pour diagnostiquer une batterie dont le checksum ne suivrait pas bms_protocol.h.
static const bool     STRICT_FRAME_CHECKSUM  = true;
static const float    MAX_VOLTAGE_V          = 70.0f;   // bornes de validite d'une lecture,
static const float    MAX_CURRENT_A          = 1000.0f; // identiques a celles de l'add-on
static const bool     DEBUG_DUMP_FRAME       = false;  // true : affiche chaque trame recue en hexadecimal (build debug seulement)
                                                        // (utile pour valider les offsets sur la vraie batterie)
