"""
Web API.

GET  /devices                          -- known device_ids + alias + hidden flag
GET  /devices/{id}/stats               -- stored-row count + time bounds
PUT  /devices/{id}/display-name        -- set/clear user-facing alias
PUT  /devices/{id}/visibility          -- hide / unhide a device on the dashboard
PUT  /devices/{id}/group               -- assign a device to a group (or clear)
DELETE /devices/{id}                   -- delete metadata (+ readings if asked)
GET  /groups                           -- user-defined sensor groups + members
POST /groups                           -- create a named group
PUT  /groups/{id}                      -- rename a group
DELETE /groups/{id}                    -- delete a group (members are unassigned)
GET  /history?device_id=...            -- recent telemetry from Postgres
GET  /history-all                      -- recent telemetry for every device
GET  /latest                           -- newest stored row per device (any age)
GET  /thresholds                       -- user-customized chart threshold lines
PUT  /thresholds/{metric}              -- set threshold lines for a metric
DELETE /thresholds/{metric}            -- revert a metric to built-in defaults
WS   /ws/live                          -- live telemetry fanned out from MQTT

Auth: PUT endpoints require header `X-API-Token: <API_WRITE_TOKEN>` when
that env var is set. When unset (default) PUTs are open — same behavior
as before this change.
"""
import asyncio
import json
import logging
import os
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timezone, timedelta

import re

import paho.mqtt.client as mqtt
import psycopg
import psycopg_pool
from psycopg.types.json import Json
from fastapi import Depends, FastAPI, Header, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("api")

MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC = "sensors/+/+/telemetry"

# Comma-separated allowed CORS origins. Default "*" preserves previous behavior;
# tighten to a real host on Tailscale/LAN if you care.
CORS_ORIGINS = [
    o.strip() for o in os.environ.get("CORS_ORIGINS", "*").split(",") if o.strip()
]
# Optional shared-secret guard for mutating endpoints. Unset = open (legacy).
API_WRITE_TOKEN = os.environ.get("API_WRITE_TOKEN", "").strip() or None

PG_DSN = (
    f"host={os.environ['POSTGRES_HOST']} "
    f"port={os.environ.get('POSTGRES_PORT', '5432')} "
    f"dbname={os.environ['POSTGRES_DB']} "
    f"user={os.environ['POSTGRES_USER']} "
    f"password={os.environ['POSTGRES_PASSWORD']}"
)


def require_write_token(x_api_token: str | None = Header(default=None)):
    if API_WRITE_TOKEN is None:
        return
    if x_api_token != API_WRITE_TOKEN:
        raise HTTPException(status_code=401, detail="invalid or missing X-API-Token")


class LiveHub:
    """One MQTT subscription, fan-out to N WebSocket clients.

    paho runs its network loop in its own thread (loop_start); messages are
    handed back to the asyncio loop via run_coroutine_threadsafe so the
    WebSocket sends happen on the event loop.
    """

    def __init__(self):
        self.clients: set[WebSocket] = set()
        self.client: mqtt.Client | None = None
        self.loop: asyncio.AbstractEventLoop | None = None

    async def add(self, ws: WebSocket):
        self.clients.add(ws)
        if self.client is None:
            self._start()

    async def remove(self, ws: WebSocket):
        self.clients.discard(ws)

    def _start(self):
        self.loop = asyncio.get_running_loop()
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"api-live-{uuid.uuid4()}",
        )
        client.on_connect = self._on_connect
        client.on_message = self._on_message
        client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
        client.loop_start()
        self.client = client
        log.info("live MQTT subscriber started")

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        client.subscribe(MQTT_TOPIC, qos=0)

    def _on_message(self, client, userdata, msg):
        # Validate just like the ingest path so the live feed only carries
        # well-formed readings (same content the dashboard expects).
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return
        if "device_id" not in payload or "ts" not in payload:
            return
        text = msg.payload.decode("utf-8")
        if self.loop is not None:
            asyncio.run_coroutine_threadsafe(self._broadcast(text), self.loop)

    async def _broadcast(self, text: str):
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send_text(text)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)


