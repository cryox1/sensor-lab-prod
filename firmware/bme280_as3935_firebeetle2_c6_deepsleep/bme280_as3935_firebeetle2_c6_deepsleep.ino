// Storm node on DFRobot FireBeetle 2 ESP32-C6 / DFR1075 (device storm01):
// DFRobot Gravity AS3935 lightning sensor (SEN0290) + optional GY-BME280
// climate (tolerated absent), deep-sleep with TWO wake sources:
//   - RTC timer (SLEEP_SECONDS): heartbeat -> read BME280 (if fitted),
//     publish climate + batt_v + lightning_count.
//   - GPIO wake from the AS3935 IRQ pin: a lightning strike (the sensor
//     detects autonomously at ~uA while the ESP sleeps) -> publish the event.
//
// POWER: current build is INDOOR (AS3935_LOCATION 0) on USB or a LiPo at the
// FireBeetle's BAT connector. The original solar option remains valid:
// solar panel -> DFR0579 Solar Power Manager Micro -> LiPo -> BAT connector.
// The DFR0579 is only the charger; WiFi TX peaks (200-350 mA) come from the
// battery via the FireBeetle's regulator -- the DFR0579's 3.3V/90mA output
// must NOT power the ESP. On a solar budget also remove/cut the SEN0290's
// red power LED (1-2 mA continuous); indoors on USB it doesn't matter.
//
// LIGHTNING SEMANTICS: the AS3935 stays powered through ESP deep sleep and
// keeps its configuration + statistics; the ESP configures it ONCE on cold
// boot. Strikes during a storm come in bursts, so publishes are rate-limited:
// after a published strike, further strikes within LIGHTNING_COOLDOWN_S are
// only accumulated in RTC memory (count / min distance / max energy) at ~ms
// wake cost and get folded into the next publish. Disturber/noise IRQs never
// publish; if disturbers flood (>DISTURB_LIMIT since the last publish) the
// disturber IRQ is masked until the next cold boot. NOTE: the AS3935 reports
// the distance to the STORM FRONT, not to the individual strike.
// lightning_count is published on EVERY publish (0 when quiet) so the stack
// has a live 0-baseline; lightning_km/lightning_energy only with strikes.
//
// FLASHING GOTCHA: after an esptool/arduino-cli flash, a C6's native
// USB-serial-JTAG often stays in ROM *download mode*. Symptom: port stays
// enumerated, zero serial, never publishes. Fix: POWER-CYCLE once. The port
// disappearing right after boot is the GOOD sign (deep sleep, USB off).
//
// Board package: esp32 (Arduino-ESP32 core >= 3.0.0). Board: "DFRobot FireBeetle
// 2 ESP32-C6" (FQBN esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc).
// "USB CDC On Boot" defaults to DISABLED on this board -- enable it or Serial
// stays silent. Libraries: PubSubClient + DFRobot_AS3935 (the BME280 driver
// is self-contained raw-Wire below).
//
// Wiring: shared I2C bus on the silkscreened defaults -- SEN0290 VCC->3V3,
// GND->GND, SDA->GPIO19, SCL->GPIO20, address DIP switches set to 0x03
// (both ON), IRQ -> GPIO2 (must be an LP GPIO 0..7 to wake the C6 from deep
// sleep; GPIO0 is taken by the onboard VBAT divider). Optional BME280 on the
// same bus (sketch auto-tries swapped SDA/SCL and runs fine without it).
// batt_v uses the onboard 1:2 divider on GPIO0 -- no external parts.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <DFRobot_AS3935_I2C.h>
#include <time.h>
#include <math.h>
#include <esp_sleep.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C (board default silkscreen SDA/SCL) ----
#define I2C_SDA        19
#define I2C_SCL        20

