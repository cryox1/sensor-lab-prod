# XIAO ESP32-C6 node — roadmap & future ideas

Future-facing notes for the `xiao_c6` node (and any other C6 boards added to the
fleet). The current firmware (`dht11_xiao_c6_deepsleep.ino`) only uses the C6 as
a plain Wi-Fi deep-sleep sensor — but the C6 is Espressif's "every radio" chip,
so there's a lot left on the table. Keep these in mind when iterating.

## Status (verified 2026-06-20)
- `dht11_xiao_c6_deepsleep` flashed + verified: DHT11 on **D10/GPIO18**, 5-min
  deep sleep, self-wakes on timer (no D0→RST wire), publishes to
  `sensors/lab/xiao_c6/telemetry`. Static IP `10.0.0.63`, scanless reconnect ~100 ms.
- Built with **arduino-cli + esp32:esp32@3.3.8** (the PlatformIO `espressif32`
  platform can't build C6 Arduino — no `platformio-build-esp32c6.py`).

## Hardware caveat: battery wiring & voltage monitoring
The board's **BAT solder pads were destroyed**; the LiPo is currently wired
directly to the **3V3 pin + GND**, bypassing the charge IC and regulator.

- ⚠️ A full LiPo (4.2 V) on the 3V3 rail exceeds the C6's ~3.6 V abs-max, and
  there's no charging/protection now. Keep the pack ≤~3.6 V, **or** move the
  battery wire to the **5V pin** (onboard LDO regulates to a safe 3.3 V; trade-off:
  brownout ~3.3–3.4 V, loses the bottom ~10–15% of LiPo capacity).
- **Voltage sense:** the 3V3 (or 5V) rail now equals the battery voltage. Add a
  **1:2 resistor divider → A0/GPIO0** (free; DHT is on D10. On the C6 the boot
  strap is GPIO9, so A0 is safe). Read `batt_v = analogReadMilliVolts(0) * 2`
  (averaged over a few samples), then add a `"batt_v"` field to the JSON payload
  so the dashboard tracks each node's battery curve — directly useful for the
  longevity test.
  - **Easy build (chosen):** two equal **~100 kΩ** resistors, **no capacitor**
    (low enough impedance that the ADC reads accurately without one). Costs a bit
    of deep-sleep current (~15 µA), so the longevity run is somewhat shorter.
  - **Max-battery alternative:** **2×1 MΩ + 100 nF** at A0 (~2 µA), or
    MOSFET-gate the divider's low leg from a GPIO for ~0 µA when asleep — the cap
    is needed at 1 MΩ to stop the ADC reading from sagging.
  - **Status: DONE (2026-06-21).** Divider wired (100 kΩ pair → A0/GPIO0);
    firmware reports `batt_v = analogReadMilliVolts(0) * 2` (8-sample avg) in the
    payload. Full pipeline support added (telemetry `batt_v` column, writer, api,
    and a **battery (V)** chart on the dashboard) and deployed to the server. First
    readings ~3.66–3.79 V. ⚠️ Still on the `3V3` pin, slightly over the ~3.6 V
    abs-max — fine in practice but a full 4.2 V pack would push it further; move
    to `5V` or keep the pack ≤3.6 V if it ever misbehaves.

## C6 features worth pursuing (ranked by how distinctive vs the classic ESP32 / our C3)

### 1. 802.15.4 radio → Thread / Zigbee / Matter  ← the headline
Neither the classic ESP32 nor our C3 has this radio. Options:
- **Matter-native sensor**: expose the node to Apple/Google/Alexa Home directly
  (over Wi-Fi or Thread), no app/cloud. Arduino-ESP32 3.x has Matter examples.
- **Zigbee end device**: join Home Assistant ZHA / Zigbee2MQTT. Arduino 3.x has
  Zigbee examples.
- **Border-router / gateway**: the C6 has *both* radios, so one C6 can bridge an
  802.15.4 mesh to Wi-Fi. Espressif ships a Thread Border Router reference on the
  C6 → run one C6 as a BR and funnel a swarm of cheap coin-cell Thread/Zigbee
  sensors into our existing **MQTT→Kafka→Timescale** pipeline. Best long-term
  extension of sensor-lab (mesh range + sub-mA sensors).

### 2. Wi-Fi 6 (802.11ax) + Target Wake Time (TWT)
C6 is Wi-Fi 6; the C3 is Wi-Fi 4. **TWT** lets the node negotiate a sleep
schedule and **stay associated**, skipping the reconnect/auth/DHCP cost we pay
every wake (our BSSID cache only mitigates it). Worth benchmarking TWT-stay-
connected vs. the current deep-sleep-and-reconnect loop. Needs a **Wi-Fi 6 router
with TWT enabled**; mostly an ESP-IDF API today.

### 3. Low-power RISC-V core + LP peripherals
The C6 has a second **LP core (~20 MHz)** with **LP_I2C / LP_UART / LP ADC** that
runs while the main chip sleeps — and it's **C-programmable** (unlike the old ULP
FSM). Use case: sample a sensor on the LP core during deep sleep, only wake the
HP core + Wi-Fi to publish when a reading crosses a threshold → **event-driven
telemetry**, far fewer transmits. Pairs best with an **I2C/analog** sensor (e.g.
the AHT21 air nodes via LP_I2C), *not* the bit-banged DHT11.

### 4. External antenna (range)
Onboard ceramic + U.FL connector. `GPIO14` low = internal (default), high =
external; the RF switch needs `GPIO3` pulled low to activate. Solder a U.FL whip
→ ~80 m range for a far-flung battery node.

### 5. BLE 5.3
vs the C3's 5.0 — coded-PHY long range; fine for BLE provisioning / beacons.

## Tooling note
Current node is plain Arduino (`.ino`, esp32 core 3.3.8 already installed).
- **Approachable in Arduino 3.x:** `batt_v` reporting, Zigbee, Matter (examples exist).
- **ESP-IDF territory:** TWT and LP-core programming → would mean porting from the
  `.ino` to an IDF project.

## Suggested order
1. ~~**`batt_v` in the payload**~~ — **DONE (2026-06-21):** wired, in the payload,
   charted on the dashboard. The longevity curve now records from here on.
2. **Matter sensor** or **C6-as-border-router → MQTT** bridge — biggest "wow",
   stays mostly in Arduino / Espressif reference code.
3. **LP-core threshold wake** with an I2C sensor on an air node — max battery, but
   commits to ESP-IDF.
