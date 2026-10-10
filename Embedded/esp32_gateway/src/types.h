/*
 * Types communs a tous les modules de la passerelle.
 */
#pragma once
#include <Arduino.h>

// Une batterie suivie par la passerelle (voir config.cpp).
struct BatteryConfig {
  const char* bleAddress;   // adresse BLE de la batterie / du simulateur (MAJUSCULES)
  uint8_t     bleAddrType;  // BLE_ADDR_PUBLIC (par defaut) ou BLE_ADDR_RANDOM si la connexion echoue
  const char* reportId;     // identifiant envoye au serveur (cle de BATTERIES dans l'add-on)
};

// Resultat d'une lecture du statut de la batterie.
struct BatteryReading {
  bool     valid;
  float    voltage_v;
  float    current_a;
  uint16_t soc;
  uint16_t soh;
  bool     dischargeOff;
  BatteryReading() : valid(false), voltage_v(0), current_a(0), soc(0), soh(0), dischargeOff(false) {}
};

// Commande de decharge renvoyee par le serveur.
enum class Cmd : uint8_t { None, DischargeOff, DischargeOn };