// ---- AS3935 lightning sensor (SEN0290) ----
#define AS3935_IRQ_PIN      2     // LP GPIO 0..7 required for deep-sleep wake
#define AS3935_I2C_ADDR     AS3935_ADD3   // 0x03: both address DIP switches ON
#define AS3935_CAPACITANCE  96    // tuning caps (pF), DFRobot's calibrated default
#define AS3935_LOCATION     0     // manualCal(): 1 = outdoors, 0 = indoors
#define AS3935_DISTURBER    1     // manualCal(): 1 = disturber detection on
// Sensitivity (all default to 2 after defInit()), tuned for maximum practical
// sensitivity to faint strikes. The noise floor is DYNAMIC: it starts at
// AS3935_NOISE_FLOOR and is raised one step per noise IRQ (INT_NH stays
// asserted while ambient noise exceeds NF_LEV, free-running GPIO wakes at
// ~1/s otherwise), then decays one step per fully quiet heartbeat interval.
// The node settles at the finest floor its location physically supports.
#define AS3935_NOISE_FLOOR   2    // 0..7, START/minimum of the dynamic floor
#define AS3935_WATCHDOG      2    // 0..10, default 2; higher rejects disturbers
#define AS3935_SPIKE_REJECT  2    // 0..11, default 2; higher rejects spikes
#define LIGHTNING_COOLDOWN_S 60   // min gap between strike-triggered publishes
#define DISTURB_LIMIT       100   // disturber IRQs since last publish before masking
// Heartbeat-starvation guards. In a marginal/noisy spot the AS3935 free-runs
// noise/disturber IRQs; each is a GPIO deep-sleep wake that (before this fix)
// re-armed the full sleep timer, so the periodic publish never fired. These
// bound it -- the node keeps reporting even when the sensor misbehaves.
#define NOISE_WAKES_HEARTBEAT 30  // force a heartbeat after N non-strike wakes while the clock is unset
#define NOISE_WAKES_MASK      60  // a real IRQ flood: drop GPIO wake, fall back to the timer heartbeat
#define GPIO_REARM_HEARTBEATS  6  // ...then retry the GPIO wake after N delivered heartbeats (~30 min)

// ---- Battery voltage sense (onboard 1:2 divider from VBAT to GPIO0) ----
#define ENABLE_BATTERY 1
#define BATT_ADC_PIN   0
#define BATT_DIVIDER   2.0f
#define BATT_SAMPLES   8

// ---- Cadence ----
// 300 s heartbeat to match the mains-adjacent climate nodes. On the solar
// option drop back to 600 s -- the DFR0579's ~0.3 W panel budget is tight in
// winter and the 5-min cadence roughly doubles the wake cost.
#define SLEEP_SECONDS      300
#define NTP_RESYNC_PUBS    24      // full NTP resync every N publishes
#define WIFI_SCANLESS_MS   5000    // cached-BSSID reconnect: fast or not at all
#define WIFI_FULLSCAN_MS   12000   // full-scan fallback
#define NTP_TIMEOUT_MS     8000
#define MQTT_RETRIES       6
#define MQTT_SOCK_TIMEOUT  4       // s; fail a dropped SYN fast
#define ECHO_TIMEOUT_MS    2000    // wait for our own retained debug record back

// ---- Diagnostics ----
// Published (retained) to a non-telemetry topic the ingest ignores.
// Watch with: mosquitto_sub -t 'sensors/lab/storm01/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 72);   // free address on your LAN (.71 = CYD wall panel)
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "storm01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Persisted across deep sleep (ESP32 RTC memory) ----
// Clock note: unlike the other fbc6 nodes there is NO simulated epoch here --
// lightning wakes at arbitrary times would corrupt a fixed-step clock. The C6
// RTC keeps system time through deep sleep, so time(nullptr) stays valid
// between the periodic NTP resyncs.
RTC_DATA_ATTR uint8_t  rtcChannel   = 0;
RTC_DATA_ATTR uint8_t  rtcBssidOk   = 0;
RTC_DATA_ATTR uint8_t  rtcBssid[6]  = {0};

