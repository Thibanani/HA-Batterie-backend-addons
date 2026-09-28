/*
 * bms_protocol.h — protocole BLE du BMS (LiTime / Redodo / PowerQueen "Smart")
 *
 * Ce fichier est IDENTIQUE dans les deux projets (passerelle et simulateur) :
 * si tu modifies un offset ou une commande apres avoir observe ta vraie
 * batterie, recopie le fichier dans l'autre dossier pour que les deux
 * restent d'accord.
 *
 * Source : documentation croisee des projets open source
 *   - https://github.com/rubenmuehlhans/litime-ble-hacs
 *   - https://github.com/dmytro-tsepilov/pq_bms_bluetooth
 * Les decalages ci-dessous sont ceux lus dans ces projets ; ils n'ont pas
 * encore ete verifies sur TA batterie (voir DEBUG_DUMP_FRAME dans la passerelle).
 */
#pragma once
#include <Arduino.h>

// ---------------- GATT ----------------
#define BMS_SERVICE_UUID16  0xFFE0   // service du BMS
#define BMS_NOTIFY_UUID16   0xFFE1   // reponses (notify)
#define BMS_WRITE_UUID16    0xFFE2   // commandes (write)
#define BMS_SERVICE_UUID128 "0000FFE0-0000-1000-8000-00805F9B34FB"
#define BMS_NOTIFY_UUID128  "0000FFE1-0000-1000-8000-00805F9B34FB"
#define BMS_WRITE_UUID128   "0000FFE2-0000-1000-8000-00805F9B34FB"

// ---------------- Commandes (8 octets) ----------------
// Trame : 00 00 04 01 <code> 55 AA <checksum>, avec checksum = (0x04 + code) & 0xFF
#define BMS_CMD_LEN        8
#define BMS_CMD_CODE_INDEX 4
#define BMS_CHK(code)      ((uint8_t)(0x04 + (code)))

#define BMS_CODE_STATUS        0x13
#define BMS_CODE_CHARGE_ON     0x0A
#define BMS_CODE_CHARGE_OFF    0x0B
#define BMS_CODE_DISCHARGE_ON  0x0C
#define BMS_CODE_DISCHARGE_OFF 0x0D

static const uint8_t BMS_CMD_STATUS[BMS_CMD_LEN]        = {0x00,0x00,0x04,0x01,BMS_CODE_STATUS,       0x55,0xAA,BMS_CHK(BMS_CODE_STATUS)};
static const uint8_t BMS_CMD_CHARGE_ON[BMS_CMD_LEN]     = {0x00,0x00,0x04,0x01,BMS_CODE_CHARGE_ON,    0x55,0xAA,BMS_CHK(BMS_CODE_CHARGE_ON)};
static const uint8_t BMS_CMD_CHARGE_OFF[BMS_CMD_LEN]    = {0x00,0x00,0x04,0x01,BMS_CODE_CHARGE_OFF,   0x55,0xAA,BMS_CHK(BMS_CODE_CHARGE_OFF)};
static const uint8_t BMS_CMD_DISCHARGE_ON[BMS_CMD_LEN]  = {0x00,0x00,0x04,0x01,BMS_CODE_DISCHARGE_ON, 0x55,0xAA,BMS_CHK(BMS_CODE_DISCHARGE_ON)};
static const uint8_t BMS_CMD_DISCHARGE_OFF[BMS_CMD_LEN] = {0x00,0x00,0x04,0x01,BMS_CODE_DISCHARGE_OFF,0x55,0xAA,BMS_CHK(BMS_CODE_DISCHARGE_OFF)};

// Verifie qu'une ecriture recue ressemble a une commande valide (cote simulateur).
static inline bool bmsCommandValid(const uint8_t* d, size_t len) {
  return len == BMS_CMD_LEN && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x04 &&
         d[3] == 0x01 && d[5] == 0x55 && d[6] == 0xAA &&
         d[7] == BMS_CHK(d[BMS_CMD_CODE_INDEX]);
}

// ---------------- Trame de statut (reponse a BMS_CMD_STATUS) ----------------
#define BMS_STATUS_LEN        104     // longueur minimale attendue
#define BMS_OFF_MARKER        2       // octet marqueur
#define BMS_MARKER_STATUS     0x65
#define BMS_OFF_VOLTAGE_MV    12      // uint32 LE : tension totale (mV)
#define BMS_OFF_CELLS_MV      16      // jusqu'a 16 x uint16 LE : tension des cellules (mV)
#define BMS_OFF_CURRENT_MA    48      // int32  LE : courant (mA), negatif = decharge
#define BMS_OFF_STATE         68      // uint32 LE : drapeaux d'etat
#define BMS_STATE_DISCHARGE_OFF 0x80  // bit "decharge coupee" dans l'octet 68
#define BMS_OFF_SOC           90      // uint16 LE : etat de charge (%)
#define BMS_OFF_SOH           92      // uint16 LE : etat de sante (%)
// Le dernier octet de la trame est un checksum : somme des octets precedents & 0xFF.

static inline uint32_t bmsReadLE(const uint8_t* d, size_t offset, size_t size) {
  uint32_t v = 0;
  for (size_t i = 0; i < size; i++) v |= ((uint32_t)d[offset + i]) << (8 * i);
  return v;
}

static inline void bmsWriteLE(uint8_t* d, size_t offset, size_t size, uint32_t value) {
  for (size_t i = 0; i < size; i++) d[offset + i] = (uint8_t)((value >> (8 * i)) & 0xFF);
}

static inline uint8_t bmsChecksum(const uint8_t* d, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += d[i];
  return sum;
}