hub = LiveHub()
pool: psycopg_pool.AsyncConnectionPool | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global pool
    pool = psycopg_pool.AsyncConnectionPool(
        PG_DSN, min_size=1, max_size=5, open=False
    )
    await pool.open()
    # Self-migrate sensor_aliases for existing volumes (init.sql only
    # runs on a fresh DB init). display_name is nullable so a row can
    # exist purely to carry hidden=true.
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                CREATE TABLE IF NOT EXISTS sensor_aliases (
                  device_id    TEXT PRIMARY KEY,
                  display_name TEXT,
                  hidden       BOOLEAN NOT NULL DEFAULT FALSE,
                  updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
                )
                """
            )
            await cur.execute(
                "ALTER TABLE sensor_aliases ALTER COLUMN display_name DROP NOT NULL"
            )
            await cur.execute(
                "ALTER TABLE sensor_aliases ADD COLUMN IF NOT EXISTS hidden BOOLEAN NOT NULL DEFAULT FALSE"
            )
            # User-customized chart threshold lines. One JSONB row per metric;
            # absence of a row means the frontend uses its built-in defaults.
            await cur.execute(
                """
                CREATE TABLE IF NOT EXISTS metric_thresholds (
                  metric_key TEXT PRIMARY KEY,
                  thresholds JSONB NOT NULL,
                  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
                )
                """
            )
            # User-defined sensor groups. A device belongs to at most one group
            # (sensor_group_members.device_id is the PK); deleting a group
            # cascades its memberships, leaving those devices ungrouped.
            await cur.execute(
                """
                CREATE TABLE IF NOT EXISTS sensor_groups (
                  id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
                  name        TEXT        NOT NULL,
                  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
                  updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
                )
                """
            )
            await cur.execute(
                """
                CREATE TABLE IF NOT EXISTS sensor_group_members (
                  device_id  TEXT   PRIMARY KEY,
                  group_id   BIGINT NOT NULL REFERENCES sensor_groups(id) ON DELETE CASCADE
                )
                """
            )
            # Telemetry columns the api SELECTs but that the writer/init.sql own.
            # Guard against the startup race where a query references a column an
            # existing volume hasn't migrated yet (writer adds these too).
            await cur.execute(
                "ALTER TABLE telemetry ADD COLUMN IF NOT EXISTS pressure_hpa DOUBLE PRECISION"
            )
    log.info("postgres pool ready")
    yield
    await pool.close()


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=CORS_ORIGINS,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health")
async def health():
    return {"ok": True}


@app.get("/devices")
async def devices():
    # FULL OUTER JOIN so a device that's been aliased/hidden but has not yet
    # sent (or no longer sends) telemetry still appears.
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                SELECT COALESCE(t.device_id, a.device_id) AS device_id,
                       MAX(t.ts)                          AS last_seen,
                       a.display_name,
                       COALESCE(a.hidden, FALSE)          AS hidden
                FROM telemetry t
                FULL OUTER JOIN sensor_aliases a USING (device_id)
                GROUP BY 1, a.display_name, a.hidden
                """
            )
            rows = await cur.fetchall()
    return [
        {
            "device_id": r[0],
            "last_seen": r[1].isoformat() if r[1] is not None else None,
            "display_name": r[2],
            "hidden": r[3],
        }
        for r in rows
    ]


DISPLAY_NAME_MAX = 64


class DisplayNameBody(BaseModel):
    display_name: str = Field(default="")


class VisibilityBody(BaseModel):
    hidden: bool


