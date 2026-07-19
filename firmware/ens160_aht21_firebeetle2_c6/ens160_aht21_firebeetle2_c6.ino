// Always-on ENS160+AHT21 air-quality node on DFRobot FireBeetle 2 ESP32-C6 /
// DFR1075 (device air03). USB-powered permanently -- NO battery, NO deep sleep.
//
// Always-on is not just convenience: the ENS160 needs continuous operation in
// STD mode (minutes of warm-up) before its eCO2/TVOC/AQI outputs mean anything,
// so a sleep/wake cycle would republish warm-up garbage forever. Same reasoning
// as the ens160_aht21_nodemcu node (air01); this is its ESP32-C6 sibling with
// the fbc6 boards' link-robustness pieces folded in.
//
// WIRING (deliberately NOT the board default!): the sensor is wired to
// SDA -> GPIO20, SCL -> GPIO21 -- unlike the other fbc6 nodes, which use the
// silkscreened default I2C bus (SDA=19/SCL=20). There is NO swapped-pin
// fallback probe here on purpose: the pins are intentional, and auto-swapping
// would mask a wiring regression. VCC -> 3V3, GND -> GND. The combo board's CS
// pin is bridged HIGH -> I2C mode, ENS160 at 0x53 (ENS160_I2CADDR_1).
// Don't use the variant's D* aliases -- on this board D6 is GPIO1 etc.
//
// FLASHING GOTCHA: after an esptool/arduino-cli flash, a C6's native
// USB-serial-JTAG often stays in ROM *download mode* (esptool's RTS hard-reset
// can't boot the app here; the host holds the boot strap). Symptom: port stays
// enumerated, zero serial, never publishes. Fix: physically POWER-CYCLE
// (unplug/replug USB) once.
//
// Board package: esp32 (Arduino-ESP32 core >= 3.0.0). Board: "DFRobot FireBeetle
// 2 ESP32-C6" (FQBN esp32:esp32:dfrobot_firebeetle2_esp32c6:CDCOnBoot=cdc).
// NOTE: unlike the XIAO, "USB CDC On Boot" defaults to DISABLED on this board --
// enable it (IDE menu / :CDCOnBoot=cdc) or Serial stays silent on the USB port.
// Libraries: Adafruit AHTX0, ScioSense_ENS160, PubSubClient.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>
#include <esp_system.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C (deliberate: NOT the board-default SDA=19/SCL=20) ----
#define I2C_SDA        20
#define I2C_SCL        21

// ---- Cadence ----
#define READ_INTERVAL  30000UL   // ms between telemetry publishes
#define MQTT_SOCK_TIMEOUT  4     // s; fail a dropped SYN fast instead of the 15 s default

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 70);   // free address on your LAN
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "air03"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID

// ---- Diagnostics ----
// Retained boot/reconnect record on a non-telemetry topic the ingest ignores,
// so it never reaches the DB. Watch with: mosquitto_sub -t 'sensors/lab/air03/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

Adafruit_AHTX0 aht;
ScioSense_ENS160 ens160(ENS160_I2CADDR_1);   // 0x53 (CS bridged HIGH)
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastRead = 0;
bool ntpStarted = false;
bool ntpSynced  = false;
bool ahtOk      = false;
bool ensOk      = false;
bool wifiWasUp  = false;         // re-entry after a drop needs a clean disconnect
uint32_t mqttConnects = 0;       // 1 = boot, >1 = reconnects since boot
uint32_t pubOk = 0, pubFail = 0;
unsigned long g_wifiMs = 0;

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

// One throwaway UDP datagram to the gateway. After a fresh assoc the AP hasn't
// yet added us to its forwarding/bridge table and the gateway ARP isn't
// resolved, so the very first MQTT TCP SYN is silently dropped -> the state=-2
// misses. Sending an outbound packet first forces the AP to learn the station
// and primes the ARP, so the SYN that follows actually lands.
void primePath() {
  WiFiUDP u;
  u.begin(0);
  u.beginPacket(WiFi.gatewayIP(), 9);   // port 9 = discard; nothing need listen
  uint8_t z = 0;
  u.write(&z, 1);
  u.endPacket();
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // always USB-powered; modem sleep only drops packets
  if (wifiWasUp) {
    WiFi.disconnect();    // re-entry after a drop: start the STA state clean
    delay(10);
  }
#ifdef USE_STATIC_IP
  Serial.printf("WiFi static: ip=%s gw=%s\n",
    STATIC_IP.toString().c_str(), STATIC_GW.toString().c_str());
  if (!WiFi.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS, STATIC_DNS2)) {
    Serial.println(F("WiFi.config FAILED"));
  }
#else
  Serial.println(F("WiFi: DHCP mode"));
