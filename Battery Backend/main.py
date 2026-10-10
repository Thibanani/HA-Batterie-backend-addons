"""
Backend de l'add-on "Battery Backend" : recoit les rapports de l'ESP32
(POST /report), les publie en MQTT (format "MQTT Discovery" de Home
Assistant, donc les entites apparaissent automatiquement, sans YAML a
ecrire), et renvoie la commande en attente (coupure / retablissement
de la decharge) a appliquer au prochain reveil de l'ESP32.

Ecrit uniquement avec la bibliotheque standard (http.server, sqlite3,
json, secrets) + paho-mqtt, pour rester leger et eviter toute dependance a
compiler sur l'image Alpine/ARM de l'add-on.

Variables d'environnement (injectees par run.sh via bashio) :
  API_KEY, MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD, DB_PATH
"""

import json
import os
import re
import secrets
import signal
import sqlite3
import threading
import traceback
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import paho.mqtt.client as mqtt

API_KEY = os.environ.get("API_KEY", "change-moi")
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT") or 1883)
MQTT_USERNAME = os.environ.get("MQTT_USERNAME") or None
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD") or None
DB_PATH = os.environ.get("DB_PATH", "/data/batteries.db")
DISCOVERY_PREFIX = "homeassistant"

# Cles livrees en exemple (add-on, sketches) : a ne jamais garder en service.
DEFAULT_API_KEYS = {"change-moi", "change-moi-en-vrai-secret", "TON_API_KEY"}
MIN_API_KEY_LENGTH = 16

MAX_BODY_SIZE = 4096  # octets ; un rapport de l'ESP32 en fait ~150
HTTP_PORT = 8000
SOCKET_TIMEOUT_S = 15      # coupe les clients trop lents (attaque "slowloris")
MAX_CONNECTIONS = 16       # connexions traitees en parallele, au-dela : refusees
BATTERY_ID_RE = re.compile(r"[A-Za-z0-9:_-]{1,64}")

# Bornes physiques acceptees (batteries LiFePO4 de 12 a 48 V).
LIMITS = {
    "voltage_v": (0.0, 70.0),
    "current_a": (-1000.0, 1000.0),
    "soc_pct": (0, 100),
    "soh_pct": (0, 100),
}

# Adapte les adresses MAC et les noms a tes deux batteries reelles.
BATTERIES = {
    "AA:BB:CC:DD:EE:FF": "battery_1",
    "11:22:33:44:55:66": "battery_2",
}

db_lock = threading.Lock()

# Caracteres de controle echappes dans les logs (anti injection de fausses lignes).
_LOG_ESCAPES = {c: f"\\x{c:02x}" for c in [*range(32), 127]}


def log(message: str):
    print(message.translate(_LOG_ESCAPES), flush=True)


@contextmanager
def db():
    with db_lock:
        conn = sqlite3.connect(DB_PATH)
        try:
            yield conn
            conn.commit()
        finally:
            conn.close()


def init_db():
    with db() as conn:
        conn.execute(
            """CREATE TABLE IF NOT EXISTS battery_state (
                mac TEXT PRIMARY KEY, voltage REAL, current REAL,
                soc INTEGER, soh INTEGER, discharge_off INTEGER,
                updated_at TEXT
            )"""
        )
        conn.execute(
            """CREATE TABLE IF NOT EXISTS pending_commands (
                mac TEXT PRIMARY KEY, action TEXT
            )"""
        )


# ---------------- MQTT (paho-mqtt 1.6.x, API "v1") ----------------
mqtt_client = mqtt.Client()