@app.put("/devices/{device_id}/display-name", dependencies=[Depends(require_write_token)])
async def set_display_name(device_id: str, body: DisplayNameBody):
    name = body.display_name.strip()
    if len(name) > DISPLAY_NAME_MAX:
        raise HTTPException(
            status_code=400,
            detail=f"display_name longer than {DISPLAY_NAME_MAX} chars",
        )
    new_name = name or None
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            if new_name is None:
                # Clearing the name: drop the row entirely if it's also not
                # hidden, else keep the row and just null the name.
                await cur.execute(
                    """
                    DELETE FROM sensor_aliases
                    WHERE device_id = %s AND hidden = FALSE
                    """,
                    (device_id,),
                )
                await cur.execute(
                    """
                    UPDATE sensor_aliases
                    SET display_name = NULL, updated_at = now()
                    WHERE device_id = %s
                    """,
                    (device_id,),
                )
            else:
                await cur.execute(
                    """
                    INSERT INTO sensor_aliases (device_id, display_name)
                    VALUES (%s, %s)
                    ON CONFLICT (device_id)
                    DO UPDATE SET display_name = EXCLUDED.display_name,
                                  updated_at = now()
                    """,
                    (device_id, new_name),
                )
    return {"device_id": device_id, "display_name": new_name}


@app.put("/devices/{device_id}/visibility", dependencies=[Depends(require_write_token)])
async def set_visibility(device_id: str, body: VisibilityBody):
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                INSERT INTO sensor_aliases (device_id, display_name, hidden)
                VALUES (%s, NULL, %s)
                ON CONFLICT (device_id)
                DO UPDATE SET hidden = EXCLUDED.hidden,
                              updated_at = now()
                """,
                (device_id, body.hidden),
            )
            # If we just un-hid a device that has no alias and no name, drop
            # the alias row so the table stays tidy.
            if not body.hidden:
                await cur.execute(
                    """
                    DELETE FROM sensor_aliases
                    WHERE device_id = %s AND display_name IS NULL AND hidden = FALSE
                    """,
                    (device_id,),
                )
    return {"device_id": device_id, "hidden": body.hidden}


@app.get("/devices/{device_id}/stats")
async def device_stats(device_id: str):
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                "SELECT COUNT(*), MIN(ts), MAX(ts) FROM telemetry WHERE device_id = %s",
                (device_id,),
            )
            rows, first_ts, last_ts = await cur.fetchone()
    return {
        "device_id": device_id,
        "rows": rows,
        "first_ts": first_ts.isoformat() if first_ts is not None else None,
        "last_ts": last_ts.isoformat() if last_ts is not None else None,
    }


@app.delete("/devices/{device_id}", dependencies=[Depends(require_write_token)])
async def delete_device(device_id: str, delete_data: bool = False):
    # Idempotent: metadata (alias, group membership) always goes; the stored
    # readings only on request. A device whose rows survive reappears in
    # /devices — that list is rebuilt from telemetry (see specs/api.md R1).
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                "DELETE FROM sensor_group_members WHERE device_id = %s",
                (device_id,),
            )
            await cur.execute(
                "DELETE FROM sensor_aliases WHERE device_id = %s",
                (device_id,),
            )
            if delete_data:
                await cur.execute(
                    "DELETE FROM telemetry WHERE device_id = %s",
                    (device_id,),
                )
            await cur.execute(
                "SELECT COUNT(*) FROM telemetry WHERE device_id = %s",
                (device_id,),
            )
            (remaining,) = await cur.fetchone()
    return {
        "device_id": device_id,
        "deleted": True,
        "data_deleted": delete_data,
        "remaining_rows": remaining,
    }


GROUP_NAME_MAX = 64


class GroupBody(BaseModel):
    name: str = Field(default="")


class DeviceGroupBody(BaseModel):
    # null => remove the device from any group
    group_id: int | None = None


def _clean_group_name(name: str) -> str:
    name = name.strip()
    if not name:
        raise HTTPException(status_code=400, detail="group name is required")
    if len(name) > GROUP_NAME_MAX:
        raise HTTPException(
            status_code=400, detail=f"group name longer than {GROUP_NAME_MAX} chars"
        )
    return name


@app.get("/groups")
async def groups():
    # LEFT JOIN so empty groups still appear with device_ids: []. array_agg with
    # a FILTER keeps the empty case a real empty array instead of [null].
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                SELECT g.id,
                       g.name,
                       COALESCE(
                         array_agg(m.device_id ORDER BY m.device_id)
                           FILTER (WHERE m.device_id IS NOT NULL),
                         '{}'
                       ) AS device_ids
                FROM sensor_groups g
                LEFT JOIN sensor_group_members m ON m.group_id = g.id
                GROUP BY g.id, g.name
                ORDER BY g.name, g.id
                """
            )
            rows = await cur.fetchall()
    return [{"id": r[0], "name": r[1], "device_ids": r[2]} for r in rows]


