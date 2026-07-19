// Always-on indoor air node on DFRobot FireBeetle 2 ESP32-C6 / DFR1075
// (device air04): BME680 driven by Bosch's BSEC2 IAQ algorithm PLUS an
// ENS160+AHT21 combo board, all on one I2C bus. USB-powered permanently --
// NO battery, NO deep sleep.
//
// Always-on is required twice over: the ENS160 needs continuous STD-mode
// operation before eCO2/TVOC/AQI mean anything, and BSEC2 samples the BME680
// every 3 s (LP rate) to run its gas baseline / IAQ calibration -- a sleep
// cycle would reset both to warm-up garbage. Skeleton (WiFi/MQTT/NTP/loop)
// comes from ens160_aht21_firebeetle2_c6 (air03).
//
// BSEC2 (closed-source Bosch blob) replaces Adafruit_BME680 here: it owns the
// BME680 (heater schedule, forced-mode timing) and outputs a calibrated static
// IAQ (0-500, for stationary devices), CO2 equivalent, breath-VOC equivalent,
// and heat-compensated temp/humidity. Its calibration state is persisted to
// NVS (Preferences namespace "bsec") whenever accuracy reaches 3 and every 6 h
// after, so a reboot resumes calibrated instead of starting over. iaq_acc:
// 0 = stabilizing, 1 = uncertain, 2 = calibrating, 3 = calibrated.
//
// C6 BLOB GOTCHA: the bsec2 Arduino library (tested: 1.10.2610) ships no
// esp32c6 precompiled archive. Fix once per machine (the C2/C3/C6 are all
// soft-float RISC-V, the blob is compatible):
//   cp -r ~/Arduino/libraries/bsec2/src/esp32c3 ~/Arduino/libraries/bsec2/src/esp32c6
//   (and append esp32c6 to architectures= in its library.properties)
// A library update wipes both edits -- redo them if the build stops linking.
//
// WIRING -- BME680 on SPI, ENS160+AHT21 on I2C:
//   BME680 (SEN0248) WITHOUT the Gravity cable, via its 6 SPI pads, in real
//   SPI mode. I2C over those pads is IMPOSSIBLE on this board (schematic
//   V1.0): the MOSI pad goes through an always-enabled unidirectional
//   74HC125 gate into SDI, so the sensor's ACKs/read data can never reach
//   the pad -- the bidirectional SDA level shifter (SI2302) sits only on the
//   Gravity connector's D pin. The pads ARE the intended SPI connector.
//   Per BME680 datasheet 6.1 the first LOW edge on CSB latches the chip into
//   SPI mode until power-on-reset -- exactly what we want here.
//     VCC->3V3  GND->GND
//     SCLK->GPIO23 (SCK)  MOSI->GPIO22 (MOSI)  MISO->GPIO21 (MISO)
//     CS->GPIO18 (silkscreen D7)
//     (23/22/21 are the board's silkscreened default SPI pins)
//   ENS160+AHT21 combo on the board-default I2C bus: VCC->3V3, GND->GND,
//     SDA->GPIO19, SCL->GPIO20; CS bridged HIGH -> I2C mode, ENS160 at 0x53
//     (ENS160_I2CADDR_1), AHT21 at 0x38.
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
// Libraries: bsec2 (+ dep BME68x Sensor library), Adafruit AHTX0,
// ScioSense_ENS160, PubSubClient.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <bsec2.h>
#include <time.h>
#include <math.h>
#include <esp_system.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST

// ---- I2C (ENS160+AHT21 combo, board-default silkscreen pins) ----
#define I2C_SDA        19
#define I2C_SCL        20

// ---- SPI (BME680 via the SEN0248 pads, board-default SPI pins) ----
#define BME_SPI_SCK    23
#define BME_SPI_MOSI   22
#define BME_SPI_MISO   21
#define BME_SPI_CS     18

// ---- Cadence ----
#define READ_INTERVAL  30000UL   // ms between telemetry publishes
#define MQTT_SOCK_TIMEOUT  4     // s; fail a dropped SYN fast instead of the 15 s default