def publish_discovery(client, slug: str):
    device = {
        "identifiers": [slug],
        "name": slug.replace("_", " ").title(),
        "manufacturer": "PowerQueen",
    }

    sensors = {
        "voltage": ("V", "voltage"),
        "current": ("A", "current"),
        "soc": ("%", "battery"),
        "soh": ("%", None),
    }
    for key, (unit, device_class) in sensors.items():
        payload = {
            "name": f"{slug} {key}",
            "unique_id": f"{slug}_{key}",
            "state_topic": f"{DISCOVERY_PREFIX}/sensor/{slug}/state",
            "unit_of_measurement": unit,
            "value_template": f"{{{{ value_json.{key} }}}}",
            "device": device,
        }
        if device_class:
            payload["device_class"] = device_class
        client.publish(
            f"{DISCOVERY_PREFIX}/sensor/{slug}_{key}/config",
            json.dumps(payload), retain=True,
        )

    switch_payload = {
        "name": f"{slug} decharge",
        "unique_id": f"{slug}_discharge",
        "state_topic": f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/state",
        "command_topic": command_topic(slug),
        "payload_on": "ON",
        "payload_off": "OFF",
        "device": device,
    }
    client.publish(
        f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/config",
        json.dumps(switch_payload), retain=True,
    )


def command_topic(slug: str) -> str:
    return f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/set"


# Seules ces deux valeurs sont acceptees : une valeur inattendue ne doit jamais
# retablir (ou couper) la decharge par erreur.
SWITCH_ACTIONS = {"OFF": "discharge_off", "ON": "discharge_on"}


def mqtt_callback(func):
    """Une exception dans un callback paho 1.6 arrete la boucle reseau : on la contient."""

    def wrapper(*args, **kwargs):
        try:
            func(*args, **kwargs)
        except Exception:
            log(f"Erreur dans le callback MQTT {func.__name__} :")
            traceback.print_exc()

    wrapper.__name__ = func.__name__
    return wrapper


@mqtt_callback
def on_connect(client, userdata, flags, rc):
    if rc != 0:
        log(f"Connexion MQTT refusee : {mqtt.connack_string(rc)} (nouvel essai automatique)")
        return
    log("MQTT connecte")
    for slug in BATTERIES.values():
        publish_discovery(client, slug)
        client.subscribe(command_topic(slug), qos=1)


@mqtt_callback
def on_connect_fail(client, userdata):
    log(f"Broker MQTT injoignable ({MQTT_HOST}:{MQTT_PORT}), nouvel essai automatique")


@mqtt_callback
def on_disconnect(client, userdata, rc):
    if rc != 0:
        log(f"MQTT deconnecte de facon inattendue (code {rc}), reconnexion automatique")


@mqtt_callback
def on_message(client, userdata, msg):
    # L'utilisateur a bascule le switch "decharge" dans Home Assistant.
    mac = next((m for m, s in BATTERIES.items() if command_topic(s) == msg.topic), None)
    if not mac:
        return
    payload = msg.payload.decode("ascii", errors="replace").strip()
    action = SWITCH_ACTIONS.get(payload)
    if action is None:
        log(f"Commande MQTT ignoree pour {mac} : valeur inattendue {payload[:20]!r}")
        return
    with db() as conn:
        conn.execute(
            "INSERT INTO pending_commands (mac, action) VALUES (?, ?) "
            "ON CONFLICT(mac) DO UPDATE SET action=excluded.action",
            (mac, action),
        )
    log(f"Commande en attente pour {mac} : {action}")


def get_all_status():
    """Renvoie le dernier etat connu de chaque batterie (pour GET /status)."""
    result = {}
    with db() as conn:
        rows = conn.execute(
            "SELECT mac, voltage, current, soc, soh, discharge_off, updated_at "
            "FROM battery_state"
        ).fetchall()
    for mac, voltage, current, soc, soh, discharge_off, updated_at in rows:
        slug = BATTERIES.get(mac, mac)
        result[slug] = {
            "mac": mac,
            "voltage_v": voltage,
            "current_a": current,
            "soc_pct": soc,
            "soh_pct": soh,
            "discharge_off": bool(discharge_off),
            "updated_at": updated_at,
        }
    return result


