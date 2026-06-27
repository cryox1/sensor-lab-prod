#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// WIFI_SSID, WIFI_PASS, MQTT_HOST. See firmware/secrets.h.example.
#include "secrets.h"

// ---- I2C (ESP8266 defaults: SDA=GPIO4/D2, SCL=GPIO5/D1) ----
#define I2C_SDA        4
#define I2C_SCL        5

// ---- Sensor read cadence ----
#define READ_INTERVAL  2000

// ---- Static IPv4 (default DHCP can't reach your LAN's subnet) ----
// Comment out USE_STATIC_IP to fall back to DHCP.
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 64);   // unused address on your LAN
IPAddress STATIC_GW   (10, 0, 0,   1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,   1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "air01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

Adafruit_AHTX0 aht;
ScioSense_ENS160 ens160(ENS160_I2CADDR_1);   // 0x53
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastRead = 0;
bool ntpSynced = false;
bool ahtOk = false;
bool ensOk = false;

const char* wifiStatusStr(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (bad password?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  // Must be called BEFORE WiFi.begin() so DHCP is skipped.
  Serial.printf("WiFi static: ip=%s gw=%s mask=%s dns=%s\n",
    STATIC_IP.toString().c_str(),
    STATIC_GW.toString().c_str(),
    STATIC_MASK.toString().c_str(),
    STATIC_DNS.toString().c_str());
  if (!WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS)) {
    Serial.println(F("WiFi.config FAILED"));
  }
#else
  Serial.println(F("WiFi: DHCP mode"));
#endif
  Serial.printf("WiFi connecting to '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi UP: ip=%s rssi=%ddBm gw=%s dns=%s\n",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI(),
      WiFi.gatewayIP().toString().c_str(),
      WiFi.dnsIP().toString().c_str());
  } else {
    Serial.printf("WiFi FAILED: status=%s\n",
      wifiStatusStr(WiFi.status()));
  }
}

void waitForNtp(unsigned long timeoutMs) {
  Serial.print(F("NTP syncing"));
  unsigned long start = millis();
  while (time(nullptr) < 1700000000UL && millis() - start < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  time_t now = time(nullptr);
  if (now >= 1700000000UL) {
    ntpSynced = true;
    Serial.printf("NTP UP: unix=%lu  (utc=%s)\n",
      (unsigned long)now, asctime(gmtime(&now)));
  } else {
    Serial.printf("NTP not yet synced after %lums (will keep retrying in loop)\n",
      timeoutMs);
  }
}

void connectMqtt() {
  if (mqttClient.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  Serial.printf("MQTT connecting to %s:%d as %s...\n",
    MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println(F("MQTT UP"));
  } else {
    Serial.printf("MQTT FAILED, state=%d\n", mqttClient.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.printf("Boot: %s on NodeMCU ESP-12F (ENS160+AHT21, no display)\n", DEVICE_ID);

  Wire.begin(I2C_SDA, I2C_SCL);

  ahtOk = aht.begin();
  Serial.printf("AHT21: %s\n", ahtOk ? "OK" : "FAILED");

  ensOk = ens160.begin();
  if (ensOk) {
    ens160.setMode(ENS160_OPMODE_STD);
    Serial.printf("ENS160: OK (rev %d.%d.%d)\n",
      ens160.getMajorRev(), ens160.getMinorRev(), ens160.getBuild());
  } else {
    Serial.println(F("ENS160: FAILED"));
  }

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    waitForNtp(15000);
  } else {
    Serial.println(F("Skipping NTP: WiFi not connected"));
  }
  connectMqtt();
  delay(500);
}

void loop() {
  if (millis() - lastRead < READ_INTERVAL) {
    mqttClient.loop();
    return;
  }
  lastRead = millis();

  connectWifi();
  connectMqtt();
  mqttClient.loop();

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
    if (!isnan(t) && !isnan(h)) {
      ens160.set_envdata(t, h);
    }
    ens160.measure(true);
    ens160.measureRaw(true);
    eco2 = ens160.geteCO2();
    tvoc = ens160.getTVOC();
    aqi  = ens160.getAQI();
  }

  if (isnan(t) && eco2 < 0) {
    Serial.println(F("All sensors failed (check I2C wiring D1/D2)"));
    return;
  }

  time_t now = time(nullptr);
  if (now < 1700000000UL) {
    Serial.printf("NTP not synced (time=%lu) | WiFi=%s ip=%s rssi=%ddBm\n",
      (unsigned long)now,
      wifiStatusStr(WiFi.status()),
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI());
    return;
  }
  if (!ntpSynced) {
    ntpSynced = true;
    Serial.printf("NTP just synced: unix=%lu\n", (unsigned long)now);
  }

  // Build payload — omit fields whose reads failed.
  char payload[200];
  int n = snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"ts\":%lu",
    DEVICE_ID, (unsigned long)now);
  if (!isnan(t)) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"temp_c\":%.1f,\"humidity\":%.0f", t, h);
  }
  if (eco2 >= 0) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"eco2_ppm\":%d,\"tvoc_ppb\":%d,\"aqi\":%d",
      eco2, tvoc, aqi);
  }
  snprintf(payload + n, sizeof(payload) - n, "}");

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(MQTT_TOPIC, payload);
    Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
  } else {
    Serial.printf("mqtt down, dropping: %s\n", payload);
  }
}
