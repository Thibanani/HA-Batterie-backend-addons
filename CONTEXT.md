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
[Passerelle ESP32]  Embedded/esp32_gateway  (cycle → deep sleep 15 min)
        │  HTTPS POST /report : LTE-M (modem SIM7080G, SIM Things Mobile) ; WiFi sur le banc
        │  (en-tête X-API-Key, certificat serveur vérifié par l'ESP32)
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
├── Battery Backend/                 # add-on Home Assistant (version 1.1.1)
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
    │   │   ├── secrets.example.h    # modèle versionné (clé API, code PIN SIM, WiFi du banc)
    │   │   └── secrets.h            # vrais secrets, IGNORÉ par git (à créer)
    │   └── src/
    │       ├── main.cpp             # cycle complet, watchdog, deep sleep
    │       ├── ble_battery.h/.cpp   # dialogue BLE avec le BMS (lecture, commande, validation)
    │       ├── network.h            # interface de la liaison internet (transport TCP)
    │       ├── network_gsm.cpp      # LTE-M : PMU, modem SIM7080G (TinyGSM), APN, extinction
    │       ├── network_wifi.cpp     # WiFi (banc de test, envs esp32dev-*)
    │       ├── report.h/.cpp        # rapport HTTPS commun (SSLClient + ArduinoHttpClient)
    │       ├── buffered_client.h    # requête HTTP envoyée en un seul bloc
    │       ├── config.h/.cpp        # réglages, serveur, APN, batteries
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
    `expire_after` (option `sensor_expire_after`, 10800 s ; avertissement au démarrage si
    ≤ 3600 s), `availability_topic`.
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
- Transport choisi à la compilation : **LTE-M** sur la LILYGO (`-DNETWORK_GSM`,
  `network_gsm.cpp`), **WiFi** sur l'ESP32 du banc (`-DNETWORK_WIFI`, `network_wifi.cpp`).
  Interface commune `network.h` : `networkConnect()`, `networkIsConnected()`, `networkStop()`,
  `networkTcpClient()`.
- HTTPS commun (`report.cpp`) : `networkTcpClient()` → `SSLClient` (TLS mbedtls sur l'ESP32,
  `ROOT_CA_BUNDLE`, SNI et nom d'hôte vérifiés) → `BufferedClient<512>` (requête en un seul
  envoi) → `ArduinoHttpClient`. User-Agent `ESP32HTTPClient` (celui déjà accepté par Cloudflare).
- Cycle : lecture BLE de chaque batterie (`readBattery`) → **si une valeur a changé ou si le
  rapport de vie est dû**, `networkConnect()` → jusqu'à `MAX_ROUNDS = 3` tours "rapport →
  commande → `applyCommand` (écrit, relit) → rapport de confirmation" → `networkStop()` →
  deep sleep `CYCLE_INTERVAL_MS` (15 min).
- Économie de data (forfait LTE-M facturé au Mo) :
  - rapport seulement si changement (tension ≥ 0,05 V, courant ≥ 1 A, SOC ≥ 1 point, SOH ou
    état de décharge) ou rapport de vie toutes les `REPORT_HEARTBEAT_S` (1 h) ; dernières
    valeurs envoyées et compteur en mémoire RTC (`g_sent`, `g_cyclesSinceReport`) ;
  - une seule connexion TLS par cycle (keep-alive, `g_tls` / `g_http` dans `report.cpp`),
    jamais fermée explicitement (l'extinction du modem suffit) ; un nouvel essai si le
    serveur l'a fermée ;
  - tension / courant envoyés avec 2 décimales.
  Conséquence : avec des valeurs stables, une commande HA attend jusqu'à 1 h (rapport de vie).
- LTE-M (`network_gsm.cpp`), broches et séquences des exemples officiels LilyGO :
  - PMU AXP2101 en I2C SDA 15 / SCL 7 : DC3 = 3,0 V (alimentation du SIM7080G), BLDO1 = 3,3 V
    (convertisseur de niveaux ESP32 ↔ modem), mesure TS désactivée. UART modem RX 4 / TX 5,
    PWRKEY GPIO 41 (HIGH = appui), DTR GPIO 42 (LOW = éveillé).
  - Démarrage : DC3 + BLDO1, test AT (modem déjà allumé ?), sinon impulsion PWRKEY (2 essais × 10 s).
  - SIM (PIN optionnel `SECRET_SIM_PIN`, jamais retenté après un refus) → `CFUN=0` → `CNMP=38`
    (LTE seul) → `CMNB=GSM_RADIO_MODE` (1 = LTE-M) → `CGDCONT` / `CNCFG` APN `TM` → `CFUN=1` →
    inscription (itinérance acceptée, ≤ 90 s) → `CNACT=0,1` (≤ 30 s). Le `gprsConnect()` de
    TinyGSM n'est volontairement pas utilisé (il détache puis rattache le réseau, attentes très
    longues, `DEACTIVE` compté comme un succès).
  - Arrêt : `AT+CPOWD=1` puis coupure de DC3 et BLDO1 ; fait aussi au démarrage après un
    redémarrage anormal (le modem a pu rester allumé).
  - Debug : opérateur, signal, IP, `+CPSI` (technologie réellement utilisée) ;
    `-DGSM_DUMP_AT_COMMANDS` affiche tout le dialogue AT.
- Délais : HTTP (TCP + TLS + réponse) 20 s en LTE-M, 10 s en WiFi ; connexion WiFi 15 s ;
  réponse serveur lue seulement si ≤ 512 octets.
- Watchdog : cycle borné à `MAX_CYCLE_DURATION_S` (420 s en LTE-M, 180 s en WiFi) ; après un
  redémarrage anormal (watchdog, panic, brownout), le modem est éteint, le cycle sauté et la
  carte se rendort.
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
7. **TLS sur l'ESP32, pas sur le modem** : mêmes racines et même vérification qu'en WiFi, pas
   de certificat à charger dans le système de fichiers du modem, couche HTTPS identique pour
   les deux transports (le banc WiFi teste le même code que le bateau).

---

## 5. Historique à connaître

- `main`, `Reprise_par_Claude` et cette branche partagent désormais le même historique
  (réécrit par l'utilisateur). Les anciens commits `d66176d`, `6fc85ff` (premier essai GSM) et
  `c5719da` ne sont plus dans aucune branche ; le durcissement du backend, les macros de debug
  et le GSM ont été refaits ici.
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

LTE-M (code compilé, **pas encore testé sur la carte**) :
- **Couverture Things Mobile à vérifier** : leur FAQ indique que LTE-M et NB-IoT ne sont pas
  disponibles dans la plupart des pays couverts, et le SIM7080G ne fait que ces deux
  technologies. Vérifier sur le portail IoT (profil d'itinérance TM-1) avant installation.
- Coût data : chaque cycle qui utilise le réseau paie une négociation TLS complète (~4 Ko,
  surtout la chaîne de certificats Cloudflare) ; SSLClient n'expose pas la reprise de session
  TLS (il faudrait forker la bibliothèque). Valeurs stables : un rapport de vie par heure,
  soit de l'ordre de 0,1 Mo/jour (estimation, à vérifier sur le portail Things Mobile).
- Les en-têtes de réponse ajoutés par Cloudflare (NEL, Report-To, alt-svc...) pèsent plusieurs
  centaines d'octets par réponse ; désactivables côté Cloudflare, pas côté code.
- Inscription LTE-M refaite à chaque réveil (modem hors tension pendant la veille) : jusqu'à
  ~2 min de radio active si la couverture est mauvaise ; pas encore de PSM ni d'espacement
  des tentatives après des échecs répétés.
- `SSLClient` (govorox) est sous licence GPL-3.0 : à garder en tête si le firmware est distribué.

Sécurité dépôt :
- L'ancien commit `6fc85ff` contenait un vrai mot de passe WiFi de box. Il n'est plus dans
  aucune branche mais peut rester accessible sur GitHub par son hash : changer le mot de passe
  reste la seule protection fiable.

---

## 7. Feuille de route envisagée

1. Migration embarqué vers PlatformIO — **passerelle faite** (simulateur et mock encore en .ino).
2. LTE-M — **code fait**, à valider sur la carte : couverture Things Mobile, temps
   d'inscription, consommation data réelle (portail Things Mobile).
3. Réduire énergie et data en LTE-M : intervalle de production (15 min ou plus), PSM du modem,
   reprise de session TLS, espacement des essais après des échecs réseau.
4. Valider les offsets BMS sur la vraie batterie.
5. Plus tard : position GNSS, mesures PMU AXP2101 (solaire, batterie de secours).
