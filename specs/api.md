# API service

**Status:** As-built
**Scope:** The FastAPI service in `api/main.py` — every HTTP route, the live
WebSocket, auth, and its startup self-migrations. Does not cover the ingest
writer (see `ingest.md`) or the web frontend (see `web-*.md`).

## Behavior (as-built)

Single-file FastAPI app (`api/main.py`) backed by a psycopg async pool
(min 1 / max 5) against the sensor-lab Postgres, plus one shared MQTT
subscription for the live WebSocket. CORS is wide open by default
(`CORS_ORIGINS` env, default `*`).

### Routes

| Method | Path | Params / body | Response | Auth |
|---|---|---|---|---|
| GET | `/health` | — | `{ok: true}` | open |
| GET | `/devices` | — | `[{device_id, last_seen (ISO\|null), display_name, hidden}]`; FULL OUTER JOIN telemetry × sensor_aliases so aliased/hidden devices without telemetry still appear | open |
| PUT | `/devices/{id}/display-name` | `{display_name}` (trimmed; empty clears; ≤64 chars else 400) | `{device_id, display_name}` | write token |
| PUT | `/devices/{id}/visibility` | `{hidden: bool}` | `{device_id, hidden}`; un-hiding a nameless device deletes its alias row | write token |
| PUT | `/devices/{id}/group` | `{group_id: int\|null}` (null unassigns) | `{device_id, group_id}`; 404 if group missing (FK violation caught) | write token |
| GET | `/groups` | — | `[{id, name, device_ids: []}]`; empty groups included with `[]` | open |
| POST | `/groups` | `{name}` (trimmed, non-empty, ≤64 else 400) | `{id, name, device_ids: []}` | write token |
| PUT | `/groups/{id}` | `{name}` (same validation) | `{id, name}`; 404 if unknown | write token |
| DELETE | `/groups/{id}` | — | `{id, deleted: true}`; 404 if unknown; memberships removed via ON DELETE CASCADE | write token |
| GET | `/thresholds` | — | `{metric_key: [{value, label, color}], …}` (only customized metrics; absence = frontend defaults) | open |
| PUT | `/thresholds/{metric}` | `{thresholds: [{value, label, color}]}` | `{metric_key, thresholds}` (cleaned, sorted ascending by value) | write token |
| DELETE | `/thresholds/{metric}` | — | `{metric_key, reset: true}`; 404 on unknown metric | write token |
| GET | `/history` | `device_id` (required), `hours` 1–720 (default 24), `bucket_seconds` 1–86400 (optional) | `[{ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa, gas_kohm, lightning_km, lightning_energy, lightning_count}]` | open |
| GET | `/history-all` | `hours`, `bucket_seconds` (as above) | `[{device_id, points: [same shape as /history]}]` | open |
| GET | `/latest` | — | `[{device_id, ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, batt_v, pressure_hpa, gas_kohm, lightning_km, lightning_energy, lightning_count}]` — newest row per device regardless of age (DISTINCT ON + `(device_id, ts DESC)` index); no GPS columns | open |
| GET | `/strikes` | `minutes` 1–1440 (default 60) | `{home: {lat, lon} \| null, radius_km, strikes: [{ts, lat, lon, distance_km}]}` — Blitzortung strikes ascending by ts, newest-first LIMIT 5000 before reversal; `home: null` = feature unconfigured (frontend hides the overlay) | open |
| WS | `/ws/live` | — | raw telemetry JSON frames + strike frames (`type: "strike"`) | open |

### Auth model

`require_write_token` (FastAPI dependency on every mutating route — the PUTs,
POST `/groups`, DELETE `/groups/{id}`, DELETE `/thresholds/{metric}`): when the
`API_WRITE_TOKEN` env var is set (non-empty), the request must carry a matching
`X-API-Token` header or it gets 401. When unset (the default), all routes are
open — legacy behavior. GET routes and the WebSocket never require a token.

### WS `/ws/live`

`LiveHub`: one lazily-started MQTT client (paho, own network thread)
subscribed to `sensors/+/+/telemetry` **and** `blitzortung/strikes`, fanned
out to all connected WebSocket clients via `run_coroutine_threadsafe` onto
the asyncio loop. Frames are the **raw MQTT payload text**: telemetry is
forwarded only if it is valid JSON containing `device_id` and `ts` (same
minimal validation as ingest); strike frames (republished by ingest's
Blitzortung consumer, `{"type":"strike", ts, lat, lon, distance_km,
delay_s}`, no `device_id`) only need `lat`+`lon`. Clients tell the two apart
via `payload.type === "strike"` — consumers keying on `device_id` must guard
against strike frames. Dead sockets are pruned on send failure. Clients need
not send anything; the server ignores inbound text.

