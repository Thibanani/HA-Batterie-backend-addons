"""
Tests du backend (bibliotheque standard + paho-mqtt) :

    python -m unittest discover -s "Battery Backend/tests" -v

MQTT est remplace par un faux client : aucun broker n'est necessaire.
"""

import contextlib
import http.client
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

_TMP = tempfile.TemporaryDirectory()
os.environ["DB_PATH"] = os.path.join(_TMP.name, "test.db")
os.environ["API_KEY"] = "cle-de-test-suffisamment-longue"
os.environ["OPTIONS_PATH"] = os.path.join(_TMP.name, "absent.json")  # -> batteries par defaut

import main  # noqa: E402

API_KEY = os.environ["API_KEY"]
KNOWN_ID = next(iter(main.BATTERIES))


class FakeMessageInfo:
    def wait_for_publish(self, timeout=None):
        pass


class FakeMqtt:
    def __init__(self, connected=True):
        self.published = []
        self.subscribed = []
        self.connected = connected
        self.disconnected = False

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.published.append((topic, payload, retain))
        return FakeMessageInfo()

    def subscribe(self, topic, qos=0):
        self.subscribed.append(topic)

    def is_connected(self):
        return self.connected

    def disconnect(self):
        self.disconnected = True

    def loop_stop(self):
        pass


def valid_report(**overrides):
    report = {
        "battery_mac": KNOWN_ID,
        "voltage_v": 13.25,
        "current_a": -2.4,
        "soc_pct": 75,
        "soh_pct": 98,
        "discharge_off": False,
    }
    report.update(overrides)
    return report


class BackendTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        main.init_db()
        cls.server = main.make_server("127.0.0.1", 0)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.mqtt = FakeMqtt()
        main.mqtt_client = self.mqtt
        with main.db() as conn:
            conn.execute("DELETE FROM pending_commands")
            conn.execute("DELETE FROM battery_state")

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            if isinstance(body, (dict, list)):
                body = json.dumps(body)
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            raw = resp.read()
            try:
                payload = json.loads(raw) if raw else None
            except ValueError:
                payload = raw
            return resp.status, payload, resp
        finally:
            conn.close()

    def post_report(self, report, key=API_KEY):
        headers = {"Content-Type": "application/json"}
        if key is not None:
            headers["X-API-Key"] = key
        return self.request("POST", "/report", report, headers)


class AuthTests(BackendTestCase):
    def test_report_without_key_is_refused(self):
        status, _, _ = self.post_report(valid_report(), key=None)
        self.assertEqual(status, 401)

    def test_report_with_wrong_key_is_refused(self):
        status, _, _ = self.post_report(valid_report(), key="mauvaise-cle")
        self.assertEqual(status, 401)
        self.assertEqual(self.mqtt.published, [])

    def test_status_requires_key(self):
        self.assertEqual(self.request("GET", "/status")[0], 401)
        status, payload, _ = self.request("GET", "/status", headers={"X-API-Key": API_KEY})
        self.assertEqual(status, 200)
        self.assertIsInstance(payload, dict)

    def test_health_is_public(self):
        status, payload, _ = self.request("GET", "/health")
        self.assertEqual((status, payload), (200, {"status": "ok"}))

    def test_empty_api_key_refuses_everything(self):
        saved, main.API_KEY = main.API_KEY, ""
        try:
            self.assertFalse(main.api_key_valid(""))
            self.assertEqual(self.post_report(valid_report(), key="")[0], 401)
        finally:
            main.API_KEY = saved

    def test_non_ascii_key_does_not_crash(self):
        self.assertFalse(main.api_key_valid("clé-étrange"))


