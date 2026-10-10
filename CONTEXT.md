# CONTEXT.md — Contexte du projet "Gestion des batteries bateau"

> Mémoire de travail pour reprendre le développement rapidement : architecture,
> état réel du code, choix de conception, problèmes connus et pistes.
> Base de travail : branche `Reprise_par_Claude` (= commit `085f169`, deep sleep),
> poursuivie sur `claude/analyze-existing-code-489616`.

---

## 1. Objectif

Surveiller et piloter à distance les batteries de servitude LiFePO4 d'un bateau
(PowerQueen / LiTime / Redodo, BMS Bluetooth intégré) depuis **Home Assistant**,
même sans WiFi au mouillage (cible finale : liaison cellulaire LTE-M).

```
[Batterie(s) PowerQueen]  BMS BLE, service 0xFFE0
        │  BLE : requête statut 0x13 → trame 104 octets (en plusieurs notifications)
        ▼
[Passerelle ESP32]  Embedded/esp32_gateway  (cycle → deep sleep 60 s)
        │  HTTPS POST /report  (en-tête X-API-Key, certificat serveur vérifié)
        ▼
[Cloudflare] → [Add-on HA "Battery Backend"]  Python, port 8000, SQLite /data/batteries.db
        │  MQTT Discovery (préfixe "homeassistant")
        ▼
[Broker MQTT HA] ⇄ [Home Assistant : capteurs + switch "décharge"]
```

**Modèle "pull" des commandes** : quand l'utilisateur bascule le switch dans HA,
l'add-on stocke la commande dans `pending_commands`. Elle est renvoyée dans la
réponse au prochain `POST /report` (`{"commands": ["discharge_off"]}`), appliquée
en BLE par l'ESP32, qui relit le statut et renvoie aussitôt un rapport de
confirmation. Latence = durée du cycle de veille.

---

## 2. Structure du dépôt

```
HA-Batterie-backend-addons/
├── repository.yaml                  # déclaration du dépôt d'add-ons HA
├── Battery Backend/                 # add-on Home Assistant (version 1.1.0)
│   ├── config.yaml                  # options api_key / sensor_expire_after / batteries, watchdog
│   ├── build.yaml                   # images de base python 3.12 alpine (aarch64, amd64)
│   ├── Dockerfile / run.sh          # run.sh exporte API_KEY, SENSOR_EXPIRE_AFTER, MQTT_*, DB_PATH
│   ├── requirements.txt             # paho-mqtt==1.6.1
│   ├── CHANGELOG.md                 # affiché dans l'interface de l'add-on
│   ├── main.py                      # HTTP + SQLite + MQTT
│   └── tests/test_backend.py        # tests unitaires (unittest, sans broker)
└── Embedded/
    ├── bms_protocol.h               # protocole BMS partagé (passerelle + simulateur)
    ├── esp32_gateway/               # PROJET PLATFORMIO (passerelle)
    │   ├── platformio.ini           # envs : lilygo-debug (défaut), lilygo-release, esp32dev-debug, esp32dev-release
    │   ├── include/
    │   │   ├── secrets.example.h    # modèle versionné (WiFi, clé API)
    │   │   └── secrets.h            # vrais secrets, IGNORÉ par git (à créer)
    │   └── src/
    │       ├── main.cpp             # cycle complet, watchdog, deep sleep
    │       ├── ble_battery.h/.cpp   # dialogue BLE avec le BMS (lecture, commande, validation)
    │       ├── network.h/.cpp       # internet + HTTPS (WiFi ; GSM à venir, même interface)
    │       ├── config.h/.cpp        # réglages, URL, batteries
    │       ├── root_ca.h            # racines TLS acceptées (GTS R1/R4, ISRG X1/X2)
    │       ├── debug.h              # macros DEBUG_* : traces série en mode debug, rien en release
    │       └── types.h              # BatteryConfig, BatteryReading, Cmd
    ├── esp32_battery_simulator/     # sketch Arduino IDE : faux BMS en périphérique BLE
    └── test_http_report_mock/       # sketch Arduino IDE : POST /report en WiFi, sans BLE
```

---

## 3. Composants

### 3.1 Battery Backend (`Battery Backend/main.py`)
- Bibliothèque standard uniquement (`http.server`, `sqlite3`, `secrets`, `threading`)
  + `paho-mqtt` 1.6.1 (API v1 : `mqtt.Client()`, `on_connect(client, userdata, flags, rc)`).
