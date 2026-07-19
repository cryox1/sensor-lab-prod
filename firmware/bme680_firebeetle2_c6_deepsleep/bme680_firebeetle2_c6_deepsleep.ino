// Headless deep-sleep BME680 node on DFRobot FireBeetle 2 ESP32-C6 / DFR1075
// (device BME680_fbc6). Sibling of bme280_firebeetle2_c6_deepsleep -- the two
// boards run the same skeleton so the BME280 vs BME680 readings are comparable.
//
// Battery-first build: wake -> one-shot BME680 read -> publish one MQTT message
// -> deep sleep. No display, no LED, no button. On top of temp/humidity/pressure
// the BME680 adds a heated MOX gas sensor: we publish its raw resistance as
// "gas_kohm" (higher = cleaner air). NOTE on gas readings at this cadence: the
// sensor needs burn-in, and one forced 320C/150ms heater shot per 5-min wake
// never reaches the steady state a continuously-run BME680 would -- treat
// gas_kohm as a relative trend, not an absolute IAQ. (Bosch's BSEC IAQ library
// is a closed blob without C6/RISC-V support, so raw resistance is what we get.)
// Self-heating from the single forced-mode heater shot is negligible for temp.
//
// The C6 is an ESP32 (RISC-V): it wakes itself on the RTC timer, so there is
// **NO D0->RST wire** to add.
//
// FLASHING GOTCHA: after an esptool/arduino-cli flash, a C6's native
// USB-serial-JTAG often stays in ROM *download mode* (esptool's RTS hard-reset
// can't boot the app here; the host holds the boot strap). Symptom: port stays
// enumerated, zero serial, never publishes. Fix: physically POWER-CYCLE
// (unplug/replug USB) once -- on a clean power-on GPIO9's pull-up boots the app,
// and from then on every deep-sleep timer wake resets cleanly on its own.
//
// Board package: esp32 (Arduino-ESP32 core >= 3.0.0). Board: "DFRobot FireBeetle
// 2 ESP32-C6" (FQBN esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc).
// NOTE: unlike the XIAO, "USB CDC On Boot" defaults to DISABLED on this board --
// enable it (IDE menu / :CDCOnBoot=cdc) or Serial stays silent on the USB port.
// Libraries: PubSubClient, Adafruit_BME680 (+ its deps Adafruit BusIO and
// Adafruit Unified Sensor).
//
// Wiring: the sensor is a DFRobot Gravity BME680 (SEN0248) -- its Gravity cable
// goes to the breadboard: + -> 3V3, - -> GND, C (clock) -> GPIO20 (SCL),
// D (data) -> GPIO19 (SDA). Those are the board's silkscreened SDA/SCL pins.
// initSensor() also tries the swapped orientation and both I2C addresses
// (DFRobot default 0x77, alternate 0x76), so miswired C/D still comes up.
// Don't use the variant's D* aliases here -- on this board D6 is GPIO1 etc.
//
// Battery: the FireBeetle 2 C6 has an ONBOARD divider (2x) from VBAT to GPIO0,
// so batt_v needs no external parts (DFRobot wiki: analogReadMilliVolts*2).
//
// Optimizations: static IP + AP BSSID/channel cached in RTC memory for a
// scanless reconnect; NTP only on cold boot and every NTP_RESYNC_WAKES wakes,
// clock carried forward in RTC memory in between.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <time.h>
#include <math.h>
#include <esp_sleep.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C / BME680 ----
// The board's default I2C pins (silkscreen SDA/SCL). initSensor() falls back
// to the swapped orientation automatically.
#define I2C_SDA        19
#define I2C_SCL        20

// ---- Battery voltage sense ----
// The FireBeetle 2 C6 has an onboard 1:2 divider from VBAT to GPIO0, so this
// works with no external parts. analogReadMilliVolts() is Vref-calibrated; the
// *2.0 divider factor restores the battery voltage (per the DFRobot wiki).
#define ENABLE_BATTERY 1
#define BATT_ADC_PIN   0
#define BATT_DIVIDER   2.0f
#define BATT_SAMPLES   8

// ---- Cadence ----
#define SLEEP_SECONDS      300
#define NTP_RESYNC_WAKES   24
#define WIFI_SCANLESS_MS   5000    // cached-BSSID reconnect: fast or not at all
#define WIFI_FULLSCAN_MS   12000   // full-scan fallback
#define NTP_TIMEOUT_MS     8000
#define MQTT_RETRIES       6       // retry the connect+publish: a single -2 TCP
                                   // transient after a fresh assoc loses the whole cycle.
                                   // Paired with primePath() + escalating backoff below.
