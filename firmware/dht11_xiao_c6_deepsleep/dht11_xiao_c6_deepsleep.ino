// Headless deep-sleep DHT11 node on Seeed XIAO ESP32-C6 (device xiao_c6).
//
// Battery-first build: wake -> read DHT11 -> publish one MQTT message -> deep
// sleep (~15 uA per the XIAO C6 datasheet). No display, no LED, no button.
//
// The C6 is an ESP32 (RISC-V): it wakes itself on the RTC timer, so -- unlike
// the ESP8266 boards in this repo -- there is **NO D0->RST wire** to add.
//
// FLASHING GOTCHA: after an esptool/arduino-cli flash, this board's native
// USB-serial-JTAG often stays in ROM *download mode* (esptool's RTS hard-reset
// can't boot the app here; the host holds the boot strap). Symptom: port stays
// enumerated, zero serial, never publishes. Fix: physically POWER-CYCLE
// (unplug/replug USB) once -- on a clean power-on GPIO9's pull-up boots the app,
// and from then on every deep-sleep timer wake resets cleanly on its own.
//
// Board package: esp32 (Arduino-ESP32 core >= 3.0.0). Board: "XIAO_ESP32C6",
// enable "USB CDC On Boot" so Serial prints over the native USB port.
// Libraries: DHT sensor library (Adafruit) + Adafruit Unified Sensor, PubSubClient.
//
// Wiring: DHT11 VCC -> 3V3, GND -> GND, DATA -> D10 (GPIO18). A 4.7-10k pull-up
// on DATA is needed for a bare 3-pin sensor (most blue DHT11 modules include it).
//
// Battery voltage: the BAT pads are damaged, so the LiPo feeds the 3V3 pin
// directly (rail voltage == battery voltage). A 1:2 divider of two equal ~100k
// resistors (3V3 -> A0 -> GND) halves it into the ADC's range; we read it back
// with batt_v = analogReadMilliVolts(A0) * 2 and report it as "batt_v".
//
// Reliability: scanless reconnect via RTC-cached BSSID/channel with a full-scan
// fallback in the same wake; modem-sleep off + a short settle so the first TCP
// packets aren't dropped; MQTT connect+publish retried; NTP only on cold boot
// and every NTP_RESYNC_WAKES wakes (clock carried forward, awake-time
// compensated). Self-diagnostics are published retained to a /debug topic.
//
// Expected life on a 2000 mAh LiPo: ~6-9 weeks @ 5 min, ~4-6 months @ 15 min.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <time.h>
#include <esp_sleep.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- DHT11 ----
#define DHT_PIN        18   // D10 (GPIO18 / MOSI) on XIAO ESP32-C6
#define DHT_TYPE       DHT11

// ---- Battery voltage sense ----
// A0 = GPIO0 (ADC1_CH0). 1:2 divider from the 3V3/battery rail; on the C6 the
// boot strap is GPIO9, so GPIO0 is free. analogReadMilliVolts() is calibrated
// against the chip's internal Vref, so the reading stays accurate as the rail sags.
#define BATT_ADC_PIN   0
#define BATT_DIVIDER   2.0f   // two equal resistors -> ADC sees half the rail
#define BATT_SAMPLES   8

// ---- Cadence ----
#define SLEEP_SECONDS      300
#define NTP_RESYNC_WAKES   24
#define WIFI_SCANLESS_MS   5000    // cached-BSSID reconnect: fast or not at all
#define WIFI_FULLSCAN_MS   12000   // full-scan fallback in the same wake
#define NTP_TIMEOUT_MS     8000
#define MQTT_RETRIES       6       // retry connect+publish: a single -2 TCP transient
                                   // right after a fresh assoc otherwise loses the cycle.
                                   // Paired with primePath() + escalating backoff below.
#define MQTT_SOCK_TIMEOUT  4       // s; fail a dropped SYN fast so retries fit the budget
// No task watchdog: every blocking op below (WiFi/NTP/MQTT/DHT) is timeout-bounded,
// so setup() always reaches goToSleep().

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 63);
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "xiao_c6"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Diagnostics ----
// Published (retained) to a non-telemetry topic the bridge ignores, so it never
// reaches Kafka/DB. Watch with: mosquitto_sub -t 'sensors/lab/xiao_c6/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

// ---- Persisted across deep sleep (ESP32 RTC memory) ----
// RTC_DATA_ATTR keeps its static initializer on a cold/power-on boot and retains
// its value across deep sleep, so `valid` doubles as the cold-boot flag.
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
int           g_wakeCause = 0;
const char   *g_wifiMode  = "none";   // scanless | fullscan | fail
unsigned long g_wifiMs    = 0;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

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

float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) {
    mv += analogReadMilliVolts(BATT_ADC_PIN);
    delay(2);
  }
  return (mv / (float)BATT_SAMPLES) * BATT_DIVIDER / 1000.0f;  // mV at pin -> V at rail
}

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
  esp_deep_sleep_start();   // never returns; full reset on wake (no D0->RST wire)
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

  dht.begin();
  float t = dht.readTemperature();
  float h = dht.readHumidity();

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
  if (isnan(t) || isnan(h)) {
    result = 4; Serial.println(F("DHT read failed (check wiring on D10/GPIO18)"));
  } else if (now < 1700000000UL) {
    result = 4; Serial.println(F("no valid time yet, skipping publish"));
  } else if (!wifiUp) {
    result = 2;   // WiFi never came up this cycle
  } else {
    float hi = dht.computeHeatIndex(t, h, false);
    float battV = readBatteryVolts();
    char payload[192];
    snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"ts\":%lu,\"temp_c\":%.1f,\"humidity\":%.0f,\"heat_index_c\":%.1f,\"batt_v\":%.2f}",
      DEVICE_ID, (unsigned long)now, t, h, hi, battV);
    result = publishWithRetry(payload) > 0 ? 1 : 3;
  }

  // Commit diagnostics for the next cycle's debug record.
  if (result == 1) { rtcPublishes++; rtcConsecFail = 0; }
  else             { rtcConsecFail++; }
  rtcPrevResult = result;

  goToSleep();
}

void loop() {}