// ---- BSEC2 ----
// Config blob: BME680, 3.3 V supply, LP rate (3 s), 4-day calibration horizon.
// The include resolves against the bsec2 library's src/ directory.
const uint8_t bsecConfig[] = {
#include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};
// LP mode fires the gas heater every 3 s, which warms the package; BSEC
// subtracts this offset before reporting temp/humidity. Start value from the
// Bosch examples -- tune against the AHT21 on the same node (compare a few
// hours of temp_c vs the AHT's raw reading and adjust).
#define BSEC_TEMP_OFFSET_C   1.6f
#define BSEC_STATE_SAVE_MS   (6UL * 60UL * 60UL * 1000UL)   // 6 h
#define BSEC_STALE_MS        60000UL   // LP outputs come every 3 s; >60 s = dead
#define ARRAY_LEN(a)         (sizeof(a) / sizeof((a)[0]))

// ---- Static IPv4 (ESP32 WiFi.config() takes TWO DNS servers) ----
#define USE_STATIC_IP
IPAddress STATIC_IP   (10, 0, 0, 73);   // free address on your LAN
IPAddress STATIC_GW   (10, 0, 0,  1);
IPAddress STATIC_MASK (255, 255, 255, 0);
IPAddress STATIC_DNS  (10, 0, 0,  1);
IPAddress STATIC_DNS2 (1, 1, 1, 1);

// ---- MQTT ----
#define MQTT_PORT      1883
#define DEVICE_ID      "air04"
#define MQTT_TOPIC     "sensors/lab/" DEVICE_ID "/telemetry"
#define MQTT_CLIENT_ID "esp-" DEVICE_ID
// Combined payload (~300 B) + topic no longer fit PubSubClient's 256 B default
// buffer -- publish() would fail silently. 512 B, set once in setup().
#define MQTT_BUF_SIZE  512

// ---- Diagnostics ----
// Retained boot/reconnect record on a non-telemetry topic the ingest ignores,
// so it never reaches the DB. Watch with: mosquitto_sub -t 'sensors/lab/air04/debug'
#define DEBUG_TOPIC    "sensors/lab/" DEVICE_ID "/debug"

Adafruit_AHTX0 aht;
ScioSense_ENS160 ens160(ENS160_I2CADDR_1);   // 0x53 (CS bridged HIGH)
Bsec2 envSensor;
Preferences prefs;                            // NVS: BSEC calibration state
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastRead = 0;
bool ntpStarted = false;
bool ntpSynced  = false;
bool ahtOk      = false;
bool ensOk      = false;
bool bsecOk     = false;
bool wifiWasUp  = false;         // re-entry after a drop needs a clean disconnect
uint32_t mqttConnects = 0;       // 1 = boot, >1 = reconnects since boot
uint32_t pubOk = 0, pubFail = 0;
unsigned long g_wifiMs = 0;

// Latest BSEC outputs (written by bsecDataCallback, read by the publish block).
float   bsecTempC   = NAN;   // heat-compensated
float   bsecHum     = NAN;   // heat-compensated
float   bsecPresHpa = NAN;
float   bsecGasKohm = NAN;   // raw gas resistance
float   bsecIaq     = NAN;   // static IAQ (stationary device)
float   bsecCo2Eq   = NAN;   // ppm
float   bsecBvocEq  = NAN;   // ppm
uint8_t bsecIaqAcc  = 0;     // 0..3
unsigned long bsecLastOutput = 0;
bool    bsecStateEverSaved = false;
unsigned long bsecLastStateSave = 0;

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
    char dbg[240];
    snprintf(dbg, sizeof(dbg),
      "{\"boot_ms\":%lu,\"reset\":%d,\"aht\":%d,\"ens\":%d,\"bsec\":%d,"
      "\"iaq_acc\":%u,\"ip\":\"%s\",\"rssi\":%d,\"wifi_ms\":%lu,"
      "\"connects\":%lu,\"pub_ok\":%lu,\"pub_fail\":%lu}",
      millis(), (int)esp_reset_reason(), ahtOk ? 1 : 0, ensOk ? 1 : 0,
      bsecOk ? 1 : 0, bsecIaqAcc,
      WiFi.localIP().toString().c_str(), WiFi.RSSI(), g_wifiMs,
      (unsigned long)mqttConnects, (unsigned long)pubOk, (unsigned long)pubFail);
    mqttClient.publish(DEBUG_TOPIC, dbg, true);   // retained
  } else {
    Serial.printf("MQTT FAILED, state=%d\n", mqttClient.state());
  }
}

// ================== BSEC2 (BME680) ==================

void printBsecStatus(const char *what) {
  Serial.printf("BSEC %s: bsec_status=%d bme68x_status=%d\n",
    what, (int)envSensor.status, (int)envSensor.sensor.status);
}