class ReportValidationTests(BackendTestCase):
    def assertRejected(self, report, status=400):
        got, payload, _ = self.post_report(report)
        self.assertEqual(got, status, payload)
        self.assertEqual(self.mqtt.published, [])

    def test_valid_report_is_accepted_and_published(self):
        status, payload, _ = self.post_report(valid_report())
        self.assertEqual((status, payload), (200, {"commands": []}))
        topics = [t for t, _, _ in self.mqtt.published]
        slug = main.BATTERIES[KNOWN_ID]
        self.assertIn(f"homeassistant/sensor/{slug}/state", topics)
        self.assertIn(f"homeassistant/switch/{slug}_discharge/state", topics)

    def test_integer_voltage_is_accepted(self):
        self.assertEqual(self.post_report(valid_report(voltage_v=13))[0], 200)

    def test_wrong_types_are_rejected(self):
        for field, value in [
            ("voltage_v", "13.2"),
            ("current_a", None),
            ("soc_pct", True),
            ("soc_pct", 75.5),
            ("soh_pct", "98"),
            ("discharge_off", "false"),
            ("discharge_off", 0),
            ("battery_mac", 42),
        ]:
            with self.subTest(field=field, value=value):
                self.assertRejected(valid_report(**{field: value}))

    def test_out_of_range_values_are_rejected(self):
        for field, value in [
            ("voltage_v", -1),
            ("voltage_v", 500.0),
            ("current_a", 5000),
            ("soc_pct", 101),
            ("soh_pct", -5),
        ]:
            with self.subTest(field=field, value=value):
                self.assertRejected(valid_report(**{field: value}))

    def test_nan_and_infinity_are_rejected(self):
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                body = json.dumps(valid_report()).replace("13.25", literal)
                self.assertRejected(body)

    def test_missing_field_is_rejected(self):
        report = valid_report()
        del report["soh_pct"]
        self.assertRejected(report)

    def test_suspicious_battery_ids_are_rejected(self):
        for battery_id in ["", "a/b", "battery#", "x" * 65, "AA:BB\n"]:
            with self.subTest(battery_id=battery_id):
                self.assertRejected(valid_report(battery_mac=battery_id))

    def test_unknown_battery_is_rejected(self):
        self.assertRejected(valid_report(battery_mac="00:00:00:00:00:00"), status=404)

    def test_malformed_bodies_are_rejected(self):
        for body in ["pas du json", "[1, 2]", "42", "[" * 3000]:
            with self.subTest(body=body[:20]):
                self.assertRejected(body)

    def test_oversized_body_is_rejected(self):
        report = valid_report(padding="x" * (main.MAX_BODY_SIZE + 1))
        self.assertRejected(report, status=413)


