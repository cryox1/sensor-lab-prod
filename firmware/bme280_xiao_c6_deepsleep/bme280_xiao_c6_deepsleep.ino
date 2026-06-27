// Headless deep-sleep BME280 node on Seeed XIAO ESP32-C6 (device BME280_xiaoc6).
//
// Battery-first build: wake -> one-shot BME280 read -> publish one MQTT message
// -> deep sleep. No display, no LED, no button. Mirrors dht11_xiao_c6_deepsleep
// but reads a GY-BME280 over I2C and adds atmospheric pressure.
//
// The C6 is an ESP32 (RISC-V): it wakes itself on the RTC timer, so there is
// **NO D0->RST wire** to add.
//
// FLASHING GOTCHA: after an esptool/arduino-cli flash, this board's native
// USB-serial-JTAG often stays in ROM *download mode* (esptool's RTS hard-reset
// can't boot the app here; the host holds the boot strap). Symptom: port stays
// enumerated, zero serial, never publishes. Fix: physically POWER-CYCLE
// (unplug/replug USB) once -- on a clean power-on GPIO9's pull-up boots the app,
// and from then on every deep-sleep timer wake resets cleanly on its own.
//
// Board package: esp32 (Arduino-ESP32 core >= 3.0.0). Board: "XIAO_ESP32C6",
// "USB CDC On Boot" enabled so Serial prints over the native USB port.
// Libraries: PubSubClient only -- the BME280 driver is self-contained (raw Wire
// + Bosch datasheet compensation), so no Adafruit_BME280 dependency.
//
// Wiring: BME280 VCC -> 3V3, GND -> GND, SDA -> D5 (GPIO23), SCL -> D6 (GPIO16).
// NOTE: this is the swapped orientation of the labels -- on this build the wires
// were left as SDA=D5/SCL=D6. initSensor() tries both orderings, so re-swapping
// the wires to SDA=D6/SCL=D5 also works with no code change.
//
// Optimizations: static IP + AP BSSID/channel cached in RTC memory for a
// scanless reconnect; NTP only on cold boot and every NTP_RESYNC_WAKES wakes,
// clock carried forward in RTC memory in between.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>
#include <esp_sleep.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C / BME280 ----
// Primary orientation (current wiring). initSensor() falls back to the swap.
#define I2C_SDA        D5   // GPIO23
#define I2C_SCL        D6   // GPIO16

// ---- Battery voltage sense ----
// Enabled: a 1:2 divider (two equal ~100k from the 3V3/battery rail to A0/GPIO0)
// is wired on this board, so we publish "batt_v". analogReadMilliVolts() is
// Vref-calibrated; the *2.0 divider factor restores the rail voltage.
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
// Published (retained) to a non-telemetry topic the bridge ignores, so it never
// reaches Kafka/DB. Watch with: mosquitto_sub -t 'sensors/lab/BME280_xiaoc6/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 61);   // free address on your LAN
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "BME280_xiaoc6"
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

// ================== self-contained BME280 driver ==================
uint8_t bmeAddr = 0x00;
bool    hasHumidity = false;
uint16_t dig_T1; int16_t dig_T2, dig_T3;
uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
uint8_t  dig_H1, dig_H3; int16_t dig_H2, dig_H4, dig_H5; int8_t dig_H6;
int32_t  t_fine;

static uint8_t read8(uint8_t reg) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom((int)bmeAddr, 1); return Wire.read();
}
static uint16_t read16LE(uint8_t reg) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom((int)bmeAddr, 2);
  uint8_t lo = Wire.read(), hi = Wire.read();
  return (uint16_t)((hi << 8) | lo);
}
static void write8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

static void readCalibration() {
  dig_T1 = read16LE(0x88); dig_T2 = (int16_t)read16LE(0x8A); dig_T3 = (int16_t)read16LE(0x8C);
  dig_P1 = read16LE(0x8E);
  dig_P2 = (int16_t)read16LE(0x90); dig_P3 = (int16_t)read16LE(0x92);
  dig_P4 = (int16_t)read16LE(0x94); dig_P5 = (int16_t)read16LE(0x96);
  dig_P6 = (int16_t)read16LE(0x98); dig_P7 = (int16_t)read16LE(0x9A);
  dig_P8 = (int16_t)read16LE(0x9C); dig_P9 = (int16_t)read16LE(0x9E);
  if (hasHumidity) {
    dig_H1 = read8(0xA1);
    dig_H2 = (int16_t)read16LE(0xE1);
    dig_H3 = read8(0xE3);
    uint8_t e4 = read8(0xE4), e5 = read8(0xE5), e6 = read8(0xE6);
    dig_H4 = (int16_t)((e4 << 4) | (e5 & 0x0F));
    dig_H5 = (int16_t)((e6 << 4) | (e5 >> 4));
    dig_H6 = (int8_t)read8(0xE7);
  }
}

