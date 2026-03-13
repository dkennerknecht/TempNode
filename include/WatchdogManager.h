#pragma once
#include <Arduino.h>

class LogManager;
struct WatchdogConfig;

class WatchdogManager {
public:
  void begin(const WatchdogConfig& cfg, LogManager& log);
  void addTask(TaskHandle_t task, const char* name);
  void feed();

private:
  const WatchdogConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  bool _enabled = false;
};
