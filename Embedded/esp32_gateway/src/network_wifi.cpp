/*
 * Transport WiFi (-DNETWORK_WIFI) : voir network.h.
 */
#ifdef NETWORK_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "network.h"
#include "debug.h"
#include "config.h"

static WiFiClient g_tcp;

bool networkIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool networkConnect() {
  if (networkIsConnected()) return true;
  DEBUG_PRINTF("Connexion WiFi a %s...\n", WIFI_SSID);
  WiFi.persistent(false);   // n'ecrit pas les identifiants en flash a chaque reveil
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (!networkIsConnected() && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);             // scrutation fine : la radio s'arrete des que possible
  }
  if (networkIsConnected()) {
    DEBUG_PRINTF("WiFi connecte en %u ms, IP : %s\n", (unsigned)(millis() - start),
                 WiFi.localIP().toString().c_str());
    return true;
  }
  DEBUG_PRINTLN("Echec de connexion WiFi.");
  networkStop();
  return false;
}

void networkStop() {
  // Avant un deep sleep, arreter la radio suffit (la memoire est perdue de toute facon).
  // WiFi.mode(WIFI_OFF) libere aussi le pilote, ce qui expire juste apres une deconnexion
  // ("wifi:timeout when WiFi un-init").
  if (WiFi.getMode() == WIFI_OFF) return;   // WiFi jamais demarre pendant ce cycle
  esp_wifi_disconnect();
  esp_wifi_stop();
}

Client& networkTcpClient() {
  return g_tcp;
}

#endif  // NETWORK_WIFI
