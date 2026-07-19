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
import math
import os
import signal
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone

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

# --- Blitzortung.org live strike consumer (disabled unless home is set) ---
# `or` instead of get(k, default): compose passes "" for unset ${VAR:-}
# passthroughs, and float("")/int("") would crash on startup.
BLITZ_HOME_LAT = os.environ.get("BLITZ_HOME_LAT") or None
BLITZ_HOME_LON = os.environ.get("BLITZ_HOME_LON") or None
BLITZ_RADIUS_KM = float(os.environ.get("BLITZ_RADIUS_KM") or "250")
BLITZ_MQTT_HOST = os.environ.get("BLITZ_MQTT_HOST") or "blitzortung.ha.sed.pl"
BLITZ_MQTT_PORT = int(os.environ.get("BLITZ_MQTT_PORT") or "1883")
BLITZ_MAX_CELLS = int(os.environ.get("BLITZ_MAX_CELLS") or "40")
BLITZ_RETENTION_DAYS = int(os.environ.get("BLITZ_RETENTION_DAYS") or "0")
BLITZ_LOCAL_TOPIC = "blitzortung/strikes"

STRIKE_INSERT_SQL = """
INSERT INTO lightning_strikes (ts, lat, lon, distance_km, delay_s)
VALUES (%s, %s, %s, %s, %s)
ON CONFLICT (ts, lat, lon) DO NOTHING
"""

STRIKE_PRUNE_SQL = (
    "DELETE FROM lightning_strikes"
    " WHERE ts < now() - make_interval(days => %s)"
)