bool loadBsecState() {
  uint8_t st[BSEC_MAX_STATE_BLOB_SIZE];
  size_t n = prefs.getBytes("state", st, sizeof(st));
  if (n != BSEC_MAX_STATE_BLOB_SIZE) return false;   // nothing stored yet
  if (!envSensor.setState(st)) { printBsecStatus("setState FAILED"); return false; }
  return true;
}

bool saveBsecState() {
  uint8_t st[BSEC_MAX_STATE_BLOB_SIZE];
  if (!envSensor.getState(st)) { printBsecStatus("getState FAILED"); return false; }
  if (prefs.putBytes("state", st, BSEC_MAX_STATE_BLOB_SIZE) != BSEC_MAX_STATE_BLOB_SIZE) {
    Serial.println(F("BSEC state: NVS write FAILED"));
    return false;
  }
  bsecStateEverSaved = true;
  bsecLastStateSave = millis();
  Serial.println(F("BSEC state saved to NVS"));
  return true;
}

// Save only calibrated (acc=3) states: overwriting a stored calibrated state
// with an early acc<3 one would throw calibration away on the next reboot.
void maybeSaveBsecState() {
  if (bsecIaqAcc < 3) return;
  if (!bsecStateEverSaved || millis() - bsecLastStateSave >= BSEC_STATE_SAVE_MS) {
    saveBsecState();
  }
}

void bsecDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
  (void)data; (void)bsec;
  if (!outputs.nOutputs) return;
  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData &o = outputs.output[i];
    switch (o.sensor_id) {
      case BSEC_OUTPUT_STATIC_IAQ:
        bsecIaq = o.signal;
        bsecIaqAcc = o.accuracy;
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:      bsecCo2Eq   = o.signal;           break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT: bsecBvocEq = o.signal;          break;
      // NOTE: RAW_PRESSURE arrives already in hPa here (verified on-device:
      // /100 produced 9.65 instead of ~965 hPa), despite Bosch docs saying Pa.
      case BSEC_OUTPUT_RAW_PRESSURE:        bsecPresHpa = o.signal;           break;
      case BSEC_OUTPUT_RAW_GAS:             bsecGasKohm = o.signal / 1000.0f; break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: bsecTempC = o.signal; break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:    bsecHum   = o.signal; break;
      default: break;   // RUN_IN_STATUS etc.: subscribed for the algorithm, not published
    }
  }
  bsecLastOutput = millis();
  maybeSaveBsecState();
}

bool initBsec() {
  // SPI, not I2C: on the SEN0248 the pads can't speak I2C (see header). The
  // first CS LOW edge flips the BME680 into SPI mode until power-on-reset.
  SPI.begin(BME_SPI_SCK, BME_SPI_MISO, BME_SPI_MOSI, BME_SPI_CS);
  if (!envSensor.begin(BME_SPI_CS, SPI)) { printBsecStatus("begin FAILED"); return false; }
  envSensor.setTemperatureOffset(BSEC_TEMP_OFFSET_C);
  if (!envSensor.setConfig(bsecConfig)) { printBsecStatus("setConfig FAILED"); return false; }
  if (loadBsecState()) {
    bsecStateEverSaved = true;   // a good state exists; periodic saves may refresh it
    Serial.println(F("BSEC state restored from NVS (calibration resumes)"));
  } else {
    Serial.println(F("BSEC no stored state (fresh calibration, iaq_acc starts at 0)"));
  }
  bsecSensor sensorList[] = {
    BSEC_OUTPUT_STATIC_IAQ,                       // IAQ for stationary devices
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_RUN_IN_STATUS,
  };
  if (!envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP)) {
    printBsecStatus("updateSubscription FAILED");
    return false;
  }
  envSensor.attachCallback(bsecDataCallback);
  Serial.printf("BSEC2 UP via SPI (CS=GPIO%d, lib %d.%d.%d.%d, LP 3s, static IAQ)\n",
    BME_SPI_CS,
    envSensor.version.major, envSensor.version.minor,
    envSensor.version.major_bugfix, envSensor.version.minor_bugfix);
  return true;
}