#define MQTT_SOCK_TIMEOUT  4       // s; fail a dropped SYN fast so retries fit the budget
// NOTE: no task watchdog. All blocking operations below (WiFi/NTP/MQTT/I2C) are
// individually timeout-bounded, so setup() always reaches goToSleep(). An earlier
// esp_task_wdt guard ran before Serial.begin() and boot-looped the C6 (USB-JTAG
// persists across resets, so it looked like a silent hang) -- removed.

// ---- Diagnostics ----
// Published (retained) to a non-telemetry topic the ingest ignores, so it never
// reaches the DB. Watch with: mosquitto_sub -t 'sensors/lab/BME680_fbc6/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 69);   // free address on your LAN
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "BME680_fbc6"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Persisted across deep sleep (ESP32 RTC memory) ----
RTC_DATA_ATTR uint8_t  rtcValid     = 0;
RTC_DATA_ATTR uint32_t rtcEpoch     = 0;
RTC_DATA_ATTR uint32_t rtcWakeCount = 0;   // wakes since last NTP sync
RTC_DATA_ATTR uint8_t  rtcChannel   = 0;
RTC_DATA_ATTR uint8_t  rtcBssidOk   = 0;
RTC_DATA_ATTR uint8_t  rtcBssid[6]  = {0};

// Diagnostics carried across sleep so a failed (no-publish) cycle is still
// reported on the next successful one.
RTC_DATA_ATTR uint32_t rtcTotalWakes  = 0;  // every wake (incl. cold boot)
RTC_DATA_ATTR uint32_t rtcPublishes   = 0;  // successful telemetry publishes
RTC_DATA_ATTR uint32_t rtcConsecFail  = 0;  // consecutive cycles with no publish
RTC_DATA_ATTR uint16_t rtcPrevAwakeMs = 0;  // awake duration of previous cycle (clock drift comp)
RTC_DATA_ATTR uint8_t  rtcPrevResult  = 0;  // 0=cold 1=ok 2=wifi_fail 3=mqtt_fail 4=no_data

// Per-cycle context for the debug payload (set during this wake).
int          g_wakeCause = 0;
const char  *g_wifiMode  = "none";   // scanless | fullscan | fail
unsigned long g_wifiMs   = 0;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ================== BME680 (Adafruit_BME680, forced mode) ==================
Adafruit_BME680 bme;   // I2C on Wire

static bool probe680(uint8_t addr) {
  if (!bme.begin(addr)) return false;
  // One-shot-friendly settings: modest oversampling, no IIR filter (each wake
  // is a fresh forced conversion; filtering across wakes is meaningless).
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_0);
  bme.setGasHeater(320, 150);   // 320 C for 150 ms (Bosch/Adafruit default)
  Serial.printf("BME680 @ 0x%02X\n", addr);
  return true;
}

// Try the wired orientation, then the swapped one, at both I2C addresses
// (0x77 = DFRobot SEN0248 default, 0x76 = alternate).
bool initSensor() {
  Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(100000);
  if (probe680(0x77) || probe680(0x76)) return true;
  Wire.end();
  Wire.begin(I2C_SCL, I2C_SDA); Wire.setClock(100000);  // swapped
  if (probe680(0x77) || probe680(0x76)) { Serial.println(F("(SDA/SCL swapped)")); return true; }
  return false;
}

// One forced-mode measurement (blocks ~200 ms incl. the gas heater shot).
bool readBme680(float *tC, float *hPa, float *rh, float *gasKohm) {
  if (!bme.performReading()) return false;
  *tC      = bme.temperature;                 // degC
  *hPa     = bme.pressure / 100.0f;           // Pa -> hPa
  *rh      = bme.humidity;                    // %RH
  *gasKohm = bme.gas_resistance / 1000.0f;    // ohm -> kOhm
  return true;
}
// ================================================================

