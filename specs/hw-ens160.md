# Hardware — ENS160+AHT21 node (`air03`)

**Status:** As-built
**Scope:** The ENS160+AHT21 combo module on a FireBeetle 2 ESP32-C6 board —
sketch `firmware/ens160_aht21_firebeetle2_c6/`, device id `air03`, static IP
`10.0.0.70`. Board facts and toolchain: `hw-firebeetle2-c6.md` (this node uses
the board, but deliberately **not** the deep-sleep skeleton — see below). The
legacy ESP8266 air nodes (`air01`, `air02`) are out of scope per the
legacy-firmware carve-out in `README.md` (this directory).

## Behavior (as-built)

- **Wiring:** VCC→3V3, GND→GND, **SDA→GPIO20, SCL→GPIO21** — a deliberate
  deviation from the board's silkscreened default I2C bus (SDA=19/SCL=20) used
  by the other fbc6 nodes; the sketch has **no** swapped-pin fallback probe so
  a wiring regression fails loudly instead of being masked. The combo board's
  CS pin is bridged HIGH → I2C mode, ENS160 at 0x53 (`ENS160_I2CADDR_1`),
  AHT21 at its fixed 0x38.
- **Drivers:** `Adafruit AHTX0` (AHT21) + `ScioSense_ENS160`, same as the
  legacy air nodes. AHT21 temp/humidity feed `set_envdata()` so the ENS160
  compensates its gas readings.
- **Always-on, no deep sleep:** the node is permanently USB-powered, and the
  ENS160 requires continuous STD-mode operation (minutes of warm-up) before
  its eCO2/TVOC/AQI outputs are trustworthy — `setMode(ENS160_OPMODE_STD)`
  runs once in `setup()` and the sensor is never re-inited. No `batt_v`
  (no battery, `ENABLE_BATTERY` absent).
- **Cadence:** one telemetry publish every **30 s** (`READ_INTERVAL 30000`),
  from a persistent WiFi+MQTT session: `connectWifi()`/`connectMqtt()`
  early-return while connected and reconnect after a drop (clean
  `WiFi.disconnect()` on re-entry, `primePath()` UDP nudge before each MQTT
  connect, 4 s socket timeout). SNTP starts once WiFi is first up; the core
  re-syncs periodically, and publishes are gated on a sane epoch.
- **Published fields:** `temp_c`, `humidity`, `heat_index_c` (shared Rothfusz
  helper, cross-node consistent), `eco2_ppm`, `tvoc_ppb`, `aqi` (ints) —
  failed reads omit their fields. All columns already exist end-to-end; the
  node needed **no** ingest/API/web/schema change.
- **Diagnostics:** a retained boot/reconnect record on
  `sensors/lab/air03/debug` (reset reason, sensor init flags, IP/RSSI,
  connect + publish counters), re-published on every MQTT reconnect so the
  retained copy always reflects the latest link state.

## Requirements

### R1 — Always-on air node (Implemented)

Field the ENS160+AHT21 pair on the standard node board as a continuously
running air-quality monitor, replacing nothing — it extends the air series
(`air01`, `air02`) onto the C6 fleet.

Acceptance criteria:

- **Given** the flashed node, **then** one telemetry message arrives on
  `sensors/lab/air03/telemetry` every 30 s containing `temp_c`, `humidity`,
  `heat_index_c`, `eco2_ppm`, `tvoc_ppb`, `aqi` (after ENS160 warm-up — the
  first minutes report the sensor's defaults 400/0/1).
- **Given** hours of uptime, **then** the ENS160 has stayed in STD mode the
  whole time (no re-`begin()`/re-`setMode()` in the loop).
- **Given** a WiFi or broker outage, **when** the outage ends, **then** the
  node resumes publishing on its own — no reflash, no power-cycle.
- **Given** the retained debug record
  (`mosquitto_sub -t 'sensors/lab/air03/debug'`), **then** it shows the boot
  sensor-init flags and a connect counter that only grows on real reconnects.
- **Given** the dashboard, **then** an `air03` card appears automatically on
  first publish and `/history/air03` fills in.

## Non-goals

- No deep sleep / battery operation and no `batt_v` — permanently USB-powered.
- No ENS160 baseline persistence across power cycles (the sensor re-warms
  after a power loss; acceptable for a mains-powered node).
- No per-cycle connect/disconnect — the persistent session is the point of
  the always-on build.

## Open questions

- Whether 30 s should widen later (ingest and dashboard don't care; purely a
  data-volume preference).