### Timestamp asymmetry (load-bearing)

- **REST** (`/history`, `/history-all`, `/latest`, `/devices.last_seen`):
  `ts` is an **ISO 8601 string** (`datetime.isoformat()` of a TIMESTAMPTZ).
- **WS `/ws/live`**: `ts` is whatever the device published — **epoch seconds
  as a number** (the firmware convention; ingest parses it with
  `datetime.fromtimestamp(int(ts))`).

The frontend must normalize; do not "fix" one side to match the other without
updating every consumer.

### Startup self-migrations (`lifespan`)

Because `postgres/init.sql` only runs on a fresh data dir, the API re-applies
its own schema on every start: `CREATE TABLE IF NOT EXISTS` for
`sensor_aliases` (+ `ALTER` display_name nullable, `ADD COLUMN IF NOT EXISTS
hidden`), `metric_thresholds`, `sensor_groups`, `sensor_group_members`; plus
`ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS` for `pressure_hpa`,
`gas_kohm`, `lightning_km`, `lightning_energy`, and `lightning_count` to
guard the startup race where the API selects a column ingest hasn't
migrated yet. The same guard creates `lightning_strikes` (+ its `ts DESC`
index) — ingest owns that table's migration, but api may start first after
a joint deploy.

### History bucketing

With `bucket_seconds`, rows are grouped by `date_bin(make_interval(secs => N),
ts, 'epoch')` and scalar metrics are `AVG`ed (`eco2_ppm`/`tvoc_ppb`/`aqi` cast
to INTEGER). **GPS columns (`lat, lon, alt_m, sats, speed_kmh`) are not
averaged** — averaging a moving track invents phantom midpoints. Instead
`(array_agg(col ORDER BY ts DESC))[1]` keeps the value at the latest row in
the bucket (same semantics as Timescale's `last()`). **Lightning columns are
the second exception** (`hw-as3935.md` R2): `SUM(lightning_count)` — a
per-publish delta, AVG would understate storm totals — plus
`MIN(lightning_km)` (closest approach of the storm front) and
`MAX(lightning_energy)` (peak raw intensity), mirroring the firmware's own
burst folding. Do not "simplify" these to AVG.

### Validation details worth pinning

- `display_name` and group `name`: trimmed, max 64 chars (400 above).
- Thresholds: `metric_key` must be in the allowlist (`temp_c, humidity,
  heat_index_c, eco2_ppm, tvoc_ppb, aqi, batt_v, pressure_hpa, gas_kohm,
  lightning_km, lightning_energy, lightning_count`; else 404);
  max 12 lines per metric (400); `color` must match `#rgb`/`#rrggbb` hex
  (400); labels truncated to 24 chars; lines stored sorted ascending by value.
- PUT `/devices/{id}/group` maps a ForeignKeyViolation to 404 "group not found".
- DELETE `/groups/{id}` relies on `ON DELETE CASCADE` to unassign members.

## Requirements

### R1 — Device stats + deletion endpoints (Implemented)

Support the manage-drawer delete flow (`web-dashboard.md` R1).

Acceptance criteria:

- `GET /devices/{id}/stats` (open) returns
  `{device_id, rows, first_ts, last_ts}` — COUNT/MIN/MAX over `telemetry` for
  that device; `rows: 0` with null timestamps when no readings exist.
- `DELETE /devices/{id}` (write token) with query param `delete_data`
  (bool, default `false`) always deletes the device's `sensor_aliases` row and
  `sensor_group_members` membership; with `delete_data=true` it also deletes
  all `telemetry` rows for the device_id. Response:
  `{device_id, deleted: true, data_deleted: bool, remaining_rows: int}`.
- Deletion is idempotent: an unknown device_id returns the same shape
  (`remaining_rows: 0`), no 404.
- `telemetry_dlq` rows are untouched (keyed by topic, not device_id).
- `metric_thresholds` are untouched (global per-metric, not per-device).

## Non-goals

- No pagination, rate limiting, or per-user auth — single shared write token
  at most; reads are always open.
- No server-side downsampling policy beyond caller-chosen `bucket_seconds`.
- No write path for telemetry — only `ingest` inserts readings (R1 adds
  deletion of a device's readings, but no inserts/updates via the API).

## Open questions

- `/latest` omits GPS columns while `/history` includes them — intentional
  (dashboard cards don't show GPS) or an oversight to align someday?
