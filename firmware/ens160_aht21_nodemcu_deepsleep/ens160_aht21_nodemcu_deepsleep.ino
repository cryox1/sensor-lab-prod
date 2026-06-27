// Headless deep-sleep ENS160 + AHT21 air node (ESP8266 NodeMCU, device air01).
//
// Battery-first build: wake -> read sensors -> publish one MQTT message -> deep
// sleep (~20 uA). I2C sensors on the NodeMCU default bus (SDA=GPIO4/D2,
// SCL=GPIO5/D1). No display.
//
// ** REQUIRES one wire: D0 (GPIO16) -> RST ** for the timer wake (470 ohm in
// series so USB flashing still works; or lift it while uploading).
//
// !! GAS-SENSOR CAVEAT: the ENS160 needs continuous operation / warm-up (minutes
//    in STD mode) for trustworthy eCO2/TVOC/AQI. It stays powered through the
//    ESP's deep sleep, but we re-begin() it each wake, so the air-quality fields
//    will often read warming-up/default values. Temp + humidity (AHT21) are fine.
//    For real air-quality logging, DON'T deep sleep this board -- use the always-on
//    ens160_aht21_nodemcu build. This deep-sleep variant is mainly for battery +
//    data-plumbing testing. !!
//
// Optimizations: static IP + cached BSSID in RTC memory; periodic NTP only.

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C (ESP8266 defaults: SDA=GPIO4/D2, SCL=GPIO5/D1) ----
#define I2C_SDA        4
#define I2C_SCL        5

// ---- Cadence ----
#define SLEEP_SECONDS      300
#define NTP_RESYNC_WAKES   24
#define WIFI_TIMEOUT_MS    15000
#define NTP_TIMEOUT_MS     8000

// ---- Static IPv4 ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 64);
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "air01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Persisted across deep sleep (RTC user memory) ----
#define RTC_MAGIC 0xD5510003
struct RtcData {
  uint32_t magic;
  uint32_t epoch;
  uint32_t wakeCount;
  uint8_t  channel;
  uint8_t  bssidValid;
  uint8_t  bssid[6];
};
RtcData rtc;
bool rtcValid = false;

Adafruit_AHTX0 aht;
ScioSense_ENS160 ens160(ENS160_I2CADDR_1);   // 0x53
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS);
#endif
  if (rtcValid && rtc.bssidValid) {
    WiFi.begin(WIFI_SSID, WIFI_PASS, rtc.channel, rtc.bssid, true);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(50);
  if (WiFi.status() == WL_CONNECTED) {
    rtc.channel = WiFi.channel();
    memcpy(rtc.bssid, WiFi.BSSID(), 6);
    rtc.bssidValid = 1;
    Serial.printf("WiFi UP: ip=%s rssi=%ddBm ch=%d in %lums\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI(), rtc.channel,
      (unsigned long)(millis() - start));
    return true;
  }
  rtc.bssidValid = 0;
  Serial.println(F("WiFi FAILED"));
  return false;
}

bool syncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000UL && millis() - start < NTP_TIMEOUT_MS) delay(100);
  return time(nullptr) >= 1700000000UL;
}

void goToSleep() {
  Serial.printf("sleeping %ds\n", SLEEP_SECONDS);
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ESP.deepSleep((uint64_t)SLEEP_SECONDS * 1000000ULL, WAKE_RF_DEFAULT);  // needs D0->RST
  delay(100);
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  Wire.begin(I2C_SDA, I2C_SCL);
  bool ahtOk = aht.begin();
  bool ensOk = ens160.begin();
  if (ensOk) ens160.setMode(ENS160_OPMODE_STD);
  Serial.printf("AHT21=%s ENS160=%s\n", ahtOk ? "ok" : "FAIL", ensOk ? "ok" : "FAIL");

  float t = NAN, h = NAN;
  if (ahtOk) {
    sensors_event_t humEvent, tempEvent;
    if (aht.getEvent(&humEvent, &tempEvent)) {
      t = tempEvent.temperature;
      h = humEvent.relative_humidity;
    }
  }
  int eco2 = -1, tvoc = -1, aqi = -1;
  if (ensOk) {
    if (!isnan(t) && !isnan(h)) ens160.set_envdata(t, h);
    ens160.measure(true);
    ens160.measureRaw(true);
    eco2 = ens160.geteCO2();
    tvoc = ens160.getTVOC();
    aqi  = ens160.getAQI();
  }

  rtcValid = ESP.rtcUserMemoryRead(0, (uint32_t*)&rtc, sizeof(rtc)) && rtc.magic == RTC_MAGIC;
  if (!rtcValid) { memset(&rtc, 0, sizeof(rtc)); rtc.magic = RTC_MAGIC; }

  bool needNtp = !rtcValid || rtc.epoch < 1700000000UL || rtc.wakeCount >= NTP_RESYNC_WAKES;
  bool wifiUp = connectWifi();

  time_t now = 0;
  if (wifiUp && needNtp && syncNtp()) {
    now = time(nullptr);
    rtc.epoch = (uint32_t)now;
    rtc.wakeCount = 0;
  } else if (rtcValid && rtc.epoch >= 1700000000UL) {
    now = (time_t)rtc.epoch + SLEEP_SECONDS;
    rtc.epoch = (uint32_t)now;
    rtc.wakeCount++;
  }

  if (isnan(t) && eco2 < 0) {
    Serial.println(F("All sensors failed (check I2C wiring D1/D2)"));
  } else if (now < 1700000000UL) {
    Serial.println(F("no valid time yet, skipping publish"));
  } else if (wifiUp) {
    char payload[200];
    int n = snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"ts\":%lu", DEVICE_ID, (unsigned long)now);
    if (!isnan(t)) n += snprintf(payload + n, sizeof(payload) - n,
      ",\"temp_c\":%.1f,\"humidity\":%.0f", t, h);
    if (eco2 >= 0) n += snprintf(payload + n, sizeof(payload) - n,
      ",\"eco2_ppm\":%d,\"tvoc_ppb\":%d,\"aqi\":%d", eco2, tvoc, aqi);
    snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      bool ok = mqtt.publish(MQTT_TOPIC, payload);
      Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
      mqtt.loop();
      mqtt.disconnect();
    } else {
      Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
    }
  }

  ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtc, sizeof(rtc));
  goToSleep();
}

void loop() {}