@app.post("/groups", dependencies=[Depends(require_write_token)])
async def create_group(body: GroupBody):
    name = _clean_group_name(body.name)
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                "INSERT INTO sensor_groups (name) VALUES (%s) RETURNING id, name",
                (name,),
            )
            row = await cur.fetchone()
    return {"id": row[0], "name": row[1], "device_ids": []}


@app.put("/groups/{group_id}", dependencies=[Depends(require_write_token)])
async def rename_group(group_id: int, body: GroupBody):
    name = _clean_group_name(body.name)
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                UPDATE sensor_groups
                SET name = %s, updated_at = now()
                WHERE id = %s
                RETURNING id, name
                """,
                (name, group_id),
            )
            row = await cur.fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="group not found")
    return {"id": row[0], "name": row[1]}


@app.delete("/groups/{group_id}", dependencies=[Depends(require_write_token)])
async def delete_group(group_id: int):
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            # Memberships are removed by ON DELETE CASCADE.
            await cur.execute(
                "DELETE FROM sensor_groups WHERE id = %s", (group_id,)
            )
            if cur.rowcount == 0:
                raise HTTPException(status_code=404, detail="group not found")
    return {"id": group_id, "deleted": True}


@app.put("/devices/{device_id}/group", dependencies=[Depends(require_write_token)])
async def set_device_group(device_id: str, body: DeviceGroupBody):
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            if body.group_id is None:
                await cur.execute(
                    "DELETE FROM sensor_group_members WHERE device_id = %s",
                    (device_id,),
                )
            else:
                try:
                    await cur.execute(
                        """
                        INSERT INTO sensor_group_members (device_id, group_id)
                        VALUES (%s, %s)
                        ON CONFLICT (device_id)
                        DO UPDATE SET group_id = EXCLUDED.group_id
                        """,
                        (device_id, body.group_id),
                    )
                except psycopg.errors.ForeignKeyViolation:
                    raise HTTPException(status_code=404, detail="group not found")
    return {"device_id": device_id, "group_id": body.group_id}


# Metrics that may carry threshold lines — matches the frontend METRICS keys.
THRESHOLD_METRICS = {
    "temp_c", "humidity", "heat_index_c", "eco2_ppm", "tvoc_ppb", "aqi", "batt_v",
    "pressure_hpa",
}
THRESHOLD_LABEL_MAX = 24
MAX_THRESHOLDS_PER_METRIC = 12
_HEX_COLOR = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")


class ThresholdLine(BaseModel):
    value: float
    label: str = Field(default="")
    color: str = Field(default="#7d8590")


class ThresholdsBody(BaseModel):
    thresholds: list[ThresholdLine]


def _clean_thresholds(metric_key: str, lines: list[ThresholdLine]):
    if metric_key not in THRESHOLD_METRICS:
        raise HTTPException(status_code=404, detail=f"unknown metric '{metric_key}'")
    if len(lines) > MAX_THRESHOLDS_PER_METRIC:
        raise HTTPException(
            status_code=400,
            detail=f"at most {MAX_THRESHOLDS_PER_METRIC} thresholds per metric",
        )
    cleaned = []
    for ln in lines:
        label = ln.label.strip()[:THRESHOLD_LABEL_MAX]
        color = ln.color.strip()
        if not _HEX_COLOR.match(color):
            raise HTTPException(
                status_code=400, detail=f"invalid hex color '{ln.color}'"
            )
        cleaned.append({"value": ln.value, "label": label, "color": color})
    # Keep ascending by value so the chart draws them predictably.
    cleaned.sort(key=lambda t: t["value"])
    return cleaned


@app.get("/thresholds")
async def get_thresholds():
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute("SELECT metric_key, thresholds FROM metric_thresholds")
            rows = await cur.fetchall()
    return {r[0]: r[1] for r in rows}


@app.put("/thresholds/{metric_key}", dependencies=[Depends(require_write_token)])
async def set_thresholds(metric_key: str, body: ThresholdsBody):
    cleaned = _clean_thresholds(metric_key, body.thresholds)
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                INSERT INTO metric_thresholds (metric_key, thresholds)
                VALUES (%s, %s)
                ON CONFLICT (metric_key)
                DO UPDATE SET thresholds = EXCLUDED.thresholds,
                              updated_at = now()
                """,
                (metric_key, Json(cleaned)),
            )
    return {"metric_key": metric_key, "thresholds": cleaned}


