CREATE TABLE IF NOT EXISTS telemetry (
  ts            TIMESTAMPTZ NOT NULL,
  device_id     TEXT        NOT NULL,
  temp_c        DOUBLE PRECISION,
  humidity      DOUBLE PRECISION,
  heat_index_c  DOUBLE PRECISION,
  eco2_ppm      INTEGER,
  tvoc_ppb      INTEGER,
  aqi           SMALLINT,
  lat           DOUBLE PRECISION,
  lon           DOUBLE PRECISION,
  alt_m         DOUBLE PRECISION,
  sats          SMALLINT,
  speed_kmh     DOUBLE PRECISION,
  batt_v        DOUBLE PRECISION,
  pressure_hpa  DOUBLE PRECISION,
  PRIMARY KEY (device_id, ts)
);

-- Idempotent migrations for upgrades on existing volumes (init.sql
-- only runs on fresh DB init; the ALTERs below also run from the
-- ingest service on startup).
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS eco2_ppm INTEGER;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS tvoc_ppb INTEGER;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS aqi      SMALLINT;
-- GPS columns (NEO-6M sensors). lat/lon are the core; the rest are optional.
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lat       DOUBLE PRECISION;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS lon       DOUBLE PRECISION;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS alt_m     DOUBLE PRECISION;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS sats      SMALLINT;
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS speed_kmh DOUBLE PRECISION;
-- Battery voltage (deep-sleep nodes with a divider on an ADC pin, e.g. xiao_c6).
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS batt_v    DOUBLE PRECISION;
-- Atmospheric pressure (BME280 nodes, e.g. BME280_xiaoc6).
ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS pressure_hpa DOUBLE PRECISION;

CREATE INDEX IF NOT EXISTS telemetry_device_ts_idx
  ON telemetry (device_id, ts DESC);

-- Dead-letter store for malformed payloads (was the Kafka
-- sensors.telemetry.dlq topic). The ingest service writes a row here for any
-- payload it can't decode/validate/insert; inspect with
-- `SELECT * FROM telemetry_dlq ORDER BY received_at DESC;`. Created here for
-- fresh inits; the ingest service runs the same CREATE on startup so existing
-- volumes pick it up.
CREATE TABLE IF NOT EXISTS telemetry_dlq (
  id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  topic       TEXT,
  payload     TEXT,
  reason      TEXT
);

-- User-editable display names + dashboard visibility for sensors.
-- Keyed by device_id so the telemetry stream and MQTT topics are
-- unaffected. display_name is nullable so a row can exist purely to
-- carry hidden=true without forcing a name. Created here for fresh
-- inits; the api also runs the same CREATE + ALTERs on startup so
-- existing volumes pick the schema up.
CREATE TABLE IF NOT EXISTS sensor_aliases (
  device_id    TEXT PRIMARY KEY,
  display_name TEXT,
  hidden       BOOLEAN NOT NULL DEFAULT FALSE,
  updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
ALTER TABLE sensor_aliases ALTER COLUMN display_name DROP NOT NULL;
ALTER TABLE sensor_aliases ADD COLUMN IF NOT EXISTS hidden BOOLEAN NOT NULL DEFAULT FALSE;

-- User-customized chart threshold lines. One JSONB row per metric_key
-- (e.g. eco2_ppm), holding an array of {value,label,color}. Absence of a
-- row means the frontend falls back to its built-in defaults. The api
-- runs the same CREATE on startup so existing volumes pick it up.
CREATE TABLE IF NOT EXISTS metric_thresholds (
  metric_key TEXT PRIMARY KEY,
  thresholds JSONB NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- User-defined sensor groups for organizing the dashboard. A device belongs
-- to at most one group (sensor_group_members.device_id is the PK); ungrouped
-- devices simply have no membership row. Keyed independently of telemetry so
-- the stream and MQTT topics are unaffected. The api runs the same CREATEs on
-- startup so existing volumes pick them up.
CREATE TABLE IF NOT EXISTS sensor_groups (
  id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  name        TEXT        NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS sensor_group_members (
  device_id  TEXT   PRIMARY KEY,
  group_id   BIGINT NOT NULL REFERENCES sensor_groups(id) ON DELETE CASCADE
);
