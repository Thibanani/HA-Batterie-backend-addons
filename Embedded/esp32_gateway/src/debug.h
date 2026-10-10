/*
 * Traces serie : mode debug / release.
 *
 * Le mode est choisi a la compilation, par l'environnement PlatformIO :
 *   pio run -e lilygo-debug     traces serie actives (-DDEBUG_ENABLE)
 *   pio run -e lilygo-release   aucune trace : Serial n'est jamais initialise
 *
 * Dans la passerelle, utiliser ces macros a la place de Serial.* (sauf dans
 * ce fichier). Sans -DDEBUG_ENABLE (release), les arguments restent verifies
 * par le compilateur mais ne sont jamais evalues : aucun code genere, aucun
 * delai. Du code qui ne sert qu'au debug se met entre #ifdef DEBUG_ENABLE.
 */
#pragma once
#include <Arduino.h>
#include <esp_log.h>

#ifdef DEBUG_ENABLE
  // La pause laisse a l'USB natif de l'ESP32-S3 le temps de se connecter au moniteur.
  #define DEBUG_BEGIN(speed) do { Serial.begin(speed); delay(1000); } while (0)
  #define DEBUG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
  #define DEBUG_FLUSH()      Serial.flush()
#else
  // Release : coupe aussi les logs internes d'ESP-IDF (WiFi, etc.).
  #define DEBUG_BEGIN(speed) esp_log_level_set("*", ESP_LOG_NONE)
  #define DEBUG_PRINT(...)   do { if (0) Serial.print(__VA_ARGS__); } while (0)
  #define DEBUG_PRINTLN(...) do { if (0) Serial.println(__VA_ARGS__); } while (0)
  #define DEBUG_PRINTF(...)  do { if (0) Serial.printf(__VA_ARGS__); } while (0)
  #define DEBUG_FLUSH()      ((void)0)
#endif
