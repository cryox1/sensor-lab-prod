# Hardware — BME280 node (`BME280_fbc6`)

**Status:** As-built
**Scope:** The GY-BME280 breakout on a standard FireBeetle 2 ESP32-C6 board —
sketch `firmware/bme280_firebeetle2_c6_deepsleep/`, device id `BME280_fbc6`,
static IP `10.0.0.68`. Board and skeleton behavior: `hw-firebeetle2-c6.md`.
Counterpart in the BME280-vs-BME680 comparison: `hw-bme680.md`.

## Behavior (as-built)

- **Wiring:** GY-BME280 VCC→3V3, GND→GND, SDA→GPIO19, SCL→GPIO20 (the board's
  default I2C bus).
- **Driver:** self-contained raw-`Wire` driver with Bosch datasheet
  compensation — no Adafruit_BME280 dependency; `PubSubClient` is the
  sketch's only library. `initSensor()` probes address 0x76 then 0x77 in the
  wired orientation, then retries both with SDA/SCL swapped, so reversed
  wires work with no code change. A BMP280 answering instead is accepted and
  degrades gracefully (T+P only, humidity NaN).
- **Measurement:** one forced-mode conversion per wake — oversampling T×1,
  P×1, H×1, IIR filter off (`ctrl_meas 0x25`); reads the raw burst at 0xF7
  and compensates T first (sets `t_fine`).
- **Published fields:** `device_id`, `ts` plus `temp_c`, `humidity`,
  `pressure_hpa`, `heat_index_c` (Rothfusz regression, same formula as the
  DHT nodes for cross-node consistency), `batt_v`. NaN fields are omitted
  from the JSON.
- **Role:** reference sensor of the comparison pair — the cheap commodity
  breakout whose readings the BME680 is judged against.

## Requirements

### R1 — Comparison reference node (Implemented)

Field the GY-BME280 on a standard board as one half of the BME280-vs-BME680
comparison (is the BME680 worth its premium?).

Acceptance criteria:

- **Given** the flashed node, **when** it wakes on the 300 s timer, **then**
  one telemetry message arrives on `sensors/lab/BME280_fbc6/telemetry` with
  `temp_c`, `humidity`, `pressure_hpa`, `heat_index_c`, `batt_v`.
- **Given** the first published message, **then** the device appears on the
  dashboard with no registration step (device universe is built from
  readings).
- **Given** the payload shape, **then** no backend change is required — every
  field already exists end-to-end.

## Non-goals

- No new metrics — this node deliberately publishes only fields the stack
  already knows, so the comparison isolates sensor quality.
- No conclusions in this spec about BME280-vs-BME680 accuracy — that is the
  experiment's outcome, judged after both sensors have settled in place.

## Open questions

- None.
