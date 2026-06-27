#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// WIFI_SSID, WIFI_PASS, MQTT_HOST. See firmware/secrets.h.example.
#include "secrets.h"

// ---- HW-364A built-in OLED (non-standard I2C) ----
#define OLED_SDA       14   // D5
#define OLED_SCL       12   // D6
#define OLED_ADDR      0x3C
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

// ---- DHT11 ----
#define DHT_PIN        13   // D7 (GPIO2/D4 fights the DHT: boot-strap pin + onboard LED)
#define DHT_TYPE       DHT11
// Longer interval = far less radio-on / CPU-active time = much longer battery
// life. 30 s is plenty for room temp/humidity; raise it to save more.
#define READ_INTERVAL  30000

// ---- Static IPv4 (default DHCP can't reach your LAN's subnet) ----
// Comment out USE_STATIC_IP to fall back to DHCP.
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 60);   // pick an unused address on your LAN
IPAddress STATIC_GW   (10, 0, 0,   1);   // router / default gateway
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,   1);   // router doubles as DNS; or use 1.1.1.1

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "indoor01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool ntpSynced = false;

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
  // Automatic light sleep: the CPU + radio nap between the router's beacons
  // while staying associated. Needs no wiring (unlike deep sleep, which would
  // require D0/GPIO16 -> RST). If your AP drops the link, swap in
  // WIFI_MODEM_SLEEP, which is gentler but saves a bit less.
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
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
  mqttClient.setKeepAlive(45);   // tolerate the sleepy gaps between publishes
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

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init failed"));
    while (true) { delay(1000); }
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("DHT11 + HW-364A"));
  display.println(F("warming up..."));
  display.display();

  dht.begin();
  connectWifi();
  // NTP — UTC, no DST offset (server stores TIMESTAMPTZ)
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    waitForNtp(15000);
  } else {
    Serial.println(F("Skipping NTP: WiFi not connected"));
  }
  connectMqtt();
  delay(500);
}

// Idle for `ms`, letting the SDK drop into automatic light sleep between the
// short wakes. We chunk the wait so MQTT keepalive/PINGREQ still gets serviced
// and the link stays up. This is where the battery savings actually happen:
// the old code busy-returned from loop(), so the CPU never slept.
void idleSleep(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    mqttClient.loop();
    delay(250);   // delay() yields to the SDK, which can light-sleep here
  }
}

void loop() {
  connectWifi();
  connectMqtt();
  mqttClient.loop();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  display.clearDisplay();
  display.setCursor(0, 0);

  if (isnan(t) || isnan(h)) {
    display.setTextSize(1);
    display.println(F("DHT11 read failed"));
    display.println(F("check wiring (D7)"));
    display.display();
    Serial.println(F("DHT read failed"));
    idleSleep(READ_INTERVAL);
    return;
  }

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(t, 1); display.print((char)247); display.println(F("C"));
  display.setCursor(0, 36);
  display.print(h, 0); display.println(F(" %"));
  display.display();

  float hi = dht.computeHeatIndex(t, h, false);

  time_t now = time(nullptr);
  if (now < 1700000000UL) {
    Serial.printf("NTP not synced (time=%lu) | WiFi=%s ip=%s rssi=%ddBm dns=%s\n",
      (unsigned long)now,
      wifiStatusStr(WiFi.status()),
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI(),
      WiFi.dnsIP().toString().c_str());
    idleSleep(READ_INTERVAL);
    return;
  }
  if (!ntpSynced) {
    ntpSynced = true;
    Serial.printf("NTP just synced: unix=%lu\n", (unsigned long)now);
  }

  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"ts\":%lu,\"temp_c\":%.1f,\"humidity\":%.0f,\"heat_index_c\":%.1f}",
    DEVICE_ID, (unsigned long)now, t, h, hi);

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(MQTT_TOPIC, payload);
    Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
  } else {
    Serial.printf("mqtt down, dropping: %s\n", payload);
  }

  idleSleep(READ_INTERVAL);
}
