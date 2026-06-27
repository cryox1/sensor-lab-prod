// NEO-6M GPS on an ESP32-C3 board with an onboard 0.42" 72x40 OLED.
// Reads location over UART, shows it on the OLED, and publishes it to the
// sensor-lab stack over MQTT — same pipeline as the ESP8266 sketches
// (MQTT -> bridge -> Kafka -> tsdb-writer -> TimescaleDB).
//
// Differences from the other sketches in this repo:
//   * ESP32-C3, not ESP8266: <WiFi.h> and WiFi.config() takes TWO DNS args.
//   * OLED is the 0.42" 72x40 panel — a window into a 128x64 SSD1306 controller
//     that needs column/page offsets. Adafruit_SSD1306 doesn't apply them, so we
//     use U8g2's dedicated U8G2_SSD1306_72X40_ER_F_HW_I2C constructor instead.
//   * GPS over a hardware UART, parsed with TinyGPSPlus.
//
// Board package: esp32 (Espressif). Board: "ESP32C3 Dev Module".
//   Enable "USB CDC On Boot" so the Serial monitor works over the native USB.
// Libraries: PubSubClient, TinyGPSPlus (Mikal Hart), U8g2 (olikraus).
//
// Wiring (NEO-6M -> ESP32-C3). The OLED is onboard, so only the GPS is wired:
//   NEO-6M VCC -> 3V3   (or 5V only if your breakout has an onboard regulator)
//   NEO-6M GND -> GND
//   NEO-6M TX  -> GPIO20 (GPS_RX_PIN)  <- the essential wire (GPS talks, ESP listens)
//   NEO-6M RX  -> GPIO21 (GPS_TX_PIN)  <- optional, only to send config to the module
//   OLED is fixed on I2C: SDA=GPIO5, SCL=GPIO6, addr 0x3C.
//
// First fix on a cold NEO-6M can take a few minutes; give it a clear view of the sky.

#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TinyGPS++.h>
#include <time.h>

// ---- WiFi + MQTT broker — copy secrets.h.example to this folder as secrets.h and fill in ----
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST (gitignored)

// ---- Onboard 0.42" OLED: SSD1306 72x40 on I2C SDA=GPIO5 / SCL=GPIO6, addr 0x3C ----
#define OLED_SDA       5
#define OLED_SCL       6

// ---- NEO-6M GPS on a hardware UART ----
#define GPS_BAUD       9600
#define GPS_RX_PIN     20   // ESP RX  <- GPS TX  (required)
#define GPS_TX_PIN     21   // ESP TX  -> GPS RX  (optional)

// ---- Publish cadence ----
#define READ_INTERVAL  2000

// ---- Static IPv4 (default DHCP may not match your LAN's subnet) ----
// Comment out USE_STATIC_IP to fall back to DHCP.
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 67);   // unused address on your LAN
IPAddress STATIC_GW   (10, 0, 0,  1);   // router / default gateway
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);   // router doubles as DNS; or use 1.1.1.1
IPAddress STATIC_DNS2 (1, 1, 1, 1);     // ESP32 WiFi.config() takes a second DNS

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "gps01"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Debug logging ----
// Set to 1 for a chatty per-cycle status line on the serial console; 0 to quiet it.
#define DEBUG_LOG 1
#if DEBUG_LOG
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...) do {} while (0)
#endif

// Echo every raw byte the GPS sends to the console. If you see lines like
// "$GPGGA,..." the GPS + wiring + baud are all good; if this stays blank, no
// bytes are arriving (wiring/baud), not a software problem. Spammy — turn off
// once confirmed.
#define GPS_RAW_ECHO 0

// Onboard RGB LED (WS2812) as a status indicator that needs no serial monitor.
// Most ESP32-C3 0.42" boards wire it to GPIO8. Set USE_STATUS_LED to 0 if your
// board has none or it's on another pin. neopixelWrite() is built into the
// esp32 core, so no extra library is needed.
//   white = booting   red = no GPS bytes   blue = bytes, no fix   green = fix
#define USE_STATUS_LED 1
#define STATUS_LED_PIN 8

