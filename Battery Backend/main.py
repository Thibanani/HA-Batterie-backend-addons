"""
Backend de l'add-on "Battery Backend" : recoit les rapports de l'ESP32
(POST /report), les publie en MQTT (format "MQTT Discovery" de Home
Assistant, donc les entites apparaissent automatiquement, sans YAML a
ecrire), et renvoie la commande en attente (coupure / retablissement
de la decharge) a appliquer au prochain reveil de l'ESP32.

Ecrit uniquement avec la bibliotheque standard (http.server, sqlite3,
json) + paho-mqtt, pour rester leger et eviter toute dependance a
compiler sur l'image Alpine/ARM de l'add-on.

Variables d'environnement (injectees par run.sh via bashio) :
  API_KEY, MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD, DB_PATH
"""

import json
import os
import sqlite3
import threading
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

# Adapte les adresses MAC et les noms a tes deux batteries reelles.
BATTERIES = {
    "AA:BB:CC:DD:EE:FF": "battery_1",
    "11:22:33:44:55:66": "battery_2",
}

db_lock = threading.Lock()


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
        "command_topic": f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/set",
        "payload_on": "ON",
        "payload_off": "OFF",
        "device": device,
    }
    client.publish(
        f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/config",
        json.dumps(switch_payload), retain=True,
    )


def on_connect(client, userdata, flags, rc):
    print(f"MQTT connecte (code {rc})", flush=True)
    for mac, slug in BATTERIES.items():
        publish_discovery(client, slug)
        client.subscribe(f"{DISCOVERY_PREFIX}/switch/{slug}_discharge/set")


def on_message(client, userdata, msg):
    # L'utilisateur a bascule le switch "decharge" dans Home Assistant.
    slug = msg.topic.split("/")[2].replace("_discharge", "")
    mac = next((m for m, s in BATTERIES.items() if s == slug), None)
    if not mac:
        return
    action = "discharge_off" if msg.payload.decode() == "OFF" else "discharge_on"
    with db() as conn:
        conn.execute(
            "INSERT INTO pending_commands (mac, action) VALUES (?, ?) "
            "ON CONFLICT(mac) DO UPDATE SET action=excluded.action",
            (mac, action),
        )
    print(f"Commande en attente pour {mac} : {action}", flush=True)


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


# ---------------- Serveur HTTP ----------------
class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"status": "ok"})
        elif self.path == "/status":
            self._send_json(200, get_all_status())
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/report":
            self._send_json(404, {"error": "not found"})
            return

        if self.headers.get("X-API-Key") != API_KEY:
            self._send_json(401, {"error": "cle API invalide"})
            return

        length = int(self.headers.get("Content-Length", 0))
        try:
            data = json.loads(self.rfile.read(length))
            mac = data["battery_mac"]
            voltage = float(data["voltage_v"])
            current = float(data["current_a"])
            soc = int(data["soc_pct"])
            soh = int(data["soh_pct"])
            discharge_off = bool(data["discharge_off"])
        except (KeyError, ValueError, json.JSONDecodeError):
            self._send_json(400, {"error": "payload invalide"})
            return

        if mac not in BATTERIES:
            self._send_json(404, {"error": "batterie inconnue"})
            return

        slug = BATTERIES[mac]

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
        print(f"{self.address_string()} - {fmt % args}", flush=True)


def main():
    # setup mqtt
    if MQTT_USERNAME:
        mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message

    # connexion au service mqtt
    init_db()
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()

    # Demarrage du serveur http
    server = ThreadingHTTPServer(("0.0.0.0", 8000), Handler)
    print("Serveur demarre sur le port 8000", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()