// Heat index (Rothfusz regression), Celsius in/out -- matches the Adafruit DHT
// computeHeatIndex used by the other climate nodes, for cross-node consistency.
float heatIndexC(float tc, float h) {
  float t = tc * 1.8f + 32.0f;               // -> Fahrenheit
  float hi = 0.5f * (t + 61.0f + ((t - 68.0f) * 1.2f) + (h * 0.094f));
  if (hi > 79.0f) {
    hi = -42.379f + 2.04901523f * t + 10.14333127f * h
       + -0.22475541f * t * h + -0.00683783f * t * t
       + -0.05481717f * h * h + 0.00122874f * t * t * h
       + 0.00085282f * t * h * h + -0.00000199f * t * t * h * h;
    if (h < 13.0f && t >= 80.0f && t <= 112.0f)
      hi -= ((13.0f - h) * 0.25f) * sqrtf((17.0f - fabsf(t - 95.0f)) * 0.05882f);
    else if (h > 85.0f && t >= 80.0f && t <= 87.0f)
      hi += ((h - 85.0f) * 0.1f) * ((87.0f - t) * 0.2f);
  }
  return (hi - 32.0f) * 0.55555f;            // -> Celsius
}

bool tryConnect(bool scanless, unsigned long timeoutMs) {
  if (scanless) {
    WiFi.begin(WIFI_SSID, WIFI_PASS, rtcChannel, rtcBssid, true);  // cached BSSID/channel
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);                              // full scan
  }
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(50);
  return WiFi.status() == WL_CONNECTED;
}

// One throwaway UDP datagram to the gateway. After a fresh (especially scanless)
// assoc the AP hasn't yet added us to its forwarding/bridge table and the gateway
// ARP isn't resolved, so the very first MQTT TCP SYN is silently dropped -> the
// state=-2 / prev_res=3 misses. Sending an outbound packet first forces the AP to
// learn the station and primes the ARP, so the SYN that follows actually lands.
void primePath() {
  WiFiUDP u;
  u.begin(0);
  u.beginPacket(WiFi.gatewayIP(), 9);   // port 9 = discard; nothing need listen
  uint8_t z = 0;
  u.write(&z, 1);
  u.endPacket();
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // no modem sleep during our brief active window: the
                          // first TCP packets after assoc are otherwise dropped,
                          // which is what produces the MQTT state=-2 transient.
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS, STATIC_DNS2);
#endif
  unsigned long t0 = millis();
  // Fast path: scanless reconnect to the cached AP. If that fails (AP moved
  // channel, BSSID stale after sleep, etc.) fall back to a full scan in the
  // SAME wake -- otherwise a single bad reconnect costs a whole publish cycle.
  bool haveCache = rtcValid && rtcBssidOk;
  bool ok = haveCache && tryConnect(true, WIFI_SCANLESS_MS);
  g_wifiMode = (ok && haveCache) ? "scanless" : "fullscan";
  if (!ok) {
    if (haveCache) { Serial.println(F("scanless failed, full-scan retry")); WiFi.disconnect(true); delay(10); }
    ok = tryConnect(false, WIFI_FULLSCAN_MS);
  }
  g_wifiMs = millis() - t0;
  if (ok) {
    rtcChannel = WiFi.channel();
    memcpy(rtcBssid, WiFi.BSSID(), 6);
    rtcBssidOk = 1;
    delay(250);   // let the AP finish adding the station / ARP settle before TCP
    primePath();  // and force the forwarding table / ARP so the first SYN lands
    Serial.printf("WiFi UP (%s): ip=%s rssi=%ddBm ch=%d in %lums\n",
      g_wifiMode, WiFi.localIP().toString().c_str(), WiFi.RSSI(), rtcChannel, g_wifiMs);
    return true;
  }
  rtcBssidOk = 0;
  g_wifiMode = "fail";
  Serial.println(F("WiFi FAILED"));
  return false;
}

// Connect + publish telemetry with retry, then publish a retained debug record
// on the same session. Returns the attempt number that succeeded (1..N), or 0.
int publishWithRetry(const char *payload) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(MQTT_SOCK_TIMEOUT);
  for (int attempt = 1; attempt <= MQTT_RETRIES; attempt++) {
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      bool ok = mqtt.publish(MQTT_TOPIC, payload);
      Serial.printf("publish %s (try %d): %s\n", ok ? "ok" : "FAIL", attempt, payload);
      if (ok) {
        char dbg[208];
        uint32_t pub = rtcPublishes + 1;  // not yet committed by caller
        snprintf(dbg, sizeof(dbg),
          "{\"wake\":%lu,\"cause\":%d,\"wifi\":\"%s\",\"wifi_ms\":%lu,\"mqtt_try\":%d,"
          "\"pub\":%lu,\"miss\":%lu,\"consec_fail\":%lu,\"prev_res\":%d,\"awake_ms\":%lu}",
          (unsigned long)rtcTotalWakes, g_wakeCause, g_wifiMode, g_wifiMs, attempt,
          (unsigned long)pub, (unsigned long)(rtcTotalWakes - pub),
          (unsigned long)rtcConsecFail, rtcPrevResult, (unsigned long)millis());
        mqtt.publish(DEBUG_TOPIC, dbg, true);   // retained
      }
      mqtt.loop();
      mqtt.disconnect();
      if (ok) return attempt;
    } else {
      Serial.printf("MQTT connect attempt %d failed, state=%d\n", attempt, mqtt.state());
      primePath();   // re-nudge the path before the next attempt
    }
    delay(200u * attempt);   // escalating backoff: 200,400,...,1200 ms
  }
  return 0;
}