// Bosch fixed-point compensation (datasheet reference code).
static int32_t compensateT(int32_t adc_T) {
  int32_t v1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
  int32_t v2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
                ((int32_t)dig_T3)) >> 14;
  t_fine = v1 + v2;
  return (t_fine * 5 + 128) >> 8;          // 0.01 degC
}
static uint32_t compensateP(int32_t adc_P) {  // Q24.8 -> Pa = /256
  int64_t v1 = ((int64_t)t_fine) - 128000;
  int64_t v2 = v1 * v1 * (int64_t)dig_P6;
  v2 = v2 + ((v1 * (int64_t)dig_P5) << 17);
  v2 = v2 + (((int64_t)dig_P4) << 35);
  v1 = ((v1 * v1 * (int64_t)dig_P3) >> 8) + ((v1 * (int64_t)dig_P2) << 12);
  v1 = (((((int64_t)1) << 47) + v1)) * ((int64_t)dig_P1) >> 33;
  if (v1 == 0) return 0;
  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - v2) * 3125) / v1;
  v1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
  v2 = (((int64_t)dig_P8) * p) >> 19;
  p = ((p + v1 + v2) >> 8) + (((int64_t)dig_P7) << 4);
  return (uint32_t)p;
}
static uint32_t compensateH(int32_t adc_H) {  // Q22.10 -> %RH = /1024
  int32_t v = (t_fine - ((int32_t)76800));
  v = (((((adc_H << 14) - (((int32_t)dig_H4) << 20) - (((int32_t)dig_H5) * v)) + ((int32_t)16384)) >> 15) *
       (((((((v * ((int32_t)dig_H6)) >> 10) * (((v * ((int32_t)dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
          ((int32_t)2097152)) * ((int32_t)dig_H2) + 8192) >> 14));
  v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)dig_H1)) >> 4);
  v = v < 0 ? 0 : v;
  v = v > 419430400 ? 419430400 : v;
  return (uint32_t)(v >> 12);
}

static bool probe(uint8_t a) {
  bmeAddr = a;
  uint8_t id = read8(0xD0);
  if (id == 0x60 || id == 0x58 || id == 0x56 || id == 0x57) {
    hasHumidity = (id == 0x60);
    readCalibration();
    if (hasHumidity) write8(0xF2, 0x01);   // ctrl_hum: humidity x1 (before ctrl_meas)
    write8(0xF5, 0x00);                      // config: filter off, min standby
    Serial.printf("BME/BMP280 @ 0x%02X id=0x%02X (%s)\n", a, id,
      hasHumidity ? "BME280: T+P+H" : "BMP280: T+P");
    return true;
  }
  return false;
}

// Try the wired orientation, then the swapped one, at both I2C addresses.
bool initSensor() {
  Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(100000);
  if (probe(0x76) || probe(0x77)) return true;
  Wire.end();
  Wire.begin(I2C_SCL, I2C_SDA); Wire.setClock(100000);  // swapped
  if (probe(0x76) || probe(0x77)) { Serial.println(F("(SDA/SCL swapped)")); return true; }
  bmeAddr = 0x00;
  return false;
}

// One forced-mode measurement. Returns false if the sensor never reports ready.
bool readBme(float *tC, float *hPa, float *rh) {
  write8(0xF4, 0x25);                        // ctrl_meas: T x1, P x1, FORCED
  unsigned long start = millis();
  while ((read8(0xF3) & 0x08) && millis() - start < 100) delay(2);  // wait "measuring"
  uint8_t d[8];
  Wire.beginTransmission(bmeAddr); Wire.write(0xF7); Wire.endTransmission();
  Wire.requestFrom((int)bmeAddr, 8);
  for (int i = 0; i < 8; i++) d[i] = Wire.read();

  int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  int32_t adc_H = ((int32_t)d[6] << 8) | d[7];
  if (adc_T == 0x80000 || adc_P == 0x80000) return false;  // skipped/no data

  *tC  = compensateT(adc_T) / 100.0f;        // must run first: sets t_fine
  *hPa = compensateP(adc_P) / 256.0f / 100.0f;
  *rh  = hasHumidity ? (compensateH(adc_H) / 1024.0f) : NAN;
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
  float t = NAN, p = NAN, h = NAN;
  if (sensorOk) {
    if (!readBme(&t, &p, &h)) Serial.println(F("BME280 read failed"));
  } else {
    Serial.println(F("No BME/BMP280 found (check SDA=D5, SCL=D6, 3V3, GND)"));
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
    char payload[224];
    int n = snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"ts\":%lu,\"temp_c\":%.2f,\"pressure_hpa\":%.2f",
      DEVICE_ID, (unsigned long)now, t, p);
    if (!isnan(h)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"humidity\":%.1f,\"heat_index_c\":%.1f", h, heatIndexC(t, h));
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
