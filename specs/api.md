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
| GET | `/history` | `device_id` (required), `hours` 1–720 (default 24), `bucket_seconds` 1–86400 (optional) | `[{ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa}]` | open |
| GET | `/history-all` | `hours`, `bucket_seconds` (as above) | `[{device_id, points: [same shape as /history]}]` | open |
| GET | `/latest` | — | `[{device_id, ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi, batt_v, pressure_hpa}]` — newest row per device regardless of age (DISTINCT ON + `(device_id, ts DESC)` index); no GPS columns | open |
| WS | `/ws/live` | — | raw telemetry JSON frames | open |

### Auth model

`require_write_token` (FastAPI dependency on every mutating route — the PUTs,
POST `/groups`, DELETE `/groups/{id}`, DELETE `/thresholds/{metric}`): when the
`API_WRITE_TOKEN` env var is set (non-empty), the request must carry a matching
`X-API-Token` header or it gets 401. When unset (the default), all routes are
open — legacy behavior. GET routes and the WebSocket never require a token.

### WS `/ws/live`

`LiveHub`: one lazily-started MQTT subscription (paho, own network thread) to
`sensors/+/+/telemetry`, fanned out to all connected WebSocket clients via
`run_coroutine_threadsafe` onto the asyncio loop. Frames are the **raw MQTT
payload text**, forwarded only if they are valid JSON containing `device_id`
and `ts` (same minimal validation as ingest). Dead sockets are pruned on send
failure. Clients need not send anything; the server ignores inbound text.

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
`ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS pressure_hpa` to guard the
startup race where the API selects a column ingest hasn't migrated yet.

### History bucketing

With `bucket_seconds`, rows are grouped by `date_bin(make_interval(secs => N),
ts, 'epoch')` and scalar metrics are `AVG`ed (`eco2_ppm`/`tvoc_ppb`/`aqi` cast
to INTEGER). **GPS columns (`lat, lon, alt_m, sats, speed_kmh`) are not
averaged** — averaging a moving track invents phantom midpoints. Instead
`(array_agg(col ORDER BY ts DESC))[1]` keeps the value at the latest row in
the bucket (same semantics as Timescale's `last()`).

### Validation details worth pinning

- `display_name` and group `name`: trimmed, max 64 chars (400 above).
- Thresholds: `metric_key` must be in the allowlist (`temp_c, humidity,
  heat_index_c, eco2_ppm, tvoc_ppb, aqi, batt_v, pressure_hpa`; else 404);
  max 12 lines per metric (400); `color` must match `#rgb`/`#rrggbb` hex
  (400); labels truncated to 24 chars; lines stored sorted ascending by value.
- PUT `/devices/{id}/group` maps a ForeignKeyViolation to 404 "group not found".
- DELETE `/groups/{id}` relies on `ON DELETE CASCADE` to unassign members.

## Requirements

None yet.

## Non-goals

- No pagination, rate limiting, or per-user auth — single shared write token
  at most; reads are always open.
- No server-side downsampling policy beyond caller-chosen `bucket_seconds`.
- No write path for telemetry — only `ingest` writes readings.

## Open questions

- `/latest` omits GPS columns while `/history` includes them — intentional
  (dashboard cards don't show GPS) or an oversight to align someday?
