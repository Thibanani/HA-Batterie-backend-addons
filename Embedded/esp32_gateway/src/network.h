/*
 * Liaison internet de la passerelle (transport TCP).
 *
 * Un seul transport est compile, choisi par platformio.ini :
 *   -DNETWORK_WIFI : WiFi (network_wifi.cpp)
 * Le HTTPS est ajoute par-dessus, a l'identique pour tous les transports (report.cpp).
 */
#pragma once
#include <Client.h>

#if !defined(NETWORK_WIFI)
  #error "Choisir le transport dans platformio.ini : -DNETWORK_WIFI"
#endif

// true si la liaison internet est etablie.
bool networkIsConnected();

// Etablit la liaison internet si elle ne l'est pas deja. Renvoie true en cas de succes.
bool networkConnect();

// Coupe la liaison et la radio avant la mise en veille.
void networkStop();

// Client TCP brut de la liaison : le TLS est ajoute par report.cpp.
Client& networkTcpClient();
