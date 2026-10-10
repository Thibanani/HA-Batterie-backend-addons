#include <NimBLEDevice.h>   // BLE_ADDR_PUBLIC
#include "config.h"

// ---------------- A CONFIGURER ----------------
const char* WIFI_SSID     = "TON_WIFI";
const char* WIFI_PASSWORD = "TON_MOT_DE_PASSE";
const char* SERVER_URL    = "https://batteries.thibaultbansha.ovh/report";
const char* API_KEY       = "TON_API_KEY";

const BatteryConfig BATTERIES[] = {
  { "AA:BB:CC:DD:EE:01", BLE_ADDR_PUBLIC, "AA:BB:CC:DD:EE:FF" },   // -> "Batterie 1" (remplace par l'adresse du simulateur)
  // { "AA:BB:CC:DD:EE:02", BLE_ADDR_PUBLIC, "11:22:33:44:55:66" }, // -> "Batterie 2"
};
const size_t BATTERY_COUNT = sizeof(BATTERIES) / sizeof(BATTERIES[0]);
