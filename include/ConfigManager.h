#pragma once
#include <Arduino.h>
#include "Config.h"

class LogManager;

class ConfigManager {
public:
  void begin(LogManager& log);
  const AppConfig& get() const { return _cfg; }

  // Load optional /config.json from SD if available
  bool loadFromSd();

  String deviceId() const;

private:
  LogManager* _log = nullptr;
  AppConfig _cfg;

  void applyDefaults();
  void syncFeatureFlags();
  bool parseJson(const String& json);
  bool validate();
};