// Lightning accumulators (strikes not yet published, see cooldown above).
RTC_DATA_ATTR uint16_t rtcStrikes      = 0;    // pending, folded into next publish
RTC_DATA_ATTR uint8_t  rtcMinDistKm    = 255;  // min storm-front distance of pending strikes
RTC_DATA_ATTR uint32_t rtcMaxEnergy    = 0;    // max raw energy of pending strikes
RTC_DATA_ATTR uint32_t rtcLastStrikePub = 0;   // epoch of last strike-triggered publish
RTC_DATA_ATTR uint32_t rtcLastPublish  = 0;    // epoch of last successful publish (any)
RTC_DATA_ATTR uint8_t  rtcDistMasked   = 0;    // disturber IRQ masked (flood guard)

// Diagnostics carried across sleep.
RTC_DATA_ATTR uint32_t rtcTotalWakes   = 0;
RTC_DATA_ATTR uint32_t rtcPublishes    = 0;
RTC_DATA_ATTR uint32_t rtcConsecFail   = 0;
RTC_DATA_ATTR uint32_t rtcPubsSinceNtp = 0;
RTC_DATA_ATTR uint32_t rtcStrikesTotal = 0;    // lifetime strikes since cold boot
RTC_DATA_ATTR uint32_t rtcDisturbers   = 0;    // disturber IRQs since last publish
RTC_DATA_ATTR uint32_t rtcNoiseWakes   = 0;    // noise IRQs since cold boot
RTC_DATA_ATTR uint8_t  rtcPrevResult   = 0;    // 0=cold 1=ok 2=wifi_fail 3=mqtt_fail 4=no_data
RTC_DATA_ATTR uint32_t rtcNoiseWakesSincePub = 0;  // non-strike GPIO wakes since last publish (heartbeat guard)
RTC_DATA_ATTR uint8_t  rtcGpioWakeDisabled   = 0;  // IRQ flooded -> fell back to timer-only heartbeat
RTC_DATA_ATTR uint8_t  rtcHbSinceMask        = 0;  // delivered heartbeats since the GPIO wake was masked
RTC_DATA_ATTR uint8_t  rtcNoiseSinceHb       = 0;  // noise IRQs since the last delivered publish (floor decay gate)

// Per-cycle context for the debug payload.
int           g_wakeCause = 0;
int           g_noiseFloor = -1;      // AS3935 NF_LEV this wake (-1 = sensor absent)
const char   *g_wifiMode  = "none";   // scanless | fullscan | fail
unsigned long g_wifiMs    = 0;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DFRobot_AS3935_I2C lightning((uint8_t)AS3935_IRQ_PIN, (uint8_t)AS3935_I2C_ADDR);

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
// (The AS3935 shares the bus but its 0x01-0x03 addresses can't collide.)
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
  delay(10);                                 // x1/x1 conversion is ~8ms. The 0xF3 "measuring"
                                             // bit is racy on BME280 clones (often not set yet on
                                             // the first read -> the poll below falls straight through
                                             // and we read back the 0x80000 reset value). Settle first.
  unsigned long start = millis();
  while ((read8(0xF3) & 0x08) && millis() - start < 100) delay(2);  // then wait out a slower conversion
  uint8_t d[8];
  Wire.beginTransmission(bmeAddr); Wire.write(0xF7); Wire.endTransmission();
  Wire.requestFrom((int)bmeAddr, 8);
  for (int i = 0; i < 8; i++) d[i] = Wire.read();

  int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  int32_t adc_H = ((int32_t)d[6] << 8) | d[7];
  if (adc_T == 0x80000 || adc_P == 0x80000) {   // skipped/no data -- log why
    Serial.printf("BME skipped: ctrl_meas=0x%02X status=0x%02X adc_T=0x%05lX adc_P=0x%05lX\n",
      read8(0xF4), read8(0xF3), (unsigned long)adc_T, (unsigned long)adc_P);
    return false;
  }

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
// state=-2 misses. Sending an outbound packet first forces the AP to learn the
// station and primes the ARP, so the SYN that follows actually lands.
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
  WiFi.setSleep(false);   // no modem sleep during our brief active window
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS, STATIC_DNS2);
#endif
  unsigned long t0 = millis();
  bool haveCache = rtcBssidOk;
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
    primePath();
    Serial.printf("WiFi UP (%s): ip=%s rssi=%ddBm ch=%d in %lums\n",
      g_wifiMode, WiFi.localIP().toString().c_str(), WiFi.RSSI(), rtcChannel, g_wifiMs);
    return true;
  }
  rtcBssidOk = 0;
  g_wifiMode = "fail";
  Serial.println(F("WiFi FAILED"));
  return false;
}

