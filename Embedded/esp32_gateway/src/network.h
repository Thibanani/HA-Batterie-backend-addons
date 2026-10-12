/*
 * Liaison internet de la passerelle (transport TCP).
 *
 * Un seul transport est compile, choisi par platformio.ini :
 *   -DNETWORK_GSM  : modem LTE-M SIM7080G de la LILYGO (network_gsm.cpp)
 *   -DNETWORK_WIFI : WiFi, banc de test (network_wifi.cpp)
 * Le HTTPS est ajoute par-dessus, a l'identique pour les deux (report.cpp).
 */
#pragma once
#include <Client.h>

#if defined(NETWORK_GSM) == defined(NETWORK_WIFI)
  #error "Choisir exactement un transport dans platformio.ini : -DNETWORK_GSM ou -DNETWORK_WIFI"
#endif

// true si la liaison internet est etablie.
bool networkIsConnected();

// Etablit la liaison internet si elle ne l'est pas deja. Renvoie true en cas de succes.
bool networkConnect();

// Coupe la liaison et la radio avant la mise en veille. En GSM : extinction propre
// du modem puis coupure de son alimentation, meme s'il a ete allume lors d'un cycle
// precedent interrompu (redemarrage par le watchdog).
void networkStop();

// Client TCP brut de la liaison : le TLS est ajoute par report.cpp.
Client& networkTcpClient();
