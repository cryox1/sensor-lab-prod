# Ingest service

**Status:** As-built
**Scope:** The MQTT → PostgreSQL writer in `ingest/main.py` plus the telemetry
schema it owns (`postgres/init.sql`). Does not cover the API's own tables
(`sensor_aliases`, `metric_thresholds`, groups — see `api.md`).

## Behavior (as-built)

Single-file Python service. Subscribes to `sensors/+/+/telemetry` on Mosquitto
(QoS 1) and inserts each JSON reading into the `telemetry` table, one INSERT
per message (no batching — sensor volume is low; the docstring says to
reintroduce a buffer only if throughput grows).

### Message handling

- Payload must be UTF-8 JSON with **`device_id` and `ts` as the only required
  fields**. `ts` is epoch seconds; parsed via
  `datetime.fromtimestamp(int(ts), tz=utc)`.
- All sensor fields are optional (devices send different subsets): `temp_c`,
  `humidity`, `heat_index_c` (float); `eco2_ppm`, `tvoc_ppb`, `aqi` (int);
  `lat`, `lon`, `alt_m` (float), `sats` (int), `speed_kmh` (float); `batt_v`,
  `pressure_hpa`, `gas_kohm` (float); `lightning_km`, `lightning_energy`,
  `lightning_count` (int, AS3935 nodes — see `hw-as3935.md`). Missing keys
  insert as NULL.
- Insert is **idempotent**: `ON CONFLICT (device_id, ts) DO NOTHING` on the
  composite primary key, so MQTT redeliveries and device retries are harmless.
- Postgres writes reconnect transparently: an `OperationalError` triggers one
  reconnect (with the 30×2s retry loop) and a retry of the statement.
- MQTT loop crashes are retried forever with a 5s backoff; SIGTERM/SIGINT
  disconnect cleanly.

### Dead-letter queue

Any payload that fails decode (bad UTF-8/JSON), is missing a required field,
has an unconvertible value, or whose INSERT errors is written to
`telemetry_dlq (id, received_at, topic, payload, reason)` — the successor of
the old Kafka `sensors.telemetry.dlq` topic. Payload is stored as text
(`utf-8` with replacement chars); inspect with
`SELECT * FROM telemetry_dlq ORDER BY received_at DESC`.

### Startup migrations

`connect_pg()` runs the `MIGRATIONS` tuple on **every start** (autocommit):
idempotent `ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS` for every optional
column (`eco2_ppm` … `lightning_count`) plus `CREATE TABLE IF NOT EXISTS
telemetry_dlq`. This is how existing production volumes pick up new columns,
since `postgres/init.sql` only executes on a fresh/empty data dir.

**Schema-change rule (AGENTS.md, mandatory):** a new telemetry column must be
added in **both** places — `postgres/init.sql` (fresh installs) and the
`MIGRATIONS` list in `ingest/main.py` (existing DBs) — and on deploy `ingest`
must restart **before** `api` so the column exists before the API queries it.
(The API additionally self-guards `pressure_hpa`, `gas_kohm`, and the three
`lightning_*` columns in its own lifespan.)

### Telemetry schema (owned here)

```
telemetry (
  ts TIMESTAMPTZ NOT NULL, device_id TEXT NOT NULL,
  temp_c / humidity / heat_index_c DOUBLE PRECISION,
  eco2_ppm / tvoc_ppb INTEGER, aqi SMALLINT,
  lat / lon / alt_m DOUBLE PRECISION, sats SMALLINT, speed_kmh DOUBLE PRECISION,
  batt_v DOUBLE PRECISION, pressure_hpa DOUBLE PRECISION,
  gas_kohm DOUBLE PRECISION,
  lightning_km SMALLINT, lightning_energy INTEGER, lightning_count INTEGER,
  PRIMARY KEY (device_id, ts)
)
```

Index `telemetry_device_ts_idx ON telemetry (device_id, ts DESC)` — serves the
API's `DISTINCT ON` latest-per-device and per-device history scans.

### Blitzortung.org strike consumer

Optional second MQTT client (`BlitzortungConsumer`), enabled only when
`BLITZ_HOME_LAT`/`BLITZ_HOME_LON` are set (unset → startup log
"blitzortung consumer disabled"). Runs via `connect_async` + `loop_start`
(own network thread, paho auto-reconnect 1–120 s) against
`BLITZ_MQTT_HOST:BLITZ_MQTT_PORT` (default `blitzortung.ha.sed.pl:1883`, the
public community feed) and owns a **dedicated** Postgres connection — the
`Ingest` connection's reconnect swap is not thread-safe to share.

- Subscribes the geohash cells covering `BLITZ_RADIUS_KM` (default 250) around
  home — `geohash_cover()` picks precision 3→2→1 until ≤ `BLITZ_MAX_CELLS`
  (default 40); topics are `blitzortung/1.1/<c>/<c>/…/#` (one char per level).
  The subscription over-covers; an exact haversine check gates each strike.
- Payload needs `time` (epoch **ns**, converted as int ns→µs), `lat`, `lon`;
  junk is dropped with a debug log, **never** written to `telemetry_dlq`.
- Accepted strikes: INSERT into `lightning_strikes` (`ON CONFLICT (ts, lat,
  lon) DO NOTHING` dedups redeliveries), then republished on the **local**
  broker as `blitzortung/strikes` with
  `{"type":"strike","ts":<epoch s>,"lat","lon","distance_km","delay_s"}` for
  the api's live WebSocket. Republish failures are debug-only (row is stored;
  the web's poll recovers).
- Optional retention: `BLITZ_RETENTION_DAYS` > 0 prunes older strikes at most
  hourly, piggybacked on message handling. Default 0 = keep forever.

### lightning_strikes schema (owned here)

```
lightning_strikes (
  ts TIMESTAMPTZ NOT NULL, lat / lon / distance_km DOUBLE PRECISION NOT NULL,
  delay_s DOUBLE PRECISION, received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (ts, lat, lon)
)
```

Index `lightning_strikes_ts_idx (ts DESC)` — serves the api's `/strikes`
time-window scan. Created in `init.sql`, `MIGRATIONS`, and (race guard) the
api lifespan.

## Requirements

None yet.

## Non-goals

- No batching/buffering of inserts (deliberate; see above).
- No payload schema enforcement beyond `device_id`+`ts` — unknown extra JSON
  keys are silently ignored, not DLQ'd.
- No DLQ replay tooling; the table is for manual inspection only.

## Open questions

- `telemetry_dlq` grows unbounded — no retention/cleanup policy is defined.
