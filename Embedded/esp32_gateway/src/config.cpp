#include <NimBLEDevice.h>   // BLE_ADDR_PUBLIC
#include "config.h"

// Secrets (WiFi, cle API) : include/secrets.h, ignore par git.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "include/secrets.h absent : valeurs factices de secrets.example.h utilisees (copier le modele et le remplir)"
  #include "secrets.example.h"
#endif

// ---------------- A CONFIGURER ----------------
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* SERVER_URL    = "https://batteries.thibaultbansha.ovh/report";
const char* API_KEY       = SECRET_API_KEY;

const BatteryConfig BATTERIES[] = {
  { "AA:BB:CC:DD:EE:01", BLE_ADDR_PUBLIC, "AA:BB:CC:DD:EE:FF" },   // -> "Batterie 1" (remplace par l'adresse du simulateur)
  // { "AA:BB:CC:DD:EE:02", BLE_ADDR_PUBLIC, "11:22:33:44:55:66" }, // -> "Batterie 2"
};
const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);