// End-to-end delivery proof for QoS0: publish() returning true only means the
// bytes reached the local TCP stack -- on a marginal link the frames are still
// in MAC-retry when deep sleep kills the radio, and the publish is silently
// lost (the storm01 room drops most publishes this way). So after telemetry we
// publish the retained debug record and SUBSCRIBE to it: TCP ordering means
// getting our own record echoed back proves the broker received everything
// sent before it. Only then is it safe to sleep.
static char g_dbg[256];
static volatile bool g_echoSeen = false;

void onMqttEcho(char *topic, byte *payload, unsigned int len) {
  (void)topic;   // only DEBUG_TOPIC is subscribed
  if (len == strlen(g_dbg) && memcmp(payload, g_dbg, len) == 0) g_echoSeen = true;
}

// Connect + publish telemetry + retained debug record, then wait for the debug
// echo as delivery confirmation. Returns the attempt that was CONFIRMED
// delivered (1..N), or 0. Duplicate telemetry from a retry after a lost echo
// is fine: ingest inserts with ON CONFLICT DO NOTHING on (device_id, ts).
int publishWithRetry(const char *payload) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(MQTT_SOCK_TIMEOUT);
  mqtt.setBufferSize(512);   // the echoed debug packet must fit the RX buffer
  mqtt.setCallback(onMqttEcho);
  for (int attempt = 1; attempt <= MQTT_RETRIES; attempt++) {
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      bool ok = mqtt.publish(MQTT_TOPIC, payload);
      if (ok) {
        snprintf(g_dbg, sizeof(g_dbg),
          "{\"wake\":%lu,\"cause\":%d,\"wifi\":\"%s\",\"wifi_ms\":%lu,\"mqtt_try\":%d,"
          "\"pub\":%lu,\"consec_fail\":%lu,\"prev_res\":%d,\"strikes_tot\":%lu,"
          "\"disturb\":%lu,\"noise\":%lu,\"dist_masked\":%d,\"nw_pub\":%lu,"
          "\"gpio_off\":%d,\"nf\":%d,\"awake_ms\":%lu}",
          (unsigned long)rtcTotalWakes, g_wakeCause, g_wifiMode, g_wifiMs, attempt,
          (unsigned long)(rtcPublishes + 1), (unsigned long)rtcConsecFail, rtcPrevResult,
          (unsigned long)rtcStrikesTotal, (unsigned long)rtcDisturbers,
          (unsigned long)rtcNoiseWakes, rtcDistMasked,
          (unsigned long)rtcNoiseWakesSincePub, rtcGpioWakeDisabled,
          g_noiseFloor, (unsigned long)millis());
        g_echoSeen = false;
        mqtt.publish(DEBUG_TOPIC, g_dbg, true);   // retained
        mqtt.subscribe(DEBUG_TOPIC);              // broker echoes the retained record
        unsigned long tEcho = millis();
        while (!g_echoSeen && millis() - tEcho < ECHO_TIMEOUT_MS) {
          mqtt.loop();
          delay(5);
        }
        ok = g_echoSeen;   // a stale/foreign echo can't match: wake+awake_ms differ
      }
      Serial.printf("publish %s (try %d): %s\n",
        ok ? "confirmed" : "NOT CONFIRMED", attempt, payload);
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

// Raw AS3935 register read (the DFRobot lib keeps singRegRead private).
static uint8_t as3935ReadReg(uint8_t reg) {
  Wire.beginTransmission(AS3935_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)AS3935_I2C_ADDR, 1);
  return Wire.read();
}

