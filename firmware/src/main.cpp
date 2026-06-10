/**
 * VolMax Edge Stack — ESP32-S3 field device firmware
 *
 * SCT-013 CT clamp -> ADC sampling -> on-device RMS + THD (FFT)
 * -> MQTT telemetry -> backend. Pull-based OTA + remote config:
 * the device periodically GETs /devices/<id> and applies
 * pending_ota_url / telemetry_interval_s. Simple, robust, no
 * inbound connections to the device.
 *
 * Hardware: ESP32-S3 DevKit, SCT-013-030 (1V @ 30A) on ADC pin,
 * midpoint bias divider (2x10k to 3.3V/GND) + burden per CT spec.
 * Calibration notes: docs/calibration.md
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "dsp.h"

// ---------- build/identity ----------
#define FW_VERSION "1.0.0"
static String deviceId;

// ---------- pins & sampling ----------
constexpr int   PIN_CT        = 5;       // ADC1 channel
constexpr int   SAMPLE_HZ     = 2000;    // 40 samples/cycle @ 50 Hz
constexpr int   N_SAMPLES     = 400;     // 10 full cycles per window
constexpr float ADC_VREF      = 3.3f;
constexpr float ADC_MAX       = 4095.0f;
constexpr float CT_A_PER_V    = 30.0f;   // SCT-013-030: 30 A -> 1 V
static float    calGain       = 1.0f;    // trimmed against MAVOWATT 45

// ---------- config (NVS-backed, remotely updatable) ----------
static Preferences prefs;
static uint32_t telemetryIntervalS = 5;

// ---------- networking ----------
static WiFiClient   net;
static PubSubClient mqtt(net);
static const char* WIFI_SSID   = "Ivan's Galaxy Tab S8 5G";
static const char* WIFI_PASS   = "44444444";
static const char* MQTT_HOST   = "192.168.212.153";
static const int   MQTT_PORT   = 1883;
static const char* BACKEND_URL = "http://192.168.212.153:8000";

static float sampleBuf[N_SAMPLES];

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    esp_task_wdt_reset();
  }
}

static void connectMqtt() {
  while (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    mqtt.connect(deviceId.c_str());
    if (!mqtt.connected()) { delay(1000); esp_task_wdt_reset(); }
  }
}

/** Sample one window at SAMPLE_HZ into sampleBuf (volts, bias removed). */
static void acquireWindow() {
  const uint32_t periodUs = 1000000UL / SAMPLE_HZ;
  uint32_t next = micros();
  double acc = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    while ((int32_t)(micros() - next) < 0) {}
    next += periodUs;
    float v = analogReadMilliVolts(PIN_CT) / 1000.0f;
    sampleBuf[i] = v;
    acc += v;
  }
  const float bias = acc / N_SAMPLES;          // remove DC midpoint
  for (int i = 0; i < N_SAMPLES; i++) sampleBuf[i] -= bias;
}

static void publishTelemetry() {
  acquireWindow();
  const float vrms   = dsp::rms(sampleBuf, N_SAMPLES);
  const float irms   = vrms * CT_A_PER_V * calGain;
  const float thd    = dsp::thd_percent(sampleBuf, N_SAMPLES, SAMPLE_HZ, 50.0f);
  const float p_est  = irms * 230.0f;          // apparent power estimate (fixed V)

  JsonDocument doc;
  doc["device_id"]  = deviceId;
  doc["irms_a"]     = serialized(String(irms, 3));
  doc["thd_pct"]    = serialized(String(thd, 1));
  doc["p_est_w"]    = serialized(String(p_est, 0));
  doc["fw_version"] = FW_VERSION;
  doc["source"]     = "ct";

  char payload[256];
  serializeJson(doc, payload);
  String topic = "volmax/" + deviceId + "/telemetry";
  mqtt.publish(topic.c_str(), payload, false);
}

/** Poll backend for remote config + pending OTA. Pull model. */
static void performOta(const String& url);
static void pollFleetControl() {
  HTTPClient http;
  http.begin(String(BACKEND_URL) + "/devices/" + deviceId);
  http.setTimeout(4000);
  if (http.GET() == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      uint32_t iv = doc["telemetry_interval_s"] | telemetryIntervalS;
      if (iv != telemetryIntervalS && iv >= 1 && iv <= 3600) {
        telemetryIntervalS = iv;
        prefs.putUInt("interval", iv);
      }
      const char* ota = doc["pending_ota_url"] | "";
      if (strlen(ota) > 8) {
        http.end();
        performOta(String(ota));
        return;
      }
    }
  }
  http.end();
}

static void performOta(const String& url) {
  // Ack first so a crashing image cannot cause an update loop.
  HTTPClient ack;
  ack.begin(String(BACKEND_URL) + "/devices/" + deviceId + "/ota/ack");
  ack.POST("");
  ack.end();

  WiFiClient otaClient;
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(otaClient, url);
  if (r == HTTP_UPDATE_FAILED) {
    Serial.printf("OTA failed: %s\n", httpUpdate.getLastErrorString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CT, ADC_11db);

  deviceId = "esp32s3-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);

  prefs.begin("volmax", false);
  telemetryIntervalS = prefs.getUInt("interval", 5);
  calGain            = prefs.getFloat("calgain", 1.0f);

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
}

void loop() {
  static uint32_t lastTelemetry = 0;
  static uint32_t lastPoll      = 0;

  esp_task_wdt_reset();
  connectWiFi();          // self-heals after router loss
  connectMqtt();
  mqtt.loop();

  if (millis() - lastTelemetry >= telemetryIntervalS * 1000UL) {
    lastTelemetry = millis();
    if (mqtt.connected()) publishTelemetry();
  }
  if (millis() - lastPoll >= 10000UL) {
    lastPoll = millis();
    if (WiFi.status() == WL_CONNECTED) pollFleetControl();
  }
  delay(5);
}
