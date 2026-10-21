/*
 * Configuration de la passerelle.
 * Les valeurs a personnaliser (reseau, serveur, batteries) sont dans config.cpp.
 */
#pragma once
#include "types.h"

// ---------------- A personnaliser (definis dans config.cpp) ----------------
extern const char* WIFI_SSID;          // secrets : include/secrets.h (WiFi : banc de test)
extern const char* WIFI_PASSWORD;
extern const char* GSM_APN;            // LTE-M : APN de l'operateur (Things Mobile : "TM")
extern const char* GSM_SIM_PIN;        // secrets : "" si le code PIN de la SIM est desactive
extern const char* SERVER_HOST;        // serveur HTTPS (add-on Home Assistant)
extern const char* SERVER_PATH;
static const uint16_t SERVER_PORT = 443;
extern const char* SERVER_ROOT_CA;     // certificats racines acceptes (nullptr = pas de verification)
extern const char* API_KEY;            // identique a l'option "api_key" de l'add-on

extern const BatteryConfig BATTERIES[];
extern const size_t        BATTERY_COUNT;
static const size_t        MAX_BATTERIES = 4;   // au-dela, les batteries suivantes sont ignorees

// ---------------- Reglages ----------------
// Duree entre deux cycles (valeur de test). En LTE-M, chaque cycle avec rapport
// consomme quelques Ko de data facturee (surtout la negociation TLS) et relance
// l'inscription du modem : prevoir 15 min ou plus en production.
static const uint32_t CYCLE_INTERVAL_MS      = 60000;

// ---------------- Economie de data ----------------
// La batterie est lue a chaque cycle, mais le reseau n'est utilise que si une
// valeur a change d'au moins un seuil ci-dessous (etat de decharge ou SOH : tout
// changement), ou pour un rapport de vie toutes les REPORT_HEARTBEAT_S secondes.
// REPORT_HEARTBEAT_S doit rester inferieur a l'option "sensor_expire_after" de
// l'add-on (900 s par defaut), sinon les capteurs passent "indisponible" entre
// deux rapports. Les commandes de Home Assistant sont recuperees aux rapports :
// leur delai maximal est donc aussi REPORT_HEARTBEAT_S. 0 = rapport a chaque cycle.
static const uint32_t REPORT_HEARTBEAT_S     = 600;
static const float    REPORT_DELTA_VOLTAGE_V = 0.05f;
static const float    REPORT_DELTA_CURRENT_A = 1.0f;
static const int      REPORT_DELTA_SOC       = 1;      // en points de %
static const uint32_t BLE_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
#ifdef NETWORK_GSM
static const uint32_t HTTP_TIMEOUT_MS        = 20000;  // TCP, TLS et reponse (latence LTE-M)
static const uint32_t MAX_CYCLE_DURATION_S   = 420;    // demarrage modem + inscription compris
#else
static const uint32_t HTTP_TIMEOUT_MS        = 10000;  // TCP, TLS et reponse
static const uint32_t MAX_CYCLE_DURATION_S   = 180;    // au-dela, le watchdog redemarre la carte
#endif
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
// ---------------- Modem LTE-M (-DNETWORK_GSM) ----------------
// AT+CMNB : 1 = LTE-M (Cat-M1), 2 = NB-IoT, 3 = les deux. Le SIM7080G ne fait ni
// 2G ni 4G classique : l'operateur doit proposer l'une de ces technologies sur place.
static const uint8_t  GSM_RADIO_MODE             = 1;
static const uint32_t MODEM_BOOT_TIMEOUT_MS      = 10000;  // reponse AT apres l'impulsion PWRKEY
static const uint32_t NETWORK_REG_TIMEOUT_MS     = 90000;  // inscription (itinerance : parfois long)
static const uint32_t DATA_ACTIVATION_TIMEOUT_MS = 30000;  // AT+CNACT
static const uint32_t MODEM_POWEROFF_TIMEOUT_MS  = 8000;   // AT+CPOWD=1

static const bool     DEBUG_DUMP_FRAME       = false;  // true : affiche chaque trame recue en hexadecimal (build debug seulement)
                                                        // (utile pour valider les offsets sur la vraie batterie)