// Read + clear the AS3935 interrupt (the IRQ line stays high until the source
// register is read) and fold the result into the RTC accumulators.
// Returns true if it was a real lightning strike.
bool handleLightningIrq() {
  delay(2);   // datasheet: wait 2 ms after IRQ before reading the source
  uint8_t src = lightning.getInterruptSrc();
  if (src == 1) {
    uint8_t  dist   = lightning.getLightningDistKm();
    uint32_t energy = lightning.getStrikeEnergyRaw();
    rtcStrikes++;
    rtcStrikesTotal++;
    if (dist < rtcMinDistKm) rtcMinDistKm = dist;
    if (energy > rtcMaxEnergy) rtcMaxEnergy = energy;
    Serial.printf("LIGHTNING: %d km, energy %lu (pending %u)\n",
      dist, (unsigned long)energy, rtcStrikes);
    return true;
  }
  if (src == 2) {
    rtcDisturbers++;
    Serial.printf("disturber (%lu since last publish)\n", (unsigned long)rtcDisturbers);
    if (rtcDisturbers > DISTURB_LIMIT && !rtcDistMasked) {
      // Flood guard: something nearby spams disturber IRQs; each wake costs
      // battery. Mask them until the next cold boot (real strikes still fire).
      lightning.disturberDis();
      rtcDistMasked = 1;
      Serial.println(F("disturber IRQ masked (flood guard)"));
    }
  } else if (src == 3) {
    rtcNoiseWakes++;
    rtcNoiseSinceHb++;
    // Dynamic noise floor: INT_NH stays asserted while ambient noise exceeds
    // NF_LEV, so the IRQ free-runs GPIO wakes and drains the battery. Step the
    // floor up until the sensor goes quiet; the publish path steps it back
    // down once a full heartbeat interval stays noise-free.
    uint8_t nf = lightning.getNoiseFloorLvl();
    if (nf < 7) {
      lightning.setNoiseFloorLvl(nf + 1);
      g_noiseFloor = nf + 1;
      Serial.printf("noise level too high -> noise floor up to %d\n", nf + 1);
    } else {
      Serial.println(F("noise level too high (noise floor already max)"));
    }
  }
  return false;
}

