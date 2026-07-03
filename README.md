# sensor-lab

ESP8266 (DHT11, CCS811, …) → MQTT (Mosquitto) → PostgreSQL → FastAPI + Next.js.

A small, fully open-source IoT pipeline sized for a homelab. Every component is
OSS: Eclipse Mosquitto, PostgreSQL, FastAPI, Next.js.

## Architecture

```
ESP8266 ──MQTT──▶ Mosquitto ──▶ ingest ──▶ PostgreSQL
                      │                        ▲
   /ws/live: api ◀────┘ (MQTT)       api reads ─┘
        │                                  │
        ▼                                  ▼
  browser WebSocket                   web (Next.js)
```

`ingest/` subscribes to MQTT and writes each reading to Postgres; `api/` is the
FastAPI read + WebSocket server (its `/ws/live` feed subscribes to MQTT and fans
out to browsers); `web/` is the Next.js dashboard. Mosquitto is required because
the ESP devices speak only MQTT.

## Quick start (local)

```bash
cp .env.example .env
docker compose up -d
```

Then open:
- `http://localhost:9530` — web app
- API at `http://localhost:8000`, MQTT on `1883`

Postgres is published on host port `5433` (set `POSTGRES_HOST_PORT` to change it;
the non-default port avoids clashing with any other PostgreSQL on the host). The
`ingest` and `api` services reach it on the compose network at the internal `5432`.

The dashboard has a **manage** panel (top-right) to rename devices and to
hide/show them on the main grid. Hidden devices keep recording — they're just
removed from the default view.

The **groups** page (`/groups`, linked top-right) lets you create named groups
and assign each sensor to at most one of them. The startpage then renders each
group as its own section (sensors with no group fall under "Ungrouped"), and
every group has a combined overview at `/groups/<id>` — the same shared
per-metric charts as `/overview`, scoped to that group's sensors, with a
**live / history** toggle (live streams the group's sensors from `/ws/live`
on a rolling 5-minute window; history shows the bucketed chosen range). Group names
and membership are stored server-side (`sensor_groups` /
`sensor_group_members`), so they're shared across browsers.

By default the API is open. To lock down the rename / hide endpoints on a
shared LAN, set `API_WRITE_TOKEN` (api) + `WEB_API_WRITE_TOKEN` (web) to the
same value in `.env`; the browser will include `X-API-Token` on writes.
Optionally also tighten `CORS_ORIGINS`. Both are documented in `.env.example`.

## Development workflow (specs)

Behavior is spec-driven: every area (each web page, the API, ingest) has a
spec file under [`specs/`](specs/). New features start as a `Proposed`
requirement with acceptance criteria in the relevant spec, get implemented,
verified against the criteria, and flipped to `Implemented` in the same
commit. Changing existing behavior means updating the spec in the same commit.
See [`specs/README.md`](specs/README.md) for the workflow.

## What runs where

| Service   | Host port | Purpose                                  |
| --------- | --------- | ---------------------------------------- |
| mosquitto | 1883      | MQTT broker (telemetry ingress)          |
| postgres  | 5433      | PostgreSQL (host port; internal is 5432) |
| ingest    | —         | MQTT → Postgres                          |
| api       | 8000      | FastAPI read + `/ws/live` WebSocket      |
| web       | 9530      | Next.js dashboard                        |

## Deploy to a server

```bash
./deploy/deploy.sh
```

Pre-reqs: SSH access to your server — set `SENSOR_LAB_HOST` / `SENSOR_LAB_USER` /
`SENSOR_LAB_SSH_KEY` / `SENSOR_LAB_REMOTE_DIR` (see `DEPLOYMENT.md`), Docker installed on
the remote.

> ⚠️ **Data safety — read `AGENTS.md`.** The production database was wiped once by a
> deploy that overwrote the server's compose and `rsync --delete`'d the tree. The live
> data is the bind mount `${SENSOR_DATA_DIR}/postgres` on the server. `deploy.sh` now
> **backs up the DB first (and aborts if the backup fails)**, syncs only source dirs
> with **no `--delete`**, and **never** overwrites the server's `docker-compose.yml` /
> `.env` or touches data dirs — it only rebuilds the app services, leaving `postgres` /
> `mosquitto` running.
>
> - If your server also runs a **separate, unrelated PostgreSQL**, sensor-lab's
>   Postgres is a dedicated container on its own host port (`5433`) and data dir, isolated
>   from it. The backup/restore scripts only ever target the sensor-lab container.
> - Back up manually any time: `./deploy/backup.sh` → `${SENSOR_DATA_DIR}/backups`
>   (e.g. `/mnt/data/sensor-lab/backups` on a server). Restore: `./deploy/restore.sh <file>`.
> - **Never** run `docker compose down -v`, `docker volume rm/prune`, or change a
>   stateful service's volume definition against the server — any of these wipes the DB.