class HttpHardeningTests(BackendTestCase):
    def test_security_headers_and_no_python_version(self):
        _, _, resp = self.request("GET", "/health")
        self.assertEqual(resp.getheader("Server"), "BatteryBackend")
        self.assertEqual(resp.getheader("X-Content-Type-Options"), "nosniff")
        self.assertEqual(resp.getheader("Cache-Control"), "no-store")

    def test_errors_are_json(self):
        status, payload, _ = self.request("GET", "/inexistant")
        self.assertEqual((status, payload), (404, {"error": "not found"}))

    def test_log_escapes_control_characters(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            main.log("ligne\nFAUSSE LIGNE\x1b[31m")
        self.assertEqual(out.getvalue(), "ligne\\x0aFAUSSE LIGNE\\x1b[31m\n")


class SlowClientTests(unittest.TestCase):
    """Serveur dedie avec un timeout court et une seule connexion a la fois."""

    def setUp(self):
        class FastTimeoutHandler(main.Handler):
            timeout = 0.5

        self.server = main.BackendHTTPServer(("127.0.0.1", 0), FastTimeoutHandler, max_connections=1)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def open_idle_connection(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        sock.sendall(b"POST /report HTTP/1.1\r\n")  # requete jamais terminee
        return sock

    def test_slow_client_is_disconnected(self):
        with contextlib.redirect_stdout(io.StringIO()), self.open_idle_connection() as sock:
            start = time.monotonic()
            self.assertEqual(sock.recv(1024), b"")
            self.assertLess(time.monotonic() - start, 3)

    def test_connections_beyond_limit_are_refused(self):
        with contextlib.redirect_stdout(io.StringIO()) as out, self.open_idle_connection():
            time.sleep(0.2)  # laisse le serveur accepter la premiere connexion
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
            with self.assertRaises((http.client.RemoteDisconnected, ConnectionError)):
                conn.request("GET", "/health")
                conn.getresponse()
            conn.close()
        self.assertIn("Trop de connexions simultanees", out.getvalue())


class PendingCommandTests(BackendTestCase):
    def test_pending_command_is_delivered_once(self):
        with main.db() as conn:
            conn.execute(
                "INSERT INTO pending_commands (mac, action) VALUES (?, ?)",
                (KNOWN_ID, "discharge_off"),
            )
        self.assertEqual(self.post_report(valid_report())[1], {"commands": ["discharge_off"]})
        self.assertEqual(self.post_report(valid_report())[1], {"commands": []})


class FakeMessage:
    def __init__(self, topic, payload):
        self.topic = topic
        self.payload = payload


class MqttCallbackTests(BackendTestCase):
    def pending(self):
        with main.db() as conn:
            return dict(conn.execute("SELECT mac, action FROM pending_commands").fetchall())

    def switch_message(self, payload, slug=None):
        slug = slug or main.BATTERIES[KNOWN_ID]
        with contextlib.redirect_stdout(io.StringIO()):
            main.on_message(self.mqtt, None, FakeMessage(main.command_topic(slug), payload))

    def test_off_and_on_queue_the_matching_command(self):
        self.switch_message(b"OFF")
        self.assertEqual(self.pending(), {KNOWN_ID: "discharge_off"})
        self.switch_message(b"ON")
        self.assertEqual(self.pending(), {KNOWN_ID: "discharge_on"})

    def test_unexpected_payloads_are_ignored(self):
        for payload in [b"", b"on", b"TOGGLE", b"\xff\xfe", b"OFF; DROP TABLE"]:
            with self.subTest(payload=payload):
                self.switch_message(payload)
                self.assertEqual(self.pending(), {})

    def test_unknown_topic_is_ignored(self):
        self.switch_message(b"OFF", slug="inconnue")
        self.assertEqual(self.pending(), {})

    def test_callback_exception_does_not_escape(self):
        saved = main.db

        def broken_db():
            raise RuntimeError("base indisponible")

        main.db = broken_db
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                main.on_message(self.mqtt, None, FakeMessage(main.command_topic(main.BATTERIES[KNOWN_ID]), b"OFF"))
        finally:
            main.db = saved

    def test_refused_connection_publishes_nothing(self):
        with contextlib.redirect_stdout(io.StringIO()):
            main.on_connect(self.mqtt, None, {}, 5)
        self.assertEqual((self.mqtt.published, self.mqtt.subscribed), ([], []))

    def test_connection_publishes_discovery_and_subscribes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            main.on_connect(self.mqtt, None, {}, 0)
        self.assertEqual(
            sorted(self.mqtt.subscribed),
            sorted(main.command_topic(s) for s in main.BATTERIES.values()),
        )
        configs = [t for t, _, retain in self.mqtt.published if t.endswith("/config") and retain]
        self.assertEqual(len(configs), 5 * len(main.BATTERIES))


class HomeAssistantEntityTests(BackendTestCase):
    def discovery(self, slug):
        main.publish_discovery(self.mqtt, slug)
        return {t: json.loads(p) for t, p, _ in self.mqtt.published if t.endswith("/config")}

    def test_sensors_have_statistics_availability_and_expiry(self):
        configs = self.discovery("battery_1")
        voltage = configs["homeassistant/sensor/battery_1_voltage/config"]
        self.assertEqual(voltage["state_class"], "measurement")
        self.assertEqual(voltage["availability_topic"], main.AVAILABILITY_TOPIC)
        self.assertEqual(voltage["expire_after"], main.SENSOR_EXPIRE_AFTER)
        switch = configs["homeassistant/switch/battery_1_discharge/config"]
        self.assertEqual(switch["availability_topic"], main.AVAILABILITY_TOPIC)
        self.assertNotIn("expire_after", switch)

    def test_expiry_can_be_disabled(self):
        saved, main.SENSOR_EXPIRE_AFTER = main.SENSOR_EXPIRE_AFTER, 0
        try:
            configs = self.discovery("battery_1")
        finally:
            main.SENSOR_EXPIRE_AFTER = saved
        self.assertNotIn("expire_after", configs["homeassistant/sensor/battery_1_soc/config"])

    def test_online_is_announced_after_discovery(self):
        with contextlib.redirect_stdout(io.StringIO()):
            main.on_connect(self.mqtt, None, {}, 0)
        self.assertEqual(self.mqtt.published[-1], (main.AVAILABILITY_TOPIC, "online", True))

    def test_clean_stop_announces_offline(self):
        main.stop_mqtt()
        self.assertEqual(self.mqtt.published, [(main.AVAILABILITY_TOPIC, "offline", True)])
        self.assertTrue(self.mqtt.disconnected)

    def test_stop_without_connection_only_disconnects(self):
        self.mqtt.connected = False
        main.stop_mqtt()
        self.assertEqual(self.mqtt.published, [])
        self.assertTrue(self.mqtt.disconnected)

    def test_null_mqtt_service_values_do_not_crash_startup(self):
        # Broker absent : bashio peut renvoyer "null" ou rien pour le service MQTT.
        env = dict(os.environ, MQTT_HOST="", MQTT_PORT="null")
        result = subprocess.run(
            [sys.executable, "-c", "import main; print(main.MQTT_HOST, main.MQTT_PORT)"],
            cwd=str(Path(main.__file__).parent), env=env, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], "localhost 1883")

    def test_invalid_integer_option_falls_back_to_default_value(self):
        os.environ["TEST_ENTIER"] = "null"
        try:
            self.assertEqual(main._int_env("TEST_ENTIER", 900), 900)
        finally:
            del os.environ["TEST_ENTIER"]


class BatteryOptionsTests(unittest.TestCase):
    def load(self, content):
        path = os.path.join(_TMP.name, "options.json")
        with open(path, "w", encoding="utf-8") as f:
            f.write(content if isinstance(content, str) else json.dumps(content))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            batteries = main.load_batteries(path)
        return batteries, out.getvalue()

    def test_batteries_are_read_from_options(self):
        batteries, _ = self.load({"batteries": [
            {"id": "60:55:F9:21:CB:A2", "name": "service"},
            {"id": "60:55:F9:21:CB:A3", "name": "moteur_2"},
        ]})
        self.assertEqual(batteries, {"60:55:F9:21:CB:A2": "service", "60:55:F9:21:CB:A3": "moteur_2"})

    def test_invalid_and_duplicate_entries_are_ignored(self):
        batteries, out = self.load({"batteries": [
            {"id": "AA:01", "name": "ok"},
            {"id": "AA:02", "name": "a/b"},          # casserait les sujets MQTT
            {"id": "AA:03", "name": "#"},
            {"id": "AA 04", "name": "espace"},
            {"id": "AA:01", "name": "doublon_id"},
            {"id": "AA:05", "name": "ok"},           # doublon de nom
            "pas un objet",
            {"id": 42, "name": "type"},
        ]})
        self.assertEqual(batteries, {"AA:01": "ok"})
        self.assertEqual(out.count("Batterie ignoree"), 7)

    def test_missing_or_unreadable_options_fall_back_to_defaults(self):
        self.assertEqual(main.load_batteries(os.path.join(_TMP.name, "nope.json")), main.DEFAULT_BATTERIES)
        for content in ["{pas du json", {"api_key": "x"}, "[]"]:
            with self.subTest(content=content):
                self.assertEqual(self.load(content)[0], main.DEFAULT_BATTERIES)

    def test_empty_list_means_no_battery(self):
        batteries, out = self.load({"batteries": []})
        self.assertEqual(batteries, {})
        self.assertIn("aucune batterie valide", out)


if __name__ == "__main__":
    unittest.main()