@app.delete("/thresholds/{metric_key}", dependencies=[Depends(require_write_token)])
async def reset_thresholds(metric_key: str):
    if metric_key not in THRESHOLD_METRICS:
        raise HTTPException(status_code=404, detail=f"unknown metric '{metric_key}'")
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                "DELETE FROM metric_thresholds WHERE metric_key = %s", (metric_key,)
            )
    return {"metric_key": metric_key, "reset": True}


@app.get("/history")
async def history(
    device_id: str = Query(...),
    hours: int = Query(24, ge=1, le=720),
    bucket_seconds: int | None = Query(None, ge=1, le=86400),
):
    since = datetime.now(timezone.utc) - timedelta(hours=hours)
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            if bucket_seconds:
                # Scalar metrics are averaged per bucket; GPS columns keep the
                # latest *real* fix in the bucket (averaging lat/lon would
                # invent phantom midpoints on a moving track). The plain-Postgres
                # `(array_agg(col ORDER BY ts DESC))[1]` returns the value at the
                # row with the max ts — same semantics as Timescale's last().
                await cur.execute(
                    """
                    SELECT date_bin(make_interval(secs => %s), ts, TIMESTAMPTZ 'epoch') AS bucket,
                           AVG(temp_c)            AS temp_c,
                           AVG(humidity)          AS humidity,
                           AVG(heat_index_c)      AS heat_index_c,
                           AVG(eco2_ppm)::INTEGER AS eco2_ppm,
                           AVG(tvoc_ppb)::INTEGER AS tvoc_ppb,
                           AVG(aqi)::INTEGER      AS aqi,
                           (array_agg(lat       ORDER BY ts DESC))[1] AS lat,
                           (array_agg(lon       ORDER BY ts DESC))[1] AS lon,
                           (array_agg(alt_m     ORDER BY ts DESC))[1] AS alt_m,
                           (array_agg(sats      ORDER BY ts DESC))[1] AS sats,
                           (array_agg(speed_kmh ORDER BY ts DESC))[1] AS speed_kmh,
                           AVG(batt_v)            AS batt_v,
                           AVG(pressure_hpa)      AS pressure_hpa
                    FROM telemetry
                    WHERE device_id = %s AND ts >= %s
                    GROUP BY bucket
                    ORDER BY bucket ASC
                    """,
                    (bucket_seconds, device_id, since),
                )
            else:
                await cur.execute(
                    """
                    SELECT ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi,
                           lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa
                    FROM telemetry
                    WHERE device_id = %s AND ts >= %s
                    ORDER BY ts ASC
                    """,
                    (device_id, since),
                )
            rows = await cur.fetchall()
    return [
        {
            "ts": r[0].isoformat(),
            "temp_c": r[1],
            "humidity": r[2],
            "heat_index_c": r[3],
            "eco2_ppm": r[4],
            "tvoc_ppb": r[5],
            "aqi": r[6],
            "lat": r[7],
            "lon": r[8],
            "alt_m": r[9],
            "sats": r[10],
            "speed_kmh": r[11],
            "batt_v": r[12],
            "pressure_hpa": r[13],
        }
        for r in rows
    ]


