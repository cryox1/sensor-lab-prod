// Headless deep-sleep NEO-6M GPS node on ESP32-C3 (device gps01).
//
// Battery-first build: wake -> wait for a GPS fix -> publish lat/lon over MQTT
// -> deep sleep. ESP32 wakes itself on the timer, so -- unlike the ESP8266
// boards -- there is NO D0->RST wire to add. No OLED, no LED.
//
// ** BATTERY CAVEAT (read this): ESP32 deep sleep only powers down the ESP32.
//    The NEO-6M stays powered from 3V3 and keeps drawing ~25-45 mA the entire
//    time, so the GPS dominates the budget -- expect only ~1-2 days on a 2000 mAh
//    cell, deep sleep or not. To really save power you must power-gate the GPS
//    (switch its VCC with a MOSFET/load switch from a spare GPIO before sleeping),
//    but then each wake is a GPS cold start that can take minutes to first fix.
//    Leaving the GPS powered (this build) trades battery for a fast fix and is
//    fine for a battery + data-plumbing test. **
//
// Board package: esp32 (Espressif). Board: "ESP32C3 Dev Module", USB CDC On Boot.
// Libraries: PubSubClient, TinyGPSPlus (Mikal Hart).
// Wiring: NEO-6M TX -> GPIO20 (required), NEO-6M RX -> GPIO21 (optional),
//         GPS VCC -> 3V3, GPS GND -> GND.

#include <WiFi.h>
#include <PubSubClient.h>
#include <TinyGPS++.h>
#include <time.h>
#include <esp_sleep.h>

// ---- WiFi + MQTT — copy secrets.h.example to this folder as secrets.h and fill in ----
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST (gitignored)

// ---- NEO-6M GPS on UART1 ----
#define GPS_BAUD       9600
#define GPS_RX_PIN     20   // ESP RX <- GPS TX (required)
#define GPS_TX_PIN     21   // ESP TX -> GPS RX (optional)

// ---- Cadence ----
#define SLEEP_SECONDS      300
#define GPS_FIX_TIMEOUT_MS 90000    // wait up to this long for a fresh fix after waking
#define WIFI_TIMEOUT_MS    15000
#define NTP_TIMEOUT_MS     8000

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 67);
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "gps01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

RTC_DATA_ATTR uint32_t bootCount = 0;   // survives deep sleep on ESP32

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool connectWifi() {
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS, STATIC_DNS2);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(50);
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up) Serial.printf("WiFi UP: ip=%s rssi=%ddBm in %lums\n",
    WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)(millis() - start));
  else Serial.println(F("WiFi FAILED"));
  return up;
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
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();   // never returns; full reset on wake
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  bootCount++;
  Serial.printf("\n=== gps01 headless deep-sleep, boot #%lu ===\n", (unsigned long)bootCount);

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Wait for a fresh fix. If the GPS stayed powered through deep sleep this is
  // quick; on a true cold start it can take minutes.
  bool haveFix = false;
  unsigned long start = millis();
  while (millis() - start < GPS_FIX_TIMEOUT_MS) {
    while (GPSSerial.available() > 0) gps.encode(GPSSerial.read());
    if (gps.location.isValid() && gps.location.age() < 3000) { haveFix = true; break; }
    delay(10);
  }
  Serial.printf("gps: fix=%s sats=%lu rx_chars=%lu\n",
    haveFix ? "YES" : "no", (unsigned long)gps.satellites.value(),
    (unsigned long)gps.charsProcessed());

  bool wifiUp = connectWifi();
  time_t now = (wifiUp && syncNtp()) ? time(nullptr) : 0;

  if (!haveFix) {
    Serial.println(F("no fix, skipping publish"));
  } else if (now < 1700000000UL) {
    Serial.println(F("no valid time, skipping publish"));
  } else if (wifiUp) {
    double lat = gps.location.lat(), lon = gps.location.lng();
    int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      char payload[220];
      int n = snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"ts\":%lu,\"lat\":%.6f,\"lon\":%.6f",
        DEVICE_ID, (unsigned long)now, lat, lon);
      if (gps.altitude.isValid())  n += snprintf(payload + n, sizeof(payload) - n, ",\"alt_m\":%.1f", gps.altitude.meters());
      if (gps.satellites.isValid()) n += snprintf(payload + n, sizeof(payload) - n, ",\"sats\":%d", sats);
      if (gps.speed.isValid())     n += snprintf(payload + n, sizeof(payload) - n, ",\"speed_kmh\":%.1f", gps.speed.kmph());
      snprintf(payload + n, sizeof(payload) - n, "}");
      bool ok = mqtt.publish(MQTT_TOPIC, payload);
      Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
      mqtt.loop();
      mqtt.disconnect();
    } else {
      Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
    }
  }

  goToSleep();
}

void loop() {}