## Firmware

Arduino sketches for the ESP8266 devices live under `firmware/`. Each
sketch is in its own folder named after the `.ino` file (Arduino IDE
requirement):

- `firmware/dht11_oled/` — DHT11 + HW-364A OLED, device `indoor01`, static IP `10.0.0.60`.
- `firmware/dht11_nodemcu/` — DHT11 only, no display, device `indoor02`, static IP `10.0.0.62`.
- `firmware/ens160_aht21_nodemcu/` — ENS160 + AHT21 (eCO₂ / TVOC / AQI + temp / humidity), no display, device `air01`, static IP `10.0.0.64`.
- `firmware/ens160_aht21_oled/` — ENS160 + AHT21 + HW-364A OLED, device `air02`, static IP `10.0.0.65`. All three I²C devices share the same bus (OLED `0x3C`, AHT21 `0x38`, ENS160 `0x53`).
- `firmware/neo6m_c3_oled/` — NEO-6M GPS + onboard 0.42″ OLED, device `gps01`, static IP `10.0.0.66`. **ESP32-C3, not ESP8266** (board package `esp32`, board "ESP32C3 Dev Module"; enable *USB CDC On Boot*). Publishes `lat`/`lon` (+ `alt_m`/`sats`/`speed_kmh`) once it has a fix. Uses `secrets.h` like the other sketches (`WIFI_SSID`/`WIFI_PASS`/`MQTT_HOST`). Wiring: GPS `VCC→3V3` (or 5V if your breakout regulates), `GND→GND`, **`TX→GPIO20`** (required), `RX→GPIO21` (optional). OLED is onboard on I²C `SDA=GPIO5`/`SCL=GPIO6`, addr `0x3C`.
- `firmware/dht11_xiao_c6_deepsleep/` — DHT11 only, no display, battery deep-sleep (5-min wake → publish → sleep), device `xiao_c6`, static IP `10.0.0.63`. **Seeed XIAO ESP32-C6, not ESP8266** (board package `esp32` / Arduino-ESP32 core **≥3.0.0**, board "XIAO_ESP32C6"; enable *USB CDC On Boot*). The C6 wakes itself on the RTC timer, so **no `D0→RST` wire** is needed (unlike the ESP8266 deep-sleep boards). Uses `secrets.h`. Wiring: DHT11 `VCC→3V3`, `GND→GND`, **`DATA→D10 (GPIO18)`**; a 4.7–10 kΩ pull-up on `DATA` is needed for a bare 3-pin sensor (most blue DHT11 modules include it).
  - **Battery voltage reporting (live):** the BAT pads on this board are damaged, so the LiPo feeds the **`3V3` pin** directly (rail voltage = battery voltage). A 1:2 divider of **two equal ~100 kΩ resistors** from `3V3` → **`A0`/GPIO0** (no capacitor needed at 100 kΩ; A0 is free since the DHT is on D10, and on the C6 the boot strap is GPIO9 not GPIO0) feeds the ADC; the firmware reports `batt_v = analogReadMilliVolts(0) * 2` (averaged over 8 samples) in the JSON payload, charted as **battery (V)** on the dashboard. ⚠️ A full 4.2 V LiPo on `3V3` exceeds the C6's ~3.6 V max — keep the pack ≤3.6 V, or feed the `5V` pin instead (regulated, but browns out ~3.3–3.4 V). Full notes in `firmware/dht11_xiao_c6_deepsleep/ROADMAP.md`.

Board package: `esp8266` (NodeMCU 1.0 / ESP-12F) for all sketches **except** `neo6m_c3_oled` (ESP32-C3) and `dht11_xiao_c6_deepsleep` (ESP32-C6, needs Arduino-ESP32 core ≥3.0.0) which use `esp32`.