- Batteries : option d'add-on `batteries` (liste `id` / `name`) lue dans `/data/options.json`
  (`OPTIONS_PATH`) → `BATTERIES = {id: name}`. `id` ∈ `[A-Za-z0-9:_-]{1,64}`, `name` ∈
  `[a-z0-9_]{1,40}` (sert dans les sujets MQTT). Fichier absent ou illisible → défauts
  `AA:BB:CC:DD:EE:FF → battery_1`, `11:22:33:44:55:66 → battery_2`.
- SQLite (accès sérialisé par `db_lock`) :
  - `battery_state(mac PK, voltage, current, soc, soh, discharge_off, updated_at)`
  - `pending_commands(mac PK, action)` — une seule commande en attente par batterie (la dernière gagne).
- Endpoints :
  - `POST /report` (clé `X-API-Key`, comparée en temps constant) → validation stricte
    (`parse_report` : types, plages 0-70 V / ±1000 A / 0-100 %, booléen), upsert de l'état,
    lecture + suppression de la commande en attente, publication MQTT, `{"commands": [...]}`.
    Erreurs : 400 / 401 / 404 (batterie inconnue) / 411 / 413 / 500, toujours en JSON.
  - `GET /status` (clé `X-API-Key`) → dernier état de chaque batterie.
  - `GET /health` (public) → `{"status": "ok"}`, utilisé par le watchdog du Supervisor.
- Serveur : `BackendHTTPServer` (16 connexions max), timeout de socket 15 s, corps ≤ 4 Ko,
  en-tête `Server: BatteryBackend`, logs échappés (`log()`).
- MQTT : `connect_async` + reconnexion automatique, callbacks protégés (`@mqtt_callback`),
  LWT `battery_backend/status` (`online` / `offline`), arrêt propre sur SIGTERM.
- MQTT Discovery, par batterie `name` :
  - capteurs `voltage` (V), `current` (A), `soc` (%), `soh` (%) — un seul `state_topic`
    JSON `homeassistant/sensor/<name>/state` ; `state_class: measurement`,
    `expire_after` (option `sensor_expire_after`, 900 s), `availability_topic`.
  - switch `homeassistant/switch/<name>_discharge/{config,state,set}` ; `ON` = décharge active,
    seules les valeurs exactes `ON` / `OFF` sont acceptées sur `set`.

### 3.2 Protocole BMS (`Embedded/bms_protocol.h`)
- GATT : service `FFE0`, notify `FFE1`, write `FFE2` (UUID 16 bits, repli 128 bits).
- Commande 8 octets : `00 00 04 01 <code> 55 AA <(0x04+code)&0xFF>`.
  Codes : statut `0x13`, charge on/off `0x0A/0x0B`, décharge on/off `0x0C/0x0D`.
- Trame statut ≥ 104 octets, little-endian : marqueur `0x65` @2, tension mV u32 @12,
  cellules u16 @16.., courant mA i32 @48 (négatif = décharge), état u32 @68
  (bit `0x80` = décharge coupée), SOC u16 @90, SOH u16 @92, checksum = somme & 0xFF (dernier octet).
- ⚠️ Offsets issus de projets open source (litime-ble-hacs, pq_bms_bluetooth),
  **pas encore validés sur la vraie batterie** → `DEBUG_DUMP_FRAME = true` dans `config.h` (build debug) pour vérifier.

### 3.3 Passerelle (`Embedded/esp32_gateway/src/`)
- Version de cette branche : **WiFi** (`WiFiClientSecure` + `setCACert(ROOT_CA_BUNDLE)`), pas encore GSM.
- Cycle : lecture BLE de chaque batterie (`readBattery`) → **si au moins une lecture valide**,
  connexion WiFi → jusqu'à `MAX_ROUNDS = 3` tours "rapport → commande → `applyCommand`
  (écrit, relit) → rapport de confirmation" → `networkStop()` → deep sleep `CYCLE_INTERVAL_MS` (60 s).
- Watchdog : cycle borné à `MAX_CYCLE_DURATION_S` (180 s) ; après un redémarrage anormal
  (watchdog, panic, brownout), le cycle est sauté et la carte se rendort.
