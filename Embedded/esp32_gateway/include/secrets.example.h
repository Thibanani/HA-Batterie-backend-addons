/*
 * MODELE des secrets de la passerelle (versionne, valeurs factices).
 *
 * Copier ce fichier en include/secrets.h et y mettre les vraies valeurs :
 * secrets.h est ignore par git et ne doit jamais etre commite.
 * Tant que secrets.h n'existe pas, la compilation utilise ce modele et
 * affiche un avertissement.
 */
#pragma once

// WiFi : utilise seulement par les environnements esp32dev-* (banc de test).
#define SECRET_WIFI_SSID     "TON_WIFI"
#define SECRET_WIFI_PASSWORD "TON_MOT_DE_PASSE"

// Code PIN de la carte SIM ("" si desactive, cas par defaut des SIM Things Mobile).
// Un code refuse n'est pas retente avant un redemarrage complet (protection PUK).
#define SECRET_SIM_PIN       ""

// Identique a l'option "api_key" de l'add-on Home Assistant.
// Generer une valeur longue et aleatoire, par exemple :
//   python -c "import secrets; print(secrets.token_urlsafe(32))"
#define SECRET_API_KEY       "TON_API_KEY"