// 72x40 SSD1306. Full framebuffer (F), hardware I2C on the default Wire bus
// (we point Wire at GPIO5/6 in setup before u8g2.begin()).
U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);   // UART1, remapped to GPS_RX_PIN / GPS_TX_PIN
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastRead = 0;
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
#ifdef USE_STATIC_IP
  // Must be called BEFORE WiFi.begin() so DHCP is skipped. NOTE: the ESP32 core
  // takes TWO DNS servers here (the ESP8266 core takes one).
  Serial.printf("WiFi static: ip=%s gw=%s mask=%s dns=%s\n",
    STATIC_IP.toString().c_str(),
    STATIC_GW.toString().c_str(),
    STATIC_MASK.toString().c_str(),
    STATIC_DNS.toString().c_str());
  if (!WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS, STATIC_DNS2)) {
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
    Serial.printf("WiFi UP: ip=%s rssi=%ddBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("WiFi FAILED: status=%s\n", wifiStatusStr(WiFi.status()));
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
    Serial.printf("NTP UP: unix=%lu\n", (unsigned long)now);
  } else {
    Serial.printf("NTP not yet synced after %lums (will keep retrying)\n", timeoutMs);
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

void setLed(uint8_t r, uint8_t g, uint8_t b) {
#if USE_STATUS_LED
  neopixelWrite(STATUS_LED_PIN, r, g, b);   // built into the esp32 core
#endif
}

// Generic 3-line OLED message — a debug channel that works even when the serial
// monitor doesn't. Pass NULL to skip a line.
void oledLines(const char* a, const char* b, const char* c) {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  if (a) display.drawStr(0, 8, a);
  if (b) display.drawStr(0, 20, b);
  if (c) display.drawStr(0, 31, c);
  display.sendBuffer();
}

void showOled(bool haveFix, double lat, double lon, int sats) {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);
  char line[24];
  if (haveFix) {
    snprintf(line, sizeof(line), "fix  sat %d", sats);
    display.drawStr(0, 8, line);
    snprintf(line, sizeof(line), "%.5f", lat);
    display.drawStr(0, 20, line);
    snprintf(line, sizeof(line), "%.5f", lon);
    display.drawStr(0, 31, line);
  } else {
    display.drawStr(0, 8, DEVICE_ID);
    display.drawStr(0, 20, "no fix");
    snprintf(line, sizeof(line), "sat %lu", (unsigned long)gps.satellites.value());
    display.drawStr(0, 31, line);
  }
  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  // On native USB-CDC the host must OPEN the port before any output is visible.
  // Wait up to 5s for that so the boot banner isn't lost, then carry on headless.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 5000) { delay(10); }
  delay(200);

  setLed(40, 40, 40);   // dim white = booting (visible with no serial / no OLED libs)

  Serial.println();
  Serial.println(F("=== neo6m_c3_oled booting ==="));
  Serial.printf("device=%s  topic=%s\n", DEVICE_ID, MQTT_TOPIC);
  Serial.printf("OLED I2C sda=%d scl=%d | GPS uart rx=%d tx=%d @ %d baud\n",
    OLED_SDA, OLED_SCL, GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
  Serial.printf("DEBUG_LOG=%d GPS_RAW_ECHO=%d STATUS_LED=%d(pin %d)\n",
    DEBUG_LOG, GPS_RAW_ECHO, USE_STATUS_LED, STATUS_LED_PIN);
  Serial.flush();

  // OLED on the onboard I2C pins.
  Wire.begin(OLED_SDA, OLED_SCL);
  display.setBusClock(400000);
  if (!display.begin()) {
    // begin() returns false on most U8g2 builds only if the bus can't init; some
    // builds return void — either way the next oledLines() makes it obvious.
    Serial.println(F("OLED begin() reported a problem (wrong I2C pins/addr?)"));
  }
  oledLines(DEVICE_ID, "booting", NULL);

  // GPS UART. begin(baud, config, rxPin, txPin) remaps UART1 to our pins.
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  oledLines(DEVICE_ID, "wifi...", WIFI_SSID);
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    oledLines("wifi ok", WiFi.localIP().toString().c_str(), "ntp...");
    // NTP — UTC, no DST offset (server stores TIMESTAMPTZ).
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    waitForNtp(15000);
    oledLines("ntp ok", "mqtt...", MQTT_HOST);
    connectMqtt();
    oledLines(mqttClient.connected() ? "mqtt ok" : "mqtt FAIL",
              "waiting for", "gps fix...");
  } else {
    Serial.println(F("Skipping NTP/MQTT: WiFi not connected"));
    oledLines("wifi FAIL", "check ssid", "/ password");
  }
}

void loop() {
  // Feed the NMEA parser continuously — it needs every byte, not just at the
  // publish interval.
  while (GPSSerial.available() > 0) {
    char c = GPSSerial.read();
    gps.encode(c);
#if GPS_RAW_ECHO
    Serial.write(c);   // raw NMEA passthrough for diagnosing wiring/baud
#endif
  }
  mqttClient.loop();

  if (millis() - lastRead < READ_INTERVAL) return;
  lastRead = millis();

  connectWifi();
  connectMqtt();

  bool haveFix = gps.location.isValid() && gps.location.age() < 5000;
  double lat = gps.location.lat();
  double lon = gps.location.lng();
  int sats   = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;

  showOled(haveFix, lat, lon, sats);

  // Per-cycle status line (gated by DEBUG_LOG). rx_chars=0 means no bytes are
  // arriving (wiring/baud); cksum_err climbing usually means the wrong baud rate.
  DBG("gps: fix=%s sats=%lu hdop=%.1f age=%ldms | rx_chars=%lu sentences=%lu cksum_err=%lu | wifi=%s mqtt=%s\n",
    haveFix ? "YES" : "no",
    (unsigned long)gps.satellites.value(),
    gps.hdop.isValid() ? gps.hdop.hdop() : 0.0,
    (long)gps.location.age(),
    (unsigned long)gps.charsProcessed(),
    (unsigned long)gps.sentencesWithFix(),
    (unsigned long)gps.failedChecksum(),
    wifiStatusStr(WiFi.status()),
    mqttClient.connected() ? "up" : "down");

  // A NEO-6M that's wired wrong (or at the wrong baud) sends no bytes — flag it
  // loudly even when DEBUG_LOG is off, and reflect the state on the RGB LED.
  if (gps.charsProcessed() < 10) {
    Serial.println(F("WARNING: no GPS bytes yet — check GPS TX -> GPIO20, shared GND, 9600 baud"));
    setLed(60, 0, 0);   // red: nothing arriving from the GPS
  } else if (!haveFix) {
    setLed(0, 0, 60);   // blue: receiving NMEA, still acquiring a fix
  } else {
    setLed(0, 60, 0);   // green: have a fix
  }

  if (!haveFix) return;

  time_t now = time(nullptr);
  if (now < 1700000000UL) {
    Serial.printf("have fix but NTP not synced (time=%lu); not publishing\n",
      (unsigned long)now);
    return;
  }
  if (!ntpSynced) {
    ntpSynced = true;
    Serial.printf("NTP just synced: unix=%lu\n", (unsigned long)now);
  }

  // Build payload. Always include lat/lon (we only get here with a fix); add the
  // optional fields when TinyGPSPlus considers them valid. Omitted fields are
  // simply stored as NULL by the stack.
  char payload[220];
  int n = snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"ts\":%lu,\"lat\":%.6f,\"lon\":%.6f",
    DEVICE_ID, (unsigned long)now, lat, lon);
  if (gps.altitude.isValid()) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"alt_m\":%.1f", gps.altitude.meters());
  }
  if (gps.satellites.isValid()) {
    n += snprintf(payload + n, sizeof(payload) - n, ",\"sats\":%d", sats);
  }
  if (gps.speed.isValid()) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"speed_kmh\":%.1f", gps.speed.kmph());
  }
  snprintf(payload + n, sizeof(payload) - n, "}");

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(MQTT_TOPIC, payload);
    Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
  } else {
    Serial.printf("mqtt down, dropping: %s\n", payload);
  }
}
