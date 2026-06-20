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
#include "DriftDetector.h"

// ---------- build/identity ----------
#define FW_VERSION "1.0.5"
static String deviceId;

// ---------- pins & sampling ----------
constexpr int   PIN_CT        = 5;       // ADC1 channel
constexpr int   SAMPLE_HZ     = 2000;    // 40 samples/cycle @ 50 Hz
constexpr int   N_SAMPLES     = 400;     // 10 full cycles per window
constexpr float ADC_VREF      = 3.3f;
constexpr float ADC_MAX       = 4095.0f;
static float    ct_a_per_v    = 30.0f;   // default fallback
static float    calGain       = 1.0f;    // trimmed against Metrawatt clamp meter

// ---------- config (NVS-backed, remotely updatable) ----------
static Preferences prefs;
static uint32_t telemetryIntervalS = 5;
static bool learningMode = true;
static BinnedDriftDetector detector(3.0, 200);
static DriftDetector irmsDetector(3.0, 500);

// ---------- networking ----------
static WiFiClient   net;
static PubSubClient mqtt(net);

#if __has_include("local_config.h")
#include "local_config.h"
#else
static const char* WIFI_SSID   = "YOUR_WIFI_SSID";
static const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
static const char* MQTT_HOST   = "YOUR_MQTT_HOST_IP";
static const int   MQTT_PORT   = 1883;
static const char* BACKEND_URL = "http://YOUR_MQTT_HOST_IP:8000";
#endif

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
  const float irms   = vrms * ct_a_per_v * calGain;
  float thd          = dsp::thd_percent(sampleBuf, N_SAMPLES, SAMPLE_HZ, 50.0f);
  if (irms < 0.5f) {
    thd = 0.0f; // Mask high-THD noise floor when current is close to zero
  }
  const float p_est  = irms * 230.0f;          // apparent power estimate (fixed V)

  // Unsupervised drift/anomaly detection on the load-binned THD%
  double z_score = detector.process(irms, thd, learningMode);
  bool is_anomaly = (z_score > detector.getZThreshold()) && !learningMode && (z_score >= 0.0);

  // Global long-term RMS current drift detection
  double irms_z = irmsDetector.processValue(irms, learningMode);
  bool is_irms_anomaly = false; // Disabled to prevent false alarms on transient load changes; irms_z is still calculated & published

  JsonDocument doc;
  doc["device_id"]    = deviceId;
  doc["irms_a"]       = serialized(String(irms, 3));
  doc["thd_pct"]      = serialized(String(thd, 1));
  doc["p_est_w"]      = serialized(String(p_est, 0));
  
  if (z_score >= 0.0) {
    doc["z_score"] = serialized(String(z_score, 2));
  } else {
    doc["z_score"] = nullptr;
  }

  if (irms_z >= 0.0) {
    doc["irms_z"] = serialized(String(irms_z, 2));
  } else {
    doc["irms_z"] = nullptr;
  }

  doc["learn_status"] = learningMode ? "learning" : (((z_score >= 0.0) || (irms_z >= 0.0)) ? "monitoring" : "uncharacterized");
  doc["fw_version"]   = FW_VERSION;
  doc["source"]       = "ct";

  char payload[256];
  serializeJson(doc, payload);
  String topic = "volmax/" + deviceId + "/telemetry";
  mqtt.publish(topic.c_str(), payload, false);

  if (is_anomaly || is_irms_anomaly) {
    JsonDocument alertDoc;
    alertDoc["device_id"]  = deviceId;
    alertDoc["irms_a"]     = serialized(String(irms, 3));
    alertDoc["thd_pct"]    = serialized(String(thd, 1));
    if (is_anomaly) {
      alertDoc["z_score"]    = serialized(String(z_score, 2));
      alertDoc["alert_type"] = "thd_drift";
    } else {
      alertDoc["z_score"]    = serialized(String(irms_z, 2));
      alertDoc["alert_type"] = "irms_drift";
    }
    
    char alertPayload[256];
    serializeJson(alertDoc, alertPayload);
    String alertTopic = "volmax/" + deviceId + "/alerts";
    mqtt.publish(alertTopic.c_str(), alertPayload, false);
  }
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
      // Parse bin edges if sent by the backend
      const char* edges_str = doc["bin_edges"] | "";
      if (strlen(edges_str) > 0) {
        double new_edges[4];
        int parsed = sscanf(edges_str, "%lf,%lf,%lf,%lf", &new_edges[0], &new_edges[1], &new_edges[2], &new_edges[3]);
        if (parsed == 4) {
          detector.setBinEdges(new_edges);
          detector.save(prefs);
          Serial.print("Applied new bin edges: ");
          Serial.println(edges_str);
        }
      }
      // Remote configuration of baseline learning state (pull model)
      bool learn = doc["learning_mode"] | true;
      if (learn != learningMode) {
        learningMode = learn;
        prefs.putBool("learn_mode", learn);
        if (!learn) {
          detector.lockAll();
          irmsDetector.lockBaseline();
          detector.save(prefs); // Safe write to flash: only triggered on manual state transition
          irmsDetector.save(prefs, "irms", 0);
          Serial.println("Baseline locked and saved to NVS.");
        } else {
          detector.resetAll();
          irmsDetector.reset();
          detector.save(prefs); // Safe write to flash: clears baseline metrics
          irmsDetector.save(prefs, "irms", 0);
          Serial.println("Baseline reset, learning started.");
        }
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

  uint64_t mac = ESP.getEfuseMac();
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "esp32s3-%02x%02x%02x%02x%02x%02x",
           (uint8_t)(mac & 0xFF),
           (uint8_t)((mac >> 8) & 0xFF),
           (uint8_t)((mac >> 16) & 0xFF),
           (uint8_t)((mac >> 24) & 0xFF),
           (uint8_t)((mac >> 32) & 0xFF),
           (uint8_t)((mac >> 40) & 0xFF));
  deviceId = String(macStr);

  prefs.begin("volmax", false);
  telemetryIntervalS = prefs.getUInt("interval", 5);
  learningMode = prefs.getBool("learn_mode", true);
  detector.load(prefs); // Restore drift detector baselines from NVS
  irmsDetector.load(prefs, "irms", 0);
  // Initialize calibration based on device type (MAC)
  // Node 3 (SCT-013-000 with 150 Ohm burden) -> 13.33 A/V, calibrated gain against Metrawatt -> 1.118
  // Node 1 & 2 (SCT-013-030) -> 30.0 A/V, default gain -> 1.0
  if (deviceId == "esp32s3-98a316f06bb0") {
    ct_a_per_v = 13.33f;
    calGain    = prefs.getFloat("calgain", 1.118f);
  } else {
    ct_a_per_v = 30.0f;
    calGain    = prefs.getFloat("calgain", 1.0f);
  }
  // Allow NVS override if configured
  ct_a_per_v         = prefs.getFloat("ct_a_per_v", ct_a_per_v);

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
