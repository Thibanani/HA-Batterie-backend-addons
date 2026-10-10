/*
 * MODELE des secrets de la passerelle (versionne, valeurs factices).
 *
 * Copier ce fichier en include/secrets.h et y mettre les vraies valeurs :
 * secrets.h est ignore par git et ne doit jamais etre commite.
 * Tant que secrets.h n'existe pas, la compilation utilise ce modele et
 * affiche un avertissement.
 */
#pragma once

#define SECRET_WIFI_SSID     "TON_WIFI"
#define SECRET_WIFI_PASSWORD "TON_MOT_DE_PASSE"

// Identique a l'option "api_key" de l'add-on Home Assistant.
// Generer une valeur longue et aleatoire, par exemple :
//   python -c "import secrets; print(secrets.token_urlsafe(32))"
#define SECRET_API_KEY       "TON_API_KEY"