#if ENABLE_BATTERY
float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) { mv += analogReadMilliVolts(BATT_ADC_PIN); delay(2); }
  return (mv / (float)BATT_SAMPLES) * BATT_DIVIDER / 1000.0f;
}
#endif

bool syncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000UL && millis() - start < NTP_TIMEOUT_MS) delay(100);
  return time(nullptr) >= 1700000000UL;
}

void goToSleep() {
  unsigned long awake = millis();
  rtcPrevAwakeMs = awake > 65000 ? 65000 : (uint16_t)awake;  // for next cycle's clock comp
  Serial.printf("awake %lums, sleeping %ds\n", awake, SLEEP_SECONDS);
  Serial.flush();
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();   // never returns; full reset on wake
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 800) delay(10);   // don't burn the budget on USB

  rtcTotalWakes++;
  g_wakeCause = (int)esp_sleep_get_wakeup_cause();   // 4 == TIMER
  Serial.printf("\n=== %s wake #%lu cause=%d (prev_res=%d consec_fail=%lu) ===\n",
    DEVICE_ID, (unsigned long)rtcTotalWakes, g_wakeCause, rtcPrevResult,
    (unsigned long)rtcConsecFail);

  bool sensorOk = initSensor();
  float t = NAN, p = NAN, h = NAN, gas = NAN;
  if (sensorOk) {
    if (!readBme680(&t, &p, &h, &gas)) Serial.println(F("BME680 read failed"));
  } else {
    Serial.println(F("No BME680 found (check D->GPIO19/SDA, C->GPIO20/SCL, 3V3, GND)"));
  }

  bool needNtp = !rtcValid || rtcEpoch < 1700000000UL || rtcWakeCount >= NTP_RESYNC_WAKES;
  bool wifiUp = connectWifi();

  time_t now = 0;
  if (wifiUp && needNtp && syncNtp()) {
    now = time(nullptr);
    rtcEpoch = (uint32_t)now;
    rtcWakeCount = 0;
  } else if (rtcValid && rtcEpoch >= 1700000000UL) {
    // Advance the simulated clock by the sleep interval PLUS the previous cycle's
    // awake time (otherwise reported ts steadily lags real time).
    now = (time_t)rtcEpoch + SLEEP_SECONDS + (rtcPrevAwakeMs / 1000);
    rtcEpoch = (uint32_t)now;
    rtcWakeCount++;
  }
  rtcValid = 1;

  uint8_t result;
  if (isnan(t)) {
    result = 4; Serial.println(F("no temperature, skipping publish"));
  } else if (now < 1700000000UL) {
    result = 4; Serial.println(F("no valid time yet, skipping publish"));
  } else if (!wifiUp) {
    result = 2;   // WiFi never came up this cycle
  } else {
    char payload[256];
    int n = snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"ts\":%lu,\"temp_c\":%.2f,\"pressure_hpa\":%.2f",
      DEVICE_ID, (unsigned long)now, t, p);
    if (!isnan(h)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"humidity\":%.1f,\"heat_index_c\":%.1f", h, heatIndexC(t, h));
    }
    if (!isnan(gas) && gas > 0.0f) {
      n += snprintf(payload + n, sizeof(payload) - n, ",\"gas_kohm\":%.1f", gas);
    }
#if ENABLE_BATTERY
    n += snprintf(payload + n, sizeof(payload) - n, ",\"batt_v\":%.2f", readBatteryVolts());
#endif
    snprintf(payload + n, sizeof(payload) - n, "}");

    result = publishWithRetry(payload) > 0 ? 1 : 3;
  }

  // Commit diagnostics for the next cycle's debug record.
  if (result == 1) { rtcPublishes++; rtcConsecFail = 0; }
  else             { rtcConsecFail++; }
  rtcPrevResult = result;

  goToSleep();
}

void loop() {}
