# Hardware — BME680 node (`BME680_fbc6`)

**Status:** As-built
**Scope:** The DFRobot Gravity BME680 (SEN0248) on a standard FireBeetle 2
ESP32-C6 board — sketch `firmware/bme680_firebeetle2_c6_deepsleep/`, device id
`BME680_fbc6`, static IP `10.0.0.69` — and the `gas_kohm` telemetry field it
introduced across the stack. Board and skeleton behavior:
`hw-firebeetle2-c6.md`. Counterpart: `hw-bme280.md`.
Wiki: <https://wiki.dfrobot.com/sen0248/>.

## Behavior (as-built)

- **Wiring:** the SEN0248 is a Gravity module (keyed 4-pin cable) but the
  DFR1075 has no Gravity I2C socket, so the cable's bare ends go to the
  breadboard: `+`→3V3, `−`→GND, `C` (clock)→SCL/GPIO20, `D` (data)→SDA/GPIO19.
  Electrically it is the same I2C bus as plain pins — the connector only adds
  keying. Module address 0x77 (DFRobot default).
- **Driver:** `Adafruit_BME680` (plus its BusIO/Unified-Sensor deps).
  `initSensor()` probes 0x77 then 0x76, then retries with SDA/SCL swapped.
  Settings: oversampling T×8 / H×2 / P×4, IIR filter off, gas heater
  320 °C for 150 ms; one blocking `performReading()` per wake (forced mode —
  heater self-heating is negligible at one shot per 300 s).
- **Published fields:** the BME280 set (`temp_c`, `humidity`, `pressure_hpa`,
  `heat_index_c`, `batt_v`) plus **`gas_kohm`** = `gas_resistance / 1000`
  (kΩ), omitted when the reading is invalid (NaN/0).
- **`gas_kohm` semantics:** raw MOX gas-sensor resistance, **higher =
  cleaner air**. It is not an IAQ score: Bosch's BSEC blob is not available
  for the C6 (RISC-V; the DFRobot lib ships it for ESP8266/xtensa only), so
  the node publishes the raw resistance. A fresh sensor needs ~24 h+ of
  burn-in cycles before values stabilize, and 300 s forced-mode one-shots
  should be read as a relative trend, not an absolute measure.
- **`gas_kohm` through the stack** (mirrors `pressure_hpa`): column in
  `postgres/init.sql` + ingest `MIGRATIONS`/`INSERT`/`to_row`
  (`ingest.md`), API self-migration + `/history`, `/history-all`, `/latest`
  projections + threshold allowlist (`api.md`), web `METRICS` entry
  (label "gas", unit kΩ, 0 digits, `#ffa657`, no default thresholds) and a
  SensorCard row (`web-dashboard.md`). Charts, CSV export, and the settings
  editor pick it up automatically because they derive from `METRICS`.

## Requirements

### R1 — Gas metric end-to-end (Implemented)

Field the BME680 as the premium half of the comparison and carry its unique
capability — the gas channel — through the whole stack so it is visible next
to the shared climate metrics.

Acceptance criteria:

- **Given** the flashed node, **when** it wakes on the 300 s timer, **then**
  one telemetry message arrives on `sensors/lab/BME680_fbc6/telemetry`
  including `gas_kohm`.
- **Given** a stored reading, **then** `GET /latest` and the history routes
  return `gas_kohm` for the device (verified in production 2026-07-10; first
  pre-burn-in value 5.9 kΩ).
- **Given** the dashboard, **then** the device card shows a
  `gas <N> kΩ` row when the value is present.
- **Given** `/overview`, `/groups/[id]`, `/history/[device_id]`, **then** a
  "gas" chart appears once the device has non-null values, and the CSV export
  includes a `gas_kohm` column.
- **Given** a device without a BME680, **then** nothing changes — the field
  is optional and inserts as NULL like every other sensor field.

## Non-goals

- No BSEC/IAQ index on this platform — raw `gas_kohm` only (see above).
- No default thresholds for `gas_kohm` — raw resistance has no canonical
  quality bands; users can add custom lines in `/settings`.
- No continuous-mode gas sampling — one heated shot per wake keeps the
  battery budget of the deep-sleep skeleton.

## Open questions

- Whether the raw gas trend is useful enough long-term, or the fleet's air
  nodes (ENS160 AQI/TVOC/eCO₂) remain the primary air-quality signal.