- Délais : WiFi 15 s, TCP / TLS / réponse HTTP 10 s ; réponse serveur lue seulement si ≤ 512 octets.
- Réassemblage des notifications : buffer 256 o protégé par `portMUX`, trame complète
  après `FRAME_QUIET_MS` (150 ms) de silence et ≥ 104 octets.
- Validation : checksum bloquant (`STRICT_FRAME_CHECKSUM`), valeurs hors bornes rejetées
  (mêmes bornes que l'add-on).
- `BATTERIES[]` : `{adresse BLE, type d'adresse, reportId}` ; `reportId` = `id` d'une batterie
  de l'option `batteries` de l'add-on.

### 3.4 Simulateur (`Embedded/esp32_battery_simulator`)
- Second ESP32 en périphérique BLE "LT-SIM01", même GATT, trame découpée en notifications de 20 o.
- Console série 115200 : `soc <n>`, `discharge on|off`, `status`, `help`.
- Affiche son adresse BLE au démarrage (à recopier dans `BATTERIES[]` de la passerelle).

---

## 4. Choix de conception

1. **Énergie** : l'ESP32 est alimenté par le bord → cycle court puis deep sleep ; radio réseau
   allumée seulement s'il y a quelque chose à envoyer.
2. **Commandes asynchrones** (pull) : pas de connexion entrante vers le bateau.
3. **Backend sans dépendance compilée** : image Alpine ARM légère.
4. **Simulateur** pour développer sans toucher à la vraie batterie.
5. **TLS** : le domaine passe par Cloudflare, qui alterne entre Google Trust Services et
   Let's Encrypt → les racines des deux sont embarquées (pas d'épinglage d'une seule racine).
6. **Validation des deux côtés** : la passerelle filtre les trames corrompues, l'add-on ne fait
   confiance à aucune donnée reçue.

---

## 5. Historique à connaître

- `main` contient 3 commits **après** `Reprise_par_Claude` : `d66176d` (durcissement sécurité
  du backend "par l'IA"), `6fc85ff` (passage passerelle en GSM/TinyGSM, macros `DEBUG_*`),
  `c5719da` (tmp). Ils ne sont **pas** dans cette branche. Le durcissement du backend et les
  macros de debug ont été refaits ici ; le GSM reste à reprendre de `6fc85ff`.
- Un ancien CONTEXT.md (non versionné) annonçait des fonctions qui n'existaient pas ; ce
  fichier-ci décrit le code réel.

---

## 6. Problèmes connus

Corrigés sur cette branche : clé API en temps constant, `/status` protégé, validation stricte
des rapports, serveur HTTP durci, reconnexion MQTT, disponibilité / expiration / statistiques
des entités, batteries configurables, secrets de la passerelle hors du dépôt, HTTPS vérifié,
réseau à la demande, délais bornés, watchdog, trames BLE validées.

Restants :
- Une commande livrée dans une réponse HTTP perdue (coupure réseau) est perdue : l'utilisateur
  doit rebasculer le switch (l'état réel remonte au rapport suivant).
- Pas de limitation des essais de clé API par IP : derrière Cloudflare, l'IP vue est celle du
  proxy (un blocage par IP bloquerait aussi la passerelle). La clé doit donc être longue.
- BLE sans appairage (limite du BMS) : un appareil à portée peut imiter la batterie.
- La clé API d'exemple doit être remplacée (l'add-on l'annonce au démarrage).
- Changer le `name` d'une batterie crée de nouvelles entités HA (anciennes à supprimer à la main).
- `test_http_report_mock.ino` utilise encore `setInsecure()` (sketch de test seulement).

Sécurité dépôt :
- Le commit `6fc85ff` (sur `main`) contient un vrai mot de passe WiFi de box dans
  `test_http_report_mock.ino` → mot de passe à changer, il reste dans l'historique git.

---

## 7. Feuille de route envisagée

1. Migration embarqué vers PlatformIO — **passerelle faite** (simulateur et mock encore en .ino).
2. Repasser la passerelle en LTE-M (LILYGO T-SIM7080G-S3, TinyGSM) en reprenant `6fc85ff`,
   derrière l'interface `network.h` (`networkStop()` = extinction du modem). Attention : le
   TLS sera alors fait par le modem, la vérification du certificat est à refaire de ce côté.
3. Valider les offsets BMS sur la vraie batterie.
4. Plus tard : position GNSS, mesures PMU AXP2101 (solaire, batterie de secours).
