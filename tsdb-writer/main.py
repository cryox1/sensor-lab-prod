"""
Kafka -> TimescaleDB writer.

Consumes sensors.telemetry, batches INSERTs into the telemetry hypertable.
Idempotent via PRIMARY KEY (device_id, ts) + ON CONFLICT DO NOTHING.
"""
import json
import logging
import os
import signal
import sys
import time
from datetime import datetime, timezone

import psycopg
from confluent_kafka import Consumer

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("tsdb-writer")

KAFKA_BOOTSTRAP = os.environ.get("KAFKA_BOOTSTRAP", "redpanda:9092")
KAFKA_TOPIC = "sensors.telemetry"
GROUP_ID = "tsdb-writer"

PG_DSN = (
    f"host={os.environ['POSTGRES_HOST']} "
    f"port={os.environ.get('POSTGRES_PORT', '5432')} "
    f"dbname={os.environ['POSTGRES_DB']} "
    f"user={os.environ['POSTGRES_USER']} "
    f"password={os.environ['POSTGRES_PASSWORD']}"
)

BATCH_SIZE = 500
BATCH_MS = 1000

INSERT_SQL = """
INSERT INTO telemetry (ts, device_id, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa)
VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
ON CONFLICT (device_id, ts) DO NOTHING
"""

# Runs on every writer start. Safe on fresh DBs and on existing
# volumes where init.sql already ran without these columns.
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
)


def connect_pg(retries: int = 30) -> psycopg.Connection:
    for attempt in range(retries):
        try:
            conn = psycopg.connect(PG_DSN, autocommit=False)
            log.info("connected to Postgres")
            with conn.cursor() as cur:
                for stmt in MIGRATIONS:
                    cur.execute(stmt)
            conn.commit()
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


def flush(conn, rows, consumer):
    if not rows:
        return
    with conn.cursor() as cur:
        cur.executemany(INSERT_SQL, rows)
    conn.commit()
    consumer.commit(asynchronous=False)
    log.info("inserted %d rows", len(rows))


def main():
    conn = connect_pg()
    consumer = Consumer({
        "bootstrap.servers": KAFKA_BOOTSTRAP,
        "group.id": GROUP_ID,
        "enable.auto.commit": False,
        "auto.offset.reset": "earliest",
    })
    consumer.subscribe([KAFKA_TOPIC])

    running = True

    def shutdown(signum, frame):
        nonlocal running
        running = False
        log.info("shutting down (signal %s)", signum)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    batch = []
    deadline = time.monotonic() * 1000 + BATCH_MS

    try:
        while running:
            msg = consumer.poll(0.5)
            now_ms = time.monotonic() * 1000

            if msg is not None:
                if msg.error():
                    log.error("consume error: %s", msg.error())
                else:
                    try:
                        payload = json.loads(msg.value().decode("utf-8"))
                        batch.append(to_row(payload))
                    except (json.JSONDecodeError, KeyError, ValueError) as exc:
                        log.warning("skipping bad message: %s", exc)

            if len(batch) >= BATCH_SIZE or (batch and now_ms >= deadline):
                flush(conn, batch, consumer)
                batch.clear()
                deadline = now_ms + BATCH_MS
    finally:
        flush(conn, batch, consumer)
        consumer.close()
        conn.close()
        sys.exit(0)


if __name__ == "__main__":
    main()