void goToSleep() {
  // A strike during our awake window would hold IRQ high and re-wake us
  // instantly -- drain (and accumulate) it first.
  if (digitalRead(AS3935_IRQ_PIN) == HIGH) handleLightningIrq();

  // Arm the heartbeat for the time REMAINING until the next publish, not a fresh
  // full interval. Otherwise every AS3935 GPIO (noise/disturber) wake resets the
  // countdown and the periodic publish is starved forever -- the "sends once then
  // nothing" failure. When the heartbeat is already overdue (we only reached here
  // because the publish failed/bailed) fall back to the full interval so we don't
  // hammer WiFi. Needs no valid clock in the common case; degrades gracefully.
  uint32_t sleepFor = SLEEP_SECONDS;
  time_t nowS = time(nullptr);
  if (nowS >= 1700000000UL && rtcLastPublish > 0) {
    uint32_t elapsed = (uint32_t)nowS - rtcLastPublish;
    if (elapsed < SLEEP_SECONDS) sleepFor = SLEEP_SECONDS - elapsed;
  }
  Serial.printf("awake %lums, sleeping %lus (or next strike)\n",
    millis(), (unsigned long)sleepFor);
  Serial.flush();
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup((uint64_t)sleepFor * 1000000ULL);
  // A flooding sensor starves the heartbeat and drains the battery; once masked
  // (rtcGpioWakeDisabled) stop waking on the IRQ -- the node reports on the pure
  // timer heartbeat until a cold boot, an AS3935 reconfig, or
  // GPIO_REARM_HEARTBEATS delivered heartbeats re-arm the wake.
  if (!rtcGpioWakeDisabled) {
    esp_deep_sleep_enable_gpio_wakeup(1ULL << AS3935_IRQ_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  esp_deep_sleep_start();   // never returns; full reset on wake
}

void setup() {
  Serial.begin(115200);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  // A GPIO (noise/strike) wake usually bails in a few ms; don't burn the full
  // 800ms Serial-settle on it. On battery !Serial never clears, so this would
  // otherwise waste ~800ms of active current on every noise wake in a flood.
  unsigned long t0 = millis();
  unsigned long serialWaitMs = (cause == ESP_SLEEP_WAKEUP_GPIO) ? 60 : 800;
  while (!Serial && millis() - t0 < serialWaitMs) delay(10);

  rtcTotalWakes++;
  g_wakeCause = (int)cause;   // 7 == GPIO (lightning/noise), 4 == TIMER (heartbeat)
  bool coldBoot      = (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_GPIO);
  bool lightningWake = (cause == ESP_SLEEP_WAKEUP_GPIO);
  Serial.printf("\n=== %s wake #%lu cause=%d (prev_res=%d consec_fail=%lu) ===\n",
    DEVICE_ID, (unsigned long)rtcTotalWakes, g_wakeCause, rtcPrevResult,
    (unsigned long)rtcConsecFail);

  if (coldBoot) {   // RTC RAM can survive a bare reset; start the flood guards clean
    rtcGpioWakeDisabled   = 0;
    rtcNoiseWakesSincePub = 0;
    rtcHbSinceMask        = 0;
    rtcNoiseSinceHb       = 0;
  }

  pinMode(AS3935_IRQ_PIN, INPUT_PULLDOWN);   // AS3935 drives push-pull; keep it
                                             // defined if the sensor is absent
  bool sensorOk = initSensor();   // BME280; also brings up the shared I2C bus
  if (!sensorOk) Serial.println(F("No BME/BMP280 found (check SDA=GPIO19, SCL=GPIO20, 3V3, GND)"));

  // The library's begin()/readReg() never check I2C ACKs (readReg returns
  // `size` unconditionally), so begin()==0 does NOT prove the sensor is
  // there — a missing sensor reads 0xFF and "configures" fine. Probe the
  // address for a real ACK first, and read a register back after config.
  Wire.beginTransmission(AS3935_I2C_ADDR);
  bool as3935Present = (Wire.endTransmission() == 0);
  if (!as3935Present) {
    Serial.println(F("AS3935: no ACK on addr 0x03 -- I2C scan:"));
    int found = 0;
    for (uint8_t a = 0x01; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  device ACKs at 0x%02X\n", a);
        found++;
      }
    }
    if (!found) Serial.println(F("  bus empty (check SDA=GPIO19, SCL=GPIO20, 3V3, GND)"));
    else Serial.println(F("  (0x01/0x02 = AS3935 with other DIP setting)"));
  }
  bool as3935Ok = as3935Present && (lightning.begin() == 0);
  Wire.setClock(100000);   // begin() bumps the shared bus to 400 kHz -- drop back
                           // BEFORE any register traffic; 400 kHz reads glitch on
                           // this wiring (the "noise floor = 7" corruption).
  if (as3935Ok) {
    // The AS3935 keeps its configuration through ESP deep sleep -- but loses it
    // whenever the SENSOR loses power while the ESP stays on battery (e.g. bus
    // rewiring). No cold boot happens then, so also check on warm wakes whether
    // the tuning caps still read what manualCal() set (96 pF -> reg 0x08 nibble
    // 12); a power-cycled sensor reads the power-on 0 and runs uncalibrated.
    // Heartbeat wakes only -- noise wakes must stay cheap -- and a single
    // glitched read must not fire the reconfig side effects, so confirm twice.
    bool needConfig = coldBoot;
    if (!needConfig && !lightningWake) {
      bool lost = (as3935ReadReg(0x08) & 0x0F) != (AS3935_CAPACITANCE >> 3);
      if (lost) {
        delay(2);
        lost = (as3935ReadReg(0x08) & 0x0F) != (AS3935_CAPACITANCE >> 3);
      }
      if (lost) {
        Serial.println(F("AS3935: config lost (sensor power-cycled) -> reconfiguring"));
        needConfig = true;
        rtcDistMasked = 0;         // the sensor-side disturber mask is gone too
        rtcGpioWakeDisabled = 0;   // freshly calibrated sensor may wake us again
        rtcNoiseWakesSincePub = 0;
        rtcHbSinceMask = 0;
      }
    }
    if (needConfig) {
      lightning.defInit();
      lightning.manualCal(AS3935_CAPACITANCE, AS3935_LOCATION, AS3935_DISTURBER);
      // singRegWrite() is a read-modify-write, so ONE corrupt read on a marginal
      // bus writes back a wrong NF_LEV -- the "noise floor = 7 (set 4)" symptom.
      // Apply, read back, and re-apply until it sticks (bounded).
      for (int i = 0; i < 4; i++) {
        lightning.setNoiseFloorLvl(AS3935_NOISE_FLOOR);
        lightning.setWatchdogThreshold(AS3935_WATCHDOG);
        lightning.setSpikeRejection(AS3935_SPIKE_REJECT);
        if (lightning.getNoiseFloorLvl() == AS3935_NOISE_FLOOR &&
            lightning.getWatchdogThreshold() == AS3935_WATCHDOG) break;
        delay(3);
      }
      Serial.println(F("AS3935: OK (configured, indoors)"));
    } else {
      Serial.println(F("AS3935: OK (config retained)"));
    }
    // Readback proof of real communication (a floating bus would read 7). The
    // floor is dynamic, so any value in [AS3935_NOISE_FLOOR..7] is healthy.
    g_noiseFloor = lightning.getNoiseFloorLvl();
    Serial.printf("AS3935: noise floor = %d (start %d, dynamic)\n",
      g_noiseFloor, AS3935_NOISE_FLOOR);
  } else {
    Serial.println(F("AS3935: FAILED (check IRQ=GPIO2, addr DIP=0x03, Gravity cable)"));
  }

  // Lightning wake: read + clear the IRQ, accumulate. Publish only outside the
  // cooldown window -- burst strikes are folded into the next publish instead.
  if (lightningWake || (as3935Ok && digitalRead(AS3935_IRQ_PIN) == HIGH)) {
    bool strike = as3935Ok && handleLightningIrq();
    if (lightningWake && !strike) {
      // Disturber/noise wake (or IRQ gone). Normally the cheapest turnaround --
      // but a noise flood would otherwise starve the heartbeat. Backstops:
      //   1. goToSleep() arms the timer for the REMAINING time to the next
      //      heartbeat, so the clock-based deadline holds despite these wakes.
      //   2. If the clock isn't set yet, count wakes and force a heartbeat after
      //      NOISE_WAKES_HEARTBEAT (that publish also re-syncs NTP, recovering it).
      //   3. If the sensor truly free-runs, mask the GPIO wake entirely.
      rtcNoiseWakesSincePub++;
      if (rtcNoiseWakesSincePub >= NOISE_WAKES_MASK && !rtcGpioWakeDisabled) {
        rtcGpioWakeDisabled = 1;
        Serial.println(F("IRQ flood -> GPIO wake off until cold boot (timer heartbeat only)"));
      }
      time_t nowGuess = time(nullptr);   // survives deep sleep on the C6 RTC
      bool heartbeatOverdue =
        (nowGuess >= 1700000000UL &&
         (uint32_t)nowGuess - rtcLastPublish >= (uint32_t)SLEEP_SECONDS) ||
        (nowGuess < 1700000000UL && rtcNoiseWakesSincePub >= NOISE_WAKES_HEARTBEAT);
      if (!heartbeatOverdue) {
        rtcPrevResult = 4;
        goToSleep();
      }
      Serial.println(F("heartbeat due despite noise wake -> publishing"));
    }
    time_t nowGuess = time(nullptr);   // survives deep sleep on the C6 RTC
    if (strike && nowGuess >= 1700000000UL &&
        (uint32_t)nowGuess - rtcLastStrikePub < LIGHTNING_COOLDOWN_S) {
      Serial.printf("strike within cooldown (%lus ago), accumulating\n",
        (unsigned long)((uint32_t)nowGuess - rtcLastStrikePub));
      rtcPrevResult = 1;
      goToSleep();
    }
  }

  // Publishing wake (heartbeat, or strike outside the cooldown).
  float t = NAN, p = NAN, h = NAN;
  if (sensorOk) {
    // Retry: shared-bus noise can make a single forced-mode read come back
    // "skipped". A couple of retries recovers the climate sample.
    for (int i = 0; i < 3 && isnan(t); i++) {
      if (readBme(&t, &p, &h)) break;
      delay(8);
    }
    if (isnan(t)) Serial.println(F("BME280 read failed (3 tries)"));
  }

  bool wifiUp = connectWifi();
  time_t now = time(nullptr);
  bool needNtp = (now < 1700000000UL) || (rtcPubsSinceNtp >= NTP_RESYNC_PUBS);
  if (wifiUp && needNtp && syncNtp()) {
    now = time(nullptr);
    rtcPubsSinceNtp = 0;
  }

#if ENABLE_BATTERY
  bool haveData = true;   // batt_v alone keeps the heartbeat alive without a BME280
#else
  bool haveData = !isnan(t) || as3935Ok || rtcStrikes > 0;
#endif
  uint8_t result;
  if (!haveData) {
    result = 4; Serial.println(F("no data at all, skipping publish"));
  } else if (now < 1700000000UL) {
    result = 4; Serial.println(F("no valid time yet, skipping publish"));
  } else if (!wifiUp) {
    result = 2;   // WiFi never came up this cycle
  } else {
    char payload[288];
    int n = snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"ts\":%lu", DEVICE_ID, (unsigned long)now);
    if (!isnan(t)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"temp_c\":%.2f,\"pressure_hpa\":%.2f", t, p);
      if (!isnan(h)) {
        n += snprintf(payload + n, sizeof(payload) - n,
          ",\"humidity\":%.1f,\"heat_index_c\":%.1f", h, heatIndexC(t, h));
      }
    }
    // lightning_count on every publish (0-baseline = "listening, heard
    // nothing") -- but only while the AS3935 answers, so a dead sensor
    // doesn't masquerade as a quiet sky. km/energy stay strike-only.
    if (as3935Ok || rtcStrikes > 0) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"lightning_count\":%u", rtcStrikes);
    }
    if (rtcStrikes > 0) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"lightning_km\":%d,\"lightning_energy\":%lu",
        rtcMinDistKm, (unsigned long)rtcMaxEnergy);
    }