Required Arduino libraries:
- `DHT sensor library` (Adafruit) — DHT11 sketches
- `Adafruit AHTX0` — ENS160+AHT21 sketches (covers AHT21)
- `ScioSense_ENS160` — ENS160+AHT21 sketches
- `PubSubClient` — all sketches
- `Adafruit_GFX` + `Adafruit_SSD1306` — HW-364A OLED variants only
- `TinyGPSPlus` (Mikal Hart) + `U8g2` (olikraus) — `neo6m_c3_oled` only (U8g2 handles the 0.42″ 72×40 panel's offset; Adafruit_SSD1306 can't)

Before flashing, set up secrets per sketch:

```bash
# Once per sketch folder — copy the template and fill in WIFI_SSID / WIFI_PASS / MQTT_HOST.
# `secrets.h` is gitignored. Arduino IDE doesn't traverse parent folders, so each
# sketch needs its own copy.
for d in firmware/*/; do cp -n firmware/secrets.h.example "$d/secrets.h"; done
```

Then edit per-device constants at the top of each `.ino`: `STATIC_IP` and
`DEVICE_ID`. `MQTT_HOST` (in `secrets.h`) is the LAN IP of the host running
compose (the example uses `10.0.0.88`). To fall back to DHCP,
comment out `#define USE_STATIC_IP`.

Adding a new device: copy a sketch folder, change `DEVICE_ID` and `STATIC_IP`,
copy `secrets.h` into the new folder, flash.

## Test publishing manually

```bash
docker compose exec mosquitto mosquitto_pub \
  -t 'sensors/lab/indoor01/telemetry' \
  -m '{"device_id":"indoor01","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3}'
```

## Topic / payload

- MQTT: `sensors/<location>/<device_id>/telemetry`
- Malformed payloads are written to the `telemetry_dlq` table (the old Kafka DLQ topic).

```json
{"device_id":"indoor01","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3}
{"device_id":"ccs01","ts":1714000000,"eco2_ppm":612,"tvoc_ppb":34}
{"device_id":"air01","ts":1714000000,"temp_c":22.5,"humidity":45,"eco2_ppm":612,"tvoc_ppb":34,"aqi":2}
{"device_id":"gps01","ts":1714000000,"lat":48.1372,"lon":11.5756,"alt_m":519,"sats":9,"speed_kmh":0}
{"device_id":"xiao_c6","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"batt_v":3.79}
{"device_id":"BME280_xiaoc6","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"pressure_hpa":1013.2}
```

Sensor-specific fields are optional — each device sends only what it
measures. `device_id` and `ts` are the only required fields. Currently
recognised optional fields: `temp_c`, `humidity`, `heat_index_c`,
`eco2_ppm`, `tvoc_ppb`, `aqi`, `batt_v` (battery voltage of deep-sleep
nodes), `pressure_hpa` (barometric pressure from BME280 nodes), and the
GPS fields `lat`, `lon`, `alt_m`, `sats`, `speed_kmh` (see
`postgres/init.sql`). GPS sensors are plotted on the **`/gps`** map view
(1h/7d/30d).

## Troubleshooting

**No data in the dashboard?** Walk the pipeline in order:

```bash
# 1. Device actually publishing?
docker compose exec mosquitto mosquitto_sub -t 'sensors/#' -v

# 2. Ingest service happy?
docker compose logs --tail=100 ingest

# 3. Rows in the DB?
docker compose exec postgres psql -U sensors -d sensors \
  -c 'SELECT count(*), max(ts) FROM telemetry;'
```

**DLQ.** The `telemetry_dlq` table collects payloads that couldn't be
decoded/validated/inserted — check it if a known-good device's data isn't landing:
`docker compose exec postgres psql -U sensors -d sensors -c 'SELECT * FROM telemetry_dlq ORDER BY received_at DESC LIMIT 20;'`

**Browser can't reach API.** `NEXT_PUBLIC_API_BASE` and
`NEXT_PUBLIC_WS_URL` are baked into the web bundle at build. For LAN
access, set both in `.env` to the host's LAN IP and rebuild `web`
(`docker compose build web && docker compose up -d web`).

**ESP8266 won't join WiFi.** The sketches default to a static IP on
`10.0.0.0/24`. If your LAN is different, either update the `STATIC_*`
constants or comment out `#define USE_STATIC_IP` to fall back to DHCP.

**Fresh deploy but no schema.** `postgres/init.sql` only runs on first
init of an empty data dir. On an existing one the `ingest` service re-runs
the idempotent `ALTER TABLE`s on startup, so new optional columns appear
without a manual migration.
