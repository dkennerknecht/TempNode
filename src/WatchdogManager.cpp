#include "WatchdogManager.h"
#include "Config.h"
#include "LogManager.h"
#include <esp_task_wdt.h>

void WatchdogManager::begin(const WatchdogConfig& cfg, LogManager& log) {
  _cfg = &cfg;
  _log = &log;

  _enabled = cfg.enabled;
  if (!_enabled) return;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t conf = {};
  conf.timeout_ms = cfg.timeoutMs;
  conf.idle_core_mask = 0;
  conf.trigger_panic = cfg.panicReset;

  esp_err_t err = esp_task_wdt_init(&conf);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    _log->error("WDT init failed");
    _enabled = false;
    return;
  }
#else
  // Arduino-ESP32 2.x API
  esp_err_t err = esp_task_wdt_init(cfg.timeoutMs / 1000, cfg.panicReset);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    _log->error("WDT init failed");
    _enabled = false;
    return;
  }
#endif

  addTask(NULL, "loopTask");
  _log->info(String("WDT enabled, timeoutMs=") + cfg.timeoutMs);
}

void WatchdogManager::addTask(TaskHandle_t task, const char* name) {
  if (!_enabled) return;
  (void)name;
  esp_task_wdt_add(task);
}

void WatchdogManager::feed() {
  if (!_enabled) return;
  esp_task_wdt_reset();
}
