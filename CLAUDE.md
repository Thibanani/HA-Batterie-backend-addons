# CLAUDE.md

Surveillance de batteries LiFePO4 de bateau : passerelle ESP32 (BLE → HTTPS) + add-on
Home Assistant (HTTP → SQLite → MQTT Discovery). **Lire `CONTEXT.md`** pour l'architecture,
l'état réel du code, l'historique des branches et la liste des problèmes connus.

L'utilisateur échange en français ; code, commentaires et messages de commit en français.

## Commandes

### Backend (`Battery Backend/`)
```bash
python -m py_compile "Battery Backend/main.py"
```
Lancement local (PowerShell) — nécessite un broker MQTT sur localhost :
```powershell
$env:API_KEY="change-moi-en-vrai-secret"; $env:MQTT_HOST="localhost"; $env:DB_PATH="./dev_batteries.db"
python "Battery Backend\main.py"
```
```bash
curl -X POST http://localhost:8000/report -H "Content-Type: application/json" -H "X-API-Key: change-moi-en-vrai-secret" -d '{"battery_mac":"AA:BB:CC:DD:EE:FF","voltage_v":13.25,"current_a":-2.4,"soc_pct":75,"soh_pct":98,"discharge_off":false}'
```
Toute modification livrée de l'add-on doit incrémenter `version` dans `Battery Backend/config.yaml`
(sinon Home Assistant ne propose pas la mise à jour).

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
- `reportId` côté passerelle = clé de `BATTERIES` dans `main.py`.
- Cible matérielle finale : LILYGO T-SIM7080G-S3 — modem TX=GPIO5, RX=GPIO4, PWRKEY=GPIO41 ;
  PMU AXP2101 en I2C SDA=18 / SCL=17 selon la révision.

### Python (backend)
- Bibliothèque standard + `paho-mqtt==1.6.1` uniquement (image Alpine ARM, rien à compiler).
- API paho **v1** (`mqtt.Client()` sans `CallbackAPIVersion`).
- Requêtes SQL paramétrées ; tout accès SQLite via `with db() as conn:` (verrou `db_lock`).
- `ThreadingHTTPServer` : protéger toute structure partagée modifiée à l'exécution.

### Secrets
Ne jamais versionner de vrais secrets (mot de passe WiFi, clé API réelle) : valeurs factices
`TON_WIFI`, `TON_MOT_DE_PASSE`, `change-moi-en-vrai-secret`.
