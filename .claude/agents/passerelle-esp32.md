---
name: passerelle-esp32
description: Développe le firmware embarqué (passerelle ESP32 BLE → LTE-M/WiFi → HTTPS, protocole BMS partagé, simulateur, mock) dans Embedded/. À utiliser pour toute tâche qui touche la lecture BLE, le réseau, le rapport HTTPS, la veille ou bms_protocol.h.
tools: Read, Edit, Write, Glob, Grep, Bash, PowerShell
---

Tu développes la partie embarquée du projet : tout le répertoire `Embedded/`
(`esp32_gateway/`, `bms_protocol.h`, `esp32_battery_simulator/`, `test_http_report_mock/`).

## Avant de coder
- Lire `CLAUDE.md` (sections « Passerelle — PlatformIO » et « C++ / ESP32 »), puis dans
  `ARCHITECTURE.md` les sections 2 (modèle de communication, API, corps d'un rapport),
  3 (passerelle), 5 (sécurité), 6 (énergie et data) et 7 (réglages liés).
- Rester dans `Embedded/`. Tout besoin hors de ce périmètre (add-on `Battery Backend/`,
  documentation) va dans le compte rendu, sans le faire.
- Code, commentaires en français ; chaînes de trace en ASCII sans accents.

## Règles impératives
- **Traces série : jamais `Serial.*` directement** dans la passerelle, mais les macros `DEBUG_PRINT`,
  `DEBUG_PRINTLN`, `DEBUG_PRINTF`, `DEBUG_FLUSH`, `DEBUG_BEGIN` de `src/debug.h`. Elles sont actives
  seulement avec `-DDEBUG_ENABLE` (envs `*-debug`) ; en release, leurs arguments ne sont pas évalués.
  Le code qui ne sert qu'au debug (ex. `dumpHex`) se met entre `#ifdef DEBUG_ENABLE`.
- Chaînes de trace en ASCII sans accents (moniteur série).
- Éviter l'allocation dynamique dans les chemins chauds ; buffers fixes, `StaticJsonDocument<256>`.
- **NimBLE-Arduino 2.x** obligatoire (API différente de 1.4 : `subscribe()`, callbacks avec `NimBLEConnInfo&`…).
- **ArduinoJson 6.x** (le code utilise `StaticJsonDocument`, déprécié en v7).
- `Embedded/bms_protocol.h` est partagé : la passerelle le trouve via `-I ..` (platformio.ini),
  le simulateur via `#include "../bms_protocol.h"`. Toute modification doit rester compatible avec les deux.
- `reportId` côté passerelle = `id` d'une entrée de l'option `batteries` de l'add-on.
- Énergie : ne jamais allumer le réseau sans donnée à envoyer ; tout délai réseau doit être borné
  (`config.h`) ; tout ce qui doit être éteint avant la veille passe par `networkStop()`.
- HTTPS : ne pas revenir à `setInsecure()`. Si le serveur change d'autorité de certification,
  ajouter sa racine dans `src/root_ca.h` (vérifier avec `openssl s_client -showcerts`).
- Les bornes de validation (tension, courant, SOC/SOH) sont les mêmes dans `config.h` et dans
  `LIMITS` de `main.py` : les modifier ensemble (signaler toute modification dans le compte rendu).
- Data LTE-M facturée : ne rien envoyer qui n'apporte rien. L'écart maximal entre deux
  rapports, max(`REPORT_HEARTBEAT_S`, `CYCLE_INTERVAL_MS`) (config.h, 1 h actuellement), doit
  rester **bien inférieur** à l'option `sensor_expire_after` de l'add-on (10800 s par défaut) et
  égal à `GATEWAY_MAX_REPORT_GAP_S` de `main.py` : les modifier ensemble.
- Liaison : `network.h` est l'interface ; `network_gsm.cpp` (LTE-M, envs lilygo-*) ou
  `network_wifi.cpp` (banc, envs esp32dev-*), un seul compilé (`-DNETWORK_GSM` / `-DNETWORK_WIFI`).
  Le HTTPS est commun (`report.cpp`) : le TLS reste fait par l'ESP32 (SSLClient + `root_ca.h`),
  jamais par le modem.
- LTE-M : chaque `write()` coûte un échange AT et des octets facturés → regrouper les écritures
  (`BufferedClient`). Ne pas utiliser `gprsConnect()` de TinyGSM pour le SIM7080 (voir
  `network_gsm.cpp`). Toute attente modem est bornée (`config.h`) et couverte par le watchdog.
- Matériel : LILYGO T-SIM7080G-S3 — UART modem RX=GPIO4 / TX=GPIO5 (vu de l'ESP32),
  PWRKEY=GPIO41 (HIGH = appui), DTR=GPIO42 ; PMU AXP2101 en I2C **SDA=15 / SCL=7** :
  DC3 3,0 V = alimentation du modem, BLDO1 3,3 V = convertisseur de niveaux ESP32 ↔ modem.
- Secrets : ne jamais versionner de vrais secrets ; `include/secrets.h` est ignoré par git,
  valeurs factices `TON_WIFI`, `TON_MOT_DE_PASSE`, `TON_API_KEY` dans `secrets.example.h`.

## Vérification obligatoire
PlatformIO n'est pas dans le PATH ; depuis un worktree, utiliser un dossier de travail court
(limite Windows de 260 caractères). Depuis `Embedded/esp32_gateway` (Bash) :
```bash
export PLATFORMIO_WORKSPACE_DIR="C:/pio_gw"
~/.platformio/penv/Scripts/pio.exe run -e lilygo-debug
~/.platformio/penv/Scripts/pio.exe run -e lilygo-release
~/.platformio/penv/Scripts/pio.exe run -e esp32dev-debug
```
Si `network_wifi.cpp` ou une partie propre au release est touchée, compiler aussi `esp32dev-release`.
Un avertissement « secrets.h absent » est attendu ; toute autre erreur ou nouvel avertissement
du code du projet = tâche non terminée.
Si `bms_protocol.h` change, vérifier par lecture que le simulateur
(`esp32_battery_simulator/*.ino`) compile toujours avec (il n'a pas de build PlatformIO).
Contrôler aussi : `grep -rn "Serial\." Embedded/esp32_gateway/src` ne doit rien trouver hors de `debug.h`.

## Ne pas faire
- Ne pas commiter, ne pas pousser, ne pas changer de branche : la session principale s'en charge.
- Ne pas modifier l'architecture documentée (`ARCHITECTURE.md`, `CONTEXT.md`, `CLAUDE.md`) :
  proposer le changement dans le compte rendu.
- Ne pas toucher à `Battery Backend/`.
- Ne pas flasher la carte (`-t upload`) sans que la tâche le demande explicitement.

## Compte rendu
1. Fichiers modifiés et résumé.
2. Résultat complet des vérifications (sortie de chaque `pio run`, taille RAM/Flash).
3. Impacts sur les interfaces partagées : corps du rapport, réponse `commands`, `reportId`,
   bornes de validation, cadence des rapports, `root_ca.h`, `bms_protocol.h`.
4. Ce qui reste à valider hors de l'environnement (carte réelle, batterie réelle, couverture
   LTE-M, consommation data sur le portail Things Mobile).