#endif
  Serial.printf("WiFi connecting to '%s'...\n", WIFI_SSID);
  unsigned long t0 = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  g_wifiMs = millis() - t0;
  if (WiFi.status() == WL_CONNECTED) {
    wifiWasUp = true;
    delay(250);   // let the AP finish adding the station / ARP settle before TCP
    primePath();  // and force the forwarding table / ARP so the first SYN lands
    Serial.printf("WiFi UP: ip=%s rssi=%ddBm gw=%s in %lums\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI(),
      WiFi.gatewayIP().toString().c_str(), g_wifiMs);
    if (!ntpStarted) {
      // Start SNTP only once WiFi is actually up; the core re-syncs periodically.
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntpStarted = true;
    }
  } else {
    Serial.printf("WiFi FAILED: status=%s\n", wifiStatusStr(WiFi.status()));
  }
}

void connectMqtt() {
  if (mqttClient.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setSocketTimeout(MQTT_SOCK_TIMEOUT);
  primePath();
  Serial.printf("MQTT connecting to %s:%d as %s...\n",
    MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    mqttConnects++;
    Serial.printf("MQTT UP (connect #%lu)\n", (unsigned long)mqttConnects);
    // Retained boot/reconnect record: the latest one is always inspectable, and
    // for a long-running node the connect count is the interesting datum.
    char dbg[208];
    snprintf(dbg, sizeof(dbg),
      "{\"boot_ms\":%lu,\"reset\":%d,\"aht\":%d,\"ens\":%d,\"ip\":\"%s\","
      "\"rssi\":%d,\"wifi_ms\":%lu,\"connects\":%lu,\"pub_ok\":%lu,\"pub_fail\":%lu}",
      millis(), (int)esp_reset_reason(), ahtOk ? 1 : 0, ensOk ? 1 : 0,
      WiFi.localIP().toString().c_str(), WiFi.RSSI(), g_wifiMs,
      (unsigned long)mqttConnects, (unsigned long)pubOk, (unsigned long)pubFail);
    mqttClient.publish(DEBUG_TOPIC, dbg, true);   // retained
  } else {
    Serial.printf("MQTT FAILED, state=%d\n", mqttClient.state());
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 800) delay(10);   // CDC: bounded wait only
  Serial.println();
  Serial.printf("Boot: %s on FireBeetle 2 ESP32-C6 (ENS160+AHT21, always-on)\n",
    DEVICE_ID);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  ahtOk = aht.begin();
  Serial.printf("AHT21: %s\n", ahtOk ? "OK" : "FAILED");

  ensOk = ens160.begin();
  if (ensOk) {
    // STD mode once, never re-inited: the ENS160 must run continuously for its
    // eCO2/TVOC/AQI outputs to be trustworthy (minutes of warm-up).
    ens160.setMode(ENS160_OPMODE_STD);
    Serial.printf("ENS160: OK (rev %d.%d.%d)\n",
      ens160.getMajorRev(), ens160.getMinorRev(), ens160.getBuild());
  } else {
    Serial.println(F("ENS160: FAILED"));
  }
  if (!ahtOk || !ensOk) {
    Serial.println(F("  -> check SDA=GPIO20 SCL=GPIO21 (non-default!), 3V3, GND, CS bridged HIGH"));
  }

  connectWifi();
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
      ens160.set_envdata(t, h);   // T/RH compensation for the gas readings
    }
    ens160.measure(true);
    ens160.measureRaw(true);
    eco2 = ens160.geteCO2();
    tvoc = ens160.getTVOC();
    aqi  = ens160.getAQI();
  }

  if (isnan(t) && eco2 < 0) {
    Serial.println(F("All sensors failed (check I2C wiring GPIO20/GPIO21)"));
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
    Serial.printf("NTP UP: unix=%lu\n", (unsigned long)now);
  }

  // Build payload -- omit fields whose reads failed.
  char payload[224];
  int n = snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"ts\":%lu",
    DEVICE_ID, (unsigned long)now);
  if (!isnan(t)) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"temp_c\":%.1f,\"humidity\":%.0f,\"heat_index_c\":%.1f",
      t, h, heatIndexC(t, h));
  }
  if (eco2 >= 0) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"eco2_ppm\":%d,\"tvoc_ppb\":%d,\"aqi\":%d",
      eco2, tvoc, aqi);
  }
  snprintf(payload + n, sizeof(payload) - n, "}");

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(MQTT_TOPIC, payload);
    if (ok) pubOk++; else pubFail++;
    Serial.printf("publish %s: %s\n", ok ? "ok" : "FAIL", payload);
  } else {
    pubFail++;
    Serial.printf("mqtt down, dropping: %s\n", payload);
  }
}
