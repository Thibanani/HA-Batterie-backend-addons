# CONTEXT.md — Contexte du projet "Gestion des batteries bateau"

> Mémoire de travail pour reprendre le développement rapidement : architecture,
> état réel du code, choix de conception, problèmes connus et pistes.
> Base de travail : branche `Reprise_par_Claude` (= commit `085f169`, deep sleep).

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
        │  HTTPS POST /report  (en-tête X-API-Key)
        ▼
[Add-on HA "Battery Backend"]  Python, port 8000, SQLite /data/batteries.db
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
├── Battery Backend/                 # add-on Home Assistant
│   ├── config.yaml                  # options (api_key), service mqtt:want, port 8000
│   ├── build.yaml                   # images de base python 3.12 alpine (aarch64, amd64)
│   ├── Dockerfile / run.sh          # run.sh exporte API_KEY, MQTT_*, DB_PATH via bashio
│   ├── requirements.txt             # paho-mqtt==1.6.1
│   └── main.py                      # HTTP + SQLite + MQTT (~260 lignes)
└── Embedded/
    ├── bms_protocol.h               # protocole BMS partagé (passerelle + simulateur)
    ├── esp32_gateway/               # PROJET PLATFORMIO (passerelle)
    │   ├── platformio.ini           # envs : lilygo-t-sim7080g-s3 (défaut), esp32dev
    │   └── src/main.cpp             # ex-esp32_gateway.ino
    ├── esp32_battery_simulator/     # sketch Arduino IDE : faux BMS en périphérique BLE
    └── test_http_report_mock/       # sketch Arduino IDE : POST /report en WiFi, sans BLE
