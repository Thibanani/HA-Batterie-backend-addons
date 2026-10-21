# CLAUDE.md

Surveillance de batteries LiFePO4 de bateau : passerelle ESP32 (BLE → HTTPS) + add-on
Home Assistant (HTTP → SQLite → MQTT Discovery). **Lire `CONTEXT.md`** pour l'architecture,
l'état réel du code, l'historique des branches et la liste des problèmes connus.

L'utilisateur échange en français ; code, commentaires et messages de commit en français.
Un commit par fonctionnalité.

## Commandes

### Backend (`Battery Backend/`)
Tests (aucun broker MQTT nécessaire, `paho-mqtt==1.6.1` installé) :
```bash
python -m unittest discover -s "Battery Backend/tests" -v
```
Lancement local (PowerShell) — l'add-on démarre même sans broker MQTT :
```powershell
$env:API_KEY="une-cle-de-test-tres-longue"; $env:MQTT_HOST="localhost"; $env:DB_PATH="./dev_batteries.db"
python "Battery Backend\main.py"
```
```bash
curl -X POST http://localhost:8000/report -H "Content-Type: application/json" -H "X-API-Key: une-cle-de-test-tres-longue" -d '{"battery_mac":"AA:BB:CC:DD:EE:FF","voltage_v":13.25,"current_a":-2.4,"soc_pct":75,"soh_pct":98,"discharge_off":false}'
```
Toute modification livrée de l'add-on doit incrémenter `version` dans `Battery Backend/config.yaml`
(sinon Home Assistant ne propose pas la mise à jour) et compléter `Battery Backend/CHANGELOG.md`.

### Passerelle — PlatformIO (`Embedded/esp32_gateway/`)
PlatformIO CLI : `~/.platformio/penv/Scripts/pio.exe` (pas dans le PATH).
Chaque carte existe en deux modes : **debug** (traces série actives) et **release**
(aucune trace, pour l'installation sur le bateau). Environnements : `lilygo-debug` (défaut),
`lilygo-release`, `esp32dev-debug`, `esp32dev-release`.
```bash
cd Embedded/esp32_gateway
pio run                                  # env par défaut : lilygo-debug
pio run -e lilygo-release                # firmware sans traces série
pio run -e lilygo-debug -t upload && pio device monitor
```
Secrets : copier `include/secrets.example.h` en `include/secrets.h` (ignoré par git) et le remplir
(clé API, code PIN de la SIM s'il est actif, WiFi du banc). Sans ce fichier, la compilation
utilise les valeurs factices et l'affiche en avertissement.
Mise au point du modem : décommenter `-DGSM_DUMP_AT_COMMANDS` dans `platformio.ini` (build debug)
pour voir tout le dialogue AT.

⚠️ **Limite Windows 260 caractères** : depuis un worktree (`.claude/worktrees/...`) les chemins
de `.pio/libdeps/.../NimBLE-Arduino/...` sont trop longs et la compilation échoue avec des
« No such file or directory » sur des includes internes. Compiler avec un dossier de travail court :
`PLATFORMIO_WORKSPACE_DIR="C:/pio_gw" pio run`.

### Simulateur et mock
Encore des sketches Arduino IDE (`.ino`) ; ils incluent `"../bms_protocol.h"`.

## Règles

### C++ / ESP32
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
  `LIMITS` de `main.py` : les modifier ensemble.
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
  DC3 3,0 V = alimentation du modem, BLDO1 3,3 V = convertisseur de niveaux ESP32 ↔ modem
  (source : exemples officiels LilyGO, `utilities.h`).

### Python (backend)
- Bibliothèque standard + `paho-mqtt==1.6.1` uniquement (image Alpine ARM, rien à compiler).
- API paho **v1** (`mqtt.Client()` sans `CallbackAPIVersion`). Tout callback MQTT porte
  `@mqtt_callback` (une exception non rattrapée arrête la boucle réseau de paho 1.6).
- Toute donnée reçue (HTTP, MQTT, options) est validée avant usage ; un nom de batterie
  finit dans des sujets MQTT, d'où `SLUG_RE`.
- Journaliser avec `log()` (échappe les caractères de contrôle), jamais `print()` direct.
- Requêtes SQL paramétrées ; tout accès SQLite via `with db() as conn:` (verrou `db_lock`).
- `ThreadingHTTPServer` : protéger toute structure partagée modifiée à l'exécution.
- Chaque fonctionnalité arrive avec ses tests dans `tests/test_backend.py`.

### Secrets
Ne jamais versionner de vrais secrets (mot de passe WiFi, clé API réelle) : valeurs factices
`TON_WIFI`, `TON_MOT_DE_PASSE`, `TON_API_KEY`, `change-moi-en-vrai-secret`.
