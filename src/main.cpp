#include <Arduino.h>
#include <SD.h>
#include <LittleFS.h>
#include <esp_log.h>

#if __has_include(<esp32-hal-rgb-led.h>)
  #include <esp32-hal-rgb-led.h> // rgbLedWrite
#endif

#include "pins.h"
#include "ConfigManager.h"
#include "SdManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "LogManager.h"
#include "AppNetworkManager.h"
#include "SensorManager.h"
#include "HistoryManager.h"
#include "RestServer.h"
#include "MqttClientManager.h"
#include "WatchdogManager.h"
#include "OtaHealthManager.h"

static ConfigManager g_cfg;
static StatsManager g_stats;
static TimeManager g_time;
static LogManager g_log;
static SdManager g_sd;
static AppNetworkManager g_net;
static SensorManager g_sensors;
static HistoryManager g_history;
static MqttClientManager g_mqtt;
static RestServer g_rest;
static WatchdogManager g_wdt;
static OtaHealthManager g_otaHealth;

static uint32_t g_nextStatsSaveMs = 0;
static uint32_t g_nextSystemPublishMs = 0;

static uint8_t g_stage = 0;
static uint32_t g_stageNextMs = 0;
static bool g_runNet = false;
static bool g_runSensors = false;
static bool g_runMqtt = false;
static bool g_mqttEnabled = false;
static vprintf_like_t g_prevEspLogVprintf = nullptr;
static volatile bool g_inEspLogBridge = false;

static int bridgedEspLogVprintf(const char* fmt, va_list args) {
  va_list argsForPrev;
  va_copy(argsForPrev, args);
  const int out = g_prevEspLogVprintf ? g_prevEspLogVprintf(fmt, argsForPrev) : vprintf(fmt, argsForPrev);
  va_end(argsForPrev);

  if (!g_inEspLogBridge) {
    g_inEspLogBridge = true;
    char line[384];
    va_list argsForLine;
    va_copy(argsForLine, args);
    const int n = vsnprintf(line, sizeof(line), fmt, argsForLine);
    va_end(argsForLine);
    if (n > 0) {
      g_log.forwardExternalLine(line, true);
    }
    g_inEspLogBridge = false;
  }

  return out;
}