// ====================================================

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 800) delay(10);   // CDC: bounded wait only
  Serial.println();
  Serial.printf("Boot: %s on FireBeetle 2 ESP32-C6 (BME680/BSEC2 + ENS160+AHT21, always-on)\n",
    DEVICE_ID);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  prefs.begin("bsec", false);   // NVS namespace for the BSEC calibration state

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

  bsecOk = initBsec();
  if (!bsecOk) Serial.println(F("BME680/BSEC2: FAILED"));

  if (!ahtOk || !ensOk) {
    Serial.println(F("  -> check I2C bus SDA=GPIO19 SCL=GPIO20, 3V3, GND, combo CS bridged HIGH"));
  }
  if (!bsecOk) {
    Serial.println(F("  -> check BME680 SPI pads: SCLK->23 MOSI->22 MISO->21 CS->18, 3V3, GND"));
  }

  mqttClient.setBufferSize(MQTT_BUF_SIZE);
  connectWifi();
  connectMqtt();
  delay(500);
}

void loop() {
  // Every iteration: keep MQTT alive and let BSEC decide when its next 3 s
  // sample is due. A blocking WiFi reconnect (<=15 s) skips a few BSEC slots;
  // that's fine, the library re-schedules.
  mqttClient.loop();
  if (bsecOk && !envSensor.run() && envSensor.status < BSEC_OK) {
    static bsec_library_return_t lastErr = BSEC_OK;
    if (envSensor.status != lastErr) {   // log state changes, don't spam
      lastErr = envSensor.status;
      printBsecStatus("run ERROR");
    }
  }

  if (millis() - lastRead < READ_INTERVAL) return;
  lastRead = millis();

  connectWifi();
  connectMqtt();
  mqttClient.loop();

  float tAht = NAN, hAht = NAN;
  if (ahtOk) {
    sensors_event_t humEvent, tempEvent;
    if (aht.getEvent(&humEvent, &tempEvent)) {
      tAht = tempEvent.temperature;
      hAht = humEvent.relative_humidity;
    }
  }

  bool bsecFresh = bsecOk && bsecLastOutput != 0 &&
                   millis() - bsecLastOutput < BSEC_STALE_MS;

  int eco2 = -1, tvoc = -1, aqi = -1;
  if (ensOk) {
    // T/RH compensation: prefer the AHT21 (the combo board pairs it with the
    // ENS160 for exactly this), fall back to BSEC's compensated values.
    float tEnv = !isnan(tAht) ? tAht : (bsecFresh ? bsecTempC : NAN);
    float hEnv = !isnan(hAht) ? hAht : (bsecFresh ? bsecHum   : NAN);
    if (!isnan(tEnv) && !isnan(hEnv)) {
      ens160.set_envdata(tEnv, hEnv);
    }
    ens160.measure(true);
    ens160.measureRaw(true);
    eco2 = ens160.geteCO2();
    tvoc = ens160.getTVOC();
    aqi  = ens160.getAQI();
  }

  // Published temp/hum: BSEC's heat-compensated values (better calibrated,
  // self-heating removed), AHT21 as fallback.
  float tPub = bsecFresh && !isnan(bsecTempC) ? bsecTempC : tAht;
  float hPub = bsecFresh && !isnan(bsecHum)   ? bsecHum   : hAht;

  if (isnan(tPub) && eco2 < 0 && !bsecFresh) {
    Serial.println(F("All sensors failed (I2C 19/20 for ENS160+AHT21, SPI 23/22/21/18 for BME680)"));
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
  char payload[384];
  int n = snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\",\"ts\":%lu",
    DEVICE_ID, (unsigned long)now);
  if (!isnan(tPub) && !isnan(hPub)) {
    n += snprintf(payload + n, sizeof(payload) - n,
      ",\"temp_c\":%.1f,\"humidity\":%.0f,\"heat_index_c\":%.1f",
      tPub, hPub, heatIndexC(tPub, hPub));
  }
  if (bsecFresh) {
    if (!isnan(bsecPresHpa)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"pressure_hpa\":%.2f", bsecPresHpa);
    }
    if (!isnan(bsecGasKohm) && bsecGasKohm > 0.0f) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"gas_kohm\":%.1f", bsecGasKohm);
    }
    if (!isnan(bsecIaq)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"iaq\":%.1f,\"iaq_acc\":%u", bsecIaq, bsecIaqAcc);
    }
    if (!isnan(bsecCo2Eq)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"co2_eq_ppm\":%.0f", bsecCo2Eq);
    }
    if (!isnan(bsecBvocEq)) {
      n += snprintf(payload + n, sizeof(payload) - n,
        ",\"bvoc_eq_ppm\":%.2f", bsecBvocEq);
    }
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
