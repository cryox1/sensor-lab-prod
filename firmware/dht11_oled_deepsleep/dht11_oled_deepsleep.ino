// Headless deep-sleep DHT11 node (ESP8266 / HW-364A board, device indoor01).
//
// Battery-first build: NO OLED, NO button. Just wake -> read DHT11 -> publish
// one MQTT message -> deep sleep (~20 uA). The board has an OLED but this build
// never touches it. (An earlier OLED+button deep-sleep experiment lived here but
// was abandoned -- on ESP8266 a button on RST can't be told apart from the
// timer wake, so the screen could never be made button-aware.)
//
// ** REQUIRES one wire: D0 (GPIO16) -> RST ** so the deep-sleep timer can reset/
// wake the chip. Put a 470 ohm resistor in series so USB flashing still works;
// otherwise lift the D0<->RST jumper while uploading (it fights the auto-reset).
//
// Expected life on a 2000 mAh LiPo (regulator bypassed, fed straight to 3V3):
//   5 min interval  -> ~6-9 weeks      15 min interval -> ~4-6 months
//
// Optimizations: static IP + AP BSSID/channel cached in RTC memory for a
// scanless ~1 s reconnect; NTP only on cold boot and every NTP_RESYNC_WAKES
// wakes, with the clock carried forward in RTC memory in between.

#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- DHT11 ----
#define DHT_PIN        13   // D7 (GPIO2/D4 fights the DHT: boot-strap pin + onboard LED)
#define DHT_TYPE       DHT11

// ---- Cadence ----
#define SLEEP_SECONDS      300      // publish interval; raise to save more battery
#define NTP_RESYNC_WAKES   24       // re-sync the clock every N wakes (~2h @5min)
#define WIFI_TIMEOUT_MS    15000
#define NTP_TIMEOUT_MS     8000

// ---- Static IPv4 ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 60);
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "indoor01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Persisted across deep sleep (RTC user memory) ----
#define RTC_MAGIC 0xD5510001
struct RtcData {
  uint32_t magic;
  uint32_t epoch;       // unix time at the last publish
  uint32_t wakeCount;   // wakes since the last NTP sync
  uint8_t  channel;     // cached AP channel for scanless reconnect
  uint8_t  bssidValid;
  uint8_t  bssid[6];
};                      // 20 bytes (multiple of 4)
RtcData rtc;
bool rtcValid = false;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS);
#endif
  if (rtcValid && rtc.bssidValid) {
    WiFi.begin(WIFI_SSID, WIFI_PASS, rtc.channel, rtc.bssid, true);  // scanless
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

  dht.begin();
  float t = dht.readTemperature();
  float h = dht.readHumidity();

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
    now = (time_t)rtc.epoch + SLEEP_SECONDS;   // carry the clock forward
    rtc.epoch = (uint32_t)now;
    rtc.wakeCount++;
  }

  if (isnan(t) || isnan(h)) {
    Serial.println(F("DHT read failed (check wiring on D7)"));
  } else if (now < 1700000000UL) {
    Serial.println(F("no valid time yet, skipping publish"));
  } else if (wifiUp) {
    float hi = dht.computeHeatIndex(t, h, false);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      char payload[160];
      snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"ts\":%lu,\"temp_c\":%.1f,\"humidity\":%.0f,\"heat_index_c\":%.1f}",
        DEVICE_ID, (unsigned long)now, t, h, hi);
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
