# sensor-lab

ESP sensor nodes (ESP8266 / ESP32-C3 / ESP32-C6 — DHT11, ENS160+AHT21, BME280,
BME680 incl. BSEC2 IAQ, AS3935 lightning, NEO-6M GPS) → MQTT (Mosquitto) →
PostgreSQL → FastAPI + Next.js.

A small, fully open-source IoT pipeline sized for a homelab. Every component is
OSS: Eclipse Mosquitto, PostgreSQL, FastAPI, Next.js.

## Architecture

![Architecture: ESP nodes publish MQTT to Mosquitto; ingest writes to PostgreSQL; the FastAPI api serves REST + /ws/live to the browser next to the Next.js web dashboard; the optional Blitzortung.org feed enters through ingest](docs/architecture.svg)

`ingest/` subscribes to MQTT and writes each reading to Postgres; `api/` is the
FastAPI read + WebSocket server (its `/ws/live` feed subscribes to MQTT and fans
out to browsers); `web/` is the Next.js dashboard. Mosquitto is required because
the ESP devices speak only MQTT. Optionally `ingest` also follows the public
Blitzortung.org feed and republishes nearby strikes on the local broker (see
[the lightning overlay](#blitzortungorg-lightning-overlay)).

The diagram source is `docs/architecture.drawio`; the exported
`docs/architecture.svg` embeds the same XML, so either file opens in
[draw.io](https://www.drawio.com/) — re-export the SVG after editing.

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

By default the API is open. To lock down the rename / hide endpoints on a
shared LAN, set `API_WRITE_TOKEN` (api) + `WEB_API_WRITE_TOKEN` (web) to the
same value in `.env`; the browser will include `X-API-Token` on writes.
Optionally also tighten `CORS_ORIGINS`. Both are documented in `.env.example`.

## Web pages

- `/` — dashboard: live sensor cards (fed by `/ws/live`), rendered per group
  (sensors without a group fall under "Ungrouped"). The **manage** drawer
  (top-right) renames devices, hides/shows them, assigns groups, and can delete
  a device (optionally including its recorded data). Hidden devices keep
  recording — they're just removed from the default view.
- `/overview` — shared per-metric charts across all sensors, with a time-range
  selector and CSV export.
- `/groups` — drag & drop board to create named groups and assign each sensor
  to at most one of them. Names and membership are stored server-side
  (`sensor_groups` / `sensor_group_members`), so they're shared across browsers.
- `/groups/<id>` — the `/overview` charts scoped to one group, with a
  **live / history** toggle (live streams the group's sensors from `/ws/live`
  on a rolling 5-minute window; history shows the bucketed chosen range), a
  clickable legend to hide individual series, and CSV export.
- `/history/<device_id>` — single-device charts with live/history toggle,
  stats, and per-device display offsets.
- `/gps` — Leaflet map of the GPS sensors (1h/7d/30d), plus the optional
  Blitzortung.org lightning overlay (see below).
- `/settings` — edit the per-metric threshold lines drawn on the charts.

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

### API at a glance

Reads: `/health`, `/devices`, `/devices/<id>/stats`, `/latest` (newest row per
device), `/history?device_id=&hours=&bucket_seconds=`, `/history-all`,
`/groups`, `/strikes?minutes=` (lightning), `/thresholds`, plus the `/ws/live`
WebSocket (telemetry + lightning strikes fanned out from MQTT). Writes — rename
/ hide / group / delete devices, create / rename / delete groups, set / reset
metric thresholds — are `PUT`/`POST`/`DELETE` siblings of those routes and
require the `X-API-Token` header once `API_WRITE_TOKEN` is set.

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

## Blitzortung.org lightning overlay

The `/gps` map can show live lightning strikes from the
[Blitzortung.org](https://www.blitzortung.org) community network (private,
non-commercial use; attribution is rendered on the map). `ingest` runs a second
MQTT client against the public feed, keeps strikes within `BLITZ_RADIUS_KM` of
home, stores them in `lightning_strikes`, and republishes them on the local
broker so the api's `/ws/live` pushes them to the map instantly.

The feature is **off until `BLITZ_HOME_LAT`/`BLITZ_HOME_LON` are set** (see
`.env.example`). On the server this is a one-time **manual** step — deploy.sh
never touches the server's compose/`.env` (AGENTS.md): append the `BLITZ_*`
lines to `~/sensor-lab/.env`, hand-add the same env passthroughs to the
`ingest` and `api` services in `~/sensor-lab/docker-compose.yml` (verify with
`docker compose config` — no other diff), then `docker compose up -d ingest api`
(never `down`, never touch `postgres`/`mosquitto`).

Note: the AS3935 node (storm01) cannot *feed into* Blitzortung — their network
locates strikes via GPS-timestamped time-of-arrival and needs their own
System Blue hardware. This overlay is one-way: their located strikes next to
our sensor's local detections.

## Firmware

Arduino sketches for the ESP devices live under `firmware/`. Each
sketch is in its own folder named after the `.ino` file (Arduino IDE
requirement):

- `firmware/dht11_oled/` — DHT11 + HW-364A OLED, device `indoor01`, static IP `10.0.0.60`.
- `firmware/dht11_nodemcu/` — DHT11 only, no display, device `indoor02`, static IP `10.0.0.62`.
- `firmware/ens160_aht21_nodemcu/` — ENS160 + AHT21 (eCO₂ / TVOC / AQI + temp / humidity), no display, device `air01`, static IP `10.0.0.64`.
- `firmware/ens160_aht21_oled/` — ENS160 + AHT21 + HW-364A OLED, device `air02`, static IP `10.0.0.65`. All three I²C devices share the same bus (OLED `0x3C`, AHT21 `0x38`, ENS160 `0x53`).
- `firmware/dht11_oled_deepsleep/`, `firmware/dht11_nodemcu_deepsleep/`, `firmware/ens160_aht21_nodemcu_deepsleep/`, `firmware/ens160_aht21_oled_deepsleep/` — battery deep-sleep variants of the four ESP8266 sketches above (same device IDs and IPs; wake → publish → 5-min sleep). They need the **`D0→RST` wire** to wake (remove it while flashing), leave the OLED dark, and on the ENS160 pair the gas readings never finish warming up between wakes — treat eCO₂ / TVOC / AQI from those two as unreliable.
- `firmware/neo6m_c3_oled/` — NEO-6M GPS + onboard 0.42″ OLED, device `gps01`, static IP `10.0.0.67`. **ESP32-C3, not ESP8266** (board package `esp32`, board "ESP32C3 Dev Module"; enable *USB CDC On Boot*). Publishes `lat`/`lon` (+ `alt_m`/`sats`/`speed_kmh`) once it has a fix. Uses `secrets.h` like the other sketches (`WIFI_SSID`/`WIFI_PASS`/`MQTT_HOST`). Wiring: GPS `VCC→3V3` (or 5V if your breakout regulates), `GND→GND`, **`TX→GPIO20`** (required), `RX→GPIO21` (optional). OLED is onboard on I²C `SDA=GPIO5`/`SCL=GPIO6`, addr `0x3C`.
- `firmware/neo6m_c3_deepsleep/` — the same GPS node without the OLED, as a deep-sleep variant (5-min wake). ⚠️ The NEO-6M itself stays powered through the sleep (~25–45 mA), so a battery lasts only ~1–2 days — prefer USB power or the OLED variant.
- `firmware/dht11_xiao_c6_deepsleep/` — DHT11 only, no display, battery deep-sleep (5-min wake → publish → sleep), device `xiao_c6`, static IP `10.0.0.63`. **Seeed XIAO ESP32-C6, not ESP8266** (board package `esp32` / Arduino-ESP32 core **≥3.0.0**, board "XIAO_ESP32C6"; enable *USB CDC On Boot*). The C6 wakes itself on the RTC timer, so **no `D0→RST` wire** is needed (unlike the ESP8266 deep-sleep boards). Uses `secrets.h`. Wiring: DHT11 `VCC→3V3`, `GND→GND`, **`DATA→D10 (GPIO18)`**; a 4.7–10 kΩ pull-up on `DATA` is needed for a bare 3-pin sensor (most blue DHT11 modules include it).
  - **Battery voltage reporting (live):** the BAT pads on this board are damaged, so the LiPo feeds the **`3V3` pin** directly (rail voltage = battery voltage). A 1:2 divider of **two equal ~100 kΩ resistors** from `3V3` → **`A0`/GPIO0** (no capacitor needed at 100 kΩ; A0 is free since the DHT is on D10, and on the C6 the boot strap is GPIO9 not GPIO0) feeds the ADC; the firmware reports `batt_v = analogReadMilliVolts(0) * 2` (averaged over 8 samples) in the JSON payload, charted as **battery (V)** on the dashboard. ⚠️ A full 4.2 V LiPo on `3V3` exceeds the C6's ~3.6 V max — keep the pack ≤3.6 V, or feed the `5V` pin instead (regulated, but browns out ~3.3–3.4 V). Full notes in `firmware/dht11_xiao_c6_deepsleep/ROADMAP.md`.
- `firmware/bme280_xiao_c6_deepsleep/` — GY-BME280 (temp / humidity / **pressure**), battery deep-sleep, device `BME280_xiaoc6`, static IP `10.0.0.61`. Seeed XIAO ESP32-C6, same board setup as `dht11_xiao_c6_deepsleep`. Self-contained BME280 driver (no Adafruit_BME280 lib). Wiring: `VCC→3V3`, `GND→GND`, `SDA→D5 (GPIO23)`, `SCL→D6 (GPIO16)` — the sketch auto-tries the swapped orientation too.
- `firmware/bme280_firebeetle2_c6_deepsleep/` — GY-BME280, battery deep-sleep, device `BME280_fbc6`, static IP `10.0.0.68`. **DFRobot FireBeetle 2 ESP32-C6 (DFR1075)** — board package `esp32`, board "DFRobot FireBeetle 2 ESP32-C6"; ⚠️ *USB CDC On Boot* defaults to **Disabled** on this board, enable it (or use FQBN `esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc`). Wiring: `VCC→3V3`, `GND→GND`, `SDA→GPIO19`, `SCL→GPIO20` (the silkscreened I²C pins). `batt_v` uses the board's **onboard** 1:2 VBAT divider on GPIO0 — no external resistors.
- `firmware/bme680_firebeetle2_c6_deepsleep/` — DFRobot Gravity **BME680** (SEN0248: temp / humidity / pressure / **gas resistance**), battery deep-sleep, device `BME680_fbc6`, static IP `10.0.0.69`. Same FireBeetle 2 ESP32-C6 setup as above. Publishes `gas_kohm` (raw MOX resistance, higher = cleaner air; a relative trend, not calibrated IAQ — one heater shot per 5-min wake never reaches steady state; `air04` runs Bosch's BSEC2 IAQ instead, which needs always-on operation). Wiring via the sensor's Gravity cable: `+→3V3`, `−→GND`, `C→GPIO20 (SCL)`, `D→GPIO19 (SDA)`. Sensor I²C addr `0x77` (DFRobot default; sketch also probes `0x76`). Built to pair with `bme280_firebeetle2_c6_deepsleep` for a BME280-vs-BME680 accuracy comparison.
- `firmware/ens160_aht21_firebeetle2_c6/` — ENS160 + AHT21 (eCO₂ / TVOC / AQI + temp / humidity / heat index), **always-on** (permanently USB-powered, no deep sleep — the ENS160 needs continuous STD-mode operation to give trustworthy readings), publishes every **30 s**, device `air03`, static IP `10.0.0.70`. Same FireBeetle 2 ESP32-C6 setup as above. Wiring: `VCC→3V3`, `GND→GND`, **`SDA→GPIO20`, `SCL→GPIO21`** — deliberately *not* the board's silkscreened default I²C pins; the combo board's CS pin is bridged HIGH → I²C mode, ENS160 addr `0x53`.
- `firmware/bme680_ens160_aht21_firebeetle2_c6/` — DFRobot Gravity **BME680** (SEN0248) via **Bosch BSEC2** + **ENS160 + AHT21** combo, **always-on** indoor air node (permanently USB-powered, no deep sleep — the ENS160 needs continuous STD mode and BSEC2 samples the BME680 every 3 s for its IAQ calibration), publishes every **30 s**, device `air04`, static IP `10.0.0.73`. Same FireBeetle 2 ESP32-C6 setup as above. Publishes BSEC's calibrated **`iaq`** (static IAQ 0–500) + `iaq_acc` (calibration 0–3, persisted to NVS so reboots resume calibrated), `co2_eq_ppm`, `bvoc_eq_ppm`, heat-compensated `temp_c`/`humidity`, `pressure_hpa`, `gas_kohm`, plus the ENS160's `eco2_ppm`/`tvoc_ppb`/`aqi`. The BME680 runs in **SPI mode** via its 6 pads, wired **without** the Gravity cable: `VCC→3V3`, `GND→GND`, `SCLK→GPIO23 (SCK)`, `MOSI→GPIO22 (MOSI)`, `MISO→GPIO21 (MISO)`, `CS→GPIO18` (the board's silkscreened default SPI pins). ⚠️ I²C over those pads does **not** work — per the SEN0248 V1.0 schematic the MOSI pad passes through a unidirectional 74HC125 gate, so the sensor's ACKs can never reach the pad (the bidirectional SDA shifter sits only on the Gravity connector's D pin). ENS160 combo on the default I²C bus: `SDA→GPIO19`, `SCL→GPIO20`, ENS160 `0x53`, AHT21 `0x38`. ⚠️ The `bsec2` Arduino library ships no ESP32-C6 blob — copy `src/esp32c3` to `src/esp32c6` and add `esp32c6` to its `library.properties` `architectures=` (redo after a library update; the sketch header has the commands).
- `firmware/bme680_bsec2_firebeetle2_c6/` — DFRobot Gravity **BME680** (SEN0248) via **Bosch BSEC2**, **always-on** single-sensor indoor air node (the ENS160-less sibling of `air04` — with BSEC2 the BME680 alone covers temp / humidity / pressure / IAQ / CO₂- and VOC-estimates), publishes every **30 s**, device `air05`, static IP `10.0.0.74`. Same FireBeetle 2 ESP32-C6 setup, same BSEC2 blob workaround, same SPI-pad wiring as `air04`: `VCC→3V3`, `GND→GND`, `SCLK→GPIO23`, `MOSI→GPIO22`, `MISO→GPIO21`, `CS→GPIO18`. Publishes `iaq`/`iaq_acc`/`co2_eq_ppm`/`bvoc_eq_ppm`, heat-compensated `temp_c`/`humidity`, `heat_index_c`, `pressure_hpa`, `gas_kohm`.
- `firmware/bme280_as3935_firebeetle2_c6_deepsleep/` — DFRobot Gravity **AS3935 lightning sensor** (SEN0290) + optional GY-BME280 (the sketch runs fine without it), **indoor** deep-sleep storm node, device `storm01`, static IP `10.0.0.72` (`.71` is the CYD wall panel). Built and running indoors (`specs/hw-as3935.md`; the outdoor-solar requirement R1 is still open). Same FireBeetle 2 ESP32-C6 setup as above, plus a second wake source: the AS3935 IRQ on **GPIO2** wakes the C6 from deep sleep on a strike (burst strikes within 60 s are accumulated in RTC memory, not published individually), with a dynamic noise floor that masks and re-arms the IRQ when the room gets too noisy. Heartbeat every **300 s** with `batt_v` + `lightning_count` (0 when quiet) and climate when a BME280 is fitted. Power: USB or LiPo at the **BAT connector**; the outdoor-solar option (solar panel → DFR0579 Solar Power Manager Micro → LiPo → BAT connector; the DFR0579's 90 mA output can't feed WiFi TX peaks; remove the SEN0290's power LED) remains documented in the spec. Wiring: SEN0290 on the default bus (`VCC→3V3`, `GND→GND`, `SDA→GPIO19`, `SCL→GPIO20`), addr DIP `0x03` (both ON), `IRQ→GPIO2`; optional BME280 on the same bus.

Board package: `esp8266` (NodeMCU 1.0 / ESP-12F) for all sketches **except** the `neo6m_c3_*` sketches (ESP32-C3), the `*_xiao_c6_deepsleep` sketches (Seeed XIAO ESP32-C6) and the `*_firebeetle2_c6*` sketches (DFRobot FireBeetle 2 ESP32-C6), which use `esp32` (Arduino-ESP32 core ≥3.0.0).

Required Arduino libraries:
- `DHT sensor library` (Adafruit) — DHT11 sketches
- `Adafruit AHTX0` — ENS160+AHT21 sketches (covers AHT21)
- `ScioSense_ENS160` — ENS160+AHT21 sketches
- `PubSubClient` — all sketches
- `Adafruit_BME680` (+ deps `Adafruit BusIO`, `Adafruit Unified Sensor`) — `bme680_firebeetle2_c6_deepsleep` only
- `bsec2` (Bosch, + dep `BME68x Sensor library`) — `bme680_ens160_aht21_firebeetle2_c6` and `bme680_bsec2_firebeetle2_c6` (needs the esp32c6 blob workaround, see the air04 entry above)
- `DFRobot_AS3935` — `bme280_as3935_firebeetle2_c6_deepsleep` only (SEN0290 lightning sensor)
- `Adafruit_GFX` + `Adafruit_SSD1306` — HW-364A OLED variants only
- `TinyGPSPlus` (Mikal Hart) — both `neo6m_c3_*` sketches; `U8g2` (olikraus) — `neo6m_c3_oled` only (U8g2 handles the 0.42″ 72×40 panel's offset; Adafruit_SSD1306 can't)

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
- Malformed payloads are written to the `telemetry_dlq` table.

```json
{"device_id":"indoor01","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3}
{"device_id":"air01","ts":1714000000,"temp_c":22.5,"humidity":45,"eco2_ppm":612,"tvoc_ppb":34,"aqi":2}
{"device_id":"gps01","ts":1714000000,"lat":48.1372,"lon":11.5756,"alt_m":519,"sats":9,"speed_kmh":0}
{"device_id":"xiao_c6","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"batt_v":3.79}
{"device_id":"BME280_xiaoc6","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"pressure_hpa":1013.2}
{"device_id":"BME680_fbc6","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"pressure_hpa":1013.2,"gas_kohm":87.4,"batt_v":3.79}
{"device_id":"air03","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"eco2_ppm":612,"tvoc_ppb":34,"aqi":2}
{"device_id":"air04","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"pressure_hpa":1013.2,"gas_kohm":87.4,"iaq":52.3,"iaq_acc":3,"co2_eq_ppm":618,"bvoc_eq_ppm":0.52,"eco2_ppm":612,"tvoc_ppb":34,"aqi":2}
{"device_id":"air05","ts":1714000000,"temp_c":22.5,"humidity":45,"heat_index_c":22.3,"pressure_hpa":1013.2,"gas_kohm":87.4,"iaq":52.3,"iaq_acc":3,"co2_eq_ppm":618,"bvoc_eq_ppm":0.52}
{"device_id":"storm01","ts":1714000000,"temp_c":18.2,"pressure_hpa":998.4,"humidity":78,"heat_index_c":18.1,"lightning_km":12,"lightning_energy":174000,"lightning_count":3,"batt_v":3.92}
```

Sensor-specific fields are optional — each device sends only what it
measures. `device_id` and `ts` are the only required fields. Currently
recognised optional fields: `temp_c`, `humidity`, `heat_index_c`,
`eco2_ppm`, `tvoc_ppb`, `aqi`, `batt_v` (battery voltage of deep-sleep
nodes), `pressure_hpa` (barometric pressure from BME280/BME680 nodes),
`gas_kohm` (raw BME680 gas resistance in kΩ), the BSEC2 fields `iaq`
(static IAQ 0–500), `iaq_acc` (calibration accuracy 0–3), `co2_eq_ppm`
and `bvoc_eq_ppm` (BME680 nodes running Bosch's IAQ algorithm), the
AS3935 lightning fields
`lightning_km` (min storm-front distance), `lightning_energy` (max raw
intensity) and `lightning_count` (strikes since last publish, 0 = quiet),
and the GPS fields `lat`, `lon`, `alt_m`, `sats`, `speed_kmh` (see
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

**ESP node won't join WiFi.** The sketches default to a static IP on
`10.0.0.0/24`. If your LAN is different, either update the `STATIC_*`
constants or comment out `#define USE_STATIC_IP` to fall back to DHCP.

**Fresh deploy but no schema.** `postgres/init.sql` only runs on first
init of an empty data dir. On an existing one the `ingest` service re-runs
the idempotent `ALTER TABLE`s on startup, so new optional columns appear
without a manual migration.