static String resetReasonStr() {
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

static void disableOnboardRgbOnce() {
  // Best effort: keep data line(s) low and also push an "off" frame.
  pinMode((int)PIN_RGB_DATA_PRIMARY, OUTPUT);
  digitalWrite((int)PIN_RGB_DATA_PRIMARY, LOW);
  pinMode((int)PIN_RGB_DATA_ALT, OUTPUT);
  digitalWrite((int)PIN_RGB_DATA_ALT, LOW);

#if __has_include(<esp32-hal-rgb-led.h>)
  // If a WS2812 is connected, this actually latches "off".
  rgbLedWrite((uint8_t)PIN_RGB_DATA_PRIMARY, 0, 0, 0);
  rgbLedWrite((uint8_t)PIN_RGB_DATA_ALT, 0, 0, 0);
#endif
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  // Kill the onboard RGB LED as early as possible (board variants).
  disableOnboardRgbOnce();

  // time baseline (uptime + async NTP)
  g_time.begin();

  // stats early (SD availability later)
  g_stats.begin(false);
  g_time.setStats(&g_stats);
  String resetReason = resetReasonStr();

  // logger (serial always) - SD availability later
  g_log.begin(g_time, g_stats, false);
  g_time.setLog(&g_log);
  g_log.setLevel(LogLevel::INFO);
  g_prevEspLogVprintf = esp_log_set_vprintf(bridgedEspLogVprintf);

  g_log.info("booting...");
  g_log.info(String("reset reason: ") + resetReason);

  // LittleFS (project config via uploadfs)
  bool littleFsOk = LittleFS.begin(false);
  if (!littleFsOk) {
    g_log.warn("LittleFS mount failed/unavailable");
  } else {
    g_log.info("LittleFS mounted");
  }

  // SD
  g_log.info("mounting SD (single attempt)...");
  bool sdOk = g_sd.mountWithTimeout(0, 20000000);
  if (!sdOk) {
    g_log.warn("SD mount failed/unavailable");
    g_stats.incrementSdMountFails();
  } else {
    g_log.info("SD mounted");
  }
  g_log.setSdAvailable(sdOk);

  g_stats.setSdAvailable(sdOk);

  // Stats load + boot count
  if (sdOk) g_stats.load();
  g_stats.setLastResetReason(resetReason);
  g_stats.incrementBootCount();

  // Config
  g_cfg.begin(g_log);
  if (!g_cfg.loadFromSources(sdOk, littleFsOk)) {
    g_log.error("FAIL-FAST: unable to load valid config from LittleFS/SD. Halting.");
    for(;;) { g_wdt.feed(); delay(0); }
  }

  const AppConfig& cfg = g_cfg.get();
  g_log.configure(cfg.logging);
  g_mqttEnabled = cfg.mqtt.enabled;
// Watchdog (after logger)
  g_wdt.begin(cfg.watchdog, g_log);

  // Network
  g_net.begin(cfg, g_log, g_time);
  g_otaHealth.begin(cfg.ota, g_log, g_net);

  // Sensors
  g_sensors.begin(cfg.sensors, g_log, g_stats, g_time);

  // History
  g_history.begin(cfg.history, g_log, g_stats, g_time, sdOk);

  // MQTT
  if (g_mqttEnabled) {
    g_mqtt.begin(cfg, g_net, g_log, g_stats, g_time);
  }

  // Sensor callback: publish + history
  g_sensors.onReading([](const SensorReading& r) {
    g_history.enqueue(r);
    // Serial log for each new temperature reading
    String v = isnan(r.tempC) ? String("nan") : String(r.tempC, 2);
    g_log.info(String("temp update: ") + r.id + "=" + v + "C status=" + String((int)r.status));
    if (g_mqttEnabled) {
      g_mqtt.publishSensor(r);
    }
  });

  // REST
  if (cfg.rest.enabled) {
    g_rest.begin(cfg, g_log, g_time, g_net, g_sensors, g_history, g_stats, g_mqtt, g_cfg);
  }

  // Save last boot epoch if time valid
  auto ts = g_time.now();
  if (ts.valid) {
    g_stats.setLastBootEpochMs(ts.epochMs);
  }

  g_nextStatsSaveMs = millis() + 5000;
  g_nextSystemPublishMs = millis() + 10000;

  g_stage = 0;
  g_stageNextMs = millis() + 1000;
  g_runNet = false;
  g_runSensors = false;
  g_runMqtt = false;
  g_log.warn("staged startup: net/sensors/mqtt initially paused");
  g_log.info("setup complete");
}

void loop() {
  g_wdt.feed();

  // staged startup (helps identify watchdog root cause)
  uint32_t now = millis();
  if ((int32_t)(now - g_stageNextMs) >= 0) {
    g_stageNextMs = now + 2000;
    if (g_stage == 0) {
      g_runNet = true;
      g_log.warn("stage 1: Network loop enabled");
      g_stage = 1;
    } else if (g_stage == 1) {
      g_runSensors = true;
      g_log.warn("stage 2: Sensor loop enabled");
      g_stage = 2;
    } else if (g_stage == 2) {
      if (g_mqttEnabled) {
        g_runMqtt = true;
        g_log.warn("stage 3: MQTT loop enabled");
      } else {
        g_log.warn("stage 3: MQTT disabled by config");
      }
      g_stage = 3;
    }
  }

  g_time.loop();
  if (g_runNet) g_net.loop();
  g_otaHealth.loop();
  if (g_runSensors) g_sensors.loop();
  if (g_runMqtt && g_mqttEnabled) g_mqtt.loop();

  // periodic system publish
  if ((int32_t)(now - g_nextSystemPublishMs) >= 0) {
    g_nextSystemPublishMs = now + 15000;
    if (g_runMqtt && g_mqttEnabled) g_mqtt.publishSystem();
  }

  // periodic stats save (SD optional)
  if ((int32_t)(now - g_nextStatsSaveMs) >= 0) {
    g_nextStatsSaveMs = now + 30000;
    (void)g_stats.save();
  }

  // yield (no delay)
  delay(0);
}
