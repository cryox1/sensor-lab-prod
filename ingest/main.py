"""
MQTT -> Postgres ingest.

Subscribes to sensors/+/+/telemetry on Mosquitto, validates the JSON, and
inserts each reading into the telemetry table. Idempotent via
PRIMARY KEY (device_id, ts) + ON CONFLICT DO NOTHING.

Malformed/unusable payloads are written to the telemetry_dlq table so they
can be inspected later (the old Kafka DLQ topic, now in Postgres).

Per-message insert: sensor volume is low, so no batching is needed. If
throughput ever grows, reintroduce a buffer flushed on size/time.
"""
import json
import logging
import os
import signal
import sys
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import psycopg

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("ingest")

MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = "sensors/+/+/telemetry"

# Sensor-specific fields (temp_c, humidity, eco2_ppm, tvoc_ppb, ...) are
# all optional — different devices send different subsets. Only the
# identity + timestamp are mandatory.
REQUIRED_FIELDS = ("device_id", "ts")

PG_DSN = (
    f"host={os.environ['POSTGRES_HOST']} "
    f"port={os.environ.get('POSTGRES_PORT', '5432')} "
    f"dbname={os.environ['POSTGRES_DB']} "
    f"user={os.environ['POSTGRES_USER']} "
    f"password={os.environ['POSTGRES_PASSWORD']}"
)

INSERT_SQL = """
INSERT INTO telemetry (ts, device_id, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa)
VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
ON CONFLICT (device_id, ts) DO NOTHING
"""

DLQ_SQL = "INSERT INTO telemetry_dlq (topic, payload, reason) VALUES (%s, %s, %s)"

# Runs on every start. Safe on fresh DBs and on existing volumes where
# init.sql already ran without these columns.
MIGRATIONS = (
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS eco2_ppm INTEGER",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS tvoc_ppb INTEGER",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS aqi      SMALLINT",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lat       DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lon       DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS alt_m     DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS sats      SMALLINT",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS speed_kmh DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS batt_v    DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS pressure_hpa DOUBLE PRECISION",
    # DLQ for malformed payloads (was the Kafka sensors.telemetry.dlq topic).
    """
    CREATE TABLE IF NOT EXISTS telemetry_dlq (
      id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
      received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
      topic       TEXT,
      payload     TEXT,
      reason      TEXT
    )
    """,
)


def connect_pg(retries: int = 30) -> psycopg.Connection:
    for attempt in range(retries):
        try:
            conn = psycopg.connect(PG_DSN, autocommit=True)
            log.info("connected to Postgres")
            with conn.cursor() as cur:
                for stmt in MIGRATIONS:
                    cur.execute(stmt)
            return conn
        except psycopg.OperationalError as exc:
            log.warning("Postgres not ready (%s/%s): %s", attempt + 1, retries, exc)
            time.sleep(2)
    raise RuntimeError("could not connect to Postgres")


def _opt_float(payload: dict, key: str):
    v = payload.get(key)
    return float(v) if v is not None else None


def _opt_int(payload: dict, key: str):
    v = payload.get(key)
    return int(v) if v is not None else None


def to_row(payload: dict):
    ts = datetime.fromtimestamp(int(payload["ts"]), tz=timezone.utc)
    return (
        ts,
        str(payload["device_id"]),
        _opt_float(payload, "temp_c"),
        _opt_float(payload, "humidity"),
        _opt_float(payload, "heat_index_c"),
        _opt_int(payload, "eco2_ppm"),
        _opt_int(payload, "tvoc_ppb"),
        _opt_int(payload, "aqi"),
        _opt_float(payload, "lat"),
        _opt_float(payload, "lon"),
        _opt_float(payload, "alt_m"),
        _opt_int(payload, "sats"),
        _opt_float(payload, "speed_kmh"),
        _opt_float(payload, "batt_v"),
        _opt_float(payload, "pressure_hpa"),
    )


class Ingest:
    def __init__(self):
        self.conn = connect_pg()

    def _reconnect_pg(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.conn = connect_pg()

    def _execute(self, sql: str, params):
        """Run a write, transparently reconnecting once if the link dropped."""
        try:
            with self.conn.cursor() as cur:
                cur.execute(sql, params)
        except psycopg.OperationalError as exc:
            log.warning("Postgres link lost (%s) — reconnecting", exc)
            self._reconnect_pg()
            with self.conn.cursor() as cur:
                cur.execute(sql, params)

    def to_dlq(self, topic: str, raw: bytes, reason: str):
        try:
            self._execute(DLQ_SQL, (topic, raw.decode("utf-8", "replace"), reason))
        except Exception as exc:
            log.error("failed to write DLQ row: %s", exc)

    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        log.info("MQTT connected (rc=%s), subscribing %s", reason_code, MQTT_TOPIC)
        client.subscribe(MQTT_TOPIC, qos=1)

    def on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            log.warning("bad payload on %s: %s", msg.topic, exc)
            self.to_dlq(msg.topic, msg.payload, f"decode error: {exc}")
            return

        if not all(k in payload for k in REQUIRED_FIELDS):
            log.warning("missing fields on %s: %s", msg.topic, payload)
            self.to_dlq(msg.topic, msg.payload, "missing required fields")
            return

        try:
            row = to_row(payload)
        except (KeyError, ValueError, TypeError) as exc:
            log.warning("unconvertible payload on %s: %s", msg.topic, exc)
            self.to_dlq(msg.topic, msg.payload, f"bad value: {exc}")
            return

        try:
            self._execute(INSERT_SQL, row)
            log.info("inserted %s key=%s", msg.topic, payload["device_id"])
        except Exception as exc:
            log.error("insert failed for %s: %s", msg.topic, exc)
            self.to_dlq(msg.topic, msg.payload, f"insert error: {exc}")


def main():
    ingest = Ingest()
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="mqtt-pg-ingest",
    )
    client.on_connect = ingest.on_connect
    client.on_message = ingest.on_message

    def shutdown(signum, frame):
        log.info("shutting down (signal %s)", signum)
        client.disconnect()
        try:
            ingest.conn.close()
        except Exception:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    while True:
        try:
            log.info("connecting to MQTT %s:%s", MQTT_HOST, MQTT_PORT)
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except Exception as exc:
            log.error("MQTT loop crashed: %s — retrying in 5s", exc)
            time.sleep(5)


if __name__ == "__main__":
    main()