INSERT_SQL = """
INSERT INTO telemetry (ts, device_id, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa, gas_kohm, lightning_km, lightning_energy, lightning_count, iaq, iaq_acc, co2_eq_ppm, bvoc_eq_ppm)
VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
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
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS gas_kohm DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lightning_km     SMALLINT",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lightning_energy INTEGER",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lightning_count  INTEGER",
    # Bosch BSEC2 outputs (BME680 nodes running the IAQ algorithm, e.g. air04).
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS iaq         DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS iaq_acc     SMALLINT",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS co2_eq_ppm  DOUBLE PRECISION",
    "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS bvoc_eq_ppm DOUBLE PRECISION",
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
    # Blitzortung.org strikes (see BlitzortungConsumer below). PK dedups
    # broker redeliveries; distance_km is haversine from home at insert time.
    """
    CREATE TABLE IF NOT EXISTS lightning_strikes (
      ts          TIMESTAMPTZ      NOT NULL,
      lat         DOUBLE PRECISION NOT NULL,
      lon         DOUBLE PRECISION NOT NULL,
      distance_km DOUBLE PRECISION NOT NULL,
      delay_s     DOUBLE PRECISION,
      received_at TIMESTAMPTZ      NOT NULL DEFAULT now(),
      PRIMARY KEY (ts, lat, lon)
    )
    """,
    "CREATE INDEX IF NOT EXISTS lightning_strikes_ts_idx"
    " ON lightning_strikes (ts DESC)",
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
        _opt_float(payload, "gas_kohm"),
        _opt_int(payload, "lightning_km"),
        _opt_int(payload, "lightning_energy"),
        _opt_int(payload, "lightning_count"),
        _opt_float(payload, "iaq"),
        _opt_int(payload, "iaq_acc"),
        _opt_float(payload, "co2_eq_ppm"),
        _opt_float(payload, "bvoc_eq_ppm"),
    )


def haversine_km(lat1, lon1, lat2, lon2):
    lat1, lon1, lat2, lon2 = map(math.radians, (lat1, lon1, lat2, lon2))
    a = (
        math.sin((lat2 - lat1) / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2
    )
    return 2 * 6371.0 * math.asin(math.sqrt(a))


_GEOHASH_B32 = "0123456789bcdefghjkmnpqrstuvwxyz"


def geohash_encode(lat, lon, precision):
    lat_lo, lat_hi = -90.0, 90.0
    lon_lo, lon_hi = -180.0, 180.0
    out = []
    ch = 0
    bit = 0
    even = True  # geohash interleaving starts with a longitude bit
    while len(out) < precision:
        if even:
            mid = (lon_lo + lon_hi) / 2
            if lon >= mid:
                ch = (ch << 1) | 1
                lon_lo = mid
            else:
                ch <<= 1
                lon_hi = mid
        else:
            mid = (lat_lo + lat_hi) / 2
            if lat >= mid:
                ch = (ch << 1) | 1
                lat_lo = mid
            else:
                ch <<= 1
                lat_hi = mid
        even = not even
        bit += 1
        if bit == 5:
            out.append(_GEOHASH_B32[ch])
            ch = 0
            bit = 0
    return "".join(out)


def geohash_cover(lat, lon, radius_km, max_cells):
    """Geohash prefixes whose cells jointly cover the radius circle.

    Over-covers (bounding box, whole cells) — the exact haversine check in
    on_message is the real gate. Tries fine-to-coarse precision until the
    cell count fits max_cells; samples aligned cell centers so every cell
    intersecting the box is hit.
    """
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * math.cos(math.radians(lat)))
    cells = set()
    for precision in (3, 2, 1):
        bits = 5 * precision
        lon_w = 360.0 / (1 << ((bits + 1) // 2))
        lat_h = 180.0 / (1 << (bits // 2))
        cells = set()
        la = math.floor((lat - dlat + 90.0) / lat_h) * lat_h - 90.0 + lat_h / 2
        while la <= lat + dlat + lat_h / 2:
            lo = math.floor((lon - dlon + 180.0) / lon_w) * lon_w - 180.0 + lon_w / 2
            while lo <= lon + dlon + lon_w / 2:
                cells.add(
                    geohash_encode(
                        min(max(la, -90.0), 90.0),
                        ((lo + 180.0) % 360.0) - 180.0,
                        precision,
                    )
                )
                lo += lon_w
            la += lat_h
        if len(cells) <= max_cells:
            break
    return sorted(cells)


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


class BlitzortungConsumer:
    """Located strikes from Blitzortung.org's public MQTT feed.

    Second paho client on its own network thread (loop_start), subscribed to
    the geohash cells covering the home radius. Owns a dedicated Postgres
    connection — Ingest._reconnect_pg swaps Ingest.conn in place, which is
    racy across threads. Accepted strikes are inserted and republished on
    the local broker (BLITZ_LOCAL_TOPIC) for the api's live WebSocket.
    Data: Blitzortung.org, private non-commercial use, attribution required.
    """

    def __init__(self, home_lat, home_lon, radius_km, local_client):
        self.home_lat = home_lat
        self.home_lon = home_lon
        self.radius_km = radius_km
        self.local_client = local_client  # paho publish() is thread-safe
        self.conn = connect_pg()
        self.prefixes = geohash_cover(home_lat, home_lon, radius_km, BLITZ_MAX_CELLS)
        # Each geohash character is one topic level: "u33" -> .../u/3/3/#
        self.topics = ["blitzortung/1.1/" + "/".join(p) + "/#" for p in self.prefixes]
        self.client = None
        self._last_prune = 0.0

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
            log.warning("blitzortung: Postgres link lost (%s) — reconnecting", exc)
            self._reconnect_pg()
            with self.conn.cursor() as cur:
                cur.execute(sql, params)

    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        log.info(
            "blitzortung: connected (rc=%s), subscribing %d cells: %s",
            reason_code,
            len(self.prefixes),
            " ".join(self.prefixes),
        )
        for topic in self.topics:
            client.subscribe(topic, qos=0)

    def on_message(self, client, userdata, msg):
        # External feed — drop junk quietly, never into telemetry_dlq.
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
            time_ns = int(payload["time"])
            lat = float(payload["lat"])
            lon = float(payload["lon"])
        except (
            json.JSONDecodeError,
            UnicodeDecodeError,
            KeyError,
            ValueError,
            TypeError,
        ) as exc:
            log.debug("blitzortung: unusable payload on %s: %s", msg.topic, exc)
            return

        dist = haversine_km(self.home_lat, self.home_lon, lat, lon)
        if dist > self.radius_km:
            return

        # int ns -> µs keeps full precision (float seconds would not).
        ts = datetime.fromtimestamp(0, tz=timezone.utc) + timedelta(
            microseconds=time_ns // 1000
        )
        delay_s = payload.get("delay")
        try:
            self._execute(STRIKE_INSERT_SQL, (ts, lat, lon, round(dist, 1), delay_s))
        except Exception as exc:
            log.error("blitzortung: insert failed: %s", exc)
            return

        out = {
            "type": "strike",
            "ts": time_ns / 1e9,  # epoch seconds, like sensor payloads
            "lat": lat,
            "lon": lon,
            "distance_km": round(dist, 1),
            "delay_s": delay_s,
        }
        info = self.local_client.publish(BLITZ_LOCAL_TOPIC, json.dumps(out), qos=0)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            # Row is committed; the web's /strikes poll recovers the marker.
            log.debug("blitzortung: local republish failed (rc=%s)", info.rc)

        self._maybe_prune()

    def _maybe_prune(self):
        if BLITZ_RETENTION_DAYS <= 0:
            return
        now = time.monotonic()
        if self._last_prune and now - self._last_prune < 3600:
            return
        self._last_prune = now
        try:
            self._execute(STRIKE_PRUNE_SQL, (BLITZ_RETENTION_DAYS,))
        except Exception as exc:
            log.warning("blitzortung: prune failed: %s", exc)

    def start(self):
        # Random client-id suffix: the shared public broker kicks duplicates.
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"sensorlab-blitz-{uuid.uuid4().hex[:8]}",
        )
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=120)
        self.client.connect_async(BLITZ_MQTT_HOST, BLITZ_MQTT_PORT, keepalive=60)
        self.client.loop_start()

    def stop(self):
        if self.client is not None:
            self.client.loop_stop()
            self.client.disconnect()
        try:
            self.conn.close()
        except Exception:
            pass


def start_blitzortung(local_client):
    if not (BLITZ_HOME_LAT and BLITZ_HOME_LON):
        log.info("blitzortung consumer disabled (BLITZ_HOME_LAT/LON unset)")
        return None
    consumer = BlitzortungConsumer(
        float(BLITZ_HOME_LAT), float(BLITZ_HOME_LON), BLITZ_RADIUS_KM, local_client
    )
    consumer.start()
    log.info(
        "blitzortung consumer started (%s:%s, radius %.0f km)",
        BLITZ_MQTT_HOST,
        BLITZ_MQTT_PORT,
        BLITZ_RADIUS_KM,
    )
    return consumer


def main():
    ingest = Ingest()
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="mqtt-pg-ingest",
    )
    client.on_connect = ingest.on_connect
    client.on_message = ingest.on_message

    blitz = start_blitzortung(local_client=client)

    def shutdown(signum, frame):
        log.info("shutting down (signal %s)", signum)
        if blitz is not None:
            blitz.stop()
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