```

---

## 3. Composants

### 3.1 Battery Backend (`Battery Backend/main.py`)
- Bibliothèque standard uniquement (`http.server.ThreadingHTTPServer`, `sqlite3`,
  `threading`) + `paho-mqtt` 1.6.1 (API v1 : `mqtt.Client()`, `on_connect(client, userdata, flags, rc)`).
- `BATTERIES` : dict **codé en dur** `{identifiant: slug}` →
  `"AA:BB:CC:DD:EE:FF": "battery_1"`, `"11:22:33:44:55:66": "battery_2"`.
  Un identifiant inconnu reçoit `404 batterie inconnue`.
- SQLite (accès sérialisé par `db_lock`) :
  - `battery_state(mac PK, voltage, current, soc, soh, discharge_off, updated_at)`
  - `pending_commands(mac PK, action)` — une seule commande en attente par batterie (la dernière gagne).
- Endpoints :
  - `POST /report` (clé `X-API-Key`) → upsert état, lit + supprime la commande en attente,
    publie l'état MQTT, répond `{"commands": [...]}`.
  - `GET /status` → dernier état de chaque batterie (**sans authentification**).
  - `GET /health` → `{"status": "ok"}`.
- MQTT Discovery, par batterie `slug` :
  - capteurs `voltage` (V), `current` (A), `soc` (%), `soh` (%) — un seul `state_topic`
    JSON `homeassistant/sensor/<slug>/state`, extraits par `value_template`.
  - switch `homeassistant/switch/<slug>_discharge/{config,state,set}` ; `ON` = décharge active.

### 3.2 Protocole BMS (`Embedded/bms_protocol.h`)
- GATT : service `FFE0`, notify `FFE1`, write `FFE2` (UUID 16 bits, repli 128 bits).
- Commande 8 octets : `00 00 04 01 <code> 55 AA <(0x04+code)&0xFF>`.
  Codes : statut `0x13`, charge on/off `0x0A/0x0B`, décharge on/off `0x0C/0x0D`.
- Trame statut ≥ 104 octets, little-endian : marqueur `0x65` @2, tension mV u32 @12,
  cellules u16 @16.., courant mA i32 @48 (négatif = décharge), état u32 @68
  (bit `0x80` = décharge coupée), SOC u16 @90, SOH u16 @92, checksum = somme & 0xFF (dernier octet).
- ⚠️ Offsets issus de projets open source (litime-ble-hacs, pq_bms_bluetooth),
  **pas encore validés sur la vraie batterie** → `DEBUG_DUMP_FRAME = true` dans la passerelle pour vérifier.

### 3.3 Passerelle (`Embedded/esp32_gateway/src/main.cpp`)
- Version de cette branche : **WiFi** (`WiFiClientSecure` + `setInsecure()`), pas encore GSM.
- Cycle : lecture BLE de chaque batterie (`readBattery`) → connexion WiFi → jusqu'à
  `MAX_ROUNDS = 3` tours "rapport → commande → `applyCommand` (écrit, relit) → rapport de confirmation"
  → deep sleep `CYCLE_INTERVAL_MS` (60 s). Au réveil, `setup()` repart de zéro.
- Réassemblage des notifications : buffer 256 o protégé par `portMUX`, trame complète
  après `FRAME_QUIET_MS` (150 ms) de silence et ≥ 104 octets.
- Checksum faux → simple avertissement, la lecture est **conservée**.
- `BATTERIES[]` : `{adresse BLE, type d'adresse, reportId}` ; `reportId` doit être une clé de `BATTERIES` côté add-on.

### 3.4 Simulateur (`Embedded/esp32_battery_simulator`)
- Second ESP32 en périphérique BLE "LT-SIM01", même GATT, trame découpée en notifications de 20 o.
- Console série 115200 : `soc <n>`, `discharge on|off`, `status`, `help`.
- Affiche son adresse BLE au démarrage (à recopier dans `BATTERIES[]` de la passerelle).

---

## 4. Choix de conception

1. **Énergie** : l'ESP32 est alimenté par le bord → cycle court puis deep sleep.
2. **Commandes asynchrones** (pull) : pas de connexion entrante vers le bateau.
3. **Backend sans dépendance compilée** : image Alpine ARM légère.
4. **Simulateur** pour développer sans toucher à la vraie batterie.

---

## 5. Historique à connaître

- `main` contient 3 commits **après** `Reprise_par_Claude` : `d66176d` (durcissement sécurité
  du backend "par l'IA" : `secrets.compare_digest`, limites de taille, validation des plages,
  slugs dynamiques), `6fc85ff` (passage passerelle en GSM/TinyGSM, macros `DEBUG_*`),
  `c5719da` (tmp). Ils ne sont **pas** dans la base actuelle ; à réintégrer de façon sélective si besoin.
- Un ancien CONTEXT.md (non versionné) annonçait du rate limiting, une clé API sur `/status`,
  un `batteries_lock` et une reconnexion MQTT : **rien de cela n'existe** dans le code.

---

## 6. Problèmes connus (analyse du code, branche Reprise_par_Claude)

Backend :
- Clé API comparée par `!=` (pas en temps constant) ; `/status` public.
- `int(Content-Length)` non protégé, pas de taille max ; pas de validation de plages ;
  `TypeError` non attrapé (ex. `"voltage_v": null`) → exception dans le handler.
- `bool(data["discharge_off"])` : la chaîne `"false"` donne `True`.
- `mqtt_client.connect()` au démarrage sans `try` : broker absent → l'add-on plante.
- `on_message` : `msg.payload.decode()` sans gestion d'erreur.
- Capteurs sans `expire_after` / `availability` ni `state_class` → valeurs figées si la passerelle meurt,
  pas de statistiques long terme ; état capteur non `retain`.
- Liste `BATTERIES` codée en dur (pas d'option d'add-on).

Passerelle :
- Le WiFi est connecté dans `setup()` même si aucune batterie n'est lisible.
- `WiFiClientSecure.setInsecure()` : pas de vérification du certificat.
- Secrets (`WIFI_*`, `API_KEY`) dans le code source → à sortir dans un fichier non versionné.
- Checksum invalide toléré.

Sécurité dépôt :
- Le commit `6fc85ff` (sur `main`) contient un vrai mot de passe WiFi de box dans
  `test_http_report_mock.ino` → mot de passe à changer, il reste dans l'historique git.

---

## 7. Feuille de route envisagée

1. Migration embarqué vers PlatformIO — **passerelle faite** (simulateur et mock encore en .ino).
2. Sortir les secrets de la passerelle (`include/secrets.h` ignoré par git + exemple versionné).
3. Repasser la passerelle en LTE-M (LILYGO T-SIM7080G-S3, TinyGSM) en reprenant `6fc85ff`,
   en n'allumant le modem que si une batterie a été lue.
4. Durcir le backend (reprendre le meilleur de `d66176d`, + reconnexion MQTT, `expire_after`, `state_class`).
5. Valider les offsets BMS sur la vraie batterie.
6. Plus tard : position GNSS, mesures PMU AXP2101 (solaire, batterie de secours).