# ---------------- Securite : cle API et validation des rapports ----------------
def api_key_valid(given) -> bool:
    """Compare la cle recue a API_KEY en temps constant (anti timing-attack)."""
    if not API_KEY or given is None:
        return False
    return secrets.compare_digest(given.encode("utf-8"), API_KEY.encode("utf-8"))


class InvalidReport(ValueError):
    pass


def _number(data: dict, key: str, integer: bool = False):
    value = data.get(key)
    allowed = int if integer else (int, float)
    # bool est un int en Python : on le refuse explicitement.
    if isinstance(value, bool) or not isinstance(value, allowed):
        raise InvalidReport(f"{key} manquant ou de type invalide")
    low, high = LIMITS[key]
    if not (low <= value <= high):  # rejette aussi NaN et l'infini
        raise InvalidReport(f"{key} hors plage ({low} a {high})")
    return value


def parse_report(data) -> dict:
    """Valide le JSON d'un rapport ; renvoie un dict normalise ou leve InvalidReport."""
    if not isinstance(data, dict):
        raise InvalidReport("le corps doit etre un objet JSON")
    battery_id = data.get("battery_mac")
    if not isinstance(battery_id, str) or not BATTERY_ID_RE.fullmatch(battery_id):
        raise InvalidReport("battery_mac manquant ou invalide")
    discharge_off = data.get("discharge_off")
    if not isinstance(discharge_off, bool):
        raise InvalidReport("discharge_off doit etre un booleen")
    return {
        "mac": battery_id,
        "voltage": float(_number(data, "voltage_v")),
        "current": float(_number(data, "current_a")),
        "soc": _number(data, "soc_pct", integer=True),
        "soh": _number(data, "soh_pct", integer=True),
        "discharge_off": discharge_off,
    }


# ---------------- Serveur HTTP ----------------
class HttpError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status
        self.message = message


class Handler(BaseHTTPRequestHandler):
    server_version = "BatteryBackend"   # ne divulgue pas la version de Python
    timeout = SOCKET_TIMEOUT_S

    def version_string(self):
        return self.server_version

    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _dispatch(self, handler):
        try:
            handler()
        except HttpError as e:
            self._send_json(e.status, {"error": e.message})
        except (TimeoutError, ConnectionError):
            log(f"{self.client_address[0]} : connexion trop lente ou interrompue")
            self.close_connection = True
        except Exception:
            traceback.print_exc()
            self._send_json(500, {"error": "erreur interne"})

    def _require_api_key(self):
        if not api_key_valid(self.headers.get("X-API-Key")):
            log(f"Cle API invalide depuis {self.client_address[0]}")
            raise HttpError(401, "cle API invalide")

    def _read_json_body(self):
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            raise HttpError(411, "en-tete Content-Length requis")
        try:
            length = int(length_header)
        except ValueError:
            raise HttpError(400, "en-tete Content-Length invalide")
        if length <= 0:
            raise HttpError(400, "corps de requete vide")
        if length > MAX_BODY_SIZE:
            raise HttpError(413, f"corps de requete trop grand (max {MAX_BODY_SIZE} octets)")
        body = self.rfile.read(length)
        if len(body) != length:
            raise HttpError(400, "corps de requete incomplet")
        try:
            return json.loads(body)
        except (ValueError, RecursionError):
            raise HttpError(400, "JSON invalide")

    def do_GET(self):
        self._dispatch(self._handle_get)

    def do_POST(self):
        self._dispatch(self._handle_post)

    def _handle_get(self):
        if self.path == "/health":
            self._send_json(200, {"status": "ok"})
        elif self.path == "/status":
            self._require_api_key()
            self._send_json(200, get_all_status())
        else:
            raise HttpError(404, "not found")

    def _handle_post(self):
        if self.path != "/report":
            raise HttpError(404, "not found")
        self._require_api_key()

        try:
            report = parse_report(self._read_json_body())
        except InvalidReport as e:
            raise HttpError(400, str(e))

        mac = report["mac"]
        if mac not in BATTERIES:
            log(f"Rapport refuse : batterie inconnue {mac}")
            raise HttpError(404, "batterie inconnue")

        slug = BATTERIES[mac]
        voltage, current = report["voltage"], report["current"]
        soc, soh = report["soc"], report["soh"]
        discharge_off = report["discharge_off"]

        with db() as conn:
            conn.execute(
                "INSERT INTO battery_state (mac, voltage, current, soc, soh, "
                "discharge_off, updated_at) VALUES (?, ?, ?, ?, ?, ?, datetime('now')) "
                "ON CONFLICT(mac) DO UPDATE SET voltage=excluded.voltage, "
                "current=excluded.current, soc=excluded.soc, soh=excluded.soh, "
                "discharge_off=excluded.discharge_off, updated_at=excluded.updated_at",
                (mac, voltage, current, soc, soh, int(discharge_off)),
            )
            row = conn.execute(
                "SELECT action FROM pending_commands WHERE mac = ?", (mac,)
            ).fetchone()
            pending = row[0] if row else None
            if pending:
                conn.execute("DELETE FROM pending_commands WHERE mac = ?", (mac,))

        # Republie l'etat reel dans Home Assistant (sensor + switch).
        mqtt_client.publish(
            f"{DISCOVERY_PREFIX}/sensor/{slug}/state",
            json.dumps({"voltage": voltage, "current": current, "soc": soc, "soh": soh}),
        )
        mqtt_client.publish(
            f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/state",
            "OFF" if discharge_off else "ON",
            retain=True,
        )

        self._send_json(200, {"commands": [pending] if pending else []})

    def log_message(self, fmt, *args):
        log(f"{self.address_string()} - {fmt % args}")


class BackendHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer qui borne le nombre de connexions traitees en parallele."""

    request_queue_size = MAX_CONNECTIONS

    def __init__(self, server_address, handler_class, max_connections=MAX_CONNECTIONS):
        super().__init__(server_address, handler_class)
        self._slots = threading.BoundedSemaphore(max_connections)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            log(f"Trop de connexions simultanees : {client_address[0]} refuse")
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()


def make_server(host="0.0.0.0", port=HTTP_PORT, **kwargs):
    return BackendHTTPServer((host, port), Handler, **kwargs)


def check_api_key():
    if not API_KEY:
        log("ERREUR : l'option api_key est vide, tous les rapports seront refuses.")
    elif API_KEY in DEFAULT_API_KEYS or len(API_KEY) < MIN_API_KEY_LENGTH:
        log(
            "ATTENTION : la cle API est une valeur d'exemple ou fait moins de "
            f"{MIN_API_KEY_LENGTH} caracteres. Remplace-la par une valeur longue et "
            "aleatoire (ex. : python -c \"import secrets; print(secrets.token_urlsafe(32))\") "
            "dans l'add-on ET dans la passerelle."
        )


def start_mqtt():
    if MQTT_USERNAME:
        mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    mqtt_client.on_connect = on_connect
    mqtt_client.on_connect_fail = on_connect_fail
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_message = on_message
    mqtt_client.reconnect_delay_set(min_delay=1, max_delay=120)
    # connect_async + loop_start : le thread paho retente la connexion (premiere
    # comprise) tant que le broker est injoignable, le serveur HTTP demarre quand meme.
    mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()


def stop_mqtt():
    mqtt_client.disconnect()
    mqtt_client.loop_stop()


def main():
    check_api_key()
    init_db()
    start_mqtt()

    server = make_server()

    def request_stop(signum, frame):
        log("Arret demande, fermeture propre...")
        # shutdown() attend la fin de serve_forever() : a appeler depuis un autre thread.
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    log(f"Serveur demarre sur le port {HTTP_PORT}")
    try:
        server.serve_forever()
    finally:
        server.server_close()
        stop_mqtt()
        log("Serveur arrete")


if __name__ == "__main__":
    main()