#if ENABLE_BATTERY
    n += snprintf(payload + n, sizeof(payload) - n, ",\"batt_v\":%.2f", readBatteryVolts());
#endif
    snprintf(payload + n, sizeof(payload) - n, "}");

    result = publishWithRetry(payload) > 0 ? 1 : 3;
    if (result == 1 && rtcStrikes > 0) {
      rtcLastStrikePub = (uint32_t)now;
      rtcStrikes = 0; rtcMinDistKm = 255; rtcMaxEnergy = 0;
      rtcDisturbers = 0;
    }
  }

  if (result == 1) {
    rtcPublishes++; rtcPubsSinceNtp++; rtcConsecFail = 0;
    rtcLastPublish = (uint32_t)now;
    rtcNoiseWakesSincePub = 0;   // heartbeat delivered: restart the starvation guards
    if (as3935Ok && rtcNoiseSinceHb == 0) {
      // A full interval with zero noise IRQs: relax the dynamic floor one step
      // back toward the start value so sensitivity recovers after noise.
      uint8_t nf = lightning.getNoiseFloorLvl();
      if (nf > AS3935_NOISE_FLOOR) {
        lightning.setNoiseFloorLvl(nf - 1);
        Serial.printf("quiet interval -> noise floor down to %d\n", nf - 1);
      }
    }
    rtcNoiseSinceHb = 0;
    if (rtcGpioWakeDisabled && ++rtcHbSinceMask >= GPIO_REARM_HEARTBEATS) {
      // The flood that masked the GPIO wake is probably over -- re-arm it.
      // If it still floods, the mask re-triggers after NOISE_WAKES_MASK wakes,
      // so this is a bounded retry, not a way back into starvation.
      rtcGpioWakeDisabled = 0;
      rtcHbSinceMask = 0;
      Serial.println(F("GPIO wake re-armed after quiet heartbeats"));
    }
  } else {
    rtcConsecFail++;
  }
  rtcPrevResult = result;

  goToSleep();
}

void loop() {}
