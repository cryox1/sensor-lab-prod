# Hardware — FireBeetle 2 ESP32-C6 (standard node board)

**Status:** As-built
**Scope:** The DFRobot FireBeetle 2 ESP32-C6 (DFR1075) as the fleet's standard
node board, and the shared headless deep-sleep firmware skeleton its sketches
are built from. Sensor-specific behavior lives in the per-node specs
(`hw-bme280.md`, `hw-bme680.md`, `hw-ens160.md` — the latter is always-on and
uses the board without the deep-sleep skeleton — and `hw-as3935.md`, which
extends the skeleton with a GPIO deep-sleep wake and solar power).
Wiki: <https://wiki.dfrobot.com/dfr1075/>.

## Behavior (as-built)

### Board facts (wiki-verified)

- **MCU:** ESP32-C6FH4 — single-core RISC-V @ 160 MHz, 512 KB SRAM,
  320 KB ROM, 4 MB flash, 16 KB RTC SRAM.
- **Radio:** 2.4 GHz WiFi 6 (802.11ax 20 MHz non-AP) + b/g/n, BLE 5 / BT mesh,
  802.15.4 (Thread 1.3 / Zigbee 3.0). Only WiFi is used (see Non-goals).
- **Power:** USB-C 5 V; VCC pin accepts 5 V or a 5 V solar panel; onboard
  LiPo charger (max 0.5 A) with a PH2.0-2P "BAT" connector. Deep-sleep draw:
  16 µA (board v1.0) / 36 µA (board v1.2). Dimensions 25.4 × 60 mm.

### Pin contract

| Function | GPIO | Notes |
|---|---|---|
| I2C SDA | **19** | silkscreen `SDA` — default `Wire` bus |
| I2C SCL | **20** | silkscreen `SCL` |
| VBAT sense | **0** | onboard 1:2 divider from VBAT; `analogReadMilliVolts(0) * 2` — **no external resistors** |
| Onboard LED | 15 | unused in headless builds |
| BOOT button | 9 | pull-up boots the app on a clean power-on |

Do **not** use the Arduino variant's `D*` aliases — they do not match the
silkscreen (`D6` is GPIO1). Sketches use raw GPIO numbers only.

### Arduino toolchain

- Core `esp32:esp32` ≥ 3.0.0 (installed: 3.3.8), built with **arduino-cli**
  (PlatformIO's espressif32 platform has no ESP32-C6 build script).
- FQBN: `esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc`. Unlike the
  XIAO_ESP32C6 variant, **"USB CDC On Boot" defaults to Disabled** here —
  omit the option and Serial is silent on the USB port.
- Flashing: upload to `/dev/ttyACM*`. Both DFR1075 units booted the app
  straight from esptool's RTS hard-reset (the XIAO-C6 stuck-in-download-mode
  gotcha did not reproduce; if a board ever shows the symptom — port
  enumerated, zero serial, never publishes — a USB power-cycle fixes it).
  The port disappearing right after upload is the *good* sign: the app is
  running and deep-sleeping (USB is off during sleep).

### Standard deep-sleep skeleton

Ported from the proven `bme280_xiao_c6_deepsleep` build; per wake:
sensor read → one MQTT publish → deep sleep. No display, no LED, no wake
wire (the C6 RTC timer wakes itself — no D0→RST).

- **Cadence:** `SLEEP_SECONDS 300`.
- **WiFi:** static IPv4 (`USE_STATIC_IP`, two DNS servers), AP BSSID+channel
  cached in RTC memory for a scanless reconnect (5 s budget) with a full-scan
  fallback (12 s).
- **Clock:** NTP only on cold boot and every `NTP_RESYNC_WAKES 24` wakes;
  in between the epoch is carried forward in RTC memory (sleep-drift
  compensated).
- **MQTT:** `primePath()` UDP nudge (forces ARP/forwarding before the first
  SYN), then connect+publish with `MQTT_RETRIES 6` and escalating backoff,
  4 s socket timeout. Telemetry topic `sensors/lab/<DEVICE_ID>/telemetry`,
  client id `esp-<DEVICE_ID>`.
- **Diagnostics:** a retained debug record on `sensors/lab/<DEVICE_ID>/debug`
  — a non-telemetry topic ingest ignores, so it never reaches the DB.
- **No task watchdog** — every blocking step is individually timeout-bounded
  so `setup()` always reaches sleep (an earlier watchdog boot-looped a C6).
- Per-node identity is confined to `DEVICE_ID`, `STATIC_IP`, and the sensor
  block; `secrets.h` (gitignored, from `firmware/secrets.h.example`) provides
  `WIFI_SSID`/`WIFI_PASS`/`MQTT_HOST`.

Static-IP ledger for this board family: `.68` = `BME280_fbc6`,
`.69` = `BME680_fbc6`, `.70` = `air03`, `.71` = **CYD wall panel** (reserved —
a non-fleet device outside this repo, do not assign), `.72` = `storm01`,
`.73` = `air04`, `.74` = `air05`
(fleet-wide list in the sketches — grep `STATIC_IP` across `firmware/`;
**ping is useless** for checking a deep-sleep node's IP, but `air03` is
always-on and does answer).

## Requirements

### R1 — Adopt as the standard node board (Implemented)

New battery/deep-sleep nodes default to the FireBeetle 2 ESP32-C6 with the
skeleton above, so every future sensor is a copy-the-skeleton job instead of
a new bring-up. First adopters: `hw-bme280.md` R1 and `hw-bme680.md` R1.

Acceptance criteria:

- **Given** a new sensor to field, **when** its sketch is created, **then** it
  is a copy of the skeleton where only `DEVICE_ID`, `STATIC_IP`, and the
  sensor block differ (WiFi/NTP/MQTT/sleep code identical across nodes).
- **Given** the board's onboard divider, **then** `batt_v` is published with
  `ENABLE_BATTERY 1`, `BATT_ADC_PIN 0`, `BATT_DIVIDER 2.0f` and no external
  parts.
- **Given** the documented FQBN (with `CDCOnBoot=cdc`), **when** the sketch is
  compiled with arduino-cli against core 3.x, **then** it builds without
  PlatformIO.
- **Given** a flashed board, **then** it publishes on the 300 s cycle and its
  retained debug record is inspectable via
  `mosquitto_sub -t 'sensors/lab/<DEVICE_ID>/debug'`.

## Non-goals

- No Zigbee/Thread/Matter and no WiFi 6 TWT — MQTT over plain WiFi stays the
  fleet transport (the 802.15.4 radio is idle).
- No OTA updates — nodes are reflashed over USB.
- No use of the GDI display FPC connector — these are headless builds.

## Open questions

- None.