@app.get("/history-all")
async def history_all(
    hours: int = Query(24, ge=1, le=720),
    bucket_seconds: int | None = Query(None, ge=1, le=86400),
):
    since = datetime.now(timezone.utc) - timedelta(hours=hours)
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            if bucket_seconds:
                # See /history: GPS columns keep the latest real fix per bucket.
                await cur.execute(
                    """
                    SELECT device_id,
                           date_bin(make_interval(secs => %s), ts, TIMESTAMPTZ 'epoch') AS bucket,
                           AVG(temp_c)            AS temp_c,
                           AVG(humidity)          AS humidity,
                           AVG(heat_index_c)      AS heat_index_c,
                           AVG(eco2_ppm)::INTEGER AS eco2_ppm,
                           AVG(tvoc_ppb)::INTEGER AS tvoc_ppb,
                           AVG(aqi)::INTEGER      AS aqi,
                           (array_agg(lat       ORDER BY ts DESC))[1] AS lat,
                           (array_agg(lon       ORDER BY ts DESC))[1] AS lon,
                           (array_agg(alt_m     ORDER BY ts DESC))[1] AS alt_m,
                           (array_agg(sats      ORDER BY ts DESC))[1] AS sats,
                           (array_agg(speed_kmh ORDER BY ts DESC))[1] AS speed_kmh,
                           AVG(batt_v)            AS batt_v,
                           AVG(pressure_hpa)      AS pressure_hpa
                    FROM telemetry
                    WHERE ts >= %s
                    GROUP BY device_id, bucket
                    ORDER BY device_id ASC, bucket ASC
                    """,
                    (bucket_seconds, since),
                )
            else:
                await cur.execute(
                    """
                    SELECT device_id, ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi,
                           lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa
                    FROM telemetry
                    WHERE ts >= %s
                    ORDER BY device_id ASC, ts ASC
                    """,
                    (since,),
                )
            rows = await cur.fetchall()

    by_device: dict[str, list[dict]] = {}
    for (device_id, ts, temp_c, humidity, heat_index_c, eco2_ppm, tvoc_ppb, aqi,
         lat, lon, alt_m, sats, speed_kmh, batt_v, pressure_hpa) in rows:
        by_device.setdefault(device_id, []).append({
            "ts": ts.isoformat(),
            "temp_c": temp_c,
            "humidity": humidity,
            "heat_index_c": heat_index_c,
            "eco2_ppm": eco2_ppm,
            "tvoc_ppb": tvoc_ppb,
            "aqi": aqi,
            "lat": lat,
            "lon": lon,
            "alt_m": alt_m,
            "sats": sats,
            "speed_kmh": speed_kmh,
            "batt_v": batt_v,
            "pressure_hpa": pressure_hpa,
        })
    return [{"device_id": d, "points": pts} for d, pts in by_device.items()]


@app.get("/latest")
async def latest():
    # Newest stored row per device, regardless of age. Lets the dashboard show a
    # last-known reading for low-duty-cycle nodes (e.g. deep-sleep sensors that
    # publish every few minutes) instead of a blank card between live messages.
    # DISTINCT ON + the (device_id, ts DESC) index makes this a cheap one-row-per
    # -device lookup.
    async with pool.connection() as conn:
        async with conn.cursor() as cur:
            await cur.execute(
                """
                SELECT DISTINCT ON (device_id)
                       device_id, ts, temp_c, humidity, heat_index_c,
                       eco2_ppm, tvoc_ppb, aqi, batt_v, pressure_hpa
                FROM telemetry
                ORDER BY device_id, ts DESC
                """
            )
            rows = await cur.fetchall()
    return [
        {
            "device_id": r[0],
            "ts": r[1].isoformat(),
            "temp_c": r[2],
            "humidity": r[3],
            "heat_index_c": r[4],
            "eco2_ppm": r[5],
            "tvoc_ppb": r[6],
            "aqi": r[7],
            "batt_v": r[8],
            "pressure_hpa": r[9],
        }
        for r in rows
    ]


@app.websocket("/ws/live")
async def ws_live(ws: WebSocket):
    await ws.accept()
    await hub.add(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await hub.remove(ws)
